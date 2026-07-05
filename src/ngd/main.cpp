// ngd - NeuralGuard daemon (Phase 1: learning mode).
//
// Records WFP net events into SQLite, each attributed to a process identity
// (normalized path + SHA-256 + Authenticode signer). Passive baseline collector
// - no enforcement, no ML. Runs as a console app for now; the Windows-service
// wrapper comes in Phase 2. See docs/DESIGN.md and docs/ROADMAP.md (Phase 1).
//
// Usage:
//   ngd [record] [dbpath]   record net events into dbpath (default ngpolicy.db)
//   ngd dump      [dbpath]   print recent events + the process-identity table
//
// Requires elevation and the "Filtering Platform Connection" audit subcategory.
// Ctrl+C to stop recording.

#include "core/db.h"
#include "core/dns.h"
#include "core/habit.h"
#include "core/identity.h"
#include "ngd/recorder.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {

ng::Recorder* g_recorder = nullptr;

bool IsElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION elev{}; DWORD cb = 0;
    bool elevated = GetTokenInformation(tok, TokenElevation, &elev, sizeof(elev), &cb) &&
                    elev.TokenIsElevated;
    CloseHandle(tok);
    return elevated;
}

void PrintUsage() {
    printf(
        "NeuralGuard ngd - learning-mode WFP recorder\n\n"
        "Usage:\n"
        "  ngd [record] [db] [seconds]   Record WFP net events into <db>\n"
        "                                (default ngpolicy.db). [seconds] auto-stops;\n"
        "                                otherwise runs until Ctrl+C.\n"
        "  ngd dump [db]                 Print the learned baseline + recent events.\n"
        "  ngd -h | --help | /?          Show this help.\n\n"
        "Recording requires an elevated (Administrator) prompt.\n");
}

BOOL WINAPI CtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        if (g_recorder) g_recorder->stop();
        return TRUE;
    }
    return FALSE;
}

int RunDump(ng::Db& db) {
    sqlite3* h = db.handle();
    sqlite3_stmt* s = nullptr;

    sqlite3_prepare_v2(h, "SELECT count(*) FROM flow_events;", -1, &s, nullptr);
    if (sqlite3_step(s) == SQLITE_ROW) printf("flow_events: %lld    ", sqlite3_column_int64(s, 0));
    sqlite3_finalize(s);
    sqlite3_prepare_v2(h, "SELECT count(*) FROM process_identity;", -1, &s, nullptr);
    if (sqlite3_step(s) == SQLITE_ROW) printf("process_identity: %lld\n", sqlite3_column_int64(s, 0));
    sqlite3_finalize(s);

    printf("\n--- process identities ---\n");
    sqlite3_prepare_v2(h,
        "SELECT image_path, signed, COALESCE(signer,'(unsigned)'), substr(COALESCE(sha256,''),1,16)"
        " FROM process_identity ORDER BY id;", -1, &s, nullptr);
    while (sqlite3_step(s) == SQLITE_ROW) {
        printf("  [%s] %-40s sha=%s..\n",
               sqlite3_column_int(s, 1) ? "signed  " : "UNSIGNED",
               (const char*)sqlite3_column_text(s, 2),
               (const char*)sqlite3_column_text(s, 3));
        printf("      %s\n", (const char*)sqlite3_column_text(s, 0));
    }
    sqlite3_finalize(s);

    printf("\n--- learned habits (top 25 by decayed count) ---\n");
    sqlite3_prepare_v2(h,
        "SELECT process_label, dest, remote_port, protocol, count FROM habits"
        " ORDER BY count DESC LIMIT 25;", -1, &s, nullptr);
    while (sqlite3_step(s) == SQLITE_ROW) {
        printf("  %6.1f  %-28s -> %s:%d/%d\n",
               sqlite3_column_double(s, 4),
               (const char*)sqlite3_column_text(s, 0),
               (const char*)sqlite3_column_text(s, 1),
               sqlite3_column_int(s, 2), sqlite3_column_int(s, 3));
    }
    sqlite3_finalize(s);

    printf("\n--- domains correlated (via DNS ETW) ---\n");
    sqlite3_prepare_v2(h,
        "SELECT remote_domain, count(*) FROM flow_events"
        " WHERE remote_domain IS NOT NULL GROUP BY remote_domain ORDER BY 2 DESC LIMIT 20;",
        -1, &s, nullptr);
    while (sqlite3_step(s) == SQLITE_ROW)
        printf("  %5d  %s\n", sqlite3_column_int(s, 1), (const char*)sqlite3_column_text(s, 0));
    sqlite3_finalize(s);

    printf("\n--- most recent 15 events ---\n");
    sqlite3_prepare_v2(h,
        "SELECT fe.ts_utc, fe.verdict, fe.protocol, fe.remote_addr, fe.remote_port,"
        " COALESCE(fe.remote_domain, fe.remote_addr), COALESCE(pi.image_path, fe.image_path)"
        " FROM flow_events fe LEFT JOIN process_identity pi ON fe.image_id = pi.id"
        " ORDER BY fe.id DESC LIMIT 15;", -1, &s, nullptr);
    while (sqlite3_step(s) == SQLITE_ROW) {
        printf("  %s %-8s p=%d -> %s:%d (%s)  %s\n",
               sqlite3_column_text(s, 0), sqlite3_column_text(s, 1),
               sqlite3_column_int(s, 2),
               sqlite3_column_text(s, 3), sqlite3_column_int(s, 4),
               (const char*)sqlite3_column_text(s, 5),
               sqlite3_column_text(s, 6) ? (const char*)sqlite3_column_text(s, 6) : "");
    }
    sqlite3_finalize(s);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2) {
        const char* a = argv[1];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0 || strcmp(a, "/?") == 0) {
            PrintUsage();
            return 0;
        }
    }

    const char* mode = "record";
    const char* dbPath = "ngpolicy.db";
    int seconds = 0;  // 0 = run until Ctrl+C
    if (argc >= 2 && (strcmp(argv[1], "dump") == 0 || strcmp(argv[1], "record") == 0)) {
        mode = argv[1];
        if (argc >= 3) dbPath = argv[2];
        if (argc >= 4 && strcmp(mode, "record") == 0) seconds = atoi(argv[3]);
    } else if (argc >= 2) {
        dbPath = argv[1];
        if (argc >= 3) seconds = atoi(argv[2]);
    }

    const bool recording = strcmp(mode, "record") == 0;
    if (recording && !IsElevated()) {
        fprintf(stderr,
            "NeuralGuard ngd must be run as Administrator.\n"
            "Recording uses the Windows Filtering Platform and ETW, which require an\n"
            "elevated token. Right-click PowerShell -> Run as administrator, then run\n"
            "  .\\ngd\nagain. (The 'dump' command works without elevation.)\n");
        return 1;
    }

    ng::Db db;
    if (!db.open(dbPath)) return 1;

    if (!recording) return RunDump(db);

    ng::IdentityResolver resolver(db);
    resolver.init();
    ng::DnsWatcher dns;
    if (!dns.start())
        fprintf(stderr, "warning: DNS correlation disabled (ETW session failed)\n");
    ng::HabitTracker habits(db);
    ng::Recorder recorder(db, resolver, dns, habits);
    g_recorder = &recorder;
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    printf("ngd - recording to %s%s. Press Ctrl+C to stop.\n", dbPath,
           seconds > 0 ? " (timed)" : "");
    std::thread timer;
    if (seconds > 0)
        timer = std::thread([&recorder, seconds]() {
            Sleep((DWORD)seconds * 1000);
            recorder.stop();
        });
    bool ok = recorder.run();
    if (timer.joinable()) timer.join();
    dns.stop();
    return ok ? 0 : 1;
}
