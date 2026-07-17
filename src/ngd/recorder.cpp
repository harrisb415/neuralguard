#include "ngd/recorder.h"

#include "common/wfp_util.h"
#include "core/db.h"
#include "core/dns.h"
#include "core/habit.h"
#include "core/identity.h"
#include "core/util.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace ng {

namespace {
// WFP calls this from its own threads; context is the Recorder instance.
void CALLBACK NetEventThunk(void* context, const FWPM_NET_EVENT5* ev) {
    if (context && ev) static_cast<Recorder*>(context)->handleEvent(ev);
}
}  // namespace

void Recorder::handleEvent(const void* evOpaque) {
    const FWPM_NET_EVENT5* ev = static_cast<const FWPM_NET_EVENT5*>(evOpaque);
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

    // Direction comes from the ALE layer the event was classified at (the layer
    // IS the direction) - not a port-number guess. Unknown for non-ALE events
    // (transport/capability/...), which are logged but never fold into a baseline.
    ngwfp::Dir dir = ngwfp::DirectionOf(ev, aleConnV4_, aleConnV6_, aleAcceptV4_, aleAcceptV6_);
    const char* dirStr = (dir == ngwfp::Dir::In) ? "in"
                       : (dir == ngwfp::Dir::Out) ? "out" : nullptr;

    // Dedup key = the flow's identity + its verdict. A rapid repeat of the exact
    // same thing is what the coalescer suppresses; direction/app are derived from
    // these fields, so they don't need to be in the key.
    std::string dkey = std::string(verdict) + "|" + local + ":" +
                       std::to_string(hasLPort ? h->localPort : 0) + "|" + remote + ":" +
                       std::to_string(hasRPort ? h->remotePort : 0) + "|" +
                       std::to_string(hasProto ? h->ipProtocol : 0);

    {
        std::lock_guard<std::mutex> lk(db_.mutex());
        // Suppress rapid identical repeats: keeps the raw log from ballooning with
        // retry-loop / multicast spam. Habit learning below still runs on every
        // event (it dedups by 5-tuple itself), so this loses only redundant rows.
        if (coalescer_.shouldRecord(dkey, GetTickCount64())) {
            sqlite3_stmt* ins = static_cast<sqlite3_stmt*>(insStmt_);
            if (ins) {
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
                if (dirStr) sqlite3_bind_text(ins, 12, dirStr, -1, SQLITE_STATIC);
                else        sqlite3_bind_null(ins, 12);
                if (sqlite3_step(ins) == SQLITE_DONE) {
                    ++count_;
                    // Fold this logged event into the per-app rollup (still under
                    // the db lock). blocked = the Per-app view's predicate.
                    const bool blocked = std::strstr(verdict, "DROP") || std::strcmp(verdict, "BLOCK") == 0;
                    db_.recordAppStat(idn.id, blocked, remote);
                }
            }
        }
    }

    // Fold genuine connection establishments into the learned baseline. Only ALE
    // connect/accept events count; Unknown is skipped.
    const bool isLoopback = remote.rfind("127.", 0) == 0 || remote == "::1" ||
                            local.rfind("127.", 0) == 0 || local == "::1";
    // Service port = remote for outbound (where it connects), local for inbound
    // (the port it listens on). The inbound peer varies, so it isn't part of the
    // identity - inbound habits key on (app, local service port) with dest="".
    const bool inbound = (dir == ngwfp::Dir::In);
    const int  svcPort = inbound ? (hasLPort ? h->localPort : 0)
                                 : (hasRPort ? h->remotePort : 0);
    const std::string dest = inbound ? std::string()
                                     : (domain.empty() ? remote : domain);
    const bool realRemote = inbound ? true
                                    : (!remote.empty() && remote != "0.0.0.0" && remote != "::");
    if (dirStr && svcPort > 0 && idn.id >= 0 && !isLoopback && realRemote) {
        SYSTEMTIME st{}; FileTimeToSystemTime(&h->timeStamp, &st);
        std::string token = std::string(dirStr) + "|" + local + "|" +
                            std::to_string(hasLPort ? h->localPort : 0) + "|" + remote + "|" +
                            std::to_string(hasRPort ? h->remotePort : 0) + "|" +
                            std::to_string(h->ipProtocol);
        habits_.observe(idn.key, idn.label, dest, svcPort, h->ipProtocol, dirStr,
                        ts, util::UnixEpoch(h->timeStamp), st.wHour, st.wDayOfWeek, token);
    }
}

void Recorder::stop() {
    if (stopEvent_) SetEvent((HANDLE)stopEvent_);
}

bool Recorder::run() {
    // Enforce the flow_events retention window on startup (the recorder is the
    // main writer, so this is where the raw log gets bounded). Cheap via
    // idx_flow_events_ts. Same shape as FlowCollector's flow_features purge.
    long long purged = db_.purgeFlowEvents(ng::kFlowEventsRetentionDays);
    if (purged > 0)
        printf("purged %lld flow_events row(s) older than %d days\n", purged, ng::kFlowEventsRetentionDays);
    // Rebuild the per-app rollup from the (now retention-trimmed) log, so it
    // starts consistent with what's actually retained; recordAppStat keeps it
    // current from here.
    db_.rebuildAppStats();

    sqlite3_stmt* ins = nullptr;
    if (sqlite3_prepare_v2(db_.handle(),
            "INSERT INTO flow_events"
            "(ts_utc,verdict,protocol,local_addr,local_port,remote_addr,remote_port,image_path,user_sid,image_id,remote_domain,direction)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,?,?);",
            -1, &ins, nullptr) != SQLITE_OK) {
        fprintf(stderr, "prepare insert failed: %s\n", sqlite3_errmsg(db_.handle()));
        return false;
    }
    insStmt_ = ins;

    WSADATA wsa{}; WSAStartup(MAKEWORD(2, 2), &wsa);
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    FWPM_SESSION0 session{};
    session.flags = 0;  // NOT dynamic: engine options can't be set from a dynamic session
    session.displayData.name = const_cast<wchar_t*>(L"ngd");

    HANDLE engine = nullptr;
    DWORD err = FwpmEngineOpen0(nullptr, RPC_C_AUTHN_WINNT, nullptr, &session, &engine);
    if (err != ERROR_SUCCESS) {
        fprintf(stderr, "FwpmEngineOpen0 failed: 0x%08lX (are you elevated?)\n", err);
        return false;
    }
    // Resolve the ALE layer ids so handleEvent can attribute direction from the
    // event's layerId (the layer is the direction) instead of guessing from ports.
    ngwfp::ResolveAleLayers(engine, aleConnV4_, aleConnV6_, aleAcceptV4_, aleAcceptV6_);

    auto setU32 = [&](FWPM_ENGINE_OPTION opt, UINT32 val) {
        FWP_VALUE0 v{}; v.type = FWP_UINT32; v.uint32 = val;
        FwpmEngineSetOption0(engine, opt, &v);
    };
    setU32(FWPM_ENGINE_COLLECT_NET_EVENTS, 1);
    setU32(FWPM_ENGINE_NET_EVENT_MATCH_ANY_KEYWORDS,
           FWPM_NET_EVENT_KEYWORD_CLASSIFY_ALLOW |
           FWPM_NET_EVENT_KEYWORD_CAPABILITY_ALLOW |
           FWPM_NET_EVENT_KEYWORD_CAPABILITY_DROP |
           FWPM_NET_EVENT_KEYWORD_INBOUND_MCAST |
           FWPM_NET_EVENT_KEYWORD_INBOUND_BCAST |
           FWPM_NET_EVENT_KEYWORD_PORT_SCANNING_DROP);

    FWPM_NET_EVENT_SUBSCRIPTION0 sub{};
    sub.enumTemplate = nullptr;
    HANDLE subHandle = nullptr;
    err = FwpmNetEventSubscribe4(engine, &sub, NetEventThunk, this, &subHandle);
    if (err != ERROR_SUCCESS) {
        fprintf(stderr, "FwpmNetEventSubscribe4 failed: 0x%08lX\n", err);
        FwpmEngineClose0(engine);
        return false;
    }

    fflush(stdout);
    WaitForSingleObject((HANDLE)stopEvent_, INFINITE);

    {
        std::lock_guard<std::mutex> lk(db_.mutex());
        FwpmNetEventUnsubscribe0(engine, subHandle);
        FwpmEngineClose0(engine);
        sqlite3_finalize(ins);
        insStmt_ = nullptr;
    }
    printf("\nStopped. %llu events recorded.\n", count_.load());
    WSACleanup();
    return true;
}

}  // namespace ng
