#include "core/db.h"

#include <cstdio>

namespace ng {

namespace {
const char* kSchema =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA synchronous=NORMAL;"
    "CREATE TABLE IF NOT EXISTS meta(k TEXT PRIMARY KEY, v TEXT);"
    "INSERT OR IGNORE INTO meta(k,v) VALUES('schema_version','2');"
    "CREATE TABLE IF NOT EXISTS process_identity("
    "  id INTEGER PRIMARY KEY,"
    "  device_path       TEXT UNIQUE,"
    "  image_path        TEXT,"   // normalized C:\...
    "  sha256            TEXT,"   // hex, NULL if unreadable
    "  signer            TEXT,"   // Authenticode signer subject, NULL if unsigned
    "  signer_thumbprint TEXT,"   // SHA1 hex, NULL if unsigned
    "  signed            INTEGER,"
    "  first_seen        TEXT,"
    "  last_seen         TEXT);"
    "CREATE TABLE IF NOT EXISTS flow_events("
    "  id INTEGER PRIMARY KEY,"
    "  ts_utc      TEXT NOT NULL,"
    "  verdict     TEXT NOT NULL,"
    "  protocol    INTEGER,"
    "  local_addr  TEXT, local_port  INTEGER,"
    "  remote_addr TEXT, remote_port INTEGER,"
    "  image_path    TEXT,"  // raw device path (kept for continuity)
    "  user_sid      TEXT,"
    "  image_id      INTEGER,"
    "  remote_domain TEXT);"  // resolved via DNS-client ETW, NULL if unknown
    // The learned baseline: one row per (process, destination, port, protocol)
    // with a decaying count and time-of-day histograms.
    "CREATE TABLE IF NOT EXISTS habits("
    "  id INTEGER PRIMARY KEY,"
    "  process_key   TEXT,"   // sig:<thumb> | sha:<hash> | dev:<path>
    "  process_label TEXT,"   // signer subject or image basename
    "  dest          TEXT,"   // domain if known, else remote IP
    "  remote_port   INTEGER,"
    "  protocol      INTEGER,"
    "  count         REAL,"   // exponentially decayed observation count
    "  first_seen    TEXT,"
    "  last_seen     TEXT,"
    "  last_epoch    REAL,"   // unix seconds of last obs (for decay math)
    "  hour_hist     TEXT,"   // 24 comma-separated counts (UTC hour)
    "  dow_hist      TEXT,"   // 7 comma-separated counts (0=Sun)
    "  UNIQUE(process_key, dest, remote_port, protocol));"
    // User-editable firewall rules. The dashboard writes these directly (no
    // per-edit elevation) and ngd enforce reads + applies them as WFP filters,
    // re-scanning when meta('rules_gen') changes so edits take effect live.
    "CREATE TABLE IF NOT EXISTS rules("
    "  id INTEGER PRIMARY KEY,"
    "  action        TEXT NOT NULL,"   // 'permit' | 'block'
    "  app_path      TEXT,"            // normalized C:\...  (NULL = any app)
    "  remote_addr   TEXT,"            // IPv4 dotted (NULL = any)
    "  remote_port   INTEGER,"         // NULL/0 = any
    "  protocol      INTEGER,"         // NULL/0 = any (6 = TCP)
    "  enabled       INTEGER NOT NULL DEFAULT 1,"
    "  expires_epoch REAL,"            // NULL = permanent; timed-allow sets a future epoch
    "  note          TEXT,"
    "  created_at    TEXT);"
    "INSERT OR IGNORE INTO meta(k,v) VALUES('rules_gen','0');"
    // Autonomy: 0 = prompt on every novel connection, 1 = auto-allow when the
    // app is already known (has a learned habit), 2 = auto-allow everything.
    "INSERT OR IGNORE INTO meta(k,v) VALUES('autonomy','0');"
    // Phase 4 data foundation: one row per COMPLETED TCP flow - the metadata
    // feature vector the ML tier scores (asynchronously, off the decision
    // path). Populated by `ngd features` from GetTcpTable2 + per-connection
    // ESTATS byte counts. Opt-in (feature_archive) and auto-purged.
    "CREATE TABLE IF NOT EXISTS flow_features("
    "  id INTEGER PRIMARY KEY,"
    "  ts_utc        TEXT NOT NULL,"   // flow completion (observation) time
    "  process_key   TEXT,"            // sig:<thumb> | sha:<hash> | dev:<path>
    "  process_label TEXT,"
    "  dest          TEXT,"            // remote IP (domain correlation added later)
    "  remote_port   INTEGER,"
    "  protocol      INTEGER,"
    "  duration_ms   INTEGER,"         // observed lifetime of the connection
    "  bytes_in      INTEGER,"
    "  bytes_out     INTEGER,"
    "  local_port    INTEGER,"
    "  anomaly_score REAL,"            // Phase 4b: shadow anomaly score, NULL if unscored
    "  malicious_score REAL);"         // Phase 4c: shadow supervised P(malicious), NULL if unscored
    // Feature archival is off by default (privacy: it records who you talked to
    // and how much). `ngd features` collecting is itself the opt-in; this flag
    // gates future collection folded into the enforce/record daemon.
    "INSERT OR IGNORE INTO meta(k,v) VALUES('feature_archive','0');"
    // ML scoring mode: 'shadow' = score + log, zero effect on rules (default,
    // even across upgrades); 'active' = high scores may feed demotions / review
    // flags (Phase 4d); 'off' = don't score.
    "INSERT OR IGNORE INTO meta(k,v) VALUES('ml_mode','shadow');"
    // Phase 4d: model outputs that crossed a confidence gate, in ACTIVE mode.
    // kind='demote' (supervised P(malicious) >= threshold) excludes that
    // (app,port,proto) from the auto-permit baseline -> it drops to default-deny
    // and PROMPTS next time (never an auto-block). kind='review' (anomaly score
    // <= threshold) is advisory only - shown to the user, touches no rule.
    "CREATE TABLE IF NOT EXISTS ml_flags("
    "  id INTEGER PRIMARY KEY,"
    "  ts_utc        TEXT,"
    "  kind          TEXT,"           // 'demote' | 'review'
    "  process_key   TEXT,"
    "  process_label TEXT,"
    "  app_path      TEXT,"           // normalized C:\...  (the baseline exclusion key)
    "  dest          TEXT,"
    "  remote_port   INTEGER,"
    "  protocol      INTEGER,"
    "  score         REAL,"
    "  UNIQUE(kind, app_path, remote_port, protocol));"
    // Confidence gates for active mode. Supervised >= malicious => demote;
    // anomaly <= anomaly (more negative = more anomalous) => review.
    "INSERT OR IGNORE INTO meta(k,v) VALUES('ml_malicious_threshold','0.9');"
    "INSERT OR IGNORE INTO meta(k,v) VALUES('ml_anomaly_threshold','-0.15');";
}  // namespace

bool Db::open(const char* path) {
    if (sqlite3_open(path, &db_) != SQLITE_OK) {
        fprintf(stderr, "sqlite3_open(%s) failed: %s\n", path, sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_busy_timeout(db_, 3000);  // tolerate a concurrent writer (recorder)
    char* errmsg = nullptr;
    if (sqlite3_exec(db_, kSchema, nullptr, nullptr, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "schema init failed: %s\n", errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
        return false;
    }
    // Defensive migrations for older DBs (ignore "duplicate column" errors).
    sqlite3_exec(db_, "ALTER TABLE flow_events ADD COLUMN image_id INTEGER;",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE flow_events ADD COLUMN remote_domain TEXT;",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE flow_features ADD COLUMN anomaly_score REAL;",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE flow_features ADD COLUMN malicious_score REAL;",
                 nullptr, nullptr, nullptr);
    return true;
}

void Db::close() {
    if (db_) { sqlite3_close(db_); db_ = nullptr; }
}

}  // namespace ng
