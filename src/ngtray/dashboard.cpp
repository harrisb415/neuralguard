// NeuralGuard config dashboard - native Win32 (Common Controls), hosted by the
// (elevated) tray. Tabs: Live / Rules / Habits from the DB. A button bar runs
// ngd/ngctl HIDDEN (no console) and streams their output into an in-window log
// pane. Status bar shows the live mode ngd publishes to meta('mode').
#include "ngtray/dashboard.h"

#include "core/db.h"

#include <windows.h>
#include <commctrl.h>

#include <string>
#include <thread>
#include <vector>

namespace ng {
namespace {

enum { TAB_LIVE = 0, TAB_RULES = 1, TAB_HABITS = 2, TAB_COUNT = 3 };
enum { IDB_ENFORCE = 100, IDB_LEARN, IDB_STOP, IDB_PANIC, IDB_REFRESH, IDB_COUNT_ = 5 };
constexpr UINT WM_APP_LOG = WM_APP + 7;

const wchar_t* kBtnLabel[IDB_COUNT_] = {L"Enforce", L"Learn", L"Stop", L"Panic", L"Refresh"};

HWND g_dash = nullptr, g_tabs = nullptr, g_status = nullptr, g_log = nullptr;
HWND g_lv[TAB_COUNT] = {};
HWND g_btn[IDB_COUNT_] = {};
int  g_cur = 0;
long long g_lastEventId = -1;
HANDLE g_child = nullptr;   // the running ngd daemon (enforce/record), if any

std::wstring ExeDir() {
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p; size_t i = s.find_last_of(L"\\/");
    return (i == std::wstring::npos) ? L"." : s.substr(0, i);
}
std::string DbPathU8() {
    std::wstring w = ExeDir() + L"\\ngpolicy.db";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring Widen(const char* s) {
    if (!s) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    if (!w.empty() && w.back() == 0) w.pop_back();
    return w;
}

// ---- log pane -------------------------------------------------------------
void AppendLog(const std::wstring& in) {
    std::wstring out; out.reserve(in.size() + 8);
    for (wchar_t c : in) { if (c == L'\n') out += L"\r\n"; else if (c != L'\r') out += c; }
    if (GetWindowTextLengthW(g_log) > 28000) {          // cap growth
        SendMessageW(g_log, EM_SETSEL, 0, 8000);
        SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)L"");
    }
    int len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, len, len);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)out.c_str());
}

// Build "<dir>\exe" args into a mutable command line + common startup info.
bool StartHidden(const wchar_t* exe, const std::wstring& args, PROCESS_INFORMATION& pi, HANDLE& rd) {
    std::wstring dir = ExeDir();
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = nullptr;
    std::wstring cmd = L"\"" + dir + L"\\" + exe + L"\" " + args;
    std::vector<wchar_t> mut(cmd.begin(), cmd.end()); mut.push_back(0);
    BOOL ok = CreateProcessW(nullptr, mut.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); rd = nullptr; }
    return ok != 0;
}

// Long-running tool (enforce/record): stream output to the log via a thread.
void RunAsync(const wchar_t* exe, const std::wstring& args, bool track) {
    PROCESS_INFORMATION pi{}; HANDLE rd = nullptr;
    if (!StartHidden(exe, args, pi, rd)) { AppendLog(L"[dashboard] failed to launch\r\n"); return; }
    if (track) g_child = pi.hProcess; else CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    std::thread([rd] {
        char buf[1024]; DWORD n = 0;
        while (ReadFile(rd, buf, sizeof(buf) - 1, &n, nullptr) && n > 0) {
            int wn = MultiByteToWideChar(CP_UTF8, 0, buf, n, nullptr, 0);
            wchar_t* w = (wchar_t*)malloc((wn + 1) * sizeof(wchar_t));
            MultiByteToWideChar(CP_UTF8, 0, buf, n, w, wn); w[wn] = 0;
            PostMessageW(g_dash, WM_APP_LOG, (WPARAM)w, 0);
        }
        CloseHandle(rd);
    }).detach();
}

// Short tool (panic): run to completion, output straight to the log.
void RunSync(const wchar_t* exe, const std::wstring& args) {
    PROCESS_INFORMATION pi{}; HANDLE rd = nullptr;
    if (!StartHidden(exe, args, pi, rd)) return;
    char buf[1024]; DWORD n = 0;
    while (ReadFile(rd, buf, sizeof(buf) - 1, &n, nullptr) && n > 0) {
        int wn = MultiByteToWideChar(CP_UTF8, 0, buf, n, nullptr, 0);
        std::wstring w(wn, 0); MultiByteToWideChar(CP_UTF8, 0, buf, n, w.data(), wn);
        AppendLog(w);
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 3000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
}

// Stop the current daemon (if any) and flush all NeuralGuard filters (fail open).
void StopDaemon() {
    if (g_child) {
        TerminateProcess(g_child, 0);
        WaitForSingleObject(g_child, 2000);
        CloseHandle(g_child);
        g_child = nullptr;
    }
    RunSync(L"ngctl.exe", L"panic");
}

// ---- data tabs ------------------------------------------------------------
void AddCol(HWND lv, int i, const wchar_t* t, int cx) {
    LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    c.pszText = const_cast<LPWSTR>(t); c.cx = cx; c.iSubItem = i;
    ListView_InsertColumn(lv, i, &c);
}
void SetCell(HWND lv, int row, int col, const char* u8) {
    std::wstring w = Widen(u8);
    LVITEMW it{}; it.mask = LVIF_TEXT; it.iItem = row; it.iSubItem = col;
    it.pszText = const_cast<LPWSTR>(w.c_str());
    if (col == 0) ListView_InsertItem(lv, &it); else ListView_SetItem(lv, &it);
}

void FillHabits(HWND lv) {
    ListView_DeleteAllItems(lv);
    Db d; if (!d.open(DbPathU8().c_str())) return;
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(d.handle(),
        "SELECT process_label, dest, remote_port, round(count,1) FROM habits"
        " ORDER BY count DESC LIMIT 1000;", -1, &s, nullptr);
    int row = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        SetCell(lv, row, 0, (const char*)sqlite3_column_text(s, 0));
        SetCell(lv, row, 1, (const char*)sqlite3_column_text(s, 1));
        SetCell(lv, row, 2, std::to_string(sqlite3_column_int(s, 2)).c_str());
        SetCell(lv, row, 3, std::to_string(sqlite3_column_int(s, 3)).c_str());
        ++row;
    }
    sqlite3_finalize(s);
}
void FillRules(HWND lv) {
    ListView_DeleteAllItems(lv);
    Db d; if (!d.open(DbPathU8().c_str())) return;
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(d.handle(),
        "SELECT pi.image_path, fe.remote_port,"
        " COUNT(DISTINCT fe.local_port || '|' || fe.remote_addr) AS c"
        " FROM flow_events fe JOIN process_identity pi ON fe.image_id = pi.id"
        " WHERE fe.remote_port > 0 AND fe.remote_port < 49152 AND pi.image_path LIKE '_:\\%'"
        "   AND fe.verdict IN ('ALLOW','CAPALLOW')"
        " GROUP BY pi.image_path, fe.remote_port HAVING c >= 3 ORDER BY c DESC;", -1, &s, nullptr);
    int row = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        SetCell(lv, row, 0, (const char*)sqlite3_column_text(s, 0));
        SetCell(lv, row, 1, std::to_string(sqlite3_column_int(s, 1)).c_str());
        SetCell(lv, row, 2, std::to_string(sqlite3_column_int(s, 2)).c_str());
        ++row;
    }
    sqlite3_finalize(s);
}
void PollLive() {
    HWND lv = g_lv[TAB_LIVE];
    Db d; if (!d.open(DbPathU8().c_str())) return;
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(d.handle(),
        "SELECT fe.id, fe.ts_utc, fe.verdict,"
        " COALESCE(pi.signer, pi.image_path, fe.image_path),"
        " COALESCE(fe.remote_domain, fe.remote_addr), fe.remote_port"
        " FROM flow_events fe LEFT JOIN process_identity pi ON fe.image_id = pi.id"
        " WHERE fe.id > ? ORDER BY fe.id ASC LIMIT 300;", -1, &s, nullptr);
    sqlite3_bind_int64(s, 1, g_lastEventId);
    while (sqlite3_step(s) == SQLITE_ROW) {
        g_lastEventId = sqlite3_column_int64(s, 0);
        const char* ts = (const char*)sqlite3_column_text(s, 1);
        std::string tm = ts ? ts : "";
        if (tm.size() >= 19) tm = tm.substr(11, 8);
        SetCell(lv, 0, 0, tm.c_str());
        SetCell(lv, 0, 1, (const char*)sqlite3_column_text(s, 2));
        SetCell(lv, 0, 2, (const char*)sqlite3_column_text(s, 3));
        SetCell(lv, 0, 3, (const char*)sqlite3_column_text(s, 4));
        SetCell(lv, 0, 4, std::to_string(sqlite3_column_int(s, 5)).c_str());
    }
    sqlite3_finalize(s);
    for (int n = ListView_GetItemCount(lv); n > 500; --n) ListView_DeleteItem(lv, n - 1);
}

std::wstring ReadMode() {
    Db d; if (!d.open(DbPathU8().c_str())) return L"unknown";
    sqlite3_stmt* s = nullptr; std::wstring m = L"idle";
    sqlite3_prepare_v2(d.handle(), "SELECT v FROM meta WHERE k='mode';", -1, &s, nullptr);
    if (sqlite3_step(s) == SQLITE_ROW) m = Widen((const char*)sqlite3_column_text(s, 0));
    sqlite3_finalize(s);
    return m;
}
void UpdateStatus() {
    std::wstring t = L"  Mode:  " + ReadMode();
    SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)t.c_str());
}

void LayoutChildren() {
    RECT rc; GetClientRect(g_dash, &rc);
    SendMessageW(g_status, WM_SIZE, 0, 0);
    RECT sb; GetWindowRect(g_status, &sb);
    int sbh = sb.bottom - sb.top;
    const int barH = 38, logH = 130, bw = 84, bh = 26, gap = 6;
    int x = 8;
    for (int i = 0; i < IDB_COUNT_; ++i) { MoveWindow(g_btn[i], x, 6, bw, bh, TRUE); x += bw + gap; }
    int bottom = rc.bottom - sbh;
    MoveWindow(g_log, 0, bottom - logH, rc.right, logH, TRUE);
    MoveWindow(g_tabs, 0, barH, rc.right, bottom - logH - barH, TRUE);
    RECT disp = {0, barH, rc.right, bottom - logH};
    TabCtrl_AdjustRect(g_tabs, FALSE, &disp);
    for (int i = 0; i < TAB_COUNT; ++i)
        MoveWindow(g_lv[i], disp.left, disp.top, disp.right - disp.left, disp.bottom - disp.top, TRUE);
}

void ShowTab(int i) {
    g_cur = i;
    for (int k = 0; k < TAB_COUNT; ++k) ShowWindow(g_lv[k], k == i ? SW_SHOW : SW_HIDE);
    if (i == TAB_LIVE)        PollLive();
    else if (i == TAB_RULES)  FillRules(g_lv[TAB_RULES]);
    else                      FillHabits(g_lv[TAB_HABITS]);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_NOTIFY: {
            LPNMHDR n = (LPNMHDR)l;
            if (n->hwndFrom == g_tabs && n->code == TCN_SELCHANGE) ShowTab(TabCtrl_GetCurSel(g_tabs));
            return 0;
        }
        case WM_SIZE:  LayoutChildren(); return 0;
        case WM_TIMER: UpdateStatus(); if (g_cur == TAB_LIVE) PollLive(); return 0;
        case WM_APP_LOG: { wchar_t* p = (wchar_t*)w; AppendLog(p); free(p); return 0; }
        case WM_COMMAND:
            switch (LOWORD(w)) {
                case IDB_ENFORCE:
                    StopDaemon();
                    RunAsync(L"ngd.exe", L"enforce \"" + ExeDir() + L"\\ngpolicy.db\" 0", true);
                    break;
                case IDB_LEARN:
                    StopDaemon();
                    RunAsync(L"ngd.exe", L"record \"" + ExeDir() + L"\\ngpolicy.db\"", true);
                    break;
                case IDB_STOP:    StopDaemon(); AppendLog(L"[dashboard] stopped; failing open.\r\n"); break;
                case IDB_PANIC:   StopDaemon(); AppendLog(L"[dashboard] PANIC - all filters removed.\r\n"); break;
                case IDB_REFRESH: ShowTab(g_cur); break;
            }
            return 0;
        case WM_DESTROY: KillTimer(h, 1); g_dash = nullptr; return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void InitLiveCursor() {
    Db d; if (!d.open(DbPathU8().c_str())) { g_lastEventId = 0; return; }
    sqlite3_stmt* s = nullptr; long long maxId = 0;
    sqlite3_prepare_v2(d.handle(), "SELECT COALESCE(max(id),0) FROM flow_events;", -1, &s, nullptr);
    if (sqlite3_step(s) == SQLITE_ROW) maxId = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    g_lastEventId = maxId > 60 ? maxId - 60 : 0;
}

}  // namespace

void OpenDashboard(HINSTANCE hInst) {
    if (g_dash) { SetForegroundWindow(g_dash); return; }

    INITCOMMONCONTROLSEX ic{sizeof(ic), ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&ic);

    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = Proc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"ngDashWnd";
        RegisterClassW(&wc);
        reg = true;
    }
    g_dash = CreateWindowW(L"ngDashWnd", L"NeuralGuard", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, 900, 680, nullptr, nullptr, hInst, nullptr);

    for (int i = 0; i < IDB_COUNT_; ++i)
        g_btn[i] = CreateWindowW(L"BUTTON", kBtnLabel[i], WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 0, 0, 0, 0, g_dash, (HMENU)(INT_PTR)(IDB_ENFORCE + i), hInst, nullptr);

    g_tabs = CreateWindowW(WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, g_dash, nullptr, hInst, nullptr);
    TCITEMW ti{}; ti.mask = TCIF_TEXT;
    ti.pszText = const_cast<LPWSTR>(L"Live");   TabCtrl_InsertItem(g_tabs, TAB_LIVE, &ti);
    ti.pszText = const_cast<LPWSTR>(L"Rules");  TabCtrl_InsertItem(g_tabs, TAB_RULES, &ti);
    ti.pszText = const_cast<LPWSTR>(L"Habits"); TabCtrl_InsertItem(g_tabs, TAB_HABITS, &ti);

    DWORD lvStyle = WS_CHILD | LVS_REPORT | LVS_SHOWSELALWAYS;
    for (int i = 0; i < TAB_COUNT; ++i) {
        g_lv[i] = CreateWindowW(WC_LISTVIEWW, L"", lvStyle, 0, 0, 0, 0, g_dash, nullptr, hInst, nullptr);
        ListView_SetExtendedListViewStyle(g_lv[i], LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    }
    AddCol(g_lv[TAB_LIVE], 0, L"Time", 80);
    AddCol(g_lv[TAB_LIVE], 1, L"Verdict", 80);
    AddCol(g_lv[TAB_LIVE], 2, L"Application", 220);
    AddCol(g_lv[TAB_LIVE], 3, L"Destination", 320);
    AddCol(g_lv[TAB_LIVE], 4, L"Port", 60);
    AddCol(g_lv[TAB_RULES], 0, L"Application (permitted)", 460);
    AddCol(g_lv[TAB_RULES], 1, L"Port", 70);
    AddCol(g_lv[TAB_RULES], 2, L"Connections", 110);
    AddCol(g_lv[TAB_HABITS], 0, L"Application", 260);
    AddCol(g_lv[TAB_HABITS], 1, L"Destination", 340);
    AddCol(g_lv[TAB_HABITS], 2, L"Port", 70);
    AddCol(g_lv[TAB_HABITS], 3, L"Count", 70);

    g_log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                            0, 0, 0, 0, g_dash, nullptr, hInst, nullptr);
    g_status = CreateWindowW(STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, g_dash, nullptr, hInst, nullptr);

    InitLiveCursor();
    LayoutChildren();
    ShowTab(TAB_LIVE);
    UpdateStatus();
    AppendLog(L"NeuralGuard dashboard ready.\r\n");
    SetTimer(g_dash, 1, 1000, nullptr);
    // The first ShowWindow in a process honors the launcher's STARTUPINFO
    // (Start-Process can pass SW_HIDE) instead of nCmdShow, so call it twice:
    // the second reliably applies SW_SHOW.
    ShowWindow(g_dash, SW_SHOW);
    ShowWindow(g_dash, SW_SHOW);
    SetForegroundWindow(g_dash);
    UpdateWindow(g_dash);
}

}  // namespace ng
