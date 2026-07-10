// ngtray - NeuralGuard system-tray app (Phase 2c, minimal).
//
// A native Win32 tray icon that runs in the interactive user session (a
// LocalSystem service can't show UI - session-0 isolation, see docs/DESIGN.md).
// It is deliberately thin: it shells out to ngctl for the privileged WFP actions
// (with a UAC elevation prompt), so the tray is pure UI. Right-click gives
// Status, the mandatory Panic (fail-open), and Quit.
//
// This first cut covers the icon + menu + panic. Actionable toast prompts for
// blocked connections (the block-notify-retry flow) need the ngd->tray pipe and
// come next.

#include <windows.h>
#include <shellapi.h>

#include <string>
#include <thread>

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
enum { ID_STATUS = 1, ID_PANIC = 2, ID_QUIT = 3, ID_DASHBOARD = 4 };

NOTIFYICONDATAW g_nid{};
HINSTANCE g_hInst = nullptr;

std::wstring ExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s = path;
    size_t p = s.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? L"." : s.substr(0, p);
}

// Run `ngctl <cmd>` elevated, keeping a console open so the output is visible.
void RunCtl(const wchar_t* sub) {
    std::wstring dir = ExeDir();
    std::wstring args = L"/k \"\"" + dir + L"\\ngctl.exe\" " + sub + L"\"";
    // Tray runs elevated, so the child inherits the token - "open", no extra UAC.
    ShellExecuteW(nullptr, L"open", L"cmd.exe", args.c_str(), dir.c_str(), SW_SHOWNORMAL);
}

// Open the WinUI dashboard (NeuralGuard.exe under dashboard\, beside ngtray).
// If it's already up, bring it to the front instead of launching a second copy.
// The tray is elevated, so the dashboard inherits the token and its Enforce/
// Learn/Stop/Panic actions run without a per-click UAC prompt.
void OpenDashboard() {
    if (HWND existing = FindWindowW(nullptr, L"NeuralGuard")) {
        ShowWindow(existing, SW_RESTORE);
        SetForegroundWindow(existing);
        return;
    }
    std::wstring dir = ExeDir() + L"\\dashboard";
    std::wstring exe = dir + L"\\NeuralGuard.exe";
    ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

// Show a balloon + an actionable dialog for a blocked connection, and return the
// user's decision: 'A' always allow, 'O' allow once, 'B' block. (A true inline-
// button toast on unpackaged Win32 needs a COM activator; balloon + dialog is the
// v1 that gives the same choice with far less plumbing.)
char PromptUser(const std::string& msg) {
    // msg is "app\tdest\tport"
    std::string app = msg, dest, port;
    size_t t1 = msg.find('\t');
    if (t1 != std::string::npos) {
        app = msg.substr(0, t1);
        size_t t2 = msg.find('\t', t1 + 1);
        if (t2 != std::string::npos) { dest = msg.substr(t1 + 1, t2 - t1 - 1); port = msg.substr(t2 + 1); }
    }

    // Balloon to draw attention.
    g_nid.uFlags |= NIF_INFO;
    wcscpy_s(g_nid.szInfoTitle, L"NeuralGuard - new connection");
    std::wstring info = Widen(app) + L" -> " + Widen(dest) + L":" + Widen(port);
    wcsncpy_s(g_nid.szInfo, info.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;

    std::wstring text = Widen(app) + L"\n\nwants to connect to  " + Widen(dest) + L":" + Widen(port) +
        L"\n\nYes = Always allow this app on this port"
        L"\nNo = Allow once"
        L"\nCancel = Block";
    int r = MessageBoxW(nullptr, text.c_str(), L"NeuralGuard",
                        MB_YESNOCANCEL | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND);
    return (r == IDYES) ? 'A' : (r == IDNO) ? 'O' : 'B';
}

// Pipe server: the privileged side (ngd/ngctl) connects and sends a prompt; we
// ask the user and write back the decision byte. One prompt at a time is fine.
void PipeServer() {
    const wchar_t* kPipe = L"\\\\.\\pipe\\neuralguard";
    for (;;) {
        HANDLE pipe = CreateNamedPipeW(kPipe, PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1, 4096, 4096, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) { Sleep(1000); continue; }

        BOOL ok = ConnectNamedPipe(pipe, nullptr) ? TRUE
                  : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (ok) {
            char buf[1024] = {};
            DWORD n = 0;
            if (ReadFile(pipe, buf, sizeof(buf) - 1, &n, nullptr) && n > 0) {
                char decision = PromptUser(std::string(buf, n));
                DWORD wr = 0;
                WriteFile(pipe, &decision, 1, &wr, nullptr);
                FlushFileBuffers(pipe);
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_TRAY:
            if (LOWORD(l) == WM_LBUTTONDBLCLK) {
                OpenDashboard();  // double-click the tray icon -> dashboard
            } else if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_CONTEXTMENU) {
                POINT pt; GetCursorPos(&pt);
                HMENU m = CreatePopupMenu();
                AppendMenuW(m, MF_STRING, ID_DASHBOARD, L"Dashboard");
                AppendMenuW(m, MF_STRING, ID_STATUS, L"Status");
                AppendMenuW(m, MF_STRING, ID_PANIC, L"Panic (fail open)");
                AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(m, MF_STRING, ID_QUIT, L"Quit");
                SetForegroundWindow(h);  // so the menu dismisses on click-away
                TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr);
                DestroyMenu(m);
            }
            return 0;
        case WM_COMMAND:
            switch (LOWORD(w)) {
                case ID_DASHBOARD: OpenDashboard(); break;
                case ID_STATUS: RunCtl(L"status"); break;
                case ID_PANIC:  RunCtl(L"panic");  break;
                case ID_QUIT:   DestroyWindow(h);  break;
            }
            return 0;
        case WM_DESTROY:
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR pCmdLine, int) {
    g_hInst = hInst;
    const bool openDash = pCmdLine && wcsstr(pCmdLine, L"dashboard");

    // Single instance - don't stack tray icons. If a tray is already running and
    // this invocation asked to open the dashboard (`ngtray dashboard`), honor
    // that (focus it if open, else launch it) before exiting, instead of silently
    // doing nothing.
    CreateMutexW(nullptr, TRUE, L"NeuralGuard_ngtray_singleton");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (openDash) OpenDashboard();
        return 0;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"ngtrayWnd";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(L"ngtrayWnd", L"ngtray", 0, 0, 0, 0, 0,
                              HWND_MESSAGE, nullptr, hInst, nullptr);

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = LoadIconW(nullptr, IDI_SHIELD);  // stock shield - fits a firewall
    wcscpy_s(g_nid.szTip, L"NeuralGuard");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    // Startup balloon so it's obvious the tray came up.
    g_nid.uFlags |= NIF_INFO;
    wcscpy_s(g_nid.szInfoTitle, L"NeuralGuard");
    wcscpy_s(g_nid.szInfo, L"Tray running. Right-click for Panic.");
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;

    // Listen for connection prompts from the privileged daemon.
    std::thread(PipeServer).detach();

    if (openDash) OpenDashboard();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
