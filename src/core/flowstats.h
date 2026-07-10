// Phase 4a - completed-flow feature collection.
//
// Watches live TCP connections (GetTcpTable2) and enables per-connection
// TCP ESTATS data collection so we can read bytes in/out. When a connection
// disappears from the table (= it closed), we write one `flow_features` row:
// the process identity, destination, observed lifetime, and byte counts - the
// metadata feature vector the Phase-4 ML tier scores off the decision path.
//
// This is the data foundation only: it collects features, it does NOT score
// anything. Needs Administrator - enabling ESTATS collection is privileged.
#pragma once

#include <atomic>

namespace ng {

class Db;
class IdentityResolver;

class FlowCollector {
public:
    FlowCollector(Db& db, IdentityResolver& id) : db_(db), id_(id) {}

    // Poll loop. seconds == 0 runs until stop()/Ctrl-C. Auto-purges old rows
    // at start. Only genuinely-closed connections are written, so the cleanest
    // data comes from flows observed start-to-finish (continuous running).
    bool run(int seconds);
    void stop();
    unsigned long long written() const { return written_.load(); }

private:
    Db& db_;
    IdentityResolver& id_;
    void* stopEvent_ = nullptr;
    std::atomic<unsigned long long> written_{ 0 };
};

// Delete flow_features rows older than `days`. Returns rows removed (-1 on error).
long long PurgeFlowFeatures(Db& db, int days);

}  // namespace ng
