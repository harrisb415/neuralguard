#include "ngd/service.h"

#include "core/db.h"
#include "core/dns.h"
#include "core/enforcer.h"
#include "core/habit.h"
#include "core/identity.h"
#include "ngd/enforce.h"
#include "ngd/recorder.h"

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
Recorder* g_recorder = nullptr;
HANDLE g_idleStop = nullptr;   // only used when the desired mode is 'idle'
std::string g_dbPath = "ngpolicy.db";

std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

// Terminate manually-launched foreground workers (another ngd.exe running
// `enforce`/`record`). A dynamic enforce session and the service both claim the
// same WFP provider, so the service can't start while one is live. Killing the
// manual enforcer closes its dynamic WFP session, which auto-removes its
// provider/filters (fail-open by design), leaving the provider free to claim.
//
// A foreground worker has no SCM lifecycle, so a kill genuinely IS its stop.
// `excludePid` exists for the one case where that ISN'T true: the service's own
// process, which must only ever be stopped through the SCM (see ServiceStop).
// Returns how many were terminated.
int StopManualEnforcers(DWORD excludePid = 0) {
    DWORD self = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    int killed = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == self) continue;                 // never our own process
        if (excludePid && pe.th32ProcessID == excludePid) continue;   // never the service
        if (lstrcmpiW(pe.szExeFile, L"ngd.exe") != 0) continue;
        HANDLE p = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
        if (p) { TerminateProcess(p, 0); WaitForSingleObject(p, 3000); CloseHandle(p); ++killed; }
    }
    CloseHandle(snap);
    if (killed) Sleep(500);   // let the killed enforcer's dynamic WFP session tear down
    return killed;
}

// ControlService only *requests* a stop; the service still has to unwind (revert
// filters, close the WFP session). Poll until it reports STOPPED so callers can
// tell the user something true rather than something hopeful.
bool WaitForStopped(SC_HANDLE svc, DWORD timeoutMs) {
    for (DWORD waited = 0;; waited += 250) {
        SERVICE_STATUS_PROCESS ssp{}; DWORD need = 0;
        if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &need))
            return false;
        if (ssp.dwCurrentState == SERVICE_STOPPED) return true;
        if (waited >= timeoutMs) return false;
        Sleep(250);
    }
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
        if (g_daemon) g_daemon->stop();     // clean revert (panic) on the way out
        if (g_recorder) g_recorder->stop();
        if (g_idleStop) SetEvent(g_idleStop);
    }
    return NO_ERROR;
}

void SetMetaMode(Db& db, const char* mode) { db.setMeta("mode", mode); }

// Run whatever meta('desired_mode') asks for, blocking until the SCM stops us.
//
// This used to hardcode enforcement: the service enforced on every start, no
// matter what the user had last chosen, and 'learning' wasn't even reachable
// (only ngd's foreground CLI ever built a Recorder). So a reboot always came up
// enforcing - the "it always reverts to enforce" bug. desired_mode is what the
// user WANTS; meta('mode') stays what IS running, and only this decides which.
void RunDesiredMode(Db& db, IdentityResolver& resolver, DnsWatcher& dns, HabitTracker& habits) {
    const std::string desired = db.meta("desired_mode", "enforcing");

    if (desired == "idle") {
        // Deliberately keep the service alive rather than exiting: it stays a
        // stopped firewall the user can turn back on, and it's where the Phase-B2
        // command listener will live. Enforcing nothing, holding no filters.
        g_idleStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        SetMetaMode(db, "idle");
        SetState(SERVICE_RUNNING);
        if (g_idleStop) {
            WaitForSingleObject(g_idleStop, INFINITE);
            CloseHandle(g_idleStop);
            g_idleStop = nullptr;
        }
        return;
    }

    if (desired == "learning") {
        Recorder recorder(db, resolver, dns, habits);
        g_recorder = &recorder;
        SetMetaMode(db, "learning");
        SetState(SERVICE_RUNNING);
        recorder.run();   // blocks: records every net event, enforces nothing
        g_recorder = nullptr;
        return;
    }

    Enforcer enf;
    EnforceDaemon daemon(db, resolver, dns, enf, habits);
    g_daemon = &daemon;
    SetMetaMode(db, "enforcing");
    SetState(SERVICE_RUNNING);
    daemon.run(0);   // blocks: baseline + default-deny + rules + prompts, until stop()
    g_daemon = nullptr;
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
    HabitTracker habits(db);

    RunDesiredMode(db, resolver, dns, habits);

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

    // Installing the service is an explicit "protect me": record that intent, or a
    // db left at desired_mode='idle' by an earlier Stop would make the freshly
    // installed service come up idle and quietly enforce nothing.
    {
        Db db;
        if (db.open(dbPath)) db.setMeta("desired_mode", "enforcing");
    }

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

// Stop everything NeuralGuard has running - each thing the way it's meant to be
// stopped, which is the whole point of this function existing.
//
// The service MUST go through the SCM. TerminateProcess on it doesn't read as a
// stop, it reads as a *crash*, and ServiceInstall deliberately configures
// SC_ACTION_RESTART as the failure action - so a hard kill gets the service
// restarted 5s later, straight back into the enforcing path ServiceMain hardcodes.
// That is exactly the "Stop didn't stop it, it came back enforcing" bug: the UI
// was killing by image name, and the watchdog was doing its job.
//
// A foreground worker (`ngd enforce`/`record`, launched by the dashboard) is the
// opposite case: no SCM lifecycle, so a kill is the only stop it has, and its
// dynamic WFP session tears down with it (fail-open by design).
int ServiceStop(const char* dbPath, bool recordOff) {
    DWORD svcPid = 0;
    bool wasRunning = false, stopped = false;

    if (SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)) {
        if (SC_HANDLE svc = OpenServiceW(scm, kSvcName, SERVICE_STOP | SERVICE_QUERY_STATUS)) {
            SERVICE_STATUS_PROCESS ssp{}; DWORD need = 0;
            if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &need)) {
                svcPid = ssp.dwProcessId;
                wasRunning = ssp.dwCurrentState != SERVICE_STOPPED;
            }
            if (wasRunning) {
                SERVICE_STATUS st{};
                if (!ControlService(svc, SERVICE_CONTROL_STOP, &st))
                    fprintf(stderr, "stop: ControlService failed: %lu\n", GetLastError());
                else if (!WaitForStopped(svc, 20000))
                    fprintf(stderr, "stop: service did not report STOPPED within 20s.\n");
                else {
                    stopped = true;
                    svcPid = 0;   // gone - nothing for the sweep below to avoid
                }
            }
            CloseServiceHandle(svc);
        }
        CloseServiceHandle(scm);
    }

    const int workers = StopManualEnforcers(svcPid);   // svcPid != 0 only if its stop FAILED

    if (stopped) printf("NeuralGuard service stopped (filters reverted).\n");
    if (workers) printf("Stopped %d foreground worker(s).\n", workers);
    if (!wasRunning && !workers) printf("Nothing to stop - NeuralGuard wasn't running.\n");

    // Record the outcome where the UI reads it. A killed worker can never write
    // this itself (TerminateProcess gives it no chance to), and the UI must not
    // write it speculatively: meta('mode') is only touched on transitions, so an
    // optimistic "idle" written before a stop that then FAILED is a lie nothing
    // would ever correct. Only the thing that actually did the stopping knows.
    //
    // desired_mode is the durable half, and ONLY when the user asked to stop
    // (recordOff): then it has to outlive this process, or the auto-start service
    // comes back enforcing at the next boot and Stop looks like it undid itself.
    // An installer stopping us to unlock files must NOT touch it - marking the
    // user "off" there would silently leave them unprotected after an upgrade.
    const bool allStopped = (!wasRunning || stopped);
    if (allStopped && dbPath && *dbPath) {
        Db db;
        if (db.open(dbPath)) {
            SetMetaMode(db, "idle");   // observed state: nothing is running, full stop
            if (recordOff) db.setMeta("desired_mode", "idle");
        }
    }
    return allStopped ? 0 : 1;
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
