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
#include "core/flowstats.h"
#include "core/habit.h"
#include "core/identity.h"
#include "core/util.h"
#include "ngd/enforce.h"
#include "ngd/recorder.h"
#include "ngd/service.h"

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
ng::FlowCollector* g_collector = nullptr;

bool IsElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION elev{}; DWORD cb = 0;
    bool elevated = GetTokenInformation(tok, TokenElevation, &elev, sizeof(elev), &cb) &&
                    elev.TokenIsElevated;
    CloseHandle(tok);
    return elevated;
}

std::string MetaGet(ng::Db& db, const char* key, const char* dflt) {
    std::string v = dflt;
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db.handle(), "SELECT v FROM meta WHERE k=?;", -1, &s, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, key, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) == SQLITE_ROW) {
            const char* t = (const char*)sqlite3_column_text(s, 0);
            if (t) v = t;
        }
        sqlite3_finalize(s);
    }
    return v;
}

void MetaSet(ng::Db& db, const char* key, const char* val) {
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db.handle(),
            "INSERT INTO meta(k,v) VALUES(?,?) ON CONFLICT(k) DO UPDATE SET v=excluded.v;",
            -1, &s, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, val, -1, SQLITE_TRANSIENT);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
}

bool FeatureArchiveOn(ng::Db& db) { return MetaGet(db, "feature_archive", "0") == "1"; }

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
        "  ngd features [db] [seconds]   Collect completed-flow features (Phase 4 ML data).\n"
        "  ngd features dump [db]        Show recent archived feature rows.\n"
        "  ngd features purge [db] [days] Delete feature rows older than [days] (default 30).\n"
        "  ngd -h | --help | /?          Show this help.\n\n"
        "Recording requires an elevated (Administrator) prompt.\n");
}

BOOL WINAPI CtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        if (g_recorder) g_recorder->stop();
        if (g_enforce) g_enforce->stop();
        if (g_collector) g_collector->stop();
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

// Publish ngd's current mode so the dashboard's status bar can show it live.
void SetMode(ng::Db& db, const char* mode) {
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(db.handle(),
        "INSERT INTO meta(k,v) VALUES('mode',?) ON CONFLICT(k) DO UPDATE SET v=excluded.v;",
        -1, &s, nullptr);
    ng::bindText(s, 1, mode);
    sqlite3_step(s);
    sqlite3_finalize(s);
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

void BumpRulesGen(sqlite3* h) {
    sqlite3_exec(h, "UPDATE meta SET v = CAST(v AS INTEGER)+1 WHERE k='rules_gen';",
                 nullptr, nullptr, nullptr);
}

int RunRules(ng::Db& db) {
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(db.handle(),
        "SELECT id, action, COALESCE(app_path,'*'), COALESCE(remote_addr,'*'),"
        " COALESCE(remote_port,0), COALESCE(protocol,0), enabled, COALESCE(expires_epoch,0)"
        " FROM rules ORDER BY id;", -1, &s, nullptr);
    printf("=== rules ===\n");
    int n = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        double exp = sqlite3_column_double(s, 7);
        printf("  #%-3d %-6s app=%s ip=%s port=%d proto=%d%s%s\n",
               sqlite3_column_int(s, 0), sqlite3_column_text(s, 1),
               sqlite3_column_text(s, 2), sqlite3_column_text(s, 3),
               sqlite3_column_int(s, 4), sqlite3_column_int(s, 5),
               sqlite3_column_int(s, 6) ? "" : " (disabled)",
               exp > 0 ? " (timed)" : "");
        ++n;
    }
    sqlite3_finalize(s);
    if (!n) printf("  (none)\n");
    return 0;
}

// rule-add <db> permit|block [ip] [port] [proto] [ttl_seconds]
int RunRuleAdd(ng::Db& db, const char* action, const char* ip, int port, int proto, int ttl) {
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(db.handle(),
        "INSERT INTO rules(action, remote_addr, remote_port, protocol, enabled, expires_epoch, created_at)"
        " VALUES(?,?,?,?,1,?,datetime('now'));", -1, &s, nullptr);
    ng::bindText(s, 1, action);
    if (ip && *ip) ng::bindText(s, 2, ip); else sqlite3_bind_null(s, 2);
    if (port) sqlite3_bind_int(s, 3, port); else sqlite3_bind_null(s, 3);
    if (proto) sqlite3_bind_int(s, 4, proto); else sqlite3_bind_null(s, 4);
    if (ttl > 0) {
        FILETIME ft; GetSystemTimeAsFileTime(&ft);
        sqlite3_bind_double(s, 5, ng::util::UnixEpoch(ft) + ttl);
    } else {
        sqlite3_bind_null(s, 5);
    }
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) { fprintf(stderr, "rule-add failed: %s\n", sqlite3_errmsg(db.handle())); return 1; }
    BumpRulesGen(db.handle());
    printf("rule added: %s ip=%s port=%d proto=%d%s\n", action, ip && *ip ? ip : "*",
           port, proto, ttl > 0 ? " (timed)" : "");
    return 0;
}

int RunAutonomy(ng::Db& db, const char* levelOrNull) {
    if (levelOrNull) {
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db.handle(),
            "INSERT INTO meta(k,v) VALUES('autonomy',?) ON CONFLICT(k) DO UPDATE SET v=excluded.v;",
            -1, &s, nullptr);
        ng::bindText(s, 1, levelOrNull);
        sqlite3_step(s); sqlite3_finalize(s);
    }
    sqlite3_stmt* s = nullptr; int v = 0;
    sqlite3_prepare_v2(db.handle(), "SELECT v FROM meta WHERE k='autonomy';", -1, &s, nullptr);
    if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    const char* names[] = {"prompt-everything", "auto-allow-known", "auto-allow-all"};
    printf("autonomy = %d (%s)\n", v, (v >= 0 && v <= 2) ? names[v] : "?");
    return 0;
}

int RunRuleDel(ng::Db& db, int id) {
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(db.handle(), "DELETE FROM rules WHERE id=?;", -1, &s, nullptr);
    sqlite3_bind_int(s, 1, id);
    sqlite3_step(s);
    sqlite3_finalize(s);
    BumpRulesGen(db.handle());
    printf("rule #%d deleted\n", id);
    return 0;
}

// Phase 4a: print the most recent completed-flow feature rows. Read-only.
int RunFeaturesDump(ng::Db& db) {
    sqlite3* h = db.handle();
    long long total = 0;
    sqlite3_stmt* c = nullptr;
    if (sqlite3_prepare_v2(h, "SELECT COUNT(*) FROM flow_features;", -1, &c, nullptr) == SQLITE_OK) {
        if (sqlite3_step(c) == SQLITE_ROW) total = sqlite3_column_int64(c, 0);
        sqlite3_finalize(c);
    }
    printf("flow_features: %lld row(s) archived\n\n", total);
    printf("  %-8s  %-26s  %-24s  %8s  %11s  %11s\n",
           "time", "app", "dest:port", "dur(ms)", "bytes_in", "bytes_out");
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(h,
            "SELECT ts_utc, COALESCE(process_label,''), COALESCE(dest,''), remote_port,"
            " duration_ms, bytes_in, bytes_out FROM flow_features ORDER BY id DESC LIMIT 40;",
            -1, &s, nullptr) != SQLITE_OK)
        return 1;
    while (sqlite3_step(s) == SQLITE_ROW) {
        std::string ts  = (const char*)sqlite3_column_text(s, 0);
        std::string tm  = ts.size() >= 19 ? ts.substr(11, 8) : ts;
        std::string app = (const char*)sqlite3_column_text(s, 1);
        std::string dst = (const char*)sqlite3_column_text(s, 2);
        if (app.size() > 26) app = app.substr(0, 25) + ">";
        std::string dp = dst + ":" + std::to_string(sqlite3_column_int(s, 3));
        if (dp.size() > 24) dp = dp.substr(0, 23) + ">";
        printf("  %-8s  %-26s  %-24s  %8d  %11lld  %11lld\n",
               tm.c_str(), app.c_str(), dp.c_str(), sqlite3_column_int(s, 4),
               (long long)sqlite3_column_int64(s, 5), (long long)sqlite3_column_int64(s, 6));
    }
    sqlite3_finalize(s);
    return 0;
}

// ngd features                    - collect completed-flow features (needs admin)
// ngd features <db> [seconds]     - collect into <db>, optional timed run
// ngd features on|off [db]        - toggle archival by the enforce/record daemon
// ngd features dump [db]          - show recent archived feature rows
// ngd features purge [db] [days]  - delete rows older than [days] (default 30)
int RunFeatures(int argc, char** argv) {
    const char* sub = (argc >= 3) ? argv[2] : nullptr;
    const bool isDump  = sub && strcmp(sub, "dump") == 0;
    const bool isPurge = sub && strcmp(sub, "purge") == 0;
    const bool isOn    = sub && strcmp(sub, "on") == 0;
    const bool isOff   = sub && strcmp(sub, "off") == 0;
    const bool isCmd   = isDump || isPurge || isOn || isOff;

    const char* dbPath = "ngpolicy.db";
    int seconds = 0, days = 30;
    if (isCmd) {
        if (argc >= 4) dbPath = argv[3];
        if (isPurge && argc >= 5) days = atoi(argv[4]);
    } else {
        if (argc >= 3) dbPath = argv[2];
        if (argc >= 4) seconds = atoi(argv[3]);
    }

    // Only live collection needs admin (ESTATS enable); the rest are DB-only.
    if (!isCmd && !IsElevated()) {
        fprintf(stderr, "ngd features (collection) needs Administrator - enabling TCP ESTATS "
                        "data collection is privileged. (dump/purge/on/off do not.)\n");
        return 1;
    }

    ng::Db db;
    if (!db.open(dbPath)) return 1;

    if (isOn || isOff) {
        MetaSet(db, "feature_archive", isOn ? "1" : "0");
        printf("feature archival is now %s. It takes effect on the next ngd enforce/record run.\n",
               isOn ? "ON" : "OFF");
        return 0;
    }
    if (isDump) return RunFeaturesDump(db);
    if (isPurge) {
        long long n = ng::PurgeFlowFeatures(db, days);
        if (n < 0) { fprintf(stderr, "purge failed\n"); return 1; }
        printf("purged %lld flow feature row(s) older than %d days.\n", n, days);
        return 0;
    }

    ng::IdentityResolver resolver(db);
    resolver.init();
    ng::FlowCollector collector(db, resolver);
    g_collector = &collector;
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    printf("ngd - collecting completed-flow features to %s%s. Press Ctrl+C to stop.\n",
           dbPath, seconds > 0 ? " (timed)" : "");
    bool ok = collector.run(seconds);
    printf("stopped. %llu flow feature row(s) written.\n", collector.written());
    return ok ? 0 : 1;
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

    // Service control - handled before the normal mode parsing.
    if (argc >= 2 && strcmp(argv[1], "service-run") == 0)   // invoked by the SCM
        return ng::ServiceRun(argc >= 3 ? argv[2] : "ngpolicy.db");
    if (argc >= 2 && (strcmp(argv[1], "install") == 0 || strcmp(argv[1], "uninstall") == 0)) {
        if (!IsElevated()) { fprintf(stderr, "service install/uninstall needs Administrator.\n"); return 1; }
        if (strcmp(argv[1], "uninstall") == 0) return ng::ServiceUninstall();
        const char* rel = argc >= 3 ? argv[2] : "ngpolicy.db";   // absolute: service runs from System32
        char abs[MAX_PATH];
        if (!GetFullPathNameA(rel, MAX_PATH, abs, nullptr)) { fprintf(stderr, "bad db path\n"); return 1; }
        return ng::ServiceInstall(abs);
    }

    // `features` has sub-subcommands (dump/purge), so handle it before the flat
    // mode parser below treats argv[2] as a db path.
    if (argc >= 2 && strcmp(argv[1], "features") == 0) return RunFeatures(argc, argv);

    const char* mode = "record";
    const char* dbPath = "ngpolicy.db";
    int seconds = 0;  // 0 = run until Ctrl+C
    if (argc >= 2 && (strcmp(argv[1], "dump") == 0 || strcmp(argv[1], "record") == 0 ||
                      strcmp(argv[1], "digest") == 0 || strcmp(argv[1], "compact") == 0 ||
                      strcmp(argv[1], "novelty") == 0 || strcmp(argv[1], "promote") == 0 ||
                      strcmp(argv[1], "enforce") == 0 || strcmp(argv[1], "rules") == 0 ||
                      strcmp(argv[1], "rule-add") == 0 || strcmp(argv[1], "rule-del") == 0 ||
                      strcmp(argv[1], "autonomy") == 0)) {
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

    if (strcmp(mode, "rules") == 0) return RunRules(db);
    if (strcmp(mode, "rule-add") == 0)
        return RunRuleAdd(db, argc >= 4 ? argv[3] : "block", argc >= 5 ? argv[4] : "",
                          argc >= 6 ? atoi(argv[5]) : 0, argc >= 7 ? atoi(argv[6]) : 0,
                          argc >= 8 ? atoi(argv[7]) : 0);
    if (strcmp(mode, "rule-del") == 0) return RunRuleDel(db, argc >= 4 ? atoi(argv[3]) : 0);
    if (strcmp(mode, "autonomy") == 0) return RunAutonomy(db, argc >= 4 ? argv[3] : nullptr);
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
        ng::HabitTracker habits(db);
        ng::EnforceDaemon daemon(db, resolver, dns, enf, habits);
        g_enforce = &daemon;
        SetConsoleCtrlHandler(CtrlHandler, TRUE);
        SetMode(db, "enforcing");

        // Phase 4a: optionally archive completed-flow features in the background,
        // correlated to domains via the same DNS watcher.
        ng::FlowCollector collector(db, resolver, &dns);
        std::thread featThread;
        const bool feat = FeatureArchiveOn(db);
        if (feat) {
            g_collector = &collector;
            printf("feature archival ON - collecting completed-flow features alongside enforcement.\n");
            featThread = std::thread([&collector] { collector.run(0); });
        }

        bool ok = daemon.run(seconds);

        if (feat) { collector.stop(); if (featThread.joinable()) featThread.join(); g_collector = nullptr; }
        SetMode(db, "idle");
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
    SetMode(db, "learning");

    // Phase 4a: optionally archive completed-flow features while learning.
    ng::FlowCollector collector(db, resolver, &dns);
    std::thread featThread;
    const bool feat = FeatureArchiveOn(db);
    if (feat) {
        g_collector = &collector;
        printf("feature archival ON - collecting completed-flow features.\n");
        featThread = std::thread([&collector] { collector.run(0); });
    }

    std::thread timer;
    if (seconds > 0)
        timer = std::thread([&recorder, seconds]() {
            Sleep((DWORD)seconds * 1000);
            recorder.stop();
        });
    bool ok = recorder.run();
    if (timer.joinable()) timer.join();
    if (feat) { collector.stop(); if (featThread.joinable()) featThread.join(); g_collector = nullptr; }
    SetMode(db, "idle");
    dns.stop();
    return ok ? 0 : 1;
}
