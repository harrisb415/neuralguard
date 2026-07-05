#include "ngd/enforce.h"

#include "common/wfp_util.h"
#include "core/db.h"
#include "core/dns.h"
#include "core/enforcer.h"
#include "core/identity.h"
#include "core/prompt.h"
#include "core/util.h"

#include <cstdio>

namespace ng {
namespace {

void CALLBACK DropThunk(void* ctx, const FWPM_NET_EVENT5* ev) {
    if (ctx && ev) static_cast<EnforceDaemon*>(ctx)->handleDrop(ev);
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

}  // namespace

int EnforceDaemon::installBaseline() {
    sqlite3* h = db_.handle();
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(h,
        "SELECT pi.image_path, fe.protocol, fe.remote_port,"
        " COUNT(DISTINCT fe.local_port || '|' || fe.remote_addr) AS conns"
        " FROM flow_events fe JOIN process_identity pi ON fe.image_id = pi.id"
        " WHERE fe.remote_port > 0 AND fe.remote_port < 49152 AND pi.image_path LIKE '_:\\%'"
        "   AND fe.verdict IN ('ALLOW','CAPALLOW')"
        " GROUP BY pi.image_path, fe.protocol, fe.remote_port"
        " HAVING conns >= 3;", -1, &s, nullptr);
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
        char d = PromptTray(label, req.dest, req.port);   // blocks on the user
        if (d == 'A' || d == 'O') {
            if (!idn.path.empty())
                enf_.addPermitAppId(util::Widen(idn.path).c_str(), (uint16_t)req.port, IPPROTO_TCP);
            fprintf(stderr, "[enforce] ALLOW  %s -> %s:%d\n", label.c_str(), req.dest.c_str(), req.port);
        } else if (d == 0) {
            fprintf(stderr, "[enforce] (tray unreachable) %s -> %s:%d stays blocked\n",
                    label.c_str(), req.dest.c_str(), req.port);
        } else {
            fprintf(stderr, "[enforce] BLOCK  %s -> %s:%d\n", label.c_str(), req.dest.c_str(), req.port);
        }
    }
}

void EnforceDaemon::stop() {
    stop_ = true;
    qcv_.notify_all();
    if (stopEvent_) SetEvent((HANDLE)stopEvent_);
}

bool EnforceDaemon::run(int seconds) {
    if (!enf_.open()) return false;
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
    e = FwpmNetEventSubscribe4(engine, &sub, DropThunk, this, &subH);
    if (e != ERROR_SUCCESS) {
        fprintf(stderr, "FwpmNetEventSubscribe4 (enforce) failed: 0x%08lX\n", e);
        FwpmEngineClose0(engine);
        stop(); if (worker_.joinable()) worker_.join(); enf_.panic(); return false;
    }

    WaitForSingleObject((HANDLE)stopEvent_, seconds > 0 ? (DWORD)seconds * 1000 : INFINITE);

    stop_ = true;
    qcv_.notify_all();
    if (worker_.joinable()) worker_.join();
    FwpmNetEventUnsubscribe0(engine, subH);
    FwpmEngineClose0(engine);
    int removed = enf_.panic();
    WSACleanup();
    printf("\nngd enforce: reverted (removed %d filters).\n", removed);
    return true;
}

}  // namespace ng
