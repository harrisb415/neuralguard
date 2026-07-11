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
class HabitTracker;

// Read-only inspector: prints the stable (app, port) permits `ngd enforce` would
// install right now - the same query, including Phase 4d demotion exclusions -
// without opening WFP or needing admin. Returns the permit count. Used by
// `ngd baseline` to see the effect of ML demotions without enforcing.
int PrintBaseline(Db& db);

class EnforceDaemon {
public:
    EnforceDaemon(Db& db, IdentityResolver& id, DnsWatcher& dns, Enforcer& enf,
                  HabitTracker& habits)
        : db_(db), id_(id), dns_(dns), enf_(enf), habits_(habits) {}

    bool run(int seconds);   // install, subscribe, block until stop()/timeout, revert
    void stop();

    void handleEvent(const void* ev);  // WFP callback: record every event, then dispatch
    void handleDrop(const void* ev);   // novel-drop -> prompt path

private:
    int  installBaseline();  // permit stable (app, port) pairs; returns count
    int  applyRules();       // apply enabled, unexpired rows from the rules table
    void reapply();          // clear + reinstall baseline + default-deny + rules (live edit)
    long long readRulesGen();// meta('rules_gen'), bumped by the dashboard on edit
    int  readAutonomy();     // meta('autonomy'): 0 prompt, 1 auto-allow known, 2 auto-allow all
    bool appKnown(const std::string& key);  // app already has a learned habit
    void recordEvent(const void* ev);  // persist to flow_events + update habits (live feed)
    void worker();           // drains the prompt queue (blocking prompts here)

    struct Req { std::string devPath, dest; int port; };

    Db& db_;
    IdentityResolver& id_;
    DnsWatcher& dns_;
    Enforcer& enf_;
    HabitTracker& habits_;
    void* insStmt_ = nullptr;   // sqlite3_stmt* for the flow_events insert

    std::mutex qmx_;
    std::condition_variable qcv_;
    std::deque<Req> queue_;
    std::unordered_set<std::string> handled_;   // dedup (app|dest|port)

    std::atomic<bool> stop_{false};
    void* stopEvent_ = nullptr;   // HANDLE
    std::thread worker_;
    double nextExpiry_ = 0;       // soonest future timed-allow expiry (epoch), 0 = none
};

}  // namespace ng
