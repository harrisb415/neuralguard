// Recorder: opens the WFP engine, enables net-event collection, subscribes, and
// writes each event (attributed via IdentityResolver) into the database.
#pragma once

#include <atomic>

namespace ng {

class Db;
class IdentityResolver;

class Recorder {
public:
    Recorder(Db& db, IdentityResolver& id) : db_(db), id_(id) {}

    bool run();   // subscribe and block until stop(); false on setup failure
    void stop();  // signal run() to return (called from a console Ctrl handler)

    // Called by the WFP callback thunk; `ev` is a const FWPM_NET_EVENT5*.
    void handleEvent(const void* ev);

private:
    Db& db_;
    IdentityResolver& id_;
    void* stopEvent_ = nullptr;   // HANDLE
    void* insStmt_   = nullptr;   // sqlite3_stmt*
    std::atomic<unsigned long long> count_{0};
};

}  // namespace ng
