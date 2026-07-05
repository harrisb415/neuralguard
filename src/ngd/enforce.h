// EnforceDaemon (Phase 2c / block-notify-retry, automatic): turns ngd into the
// live enforcer. It installs the stable baseline permits + default-deny, then
// watches WFP drop events; when a novel public connection is blocked it prompts
// the tray (off the callback thread) and, on Allow, adds a permit so the app's
// retry succeeds. Reverts (panic) on stop.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

namespace ng {

class Db;
class IdentityResolver;
class DnsWatcher;
class Enforcer;

class EnforceDaemon {
public:
    EnforceDaemon(Db& db, IdentityResolver& id, DnsWatcher& dns, Enforcer& enf)
        : db_(db), id_(id), dns_(dns), enf_(enf) {}

    bool run(int seconds);   // install, subscribe, block until stop()/timeout, revert
    void stop();

    void handleDrop(const void* ev);   // WFP callback; ev is const FWPM_NET_EVENT5*

private:
    int  installBaseline();  // permit stable (app, port) pairs; returns count
    void worker();           // drains the prompt queue (blocking prompts here)

    struct Req { std::string devPath, dest; int port; };

    Db& db_;
    IdentityResolver& id_;
    DnsWatcher& dns_;
    Enforcer& enf_;

    std::mutex qmx_;
    std::condition_variable qcv_;
    std::deque<Req> queue_;
    std::unordered_set<std::string> handled_;   // dedup (app|dest|port)

    std::atomic<bool> stop_{false};
    void* stopEvent_ = nullptr;   // HANDLE
    std::thread worker_;
};

}  // namespace ng
