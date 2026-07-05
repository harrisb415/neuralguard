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
#include "core/identity.h"
#include "ngd/recorder.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace {

ng::Recorder* g_recorder = nullptr;

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

    printf("\n--- most recent 15 events ---\n");
    sqlite3_prepare_v2(h,
        "SELECT fe.ts_utc, fe.verdict, fe.protocol, fe.remote_addr, fe.remote_port,"
        " COALESCE(pi.image_path, fe.image_path), COALESCE(pi.signer,'')"
        " FROM flow_events fe LEFT JOIN process_identity pi ON fe.image_id = pi.id"
        " ORDER BY fe.id DESC LIMIT 15;", -1, &s, nullptr);
    while (sqlite3_step(s) == SQLITE_ROW) {
        printf("  %s %-8s p=%d -> %s:%d  %s  {%s}\n",
               sqlite3_column_text(s, 0), sqlite3_column_text(s, 1),
               sqlite3_column_int(s, 2),
               sqlite3_column_text(s, 3), sqlite3_column_int(s, 4),
               sqlite3_column_text(s, 5) ? (const char*)sqlite3_column_text(s, 5) : "",
               sqlite3_column_text(s, 6) ? (const char*)sqlite3_column_text(s, 6) : "");
    }
    sqlite3_finalize(s);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const char* mode = "record";
    const char* dbPath = "ngpolicy.db";
    if (argc >= 2 && (strcmp(argv[1], "dump") == 0 || strcmp(argv[1], "record") == 0)) {
        mode = argv[1];
        if (argc >= 3) dbPath = argv[2];
    } else if (argc >= 2) {
        dbPath = argv[1];
    }

    ng::Db db;
    if (!db.open(dbPath)) return 1;

    if (strcmp(mode, "dump") == 0) return RunDump(db);

    ng::IdentityResolver resolver(db);
    resolver.init();
    ng::Recorder recorder(db, resolver);
    g_recorder = &recorder;
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    printf("ngd - recording WFP net events to %s\n", dbPath);
    return recorder.run() ? 0 : 1;
}
