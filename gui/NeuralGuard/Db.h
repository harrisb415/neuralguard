#pragma once
// Minimal read-only SQLite wrapper for the dashboard. The dashboard only reads
// ngpolicy.db; all writes (rules, etc.) go through ngd/ngctl.
#include "deps/sqlite3.h"

#include <functional>
#include <string>

namespace ng {

class Db {
public:
    ~Db() { close(); }

    bool open(const char* path) {
        if (sqlite3_open_v2(path, &db_, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
            if (db_) { sqlite3_close(db_); db_ = nullptr; }
            return false;
        }
        sqlite3_busy_timeout(db_, 3000);
        return true;
    }
    void close() { if (db_) { sqlite3_close(db_); db_ = nullptr; } }
    sqlite3* handle() const { return db_; }

    // Run a query; invoke fn(stmt) for each row.
    void each(const char* sql, const std::function<void(sqlite3_stmt*)>& fn) {
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &s, nullptr) != SQLITE_OK) return;
        while (sqlite3_step(s) == SQLITE_ROW) fn(s);
        sqlite3_finalize(s);
    }

    // Convenience: text of a single-value query (e.g. meta lookups).
    std::string scalar(const char* sql) {
        std::string out;
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &s, nullptr) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW) {
            const char* t = (const char*)sqlite3_column_text(s, 0);
            if (t) out = t;
        }
        sqlite3_finalize(s);
        return out;
    }

    static std::string ColText(sqlite3_stmt* s, int i) {
        const char* t = (const char*)sqlite3_column_text(s, i);
        return t ? t : "";
    }

private:
    sqlite3* db_ = nullptr;
};

}  // namespace ng
