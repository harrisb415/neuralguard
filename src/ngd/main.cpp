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
#include "core/enforcer.h"
#include "core/habit.h"
#include "core/identity.h"
#include "core/util.h"
#include "ngd/enforce.h"
#include "ngd/recorder.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

ng::Recorder* g_recorder = nullptr;
ng::EnforceDaemon* g_enforce = nullptr;

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
        "  ngd digest [db]               A 'what's new' digest of the learned baseline.\n"
        "  ngd compact [db]              Decay habit counts to now and evict faded ones.\n"
        "  ngd novelty [db]              Rank habits by novelty (rare + recently seen).\n"
        "  ngd promote [db]              Show stable vs provisional (app, port) pairs.\n"
        "  ngd enforce [db] [seconds]    LIVE: permit the stable baseline, default-deny the\n"
        "                                rest, and prompt the tray on novel connections.\n"
        "  ngd -h | --help | /?          Show this help.\n\n"
        "Recording requires an elevated (Administrator) prompt.\n");
}

BOOL WINAPI CtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        if (g_recorder) g_recorder->stop();
        if (g_enforce) g_enforce->stop();
        return TRUE;
    }
    return FALSE;
}

void DigestQuery(sqlite3* h, const char* title, const char* sql,
                 const char* bindIso = nullptr) {
    printf("\n%s\n", title);
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(h, sql, -1, &s, nullptr) != SQLITE_OK) return;
    if (bindIso) sqlite3_bind_text(s, 1, bindIso, -1, SQLITE_TRANSIENT);
    int cols = sqlite3_column_count(s);
    int rows = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        printf("  ");
        for (int i = 0; i < cols; ++i) {
            const char* t = (const char*)sqlite3_column_text(s, i);
            printf("%s%s", i ? "  " : "", t ? t : "");
        }
        printf("\n");
        ++rows;
    }
    if (!rows) printf("  (none)\n");
    sqlite3_finalize(s);
}

// A "what's new" digest over the learned baseline - the Phase 3 report that
// later feeds novelty scoring and the weekly summary. Read-only.
int RunDigest(ng::Db& db) {
    sqlite3* h = db.handle();

    // ISO cutoff for "last 7 days" (ISO-8601 sorts chronologically, so a string
    // compare avoids julianday choking on our 'Z'-suffixed timestamps).
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    u.QuadPart -= 7ULL * 24 * 3600 * 10000000ULL;
    FILETIME cf; cf.dwLowDateTime = u.LowPart; cf.dwHighDateTime = u.HighPart;
    std::string cutoff = ng::util::IsoTime(cf);

    printf("=== NeuralGuard digest ===\n");
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(h,
        "SELECT (SELECT count(*) FROM habits), (SELECT count(DISTINCT process_label) FROM habits),"
        " (SELECT count(DISTINCT dest) FROM habits), (SELECT count(*) FROM flow_events);",
        -1, &s, nullptr);
    if (sqlite3_step(s) == SQLITE_ROW)
        printf("habits=%d  apps=%d  destinations=%d  events=%d\n",
               sqlite3_column_int(s, 0), sqlite3_column_int(s, 1),
               sqlite3_column_int(s, 2), sqlite3_column_int(s, 3));
    sqlite3_finalize(s);

    DigestQuery(h, "-- top talkers (by decayed count) --",
        "SELECT round(count,1), process_label, dest||':'||remote_port FROM habits"
        " ORDER BY count DESC LIMIT 10;");

    DigestQuery(h, "-- new in the last 7 days --",
        "SELECT process_label, dest||':'||remote_port, first_seen FROM habits"
        " WHERE first_seen >= ? ORDER BY first_seen DESC LIMIT 15;", cutoff.c_str());

    DigestQuery(h, "-- rare / one-off (count < 2) - the novel ones --",
        "SELECT process_label, dest||':'||remote_port, last_seen FROM habits"
        " WHERE count < 2 ORDER BY last_seen DESC LIMIT 15;");

    DigestQuery(h, "-- chattiest apps (distinct destinations) --",
        "SELECT count(DISTINCT dest), process_label FROM habits"
        " GROUP BY process_label ORDER BY 1 DESC LIMIT 10;");

    return 0;
}

// Promotion report: classify each observed (app, port) as STABLE (seen on
// enough distinct connections to auto-permit) vs PROVISIONAL (rare/novel - would
// warrant a prompt rather than a silent permit). Distinct connections are counted
// by (local_port, remote_addr) since flow_events holds several rows per
// connection. Read-only. See docs/DESIGN.md section 5.
int RunPromote(ng::Db& db) {
    const int kMinConns = 3;
    sqlite3* h = db.handle();
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(h,
        "SELECT pi.image_path, fe.remote_port,"
        " COUNT(DISTINCT fe.local_port || '|' || fe.remote_addr) AS conns"
        " FROM flow_events fe JOIN process_identity pi ON fe.image_id = pi.id"
        " WHERE fe.remote_port > 0 AND fe.remote_port < 49152 AND pi.image_path LIKE '_:\\%'"
        "   AND fe.verdict IN ('ALLOW','CAPALLOW')"
        " GROUP BY pi.image_path, fe.protocol, fe.remote_port"
        " ORDER BY conns DESC;", -1, &s, nullptr);

    int stable = 0, provisional = 0;
    std::vector<std::string> provList;
    while (sqlite3_step(s) == SQLITE_ROW) {
        std::string path = (const char*)sqlite3_column_text(s, 0);
        int port = sqlite3_column_int(s, 1);
        int conns = sqlite3_column_int(s, 2);
        if (conns >= kMinConns) {
            ++stable;
        } else {
            ++provisional;
            size_t p = path.find_last_of('\\');
            std::string base = (p == std::string::npos) ? path : path.substr(p + 1);
            if (provList.size() < 25)
                provList.push_back(base + ":" + std::to_string(port) +
                                   " (" + std::to_string(conns) + ")");
        }
    }
    sqlite3_finalize(s);

    printf("=== promotion (>= %d distinct connections = stable) ===\n", kMinConns);
    printf("stable (would auto-permit):    %d (app, port) pairs\n", stable);
    printf("provisional (would prompt):    %d\n", provisional);
    if (!provList.empty()) {
        printf("\n-- provisional (rare) --\n");
        for (const std::string& v : provList) printf("  %s\n", v.c_str());
    }
    return 0;
}

// Novelty score for the learned baseline. A habit is "novel" (surprising) when
// it is rare AND was first seen recently - exactly the connections a firewall
// should scrutinize. novelty = 0.6*rarity + 0.4*newness, in [0,1]. This is the
// signal that will later drive auto-allow of low-novelty connections (Phase 3).
int RunNovelty(ng::Db& db) {
    sqlite3* h = db.handle();
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    double now = ng::util::UnixEpoch(ft);

    struct Row { double score, count; std::string label, dest; int port; };
    std::vector<Row> rows;

    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(h,
        "SELECT process_label, dest, remote_port, count, first_seen FROM habits;",
        -1, &s, nullptr);
    while (sqlite3_step(s) == SQLITE_ROW) {
        double count = sqlite3_column_double(s, 3);
        const char* fs = (const char*)sqlite3_column_text(s, 4);
        double firstEpoch = fs ? ng::util::EpochFromIso(fs) : 0;
        double ageDays = firstEpoch > 0 ? (now - firstEpoch) / 86400.0 : 999;
        double rarity = 1.0 / (1.0 + count);
        double newness = std::exp(-ageDays / 7.0);   // decays over ~a week
        double score = 0.6 * rarity + 0.4 * newness;
        rows.push_back({score, count,
                        (const char*)sqlite3_column_text(s, 0),
                        (const char*)sqlite3_column_text(s, 1),
                        sqlite3_column_int(s, 2)});
    }
    sqlite3_finalize(s);

    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) { return a.score > b.score; });

    printf("=== most novel habits (rare + recently first seen) ===\n");
    printf("score  count  app -> dest:port\n");
    int n = 0;
    for (const Row& r : rows) {
        if (n++ >= 25) break;
        printf("%.2f   %5.1f  %s -> %s:%d\n", r.score, r.count,
               r.label.c_str(), r.dest.c_str(), r.port);
    }
    return 0;
}

// Nightly compaction: decay every habit's count forward to now (same 14-day
// half-life the recorder uses) and evict the ones that have faded away. Safe to
// run repeatedly. See docs/DESIGN.md section 5.
int RunCompact(ng::Db& db) {
    sqlite3* h = db.handle();
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    double now = ng::util::UnixEpoch(ft);

    int before = 0;
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(h, "SELECT count(*) FROM habits;", -1, &s, nullptr);
    if (sqlite3_step(s) == SQLITE_ROW) before = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);

    sqlite3_prepare_v2(h,
        "UPDATE habits SET count = count * power(0.5, ((?1 - last_epoch)/86400.0)/14.0),"
        " last_epoch = ?1 WHERE last_epoch IS NOT NULL;", -1, &s, nullptr);
    sqlite3_bind_double(s, 1, now);
    if (sqlite3_step(s) != SQLITE_DONE)
        fprintf(stderr, "compact decay failed: %s\n", sqlite3_errmsg(h));
    sqlite3_finalize(s);

    char* err = nullptr;
    sqlite3_exec(h, "DELETE FROM habits WHERE count < 0.1;", nullptr, nullptr, &err);
    if (err) { fprintf(stderr, "evict failed: %s\n", err); sqlite3_free(err); }

    int after = 0;
    sqlite3_prepare_v2(h, "SELECT count(*) FROM habits;", -1, &s, nullptr);
    if (sqlite3_step(s) == SQLITE_ROW) after = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);

    printf("compact: %d habits decayed, %d evicted (faded), %d remain.\n",
           before, before - after, after);
    return 0;
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
    if (argc >= 2 && (strcmp(argv[1], "dump") == 0 || strcmp(argv[1], "record") == 0 ||
                      strcmp(argv[1], "digest") == 0 || strcmp(argv[1], "compact") == 0 ||
                      strcmp(argv[1], "novelty") == 0 || strcmp(argv[1], "promote") == 0 ||
                      strcmp(argv[1], "enforce") == 0)) {
        mode = argv[1];
        if (argc >= 3) dbPath = argv[2];
        if (argc >= 4 && (strcmp(mode, "record") == 0 || strcmp(mode, "enforce") == 0))
            seconds = atoi(argv[3]);
    } else if (argc >= 2) {
        dbPath = argv[1];
        if (argc >= 3) seconds = atoi(argv[2]);
    }

    const bool needsAdmin = strcmp(mode, "record") == 0 || strcmp(mode, "enforce") == 0;
    if (needsAdmin && !IsElevated()) {
        fprintf(stderr,
            "NeuralGuard ngd must be run as Administrator.\n"
            "record/enforce use the Windows Filtering Platform and ETW, which require an\n"
            "elevated token. Right-click PowerShell -> Run as administrator, then run\n"
            "  .\\ngd\nagain. (dump/digest/novelty/promote work without elevation.)\n");
        return 1;
    }

    ng::Db db;
    if (!db.open(dbPath)) return 1;

    if (strcmp(mode, "dump") == 0) return RunDump(db);
    if (strcmp(mode, "digest") == 0) return RunDigest(db);
    if (strcmp(mode, "compact") == 0) return RunCompact(db);
    if (strcmp(mode, "novelty") == 0) return RunNovelty(db);
    if (strcmp(mode, "promote") == 0) return RunPromote(db);

    if (strcmp(mode, "enforce") == 0) {
        ng::IdentityResolver resolver(db);
        resolver.init();
        ng::DnsWatcher dns;
        if (!dns.start())
            fprintf(stderr, "warning: DNS correlation disabled (ETW session failed)\n");
        ng::Enforcer enf;
        ng::EnforceDaemon daemon(db, resolver, dns, enf);
        g_enforce = &daemon;
        SetConsoleCtrlHandler(CtrlHandler, TRUE);
        bool ok = daemon.run(seconds);
        dns.stop();
        return ok ? 0 : 1;
    }

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
