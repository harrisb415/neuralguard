#include "ngd/service.h"

#include "core/db.h"
#include "core/dns.h"
#include "core/enforcer.h"
#include "core/habit.h"
#include "core/identity.h"
#include "ngd/enforce.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <string>

namespace ng {
namespace {

const wchar_t* kSvcName = L"NeuralGuard";
SERVICE_STATUS_HANDLE g_ssh = nullptr;
SERVICE_STATUS g_ss{};
EnforceDaemon* g_daemon = nullptr;
std::string g_dbPath = "ngpolicy.db";

std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

// Terminate any manually-launched enforcer (another ngd.exe running `enforce`).
// A dynamic enforce session and the service both claim the same WFP provider,
// so the service can't start while one is live. Killing the manual enforcer
// closes its dynamic WFP session, which auto-removes its provider/filters
// (fail-open by design), leaving the provider free for the service to claim.
void StopManualEnforcers() {
    DWORD self = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    bool killed = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == self) continue;                 // never our own process
        if (lstrcmpiW(pe.szExeFile, L"ngd.exe") != 0) continue;
        HANDLE p = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
        if (p) { TerminateProcess(p, 0); WaitForSingleObject(p, 3000); CloseHandle(p); killed = true; }
    }
    CloseHandle(snap);
    if (killed) Sleep(500);   // let the killed enforcer's dynamic WFP session tear down
}

void SetState(DWORD state, DWORD exitCode = 0) {
    g_ss.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ss.dwCurrentState = state;
    g_ss.dwWin32ExitCode = exitCode;
    g_ss.dwControlsAccepted =
        (state == SERVICE_RUNNING) ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN) : 0;
    if (g_ssh) SetServiceStatus(g_ssh, &g_ss);
}

DWORD WINAPI HandlerEx(DWORD ctrl, DWORD, LPVOID, LPVOID) {
    if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
        SetState(SERVICE_STOP_PENDING);
        if (g_daemon) g_daemon->stop();   // clean revert (panic) on the way out
    }
    return NO_ERROR;
}

void SetMetaMode(Db& db, const char* mode) {
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(db.handle(),
        "INSERT INTO meta(k,v) VALUES('mode',?) ON CONFLICT(k) DO UPDATE SET v=excluded.v;",
        -1, &s, nullptr);
    bindText(s, 1, mode);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

void WINAPI ServiceMain(DWORD, wchar_t**) {
    g_ssh = RegisterServiceCtrlHandlerExW(kSvcName, HandlerEx, nullptr);
    if (!g_ssh) return;
    SetState(SERVICE_START_PENDING);

    Db db;
    if (!db.open(g_dbPath.c_str())) { SetState(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR); return; }
    IdentityResolver resolver(db);
    resolver.init();
    DnsWatcher dns;
    dns.start();
    Enforcer enf;
    HabitTracker habits(db);
    EnforceDaemon daemon(db, resolver, dns, enf, habits);
    g_daemon = &daemon;
    SetMetaMode(db, "enforcing");
    SetState(SERVICE_RUNNING);

    daemon.run(0);   // blocks: baseline + default-deny + rules + prompts, until stop()

    g_daemon = nullptr;
    dns.stop();
    SetMetaMode(db, "idle");
    SetState(SERVICE_STOPPED);
}

}  // namespace

int ServiceRun(const char* dbPath) {
    if (dbPath && *dbPath) g_dbPath = dbPath;
    SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(kSvcName), ServiceMain}, {nullptr, nullptr}};
    return StartServiceCtrlDispatcherW(table) ? 0 : 1;
}

int ServiceInstall(const char* dbPath) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    // The service runs as SYSTEM with CWD=System32, so the DB path must be absolute.
    std::wstring bin = L"\"" + std::wstring(exe) + L"\" service-run \"" + Wide(dbPath) + L"\"";

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) { fprintf(stderr, "OpenSCManager failed: %lu (run as Administrator)\n", GetLastError()); return 1; }

    SC_HANDLE svc = CreateServiceW(
        scm, kSvcName, L"NeuralGuard Firewall", SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        bin.c_str(), nullptr, nullptr, nullptr, nullptr /* LocalSystem */, nullptr);
    if (!svc) {
        DWORD e = GetLastError();
        if (e == ERROR_SERVICE_EXISTS) fprintf(stderr, "service already installed (uninstall first)\n");
        else fprintf(stderr, "CreateService failed: %lu\n", e);
        CloseServiceHandle(scm);
        return 1;
    }

    // Watchdog: have the SCM restart the service if it ever crashes.
    SC_ACTION acts[3] = {{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 5000}, {SC_ACTION_NONE, 0}};
    SERVICE_FAILURE_ACTIONS fa{};
    fa.dwResetPeriod = 86400;
    fa.cActions = 3;
    fa.lpsaActions = acts;
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);

    // Free the WFP provider from any manually-launched enforcer, or the
    // service's own enforcer can't claim it and the service exits at once.
    StopManualEnforcers();

    if (!StartServiceW(svc, 0, nullptr))
        fprintf(stderr, "installed, but StartService failed: %lu\n", GetLastError());
    else
        printf("NeuralGuard service installed (auto-start, LocalSystem) and started.\n");
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

int ServiceUninstall() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) { fprintf(stderr, "OpenSCManager failed (run as Administrator)\n"); return 1; }
    SC_HANDLE svc = OpenServiceW(scm, kSvcName, SERVICE_ALL_ACCESS);
    if (!svc) { fprintf(stderr, "service not installed\n"); CloseServiceHandle(scm); return 1; }

    SERVICE_STATUS st{};
    ControlService(svc, SERVICE_CONTROL_STOP, &st);   // triggers clean revert in the service
    Sleep(1500);
    bool ok = DeleteService(svc);
    printf(ok ? "NeuralGuard service stopped and removed.\n" : "DeleteService failed: %lu\n", GetLastError());
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

}  // namespace ng
