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
    "  remote_domain TEXT,"  // resolved via DNS-client ETW, NULL if unknown
    "  direction     TEXT);" // 'out' (ALE connect) | 'in' (ALE recv-accept) | NULL
                             // (non-ALE event). From the event's layer, not a guess.
    // The learned baseline: one row per (process, destination, port, protocol)
    // with a decaying count and time-of-day histograms.
    "CREATE TABLE IF NOT EXISTS habits("
    "  id INTEGER PRIMARY KEY,"
    "  process_key   TEXT,"   // sig:<thumb> | sha:<hash> | dev:<path>
    "  process_label TEXT,"   // signer subject or image basename
    "  dest          TEXT,"   // outbound: domain/remote IP; inbound: '' (any peer)
    "  remote_port   INTEGER,"// the SERVICE port: remote port outbound, local port inbound
    "  protocol      INTEGER,"
    "  direction     TEXT,"   // 'out' (ALE connect) | 'in' (ALE recv-accept)
    "  count         REAL,"   // exponentially decayed observation count
    "  first_seen    TEXT,"
    "  last_seen     TEXT,"
    "  last_epoch    REAL,"   // unix seconds of last obs (for decay math)
    "  hour_hist     TEXT,"   // 24 comma-separated counts (UTC hour)
    "  dow_hist      TEXT,"   // 7 comma-separated counts (0=Sun)
    "  UNIQUE(process_key, dest, remote_port, protocol, direction));"
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
    // Inbound enforcement: 'off' (default) = enforce outbound only, exactly as
    // before - inbound accepts are still LEARNED (direction-aware habits), just
    // never blocked. 'enforce' = also install the stable inbound baseline permits
    // + inbound default-deny at RECV_ACCEPT. Opt-in by design: watch
    // `ngd inbound` preview until the inbound baseline covers your real services,
    // THEN turn it on, so enabling it can't blindside a listening service.
    "INSERT OR IGNORE INTO meta(k,v) VALUES('inbound_mode','off');"
    // What the user WANTS to be running, as opposed to meta('mode'), which is what
    // IS running right now. The service reads this at startup and resumes it, so a
    // reboot honours the last decision instead of always coming up enforcing.
    // 'enforcing' | 'learning' | 'idle'. Defaults to 'enforcing' so an upgrade
    // can never silently leave an existing install unprotected; `ngd stop` (and
    // the Stop button behind it) is what sets 'idle'.
    "INSERT OR IGNORE INTO meta(k,v) VALUES('desired_mode','enforcing');"
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
    "INSERT OR IGNORE INTO meta(k,v) VALUES('ml_anomaly_threshold','-0.15');"
    // Inbound services WE blocked (inbound_mode='enforce'), for passive review.
    // Inbound is never prompted: a remote party must never be able to pop a dialog
    // on your screen, and the decision you actually want to make is per SERVICE
    // ("should sshd be reachable?"), not per SYN. So novel inbound is blocked
    // silently and recorded here, deduped per (app, local port, proto); the tray
    // balloons ONCE per new service (notified) and you allow it at your leisure.
    // Only drops from our own inbound catch-all land here - Windows Firewall's own
    // inbound drops are not ours to offer, and would swamp the list.
    "CREATE TABLE IF NOT EXISTS inbound_blocked("
    "  id INTEGER PRIMARY KEY,"
    "  app_path      TEXT,"            // normalized C:\...
    "  process_label TEXT,"
    "  local_port    INTEGER,"         // the service port that was connected TO
    "  protocol      INTEGER,"
    "  first_seen    TEXT,"
    "  last_seen     TEXT,"
    "  attempts      INTEGER DEFAULT 0,"
    "  last_peer     TEXT,"            // most recent remote address seen
    "  notified      INTEGER DEFAULT 0,"  // tray balloon shown once
    "  allowed       INTEGER DEFAULT 0,"  // user permitted it -> joins the inbound baseline
    "  UNIQUE(app_path, local_port, protocol));"
    // Phase 4e: the feedback loop. Every enforcement prompt decision (and each
    // autonomy auto-allow) is logged here as a labeled example - the user's own
    // verdict on an (app, dest, port). A manual retraining script folds these into
    // the next offline LightGBM run. label: 0 = benign (allowed), 1 = malicious
    // (blocked). Grows slowly by design - prompts get rare as the baseline learns.
    "CREATE TABLE IF NOT EXISTS feedback_labels("
    "  id INTEGER PRIMARY KEY,"
    "  ts_utc        TEXT,"
    "  process_key   TEXT,"
    "  process_label TEXT,"
    "  app_path      TEXT,"
    "  dest          TEXT,"
    "  remote_port   INTEGER,"
    "  protocol      INTEGER,"
    "  decision      TEXT,"           // 'allow' | 'once' | 'block' | 'auto-allow'
    "  label         INTEGER);";      // 0 = benign, 1 = malicious
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
    // Direction-aware habits (both-direction learning). Existing rows are all
    // outbound (that's all the old heuristic could learn), so default to 'out'.
    // On an old DB the UNIQUE constraint stays keyed without direction, but
    // inbound rows use dest='' while outbound never does, so they can't collide.
    sqlite3_exec(db_, "ALTER TABLE habits ADD COLUMN direction TEXT DEFAULT 'out';",
                 nullptr, nullptr, nullptr);
    // Direction on raw events too - the inbound baseline needs to tell an inbound
    // accept from an outbound connect, which the local/remote ports alone can't
    // (that ambiguity is exactly what the old port heuristic was guessing at).
    // Left NULL on old rows: they predate direction, so they're simply not
    // eligible for the inbound baseline rather than being mislabelled.
    sqlite3_exec(db_, "ALTER TABLE flow_events ADD COLUMN direction TEXT;",
                 nullptr, nullptr, nullptr);
    return true;
}

void Db::close() {
    if (db_) { sqlite3_close(db_); db_ = nullptr; }
}

std::string Db::meta(const char* key, const char* dflt) {
    std::string out = dflt ? dflt : "";
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT v FROM meta WHERE k=?;", -1, &s, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, key, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW)
            if (const char* t = (const char*)sqlite3_column_text(s, 0)) out = t;
        sqlite3_finalize(s);
    }
    return out;
}

void Db::setMeta(const char* key, const std::string& val) {
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db_,
            "INSERT INTO meta(k,v) VALUES(?,?) ON CONFLICT(k) DO UPDATE SET v=excluded.v;",
            -1, &s, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, key, -1, SQLITE_STATIC);
        bindText(s, 2, val);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
}

}  // namespace ng
