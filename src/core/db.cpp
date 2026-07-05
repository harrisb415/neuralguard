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
    "  image_path  TEXT,"   // raw device path (kept for continuity)
    "  user_sid    TEXT,"
    "  image_id    INTEGER);";
}  // namespace

bool Db::open(const char* path) {
    if (sqlite3_open(path, &db_) != SQLITE_OK) {
        fprintf(stderr, "sqlite3_open(%s) failed: %s\n", path, sqlite3_errmsg(db_));
        return false;
    }
    char* errmsg = nullptr;
    if (sqlite3_exec(db_, kSchema, nullptr, nullptr, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "schema init failed: %s\n", errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
        return false;
    }
    // Defensive migration for a v1 DB (ignore "duplicate column" error).
    sqlite3_exec(db_, "ALTER TABLE flow_events ADD COLUMN image_id INTEGER;",
                 nullptr, nullptr, nullptr);
    return true;
}

void Db::close() {
    if (db_) { sqlite3_close(db_); db_ = nullptr; }
}

}  // namespace ng
