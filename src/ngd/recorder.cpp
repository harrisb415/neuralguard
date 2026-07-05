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
        if (sqlite3_step(ins) == SQLITE_DONE) ++count_;
    }

    // Update the learned baseline for real outbound connections. Count only the
    // ALE classify verdicts (not capability/other events) and require a real
    // remote endpoint + a resolved process, to approximate one obs per connection.
    const bool isClassify = (strcmp(verdict, "ALLOW") == 0 || strcmp(verdict, "DROP") == 0);
    // Skip ephemeral remote ports (>= 49152): those are the peer's random port on
    // an INBOUND connection, which would otherwise create one junk habit each. A
    // habit is about an outbound *service* port. Also skip loopback destinations -
    // internal IPC (e.g. a browser talking to itself), always Tier-0 exempt anyway.
    const bool isLoopback = remote.rfind("127.", 0) == 0 || remote == "::1";
    const bool realRemote = hasRPort && h->remotePort > 0 && h->remotePort < 49152 &&
                            !remote.empty() && remote != "0.0.0.0" && remote != "::" &&
                            !isLoopback;
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

void Recorder::stop() {
    if (stopEvent_) SetEvent((HANDLE)stopEvent_);
}

bool Recorder::run() {
    sqlite3_stmt* ins = nullptr;
    if (sqlite3_prepare_v2(db_.handle(),
            "INSERT INTO flow_events"
            "(ts_utc,verdict,protocol,local_addr,local_port,remote_addr,remote_port,image_path,user_sid,image_id,remote_domain)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,?);",
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
