#include "ngd/enforce.h"

#include "common/wfp_util.h"
#include "core/db.h"
#include "core/dns.h"
#include "core/enforcer.h"
#include "core/habit.h"
#include "core/identity.h"
#include "core/prompt.h"
#include "core/util.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace ng {
namespace {

void CALLBACK EventThunk(void* ctx, const FWPM_NET_EVENT5* ev) {
    if (ctx && ev) static_cast<EnforceDaemon*>(ctx)->handleEvent(ev);
}

// A remote we should NOT prompt for: loopback/private/link-local/multicast, or
// any IPv6 (default-deny is IPv4-only for now, so v6 drops aren't ours).
bool IsExemptRemote(const std::string& r) {
    if (r.empty() || r == "::" || r == "::1") return true;
    in_addr a{};
    if (InetPtonA(AF_INET, r.c_str(), &a) != 1) return true;   // not IPv4 -> exempt
    uint32_t ip = ntohl(a.s_addr);
    uint8_t o1 = (uint8_t)(ip >> 24), o2 = (uint8_t)(ip >> 16);
    if (o1 == 0 || o1 == 127 || o1 == 10) return true;
    if (o1 == 192 && o2 == 168) return true;
    if (o1 == 169 && o2 == 254) return true;
    if (o1 == 172 && o2 >= 16 && o2 <= 31) return true;
    if (o1 >= 224) return true;                                 // multicast/reserved
    return false;
}

// The stable-permit baseline query, shared by installBaseline (which installs
// each row as a WFP permit) and PrintBaseline (read-only inspector) so the two
// can never drift. Phase 4d: an (app,port,proto) the ML demoted in active mode
// is excluded here, so it drops to default-deny and prompts next time. This is
// the ONLY place ML scores affect enforcement, and only ever removes a permit
// (never adds a block) - a demoted app still gets the block-notify-retry prompt
// like any other novel connection.
static const char* const kBaselineSQL =
    "SELECT pi.image_path, fe.protocol, fe.remote_port,"
    " COUNT(DISTINCT fe.local_port || '|' || fe.remote_addr) AS conns"
    " FROM flow_events fe JOIN process_identity pi ON fe.image_id = pi.id"
    " WHERE fe.remote_port > 0 AND fe.remote_port < 49152 AND pi.image_path LIKE '_:\\%'"
    "   AND fe.verdict IN ('ALLOW','CAPALLOW')"
    "   AND NOT EXISTS (SELECT 1 FROM ml_flags m WHERE m.kind='demote'"
    "     AND m.app_path = pi.image_path AND m.remote_port = fe.remote_port"
    "     AND m.protocol = fe.protocol)"
    " GROUP BY pi.image_path, fe.protocol, fe.remote_port"
    " HAVING conns >= 3;";

}  // namespace

int EnforceDaemon::installBaseline() {
    sqlite3* h = db_.handle();
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(h, kBaselineSQL, -1, &s, nullptr);
    int permits = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char* path = (const char*)sqlite3_column_text(s, 0);
        int proto = sqlite3_column_int(s, 1);
        int port = sqlite3_column_int(s, 2);
        if (path && enf_.addPermitAppId(util::Widen(path).c_str(),
                                        (uint16_t)port, (uint8_t)proto))
            ++permits;
    }
    sqlite3_finalize(s);
    return permits;
}

// Every WFP event during enforcement: persist it (live feed + decision history)
// and, if it's a novel public drop, kick off the prompt path.
void EnforceDaemon::handleEvent(const void* evp) {
    recordEvent(evp);
    handleDrop(evp);
}

// Persist one event to flow_events and fold it into the learned baseline - the
// same write the recorder does, so enforcement keeps learning and the dashboard
// Live tab streams while enforcing.
void EnforceDaemon::recordEvent(const void* evp) {
    const FWPM_NET_EVENT5* ev = static_cast<const FWPM_NET_EVENT5*>(evp);
    const FWPM_NET_EVENT_HEADER3* h = &ev->header;

    const bool hasProto = (h->flags & FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET) != 0;
    const bool hasLPort = (h->flags & FWPM_NET_EVENT_FLAG_LOCAL_PORT_SET) != 0;
    const bool hasRPort = (h->flags & FWPM_NET_EVENT_FLAG_REMOTE_PORT_SET) != 0;

    const char* verdict = ngwfp::TypeName(ev->type);
    std::string ts     = util::IsoTime(h->timeStamp);
    std::string local  = ngwfp::IpToStr(h, false);
    std::string remote = ngwfp::IpToStr(h, true);
    std::string app    = ngwfp::AppIdToStr(&h->appId);
    std::string sid    = ngwfp::UserSid(h);

    Identity idn = app.empty() ? Identity{} : id_.resolve(app);
    std::string domain = remote.empty() ? "" : dns_.lookup(remote);

    {
        std::lock_guard<std::mutex> lk(db_.mutex());
        sqlite3_stmt* ins = static_cast<sqlite3_stmt*>(insStmt_);
        if (!ins) return;
        sqlite3_reset(ins);
        sqlite3_clear_bindings(ins);
        bindText(ins, 1, ts);
        bindText(ins, 2, verdict);
        if (hasProto) sqlite3_bind_int(ins, 3, h->ipProtocol); else sqlite3_bind_null(ins, 3);
        bindText(ins, 4, local);
        if (hasLPort) sqlite3_bind_int(ins, 5, h->localPort); else sqlite3_bind_null(ins, 5);
        bindText(ins, 6, remote);
        if (hasRPort) sqlite3_bind_int(ins, 7, h->remotePort); else sqlite3_bind_null(ins, 7);
        bindText(ins, 8, app);
        bindText(ins, 9, sid);
        if (idn.id >= 0) sqlite3_bind_int64(ins, 10, idn.id); else sqlite3_bind_null(ins, 10);
        if (domain.empty()) sqlite3_bind_null(ins, 11); else bindText(ins, 11, domain);
        sqlite3_step(ins);
    }

    const bool isClassify = (strcmp(verdict, "ALLOW") == 0 || strcmp(verdict, "DROP") == 0);
    const bool isLoopback = remote.rfind("127.", 0) == 0 || remote == "::1";
    const bool realRemote = hasRPort && h->remotePort > 0 && h->remotePort < 49152 &&
                            !remote.empty() && remote != "0.0.0.0" && remote != "::" && !isLoopback;
    if (isClassify && realRemote && idn.id >= 0) {
        std::string dest = domain.empty() ? remote : domain;
        SYSTEMTIME st{}; FileTimeToSystemTime(&h->timeStamp, &st);
        std::string token = local + "|" + std::to_string(hasLPort ? h->localPort : 0) + "|" +
                            remote + "|" + std::to_string(h->remotePort) + "|" +
                            std::to_string(h->ipProtocol);
        habits_.observe(idn.key, idn.label, dest, h->remotePort, h->ipProtocol,
                        ts, util::UnixEpoch(h->timeStamp), st.wHour, st.wDayOfWeek, token);
    }
}

void EnforceDaemon::handleDrop(const void* evp) {
    const FWPM_NET_EVENT5* ev = static_cast<const FWPM_NET_EVENT5*>(evp);
    if (ev->type != FWPM_NET_EVENT_TYPE_CLASSIFY_DROP) return;
    const FWPM_NET_EVENT_HEADER3* h = &ev->header;
    if (!(h->flags & FWPM_NET_EVENT_FLAG_APP_ID_SET)) return;
    if (!(h->flags & FWPM_NET_EVENT_FLAG_REMOTE_PORT_SET)) return;

    int port = h->remotePort;
    if (port <= 0 || port >= 49152) return;             // ephemeral -> inbound peer
    std::string remote = ngwfp::IpToStr(h, true);
    if (IsExemptRemote(remote)) return;                 // only novel public outbound
    std::string dev = ngwfp::AppIdToStr(&h->appId);
    if (dev.empty()) return;

    std::string dest = dns_.lookup(remote);
    if (dest.empty()) dest = remote;
    std::string key = dev + "|" + dest + "|" + std::to_string(port);

    {
        std::lock_guard<std::mutex> lk(qmx_);
        if (handled_.count(key)) return;                // one prompt per (app,dest,port)
        handled_.insert(key);
        queue_.push_back({dev, dest, port});
    }
    qcv_.notify_one();
}

void EnforceDaemon::worker() {
    for (;;) {
        Req req;
        {
            std::unique_lock<std::mutex> lk(qmx_);
            qcv_.wait(lk, [&] { return stop_.load() || !queue_.empty(); });
            if (queue_.empty()) { if (stop_) return; else continue; }
            req = queue_.front();
            queue_.pop_front();
        }
        Identity idn = id_.resolve(req.devPath);
        std::string label = idn.label.empty() ? req.devPath : idn.label;
        // Autonomy: skip the prompt when the policy says to auto-allow.
        int autonomy = readAutonomy();
        bool autoAllow = autonomy >= 2 || (autonomy == 1 && appKnown(idn.key));
        char d = autoAllow ? 'A' : PromptTray(label, req.dest, req.port);   // may block on the user
        if (d == 'A' || d == 'O') {
            if (!idn.path.empty())
                enf_.addPermitAppId(util::Widen(idn.path).c_str(), (uint16_t)req.port, IPPROTO_TCP);
            fprintf(stderr, "[enforce] %s  %s -> %s:%d\n", autoAllow ? "AUTO-ALLOW" : "ALLOW",
                    label.c_str(), req.dest.c_str(), req.port);
        } else if (d == 0) {
            fprintf(stderr, "[enforce] (tray unreachable) %s -> %s:%d stays blocked\n",
                    label.c_str(), req.dest.c_str(), req.port);
        } else {
            fprintf(stderr, "[enforce] BLOCK  %s -> %s:%d\n", label.c_str(), req.dest.c_str(), req.port);
        }
    }
}

long long EnforceDaemon::readRulesGen() {
    std::lock_guard<std::mutex> lk(db_.mutex());
    sqlite3_stmt* s = nullptr; long long g = 0;
    sqlite3_prepare_v2(db_.handle(), "SELECT v FROM meta WHERE k='rules_gen';", -1, &s, nullptr);
    if (sqlite3_step(s) == SQLITE_ROW) g = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return g;
}

int EnforceDaemon::readAutonomy() {
    std::lock_guard<std::mutex> lk(db_.mutex());
    sqlite3_stmt* s = nullptr; int v = 0;
    sqlite3_prepare_v2(db_.handle(), "SELECT v FROM meta WHERE k='autonomy';", -1, &s, nullptr);
    if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return v;
}

bool EnforceDaemon::appKnown(const std::string& key) {
    if (key.empty()) return false;
    std::lock_guard<std::mutex> lk(db_.mutex());
    sqlite3_stmt* s = nullptr; int n = 0;
    sqlite3_prepare_v2(db_.handle(), "SELECT count(*) FROM habits WHERE process_key=?;", -1, &s, nullptr);
    bindText(s, 1, key);
    if (sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return n > 0;
}

// Read enabled, unexpired rows from the rules table and install each as a WFP
// filter. Rows are collected under the DB lock, then applied without it (the WFP
// calls mustn't stall the recorder callback). Tracks the soonest future expiry
// so the run loop can re-apply when a timed allow lapses.
int EnforceDaemon::applyRules() {
    struct R { bool block; std::string app; uint32_t ip; uint16_t port; uint8_t proto; };
    std::vector<R> rules;
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    double now = util::UnixEpoch(ft);
    nextExpiry_ = 0;
    {
        std::lock_guard<std::mutex> lk(db_.mutex());
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db_.handle(),
            "SELECT action, app_path, remote_addr, remote_port, protocol, expires_epoch"
            " FROM rules WHERE enabled=1;", -1, &s, nullptr);
        while (s && sqlite3_step(s) == SQLITE_ROW) {
            const char* action = (const char*)sqlite3_column_text(s, 0);
            const char* appPath = (const char*)sqlite3_column_text(s, 1);
            const char* remote = (const char*)sqlite3_column_text(s, 2);
            double expires = sqlite3_column_type(s, 5) == SQLITE_NULL ? 0 : sqlite3_column_double(s, 5);
            if (expires > 0 && expires <= now) continue;   // lapsed timed allow -> skip
            if (expires > now && (nextExpiry_ == 0 || expires < nextExpiry_)) nextExpiry_ = expires;
            R r{};
            r.block = action && strcmp(action, "block") == 0;
            r.app = appPath ? appPath : "";
            if (remote && *remote) {
                in_addr a{};
                if (InetPtonA(AF_INET, remote, &a) == 1) r.ip = ntohl(a.s_addr);
            }
            r.port = (uint16_t)sqlite3_column_int(s, 3);
            r.proto = (uint8_t)sqlite3_column_int(s, 4);
            rules.push_back(r);
        }
        sqlite3_finalize(s);
    }
    int n = 0;
    for (const R& r : rules) {
        std::wstring appW = r.app.empty() ? std::wstring() : util::Widen(r.app);
        if (enf_.applyUserRule(appW.empty() ? nullptr : appW.c_str(), r.ip, r.port, r.proto, r.block))
            ++n;
    }
    return n;
}

// Live re-apply after a rule edit or a timed-allow expiry: strip our filters
// (keeping the sublayer) and reinstall baseline + default-deny + user rules.
// The brief window between clear and reinstall fails open, which is the safe
// direction. Session permits granted via prompts are re-derived from the
// baseline where they became habits.
void EnforceDaemon::reapply() {
    enf_.clearFilters();
    int permits = installBaseline();
    enf_.enableDefaultDeny();
    int nrules = applyRules();
    printf("ngd enforce: re-applied (%d baseline permits, %d user rules).\n", permits, nrules);
    fflush(stdout);
}

void EnforceDaemon::stop() {
    stop_ = true;
    qcv_.notify_all();
    if (stopEvent_) SetEvent((HANDLE)stopEvent_);
}

bool EnforceDaemon::run(int seconds) {
    if (!enf_.open()) return false;

    // Prepare the flow_events insert so enforcement also records the live feed.
    sqlite3_stmt* ins = nullptr;
    if (sqlite3_prepare_v2(db_.handle(),
            "INSERT INTO flow_events"
            "(ts_utc,verdict,protocol,local_addr,local_port,remote_addr,remote_port,image_path,user_sid,image_id,remote_domain)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,?);", -1, &ins, nullptr) != SQLITE_OK) {
        fprintf(stderr, "enforce: prepare insert failed: %s\n", sqlite3_errmsg(db_.handle()));
        enf_.panic(); return false;
    }
    insStmt_ = ins;

    int permits = installBaseline();
    if (!enf_.enableDefaultDeny()) { enf_.panic(); return false; }
    printf("ngd enforce: %d stable permits + default-deny active.%s\n", permits,
           seconds > 0 ? " (timed)" : " Ctrl+C to revert.");
    fflush(stdout);

    WSADATA wsa{}; WSAStartup(MAKEWORD(2, 2), &wsa);
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    worker_ = std::thread([this] { worker(); });

    // Separate engine handle for the drop subscription.
    FWPM_SESSION0 session{};
    session.displayData.name = const_cast<wchar_t*>(L"ngd-enforce");
    HANDLE engine = nullptr;
    DWORD e = FwpmEngineOpen0(nullptr, RPC_C_AUTHN_WINNT, nullptr, &session, &engine);
    if (e != ERROR_SUCCESS) {
        fprintf(stderr, "FwpmEngineOpen0 (enforce sub) failed: 0x%08lX\n", e);
        stop(); if (worker_.joinable()) worker_.join(); enf_.panic(); return false;
    }
    auto setU32 = [&](FWPM_ENGINE_OPTION opt, UINT32 v) {
        FWP_VALUE0 val{}; val.type = FWP_UINT32; val.uint32 = v;
        FwpmEngineSetOption0(engine, opt, &val);
    };
    setU32(FWPM_ENGINE_COLLECT_NET_EVENTS, 1);
    setU32(FWPM_ENGINE_NET_EVENT_MATCH_ANY_KEYWORDS,
           FWPM_NET_EVENT_KEYWORD_CLASSIFY_ALLOW | FWPM_NET_EVENT_KEYWORD_CAPABILITY_DROP |
           FWPM_NET_EVENT_KEYWORD_CAPABILITY_ALLOW | FWPM_NET_EVENT_KEYWORD_PORT_SCANNING_DROP);

    FWPM_NET_EVENT_SUBSCRIPTION0 sub{};
    sub.enumTemplate = nullptr;
    HANDLE subH = nullptr;
    e = FwpmNetEventSubscribe4(engine, &sub, EventThunk, this, &subH);
    if (e != ERROR_SUCCESS) {
        fprintf(stderr, "FwpmNetEventSubscribe4 (enforce) failed: 0x%08lX\n", e);
        FwpmEngineClose0(engine);
        stop(); if (worker_.joinable()) worker_.join(); enf_.panic(); return false;
    }

    // Apply user-editable rules on top of the baseline, then watch for edits
    // (meta.rules_gen bump) and timed-allow expiries, re-applying live.
    int nrules = applyRules();
    printf("ngd enforce: applied %d user rule(s).\n", nrules);
    fflush(stdout);
    long long gen = readRulesGen();
    DWORD startTick = GetTickCount();
    for (;;) {
        if (WaitForSingleObject((HANDLE)stopEvent_, 2000) == WAIT_OBJECT_0) break;
        if (seconds > 0 && GetTickCount() - startTick >= (DWORD)seconds * 1000) break;
        FILETIME ft; GetSystemTimeAsFileTime(&ft);
        bool expired = nextExpiry_ > 0 && util::UnixEpoch(ft) >= nextExpiry_;
        long long g = readRulesGen();
        if (g != gen || expired) { reapply(); gen = readRulesGen(); }
    }

    stop_ = true;
    qcv_.notify_all();
    if (worker_.joinable()) worker_.join();
    FwpmNetEventUnsubscribe0(engine, subH);
    FwpmEngineClose0(engine);
    {
        std::lock_guard<std::mutex> lk(db_.mutex());
        sqlite3_finalize(ins);
        insStmt_ = nullptr;
    }
    int removed = enf_.panic();
    WSACleanup();
    printf("\nngd enforce: reverted (removed %d filters).\n", removed);
    return true;
}

int PrintBaseline(Db& db) {
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db.handle(), kBaselineSQL, -1, &s, nullptr) != SQLITE_OK) {
        fprintf(stderr, "baseline query failed: %s\n", sqlite3_errmsg(db.handle()));
        return -1;
    }
    printf("  %-5s %6s %6s  %s\n", "proto", "port", "conns", "app");
    int n = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char* path = (const char*)sqlite3_column_text(s, 0);
        printf("  %-5d %6d %6d  %s\n", sqlite3_column_int(s, 1), sqlite3_column_int(s, 2),
               sqlite3_column_int(s, 3), path ? path : "");
        ++n;
    }
    sqlite3_finalize(s);
    printf("\n%d stable permit(s) would be installed (Phase 4d demotions excluded).\n", n);
    return n;
}

}  // namespace ng
