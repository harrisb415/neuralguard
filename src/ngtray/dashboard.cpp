// NeuralGuard config dashboard - native Win32 (Common Controls), hosted by the
// (elevated) tray. Tabs: Live / Rules / Habits from the DB. A button bar runs
// ngd/ngctl HIDDEN (no console) and streams their output into an in-window log
// pane. Status bar shows the live mode ngd publishes to meta('mode').
#include "ngtray/dashboard.h"

#include "core/db.h"
#include "core/util.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>

#include <cctype>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace ng {
namespace {

enum { TAB_LIVE = 0, TAB_RULES = 1, TAB_HABITS = 2, TAB_APPS = 3, TAB_HISTORY = 4,
       TAB_SETTINGS = 5, TAB_COUNT = 6 };
enum { IDB_ENFORCE = 100, IDB_LEARN, IDB_STOP, IDB_PANIC, IDB_REFRESH, IDB_COUNT_ = 5 };
enum { IDM_BLOCK_DEST = 300, IDM_ALLOW_DEST, IDM_ALLOW_APP, IDM_BLOCK_APP,
       IDM_ALLOW_DEST_1H, IDM_DEL_RULE };
enum { IDC_SEARCH = 400, IDB_EXPORT = 401, IDB_IMPORT = 402,
       IDC_AUTO0 = 410, IDC_AUTO1 = 411, IDC_AUTO2 = 412,
       IDB_SVC_INSTALL = 420, IDB_SVC_REMOVE = 421 };
constexpr UINT WM_APP_LOG = WM_APP + 7;

const wchar_t* kBtnLabel[IDB_COUNT_] = {L"Enforce", L"Learn", L"Stop", L"Panic", L"Refresh"};

HWND g_dash = nullptr, g_tabs = nullptr, g_status = nullptr, g_log = nullptr;
HWND g_lv[TAB_COUNT] = {};
HWND g_btn[IDB_COUNT_] = {};
HWND g_search = nullptr, g_export = nullptr, g_import = nullptr;
HWND g_autoLabel = nullptr, g_autoRadio[3] = {};
HWND g_svcLabel = nullptr, g_svcInstall = nullptr, g_svcRemove = nullptr;
int  g_cur = 0;
long long g_lastEventId = -1;
long long g_ctxParam = 0;   // DB id of the row the context menu was opened on
std::string g_filter;       // search box text (lower-cased); "" = no filter
HANDLE g_child = nullptr;   // the running ngd daemon (enforce/record), if any

void InitLiveCursor();   // defined below; reset the Live feed cursor

// Case-insensitive substring filter for the search box. Empty filter matches all.
bool MatchFilter(const std::string& hay) {
    if (g_filter.empty()) return true;
    std::string h = hay;
    for (char& c : h) c = (char)tolower((unsigned char)c);
    return h.find(g_filter) != std::string::npos;
}

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
// Insert a row's first cell carrying a DB id in lParam (for right-click actions).
void InsertRow(HWND lv, int row, const char* col0, long long param) {
    std::wstring w = Widen(col0);
    LVITEMW it{}; it.mask = LVIF_TEXT | LVIF_PARAM; it.iItem = row; it.iSubItem = 0;
    it.pszText = const_cast<LPWSTR>(w.c_str()); it.lParam = (LPARAM)param;
    ListView_InsertItem(lv, &it);
}

// ---- rule writes (dashboard edits the rules table directly, no elevation) --
void BumpGen(sqlite3* h) {
    sqlite3_exec(h, "UPDATE meta SET v=CAST(v AS INTEGER)+1 WHERE k='rules_gen';", nullptr, nullptr, nullptr);
}
// Add a rule from a live flow_event: block/allow, matching either the app (any
// port) or the destination IP:port. ngd enforce picks it up on the gen bump.
void AddRuleFromEvent(long long eventId, bool block, bool useApp, int ttlSeconds = 0) {
    Db d; if (!d.open(DbPathU8().c_str())) return;
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(d.handle(),
        "SELECT fe.remote_addr, fe.remote_port, COALESCE(pi.image_path, fe.image_path)"
        " FROM flow_events fe LEFT JOIN process_identity pi ON fe.image_id=pi.id WHERE fe.id=?;",
        -1, &s, nullptr);
    sqlite3_bind_int64(s, 1, eventId);
    std::string ip, path; int port = 0;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char* a = (const char*)sqlite3_column_text(s, 0); ip = a ? a : "";
        port = sqlite3_column_int(s, 1);
        const char* p = (const char*)sqlite3_column_text(s, 2); path = p ? p : "";
    }
    sqlite3_finalize(s);
    if (!useApp && ip.find('.') == std::string::npos) {
        AppendLog(L"[rules] can't add: row has no IPv4 destination.\r\n"); return;
    }
    if (useApp && (path.size() < 3 || path[1] != ':')) {
        AppendLog(L"[rules] can't add: no app path for this row.\r\n"); return;
    }
    sqlite3_stmt* ins = nullptr;
    sqlite3_prepare_v2(d.handle(),
        "INSERT INTO rules(action,app_path,remote_addr,remote_port,protocol,enabled,expires_epoch,created_at)"
        " VALUES(?,?,?,?,?,1,?,datetime('now'));", -1, &ins, nullptr);
    bindText(ins, 1, block ? "block" : "permit");
    if (useApp) {
        bindText(ins, 2, path); sqlite3_bind_null(ins, 3);
        sqlite3_bind_null(ins, 4); sqlite3_bind_null(ins, 5);
    } else {
        sqlite3_bind_null(ins, 2); bindText(ins, 3, ip);
        sqlite3_bind_int(ins, 4, port); sqlite3_bind_int(ins, 5, 6);
    }
    if (ttlSeconds > 0) {
        FILETIME ft; GetSystemTimeAsFileTime(&ft);
        sqlite3_bind_double(ins, 6, util::UnixEpoch(ft) + ttlSeconds);
    } else {
        sqlite3_bind_null(ins, 6);
    }
    sqlite3_step(ins);
    sqlite3_finalize(ins);
    BumpGen(d.handle());
    std::wstring what = useApp ? Widen(path.c_str())
                               : Widen(ip.c_str()) + L":" + std::to_wstring(port);
    std::wstring ttl = ttlSeconds > 0 ? L" for " + std::to_wstring(ttlSeconds / 60) + L" min" : L"";
    AppendLog((block ? L"[rules] BLOCK " : L"[rules] ALLOW ") + what + ttl + L" added (live).\r\n");
}
void DelRule(long long ruleId) {
    Db d; if (!d.open(DbPathU8().c_str())) return;
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(d.handle(), "DELETE FROM rules WHERE id=?;", -1, &s, nullptr);
    sqlite3_bind_int64(s, 1, ruleId);
    sqlite3_step(s); sqlite3_finalize(s);
    BumpGen(d.handle());
    AppendLog(L"[rules] rule deleted (live).\r\n");
}

// Common file-dialog helper (save/open a .txt allow-list).
std::wstring PickFile(bool save) {
    wchar_t path[MAX_PATH] = L"neuralguard-rules.txt";
    OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = g_dash;
    ofn.lpstrFilter = L"NeuralGuard rules\0*.txt\0All files\0*.*\0";
    ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH; ofn.lpstrDefExt = L"txt";
    ofn.Flags = save ? (OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST)
                     : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);
    BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    return ok ? std::wstring(path) : std::wstring();
}

// Export enabled rules to a pipe-delimited text file.
void ExportRules() {
    std::wstring file = PickFile(true);
    if (file.empty()) return;
    Db d; if (!d.open(DbPathU8().c_str())) return;
    FILE* f = _wfopen(file.c_str(), L"w");
    if (!f) { AppendLog(L"[rules] export: can't write file.\r\n"); return; }
    fprintf(f, "# NeuralGuard rules: action|app_path|remote_addr|remote_port|protocol|expires_epoch\n");
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(d.handle(),
        "SELECT action, COALESCE(app_path,''), COALESCE(remote_addr,''),"
        " COALESCE(remote_port,0), COALESCE(protocol,0), COALESCE(expires_epoch,0)"
        " FROM rules WHERE enabled=1;", -1, &s, nullptr);
    int n = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        fprintf(f, "%s|%s|%s|%d|%d|%.0f\n",
                (const char*)sqlite3_column_text(s, 0), (const char*)sqlite3_column_text(s, 1),
                (const char*)sqlite3_column_text(s, 2), sqlite3_column_int(s, 3),
                sqlite3_column_int(s, 4), sqlite3_column_double(s, 5));
        ++n;
    }
    sqlite3_finalize(s);
    fclose(f);
    AppendLog(L"[rules] exported " + std::to_wstring(n) + L" rule(s).\r\n");
}

// Import rules from a pipe-delimited file (skips comments/blank lines).
void ImportRules() {
    std::wstring file = PickFile(false);
    if (file.empty()) return;
    FILE* f = _wfopen(file.c_str(), L"r");
    if (!f) { AppendLog(L"[rules] import: can't read file.\r\n"); return; }
    Db d; if (!d.open(DbPathU8().c_str())) { fclose(f); return; }
    char line[1024]; int n = 0;
    while (fgets(line, sizeof(line), f)) {
        std::string ln(line);
        while (!ln.empty() && (ln.back() == '\n' || ln.back() == '\r')) ln.pop_back();
        if (ln.empty() || ln[0] == '#') continue;
        std::string col[6]; int nc = 0; size_t p = 0;
        while (nc < 6) {
            size_t q = ln.find('|', p);
            col[nc++] = ln.substr(p, q == std::string::npos ? std::string::npos : q - p);
            if (q == std::string::npos) break;
            p = q + 1;
        }
        if (nc < 5 || (col[0] != "permit" && col[0] != "block")) continue;
        int port = atoi(col[3].c_str()), proto = atoi(col[4].c_str());
        double expires = nc >= 6 ? atof(col[5].c_str()) : 0;
        sqlite3_stmt* ins = nullptr;
        sqlite3_prepare_v2(d.handle(),
            "INSERT INTO rules(action,app_path,remote_addr,remote_port,protocol,enabled,expires_epoch,created_at)"
            " VALUES(?,?,?,?,?,1,?,datetime('now'));", -1, &ins, nullptr);
        bindText(ins, 1, col[0].c_str());
        if (!col[1].empty()) bindText(ins, 2, col[1].c_str()); else sqlite3_bind_null(ins, 2);
        if (!col[2].empty()) bindText(ins, 3, col[2].c_str()); else sqlite3_bind_null(ins, 3);
        if (port) sqlite3_bind_int(ins, 4, port); else sqlite3_bind_null(ins, 4);
        if (proto) sqlite3_bind_int(ins, 5, proto); else sqlite3_bind_null(ins, 5);
        if (expires > 0) sqlite3_bind_double(ins, 6, expires); else sqlite3_bind_null(ins, 6);
        sqlite3_step(ins); sqlite3_finalize(ins);
        ++n;
    }
    fclose(f);
    BumpGen(d.handle());
    AppendLog(L"[rules] imported " + std::to_wstring(n) + L" rule(s).\r\n");
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
        const char* c0 = (const char*)sqlite3_column_text(s, 0);
        const char* c1 = (const char*)sqlite3_column_text(s, 1);
        if (!MatchFilter(std::string(c0 ? c0 : "") + " " + (c1 ? c1 : ""))) continue;
        SetCell(lv, row, 0, c0);
        SetCell(lv, row, 1, c1);
        SetCell(lv, row, 2, std::to_string(sqlite3_column_int(s, 2)).c_str());
        SetCell(lv, row, 3, std::to_string(sqlite3_column_int(s, 3)).c_str());
        ++row;
    }
    sqlite3_finalize(s);
}
// Per-app rollup: one row per application, with distinct destinations, total
// events, and how many were blocked.
void FillApps(HWND lv) {
    ListView_DeleteAllItems(lv);
    Db d; if (!d.open(DbPathU8().c_str())) return;
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(d.handle(),
        "SELECT COALESCE(pi.signer, pi.image_path, fe.image_path) app,"
        " COUNT(DISTINCT fe.remote_addr), COUNT(*),"
        " SUM(CASE WHEN fe.verdict LIKE '%DROP%' OR fe.verdict='BLOCK' THEN 1 ELSE 0 END)"
        " FROM flow_events fe LEFT JOIN process_identity pi ON fe.image_id = pi.id"
        " GROUP BY app ORDER BY COUNT(*) DESC LIMIT 500;", -1, &s, nullptr);
    int row = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char* c0 = (const char*)sqlite3_column_text(s, 0);
        if (!MatchFilter(c0 ? c0 : "")) continue;
        SetCell(lv, row, 0, c0);
        SetCell(lv, row, 1, std::to_string(sqlite3_column_int(s, 1)).c_str());
        SetCell(lv, row, 2, std::to_string(sqlite3_column_int(s, 2)).c_str());
        SetCell(lv, row, 3, std::to_string(sqlite3_column_int(s, 3)).c_str());
        ++row;
    }
    sqlite3_finalize(s);
}

// Decision history: the most recent connections the firewall denied.
void FillHistory(HWND lv) {
    ListView_DeleteAllItems(lv);
    Db d; if (!d.open(DbPathU8().c_str())) return;
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(d.handle(),
        "SELECT fe.ts_utc, fe.verdict, COALESCE(pi.signer, pi.image_path, fe.image_path),"
        " COALESCE(fe.remote_domain, fe.remote_addr), fe.remote_port"
        " FROM flow_events fe LEFT JOIN process_identity pi ON fe.image_id = pi.id"
        " WHERE fe.verdict LIKE '%DROP%' OR fe.verdict='BLOCK'"
        " ORDER BY fe.id DESC LIMIT 500;", -1, &s, nullptr);
    int row = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char* ts = (const char*)sqlite3_column_text(s, 0);
        const char* c2 = (const char*)sqlite3_column_text(s, 2);
        const char* c3 = (const char*)sqlite3_column_text(s, 3);
        if (!MatchFilter(std::string(c2 ? c2 : "") + " " + (c3 ? c3 : ""))) continue;
        std::string tm = ts ? ts : ""; if (tm.size() >= 19) tm = tm.substr(11, 8);
        SetCell(lv, row, 0, tm.c_str());
        SetCell(lv, row, 1, (const char*)sqlite3_column_text(s, 1));
        SetCell(lv, row, 2, c2);
        SetCell(lv, row, 3, c3);
        SetCell(lv, row, 4, std::to_string(sqlite3_column_int(s, 4)).c_str());
        ++row;
    }
    sqlite3_finalize(s);
}

void FillRules(HWND lv) {
    ListView_DeleteAllItems(lv);
    Db d; if (!d.open(DbPathU8().c_str())) return;
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(d.handle(),
        "SELECT id, action, COALESCE(app_path, remote_addr, '(any)'),"
        " COALESCE(remote_port,0), enabled, COALESCE(expires_epoch,0)"
        " FROM rules ORDER BY id DESC;", -1, &s, nullptr);
    int row = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char* act = (const char*)sqlite3_column_text(s, 1);
        const char* tgt = (const char*)sqlite3_column_text(s, 2);
        if (!MatchFilter(std::string(act ? act : "") + " " + (tgt ? tgt : ""))) continue;
        InsertRow(lv, row, act, sqlite3_column_int64(s, 0));
        SetCell(lv, row, 1, tgt);
        int port = sqlite3_column_int(s, 3);
        SetCell(lv, row, 2, port ? std::to_string(port).c_str() : "any");
        std::string info = sqlite3_column_int(s, 4) ? "" : "disabled";
        if (sqlite3_column_double(s, 5) > 0) info += info.empty() ? "timed" : ", timed";
        SetCell(lv, row, 3, info.c_str());
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
        const char* app = (const char*)sqlite3_column_text(s, 3);
        const char* dst = (const char*)sqlite3_column_text(s, 4);
        if (!MatchFilter(std::string(app ? app : "") + " " + (dst ? dst : ""))) continue;
        std::string tm = ts ? ts : "";
        if (tm.size() >= 19) tm = tm.substr(11, 8);
        InsertRow(lv, 0, tm.c_str(), g_lastEventId);   // lParam = flow_event id
        SetCell(lv, 0, 1, (const char*)sqlite3_column_text(s, 2));
        SetCell(lv, 0, 2, app);
        SetCell(lv, 0, 3, dst);
        SetCell(lv, 0, 4, std::to_string(sqlite3_column_int(s, 5)).c_str());
    }
    sqlite3_finalize(s);
    for (int n = ListView_GetItemCount(lv); n > 500; --n) ListView_DeleteItem(lv, n - 1);
}

int ReadAutonomy() {
    Db d; if (!d.open(DbPathU8().c_str())) return 0;
    sqlite3_stmt* s = nullptr; int v = 0;
    sqlite3_prepare_v2(d.handle(), "SELECT v FROM meta WHERE k='autonomy';", -1, &s, nullptr);
    if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return v;
}
void WriteAutonomy(int v) {
    Db d; if (!d.open(DbPathU8().c_str())) return;
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(d.handle(),
        "INSERT INTO meta(k,v) VALUES('autonomy',?) ON CONFLICT(k) DO UPDATE SET v=excluded.v;",
        -1, &s, nullptr);
    bindText(s, 1, std::to_string(v).c_str());
    sqlite3_step(s); sqlite3_finalize(s);
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
    MoveWindow(g_export, x, 6, bw, bh, TRUE); x += bw + gap;
    MoveWindow(g_import, x, 6, bw, bh, TRUE); x += bw + gap;
    int sx = x + 12, sw = rc.right - sx - 10;
    MoveWindow(g_search, sx, 8, sw > 80 ? sw : 80, bh - 2, TRUE);
    int bottom = rc.bottom - sbh;
    MoveWindow(g_log, 0, bottom - logH, rc.right, logH, TRUE);
    MoveWindow(g_tabs, 0, barH, rc.right, bottom - logH - barH, TRUE);
    RECT disp = {0, barH, rc.right, bottom - logH};
    TabCtrl_AdjustRect(g_tabs, FALSE, &disp);
    for (int i = 0; i < TAB_COUNT; ++i)
        MoveWindow(g_lv[i], disp.left, disp.top, disp.right - disp.left, disp.bottom - disp.top, TRUE);
    // Settings controls share the tab display rect.
    MoveWindow(g_autoLabel, disp.left + 16, disp.top + 16, disp.right - disp.left - 32, 22, TRUE);
    for (int k = 0; k < 3; ++k)
        MoveWindow(g_autoRadio[k], disp.left + 24, disp.top + 52 + k * 34,
                   disp.right - disp.left - 48, 26, TRUE);
    int sy = disp.top + 52 + 3 * 34 + 18;
    MoveWindow(g_svcLabel, disp.left + 16, sy, disp.right - disp.left - 32, 22, TRUE);
    MoveWindow(g_svcInstall, disp.left + 24, sy + 28, 200, 26, TRUE);
    MoveWindow(g_svcRemove, disp.left + 24 + 210, sy + 28, 160, 26, TRUE);
}

void ShowTab(int i) {
    g_cur = i;
    bool settings = (i == TAB_SETTINGS);
    for (int k = 0; k < TAB_COUNT; ++k) ShowWindow(g_lv[k], (k == i && !settings) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_autoLabel, settings ? SW_SHOW : SW_HIDE);
    for (int k = 0; k < 3; ++k) ShowWindow(g_autoRadio[k], settings ? SW_SHOW : SW_HIDE);
    ShowWindow(g_svcLabel, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(g_svcInstall, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(g_svcRemove, settings ? SW_SHOW : SW_HIDE);
    if (settings) {
        int a = ReadAutonomy();
        for (int k = 0; k < 3; ++k)
            SendMessageW(g_autoRadio[k], BM_SETCHECK, k == a ? BST_CHECKED : BST_UNCHECKED, 0);
        return;
    }
    if (i == TAB_LIVE)         PollLive();
    else if (i == TAB_RULES)   FillRules(g_lv[TAB_RULES]);
    else if (i == TAB_HABITS)  FillHabits(g_lv[TAB_HABITS]);
    else if (i == TAB_APPS)    FillApps(g_lv[TAB_APPS]);
    else                       FillHistory(g_lv[TAB_HISTORY]);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_NOTIFY: {
            LPNMHDR n = (LPNMHDR)l;
            if (n->hwndFrom == g_tabs && n->code == TCN_SELCHANGE) ShowTab(TabCtrl_GetCurSel(g_tabs));
            // Right-click a Live or Rules row -> context menu (add / delete a rule).
            if (n->code == NM_RCLICK &&
                (n->hwndFrom == g_lv[TAB_LIVE] || n->hwndFrom == g_lv[TAB_RULES])) {
                LPNMITEMACTIVATE ia = (LPNMITEMACTIVATE)l;
                if (ia->iItem < 0) return 0;
                LVITEMW it{}; it.mask = LVIF_PARAM; it.iItem = ia->iItem;
                ListView_GetItem(n->hwndFrom, &it);
                g_ctxParam = (long long)it.lParam;
                HMENU menu = CreatePopupMenu();
                if (g_cur == TAB_LIVE) {
                    AppendMenuW(menu, MF_STRING, IDM_BLOCK_DEST, L"Block this destination (IP:port)");
                    AppendMenuW(menu, MF_STRING, IDM_ALLOW_DEST, L"Allow this destination (IP:port)");
                    AppendMenuW(menu, MF_STRING, IDM_ALLOW_DEST_1H, L"Allow this destination for 1 hour");
                    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                    AppendMenuW(menu, MF_STRING, IDM_ALLOW_APP, L"Allow this app (any port)");
                    AppendMenuW(menu, MF_STRING, IDM_BLOCK_APP, L"Block this app (any port)");
                } else {
                    AppendMenuW(menu, MF_STRING, IDM_DEL_RULE, L"Delete rule");
                }
                POINT pt; GetCursorPos(&pt);
                SetForegroundWindow(g_dash);
                TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_dash, nullptr);
                DestroyMenu(menu);
            }
            return 0;
        }
        case WM_SIZE:  LayoutChildren(); return 0;
        case WM_TIMER: UpdateStatus(); if (g_cur == TAB_LIVE) PollLive(); return 0;
        case WM_APP_LOG: { wchar_t* p = (wchar_t*)w; AppendLog(p); free(p); return 0; }
        case WM_COMMAND:
            if (LOWORD(w) == IDC_SEARCH && HIWORD(w) == EN_CHANGE) {
                wchar_t buf[128] = {}; GetWindowTextW(g_search, buf, 128);
                std::wstring ws = buf; for (wchar_t& c : ws) c = towlower(c);
                int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
                g_filter.assign(n, 0);
                WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), g_filter.data(), n, nullptr, nullptr);
                if (g_cur == TAB_LIVE) { ListView_DeleteAllItems(g_lv[TAB_LIVE]); InitLiveCursor(); }
                ShowTab(g_cur);
                return 0;
            }
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
                case IDM_BLOCK_DEST:    AddRuleFromEvent(g_ctxParam, true,  false);       break;
                case IDM_ALLOW_DEST:    AddRuleFromEvent(g_ctxParam, false, false);       break;
                case IDM_ALLOW_DEST_1H: AddRuleFromEvent(g_ctxParam, false, false, 3600); break;
                case IDM_ALLOW_APP:     AddRuleFromEvent(g_ctxParam, false, true);        break;
                case IDM_BLOCK_APP:     AddRuleFromEvent(g_ctxParam, true,  true);        break;
                case IDM_DEL_RULE:      DelRule(g_ctxParam); FillRules(g_lv[TAB_RULES]);  break;
                case IDB_EXPORT:        ExportRules(); break;
                case IDB_IMPORT:        ImportRules(); FillRules(g_lv[TAB_RULES]); break;
                case IDC_AUTO0: WriteAutonomy(0); AppendLog(L"[settings] autonomy: prompt on every new connection.\r\n"); break;
                case IDC_AUTO1: WriteAutonomy(1); AppendLog(L"[settings] autonomy: auto-allow apps you already use.\r\n"); break;
                case IDC_AUTO2: WriteAutonomy(2); AppendLog(L"[settings] autonomy: auto-allow everything (log only).\r\n"); break;
                case IDB_SVC_INSTALL:
                    StopDaemon();   // avoid two enforcers fighting over the sublayer
                    RunSync(L"ngd.exe", L"install \"" + ExeDir() + L"\\ngpolicy.db\"");
                    break;
                case IDB_SVC_REMOVE:
                    RunSync(L"ngd.exe", L"uninstall");
                    break;
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
    g_export = CreateWindowW(L"BUTTON", L"Export", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                             0, 0, 0, 0, g_dash, (HMENU)(INT_PTR)IDB_EXPORT, hInst, nullptr);
    g_import = CreateWindowW(L"BUTTON", L"Import", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                             0, 0, 0, 0, g_dash, (HMENU)(INT_PTR)IDB_IMPORT, hInst, nullptr);
    g_search = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                               WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                               0, 0, 0, 0, g_dash, (HMENU)(INT_PTR)IDC_SEARCH, hInst, nullptr);
    SendMessageW(g_search, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search…");

    // Settings tab controls (hidden until the Settings tab is shown).
    g_autoLabel = CreateWindowW(L"STATIC",
        L"Autonomy — what NeuralGuard does when an app makes a new connection while enforcing:",
        WS_CHILD, 0, 0, 0, 0, g_dash, nullptr, hInst, nullptr);
    g_autoRadio[0] = CreateWindowW(L"BUTTON", L"Prompt me on every new connection",
        WS_CHILD | BS_AUTORADIOBUTTON | WS_GROUP, 0, 0, 0, 0, g_dash, (HMENU)(INT_PTR)IDC_AUTO0, hInst, nullptr);
    g_autoRadio[1] = CreateWindowW(L"BUTTON", L"Auto-allow apps I already use (prompt only for unknown apps)",
        WS_CHILD | BS_AUTORADIOBUTTON, 0, 0, 0, 0, g_dash, (HMENU)(INT_PTR)IDC_AUTO1, hInst, nullptr);
    g_autoRadio[2] = CreateWindowW(L"BUTTON", L"Auto-allow everything (log only, never prompt)",
        WS_CHILD | BS_AUTORADIOBUTTON, 0, 0, 0, 0, g_dash, (HMENU)(INT_PTR)IDC_AUTO2, hInst, nullptr);
    g_svcLabel = CreateWindowW(L"STATIC",
        L"Background service — enforce as SYSTEM at boot (no UAC prompts ever):",
        WS_CHILD, 0, 0, 0, 0, g_dash, nullptr, hInst, nullptr);
    g_svcInstall = CreateWindowW(L"BUTTON", L"Install service", WS_CHILD | BS_PUSHBUTTON,
        0, 0, 0, 0, g_dash, (HMENU)(INT_PTR)IDB_SVC_INSTALL, hInst, nullptr);
    g_svcRemove = CreateWindowW(L"BUTTON", L"Remove service", WS_CHILD | BS_PUSHBUTTON,
        0, 0, 0, 0, g_dash, (HMENU)(INT_PTR)IDB_SVC_REMOVE, hInst, nullptr);

    g_tabs = CreateWindowW(WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, g_dash, nullptr, hInst, nullptr);
    TCITEMW ti{}; ti.mask = TCIF_TEXT;
    ti.pszText = const_cast<LPWSTR>(L"Live");    TabCtrl_InsertItem(g_tabs, TAB_LIVE, &ti);
    ti.pszText = const_cast<LPWSTR>(L"Rules");   TabCtrl_InsertItem(g_tabs, TAB_RULES, &ti);
    ti.pszText = const_cast<LPWSTR>(L"Habits");  TabCtrl_InsertItem(g_tabs, TAB_HABITS, &ti);
    ti.pszText = const_cast<LPWSTR>(L"Per-app");  TabCtrl_InsertItem(g_tabs, TAB_APPS, &ti);
    ti.pszText = const_cast<LPWSTR>(L"History");  TabCtrl_InsertItem(g_tabs, TAB_HISTORY, &ti);
    ti.pszText = const_cast<LPWSTR>(L"Settings"); TabCtrl_InsertItem(g_tabs, TAB_SETTINGS, &ti);

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
    AddCol(g_lv[TAB_RULES], 0, L"Action", 80);
    AddCol(g_lv[TAB_RULES], 1, L"Target (app / IP)", 380);
    AddCol(g_lv[TAB_RULES], 2, L"Port", 70);
    AddCol(g_lv[TAB_RULES], 3, L"Info", 120);
    AddCol(g_lv[TAB_HABITS], 0, L"Application", 260);
    AddCol(g_lv[TAB_HABITS], 1, L"Destination", 340);
    AddCol(g_lv[TAB_HABITS], 2, L"Port", 70);
    AddCol(g_lv[TAB_HABITS], 3, L"Count", 70);
    AddCol(g_lv[TAB_APPS], 0, L"Application", 320);
    AddCol(g_lv[TAB_APPS], 1, L"Destinations", 110);
    AddCol(g_lv[TAB_APPS], 2, L"Events", 90);
    AddCol(g_lv[TAB_APPS], 3, L"Blocked", 90);
    AddCol(g_lv[TAB_HISTORY], 0, L"Time", 80);
    AddCol(g_lv[TAB_HISTORY], 1, L"Verdict", 80);
    AddCol(g_lv[TAB_HISTORY], 2, L"Application", 220);
    AddCol(g_lv[TAB_HISTORY], 3, L"Destination", 300);
    AddCol(g_lv[TAB_HISTORY], 4, L"Port", 60);

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
