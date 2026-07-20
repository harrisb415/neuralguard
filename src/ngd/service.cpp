#include "ngd/service.h"

#include "core/db.h"
#include "core/dns.h"
#include "core/enforcer.h"
#include "core/flowstats.h"
#include "core/habit.h"
#include "core/identity.h"
#include "core/cmd.h"
#include "ngd/enforce.h"
#include "ngd/recorder.h"

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

namespace ng {
namespace {

const wchar_t* kSvcName = L"NeuralGuard";
SERVICE_STATUS_HANDLE g_ssh = nullptr;
SERVICE_STATUS g_ss{};
EnforceDaemon* g_daemon = nullptr;
Recorder* g_recorder = nullptr;
FlowCollector* g_collector = nullptr;   // Phase-4 feature archival, runs alongside the mode
HANDLE g_idleStop = nullptr;   // signalled to break the 'idle' mode's wait
std::string g_dbPath = "ngpolicy.db";

// Feature archival helpers (mirror main.cpp's for the CLI record/enforce paths).
// The service used to run the mode WITHOUT the collector, so flow_features never
// grew for anyone running NeuralGuard as a service - only manual `ngd record`/
// `ngd features` sessions collected. That starved the Phase-4 ML pipeline.
bool FeatureArchiveOn(Db& db) { return db.meta("feature_archive", "0") == "1"; }

std::string ModelPathFor(const char* name) {
    size_t s = g_dbPath.find_last_of("\\/");
    std::string dir = (s == std::string::npos) ? "." : g_dbPath.substr(0, s);
    return dir + "\\" + name;
}

void MaybeEnableScoring(FlowCollector& collector, Db& db) {
    std::string mode = db.meta("ml_mode", "shadow");
    if (mode != "off")
        collector.enableScoring(ModelPathFor("anomaly.onnx"), ModelPathFor("supervised.onnx"),
                                mode == "active");
}

// Run `body` (the blocking recorder/daemon) with the feature collector archiving
// completed flows alongside it, when feature_archive is on. Collection happens
// regardless of whether a model exists - scoring just fills in the score columns
// if one is present, so this accumulates training data even before any model.
template <class Body>
bool WithCollector(Db& db, IdentityResolver& resolver, DnsWatcher& dns, Body&& body) {
    FlowCollector collector(db, resolver, &dns);
    MaybeEnableScoring(collector, db);
    std::thread featThread;
    const bool feat = FeatureArchiveOn(db);
    if (feat) {
        g_collector = &collector;
        featThread = std::thread([&collector] { collector.run(0); });
    }
    const bool ok = body();
    if (feat) {
        collector.stop();
        if (featThread.joinable()) featThread.join();
        g_collector = nullptr;
    }
    return ok;
}

// Two different reasons the currently-running mode gets told to return, and the
// mode loop has to tell them apart: the SCM is stopping us for good, versus a
// MODE command asking us to switch to something else and keep running.
std::atomic<bool> g_svcStopping{false};
std::atomic<bool> g_modeSwitch{false};

// Ask whatever mode is running to return. Doesn't itself decide what happens
// next - g_svcStopping / g_modeSwitch do.
void SignalCurrentModeStop() {
    if (g_daemon) g_daemon->stop();
    if (g_recorder) g_recorder->stop();
    if (g_idleStop) SetEvent(g_idleStop);
}

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
        g_svcStopping = true;
        SetState(SERVICE_STOP_PENDING);
        SignalCurrentModeStop();   // the daemon reverts its filters on the way out
    }
    return NO_ERROR;
}

void SetMetaMode(Db& db, const char* mode) { db.setMeta("mode", mode); }

// Run one mode, blocking until it's told to return (by the SCM stopping us, or by
// a MODE command switching us to something else).
//
// The service used to hardcode enforcement here: it enforced on every start no
// matter what the user had last chosen, and 'learning' wasn't even reachable
// (only ngd's foreground CLI ever built a Recorder). desired_mode is what the
// user WANTS; meta('mode') stays what IS running, and this is what connects them.
// Returns false if the mode couldn't be set up at all, which the caller turns into
// a non-zero service exit. That distinction matters: a failed enforcer used to
// report a perfectly clean SERVICE_STOPPED/exit-0, so a firewall that never
// started looked identical to one you'd deliberately stopped - no error, no event
// log entry, and no restart, because the SCM only retries on a *failure* exit.
bool RunOneMode(const std::string& desired, Db& db, IdentityResolver& resolver,
                DnsWatcher& dns, HabitTracker& habits) {
    if (desired == "idle") {
        // Deliberately stay alive rather than exiting: a stopped firewall the user
        // can turn back on with one command, holding no filters and enforcing
        // nothing. Exiting would mean nothing was left listening to turn it on.
        ResetEvent(g_idleStop);
        SetMetaMode(db, "idle");
        SetState(SERVICE_RUNNING);
        WaitForSingleObject(g_idleStop, INFINITE);
        return true;
    }

    if (desired == "learning") {
        Recorder recorder(db, resolver, dns, habits);
        g_recorder = &recorder;
        SetMetaMode(db, "learning");
        SetState(SERVICE_RUNNING);
        const bool ok = WithCollector(db, resolver, dns, [&] {
            return recorder.run();   // blocks: records every event, enforces nothing
        });
        g_recorder = nullptr;
        return ok;
    }

    Enforcer enf;
    EnforceDaemon daemon(db, resolver, dns, enf, habits);
    g_daemon = &daemon;
    SetMetaMode(db, "enforcing");
    SetState(SERVICE_RUNNING);
    const bool ok = WithCollector(db, resolver, dns, [&] {
        return daemon.run(0);   // blocks: baseline + default-deny + rules + prompts
    });
    g_daemon = nullptr;
    return ok;
}

// Run the desired mode, and keep running whatever it becomes. A MODE command sets
// g_modeSwitch and stops the current mode, which lands us back here to pick up the
// new one - so switching enforce->learn no longer means tearing down the service
// (or, as it used to, spawning a rival ngd.exe alongside it).
bool RunModeLoop(Db& db, IdentityResolver& resolver, DnsWatcher& dns, HabitTracker& habits) {
    for (;;) {
        g_modeSwitch = false;
        if (!RunOneMode(db.meta("desired_mode", "idle"), db, resolver, dns, habits))
            return false;   // setup failed: report it rather than silently spinning or idling
        // Only a switch continues the loop; the SCM stopping us ends it.
        if (g_svcStopping || !g_modeSwitch) return true;
    }
}

// --- command channel (see core/cmd.h) ---------------------------------------

std::string HandleCommand(Db& db, const std::string& req) {
    const size_t sp = req.find(' ');
    const std::string verb = req.substr(0, sp);
    const std::string arg = (sp == std::string::npos) ? "" : req.substr(sp + 1);

    if (verb == "STATUS")
        return "OK mode=" + db.meta("mode", "idle") + " desired=" + db.meta("desired_mode", "idle");

    if (verb == "MODE") {
        if (arg != "enforcing" && arg != "learning" && arg != "idle")
            return "ERR mode must be enforcing|learning|idle";
        db.setMeta("desired_mode", arg);   // persist first: a switch that crashes still resumes right
        if (db.meta("mode", "idle") == arg) return "OK already " + arg;
        g_modeSwitch = true;
        SignalCurrentModeStop();
        return "OK switching to " + arg;
    }

    if (verb == "PANIC") {
        // Panic means off and STAYS off - persisting idle is the difference between
        // a panic and a five-second pause before the next boot re-enforces.
        db.setMeta("desired_mode", "idle");
        if (db.meta("mode", "idle") == "idle") return "OK already idle (no filters)";
        g_modeSwitch = true;
        SignalCurrentModeStop();   // the daemon reverts every filter as it returns
        return "OK panic - filters reverted, going idle";
    }

    return "ERR unknown command: " + verb;
}

// One client at a time is plenty: these are human-scale commands, not a data path.
void CommandServer(Db* db) {
    while (!g_svcStopping) {
        HANDLE pipe = CreateNamedPipeW(kCmdPipe, PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) { Sleep(1000); continue; }

        const BOOL connected = ConnectNamedPipe(pipe, nullptr)
                                   ? TRUE
                                   : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (connected && !g_svcStopping) {
            char buf[1024] = {};
            DWORD n = 0;
            if (ReadFile(pipe, buf, sizeof(buf) - 1, &n, nullptr) && n > 0) {
                const std::string reply = HandleCommand(*db, std::string(buf, n));
                DWORD wr = 0;
                WriteFile(pipe, reply.data(), (DWORD)reply.size(), &wr, nullptr);
                FlushFileBuffers(pipe);
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

// ConnectNamedPipe blocks, so on shutdown the server thread is parked inside it.
// Connecting to our own pipe unblocks it; it then sees g_svcStopping and returns,
// which is what lets us join rather than detach a thread still using `db`.
void WakeCommandServer() {
    HANDLE h = CreateFileW(kCmdPipe, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
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

    // Created once for the whole service lifetime: HandlerEx and the command
    // server both signal it from other threads, so it must not be a handle that
    // comes and goes with whichever mode happens to be running.
    g_idleStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread cmdThread(CommandServer, &db);

    const bool ok = RunModeLoop(db, resolver, dns, habits);

    g_svcStopping = true;   // also covers RunModeLoop returning on its own
    WakeCommandServer();
    if (cmdThread.joinable()) cmdThread.join();   // joined, not detached: it holds &db
    CloseHandle(g_idleStop);
    g_idleStop = nullptr;

    dns.stop();
    SetMetaMode(db, "idle");
    // A failed start must exit non-zero, or the SCM treats it as a clean stop:
    // no event logged, and the restart policy - which is what recovers a transient
    // failure like the WFP provider still being held by a just-killed enforcer -
    // never fires.
    SetState(SERVICE_STOPPED, ok ? 0 : ERROR_SERVICE_SPECIFIC_ERROR);
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

    // Deliberately does NOT touch desired_mode. Installing the service is
    // infrastructure - it makes protection POSSIBLE, it is not the same act as the
    // user asking to BE protected. Forcing 'enforcing' here meant clicking
    // "Install as service" once, on a machine that had never been told to enforce
    // anything, jumped straight to enforcing with no button ever pressed for it.
    // Whatever's already in the db - idle for a database that's never had a mode
    // chosen (see db.cpp's seed), or whatever the user explicitly set before
    // installing - is what the service picks up the moment it actually starts.

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
