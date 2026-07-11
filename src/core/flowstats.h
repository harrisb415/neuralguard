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

#include "core/scorer.h"

#include <atomic>
#include <string>

namespace ng {

class Db;
class IdentityResolver;
class DnsWatcher;

class FlowCollector {
public:
    // `dns` is optional: when the collector runs alongside the enforce/record
    // daemon it correlates each flow's remote IP to the resolved domain; when
    // run standalone (`ngd features`) it's null and the dest stays the raw IP.
    FlowCollector(Db& db, IdentityResolver& id, DnsWatcher* dns = nullptr)
        : db_(db), id_(id), dns_(dns) {}

    // Poll loop. seconds == 0 runs until stop()/Ctrl-C. Auto-purges old rows
    // at start. Only genuinely-closed connections are written, so the cleanest
    // data comes from flows observed start-to-finish (continuous running).
    // Writes are serialized on Db::mutex(), so this is safe to run on its own
    // thread beside the recorder (which also writes from WFP callback threads).
    bool run(int seconds);
    void stop();
    unsigned long long written() const { return written_.load(); }

    // Enable scoring: each completed flow is scored with the anomaly model and/or
    // the supervised classifier, and the scores stored. A no-op for whichever
    // model (or onnxruntime.dll) is missing. When `active` is true (ml_mode=active)
    // scores over their confidence gates also write ml_flags (demote / review) -
    // the only path where a score touches enforcement. Call before run().
    void enableScoring(const std::string& anomalyPath, const std::string& supervisedPath, bool active) {
        anomalyPath_ = anomalyPath;
        supervisedPath_ = supervisedPath;
        active_ = active;
    }

private:
    Db& db_;
    IdentityResolver& id_;
    DnsWatcher* dns_ = nullptr;
    std::string anomalyPath_, supervisedPath_;
    bool active_ = false;
    OnnxModel anomaly_, supervised_;
    void* stopEvent_ = nullptr;
    std::atomic<unsigned long long> written_{ 0 };
};

// Delete flow_features rows older than `days`. Returns rows removed (-1 on error).
long long PurgeFlowFeatures(Db& db, int days);

}  // namespace ng
