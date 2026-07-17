// SQLite connection wrapper: owns the handle, creates/migrates the schema, and
// exposes a shared mutex that callers serialize writes on (WFP net-event
// callbacks fire from multiple threads).
#pragma once

#include "sqlite3.h"

#include <mutex>
#include <string>

namespace ng {

// flow_events grows fast - it's one row per WFP net event, system-wide, and a
// chatty machine produces >100k/day. Left unbounded it reaches millions of rows
// and gigabytes, which slows every scan (Per-app, the enforce baseline) and the
// Live view's own writer. This caps the raw log's age. 14 days keeps enough
// history for the baseline (an app must connect >=3 times within the window to
// stay auto-permitted) and weekly patterns, while bounding steady-state size.
// The distinct ML-feature table (flow_features) has its own 30-day retention.
constexpr int kFlowEventsRetentionDays = 14;

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

    // meta(k,v) accessors. Every binary reads or writes these (mode, desired_mode,
    // autonomy, ml_mode, ...) and each had grown its own private copy of the same
    // two statements; this is the one implementation.
    std::string meta(const char* key, const char* dflt = "");
    void setMeta(const char* key, const std::string& val);

    // Delete flow_events older than `days`. Returns rows removed (-1 on error).
    // Cheap thanks to idx_flow_events_ts. Called at record/enforce startup and
    // exposed via `ngd events purge`.
    long long purgeFlowEvents(int days);

    // Per-app rollup (see app_stats/app_dests in the schema).
    // recordAppStat folds one just-logged event into the rollup - CALLER MUST
    // HOLD mutex() (it runs in the recorder's insert critical section, so it does
    // not lock itself). imageId < 0 (unattributed) is ignored.
    void recordAppStat(long long imageId, bool blocked, const std::string& remoteAddr);
    // Rebuild the rollup from the current (retained) flow_events. Locks mutex()
    // itself - called at daemon startup, outside the insert path.
    void rebuildAppStats();

private:
    sqlite3* db_ = nullptr;
    std::mutex mutex_;
};

}  // namespace ng
