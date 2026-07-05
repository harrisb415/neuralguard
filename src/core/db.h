// SQLite connection wrapper: owns the handle, creates/migrates the schema, and
// exposes a shared mutex that callers serialize writes on (WFP net-event
// callbacks fire from multiple threads).
#pragma once

#include "sqlite3.h"

#include <mutex>
#include <string>

namespace ng {

// Bind a std::string to a statement parameter (copying, so temporaries are safe).
inline void bindText(sqlite3_stmt* s, int i, const std::string& v) {
    sqlite3_bind_text(s, i, v.c_str(), (int)v.size(), SQLITE_TRANSIENT);
}

class Db {
public:
    ~Db() { close(); }

    bool open(const char* path);   // open + create/migrate schema
    void close();

    sqlite3* handle() const { return db_; }
    std::mutex& mutex() { return mutex_; }

private:
    sqlite3* db_ = nullptr;
    std::mutex mutex_;
};

}  // namespace ng
