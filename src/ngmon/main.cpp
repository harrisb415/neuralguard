// ngmon - NeuralGuard Phase 0 spike.
//
// A user-mode monitor that subscribes to Windows Filtering Platform (WFP) net
// events and prints each connection allow/drop with process attribution. No
// kernel driver, no external dependencies. See docs/ROADMAP.md (Phase 0).
//
// Requires: run elevated. To see ALLOW events (not just drops), enable the
// audit subcategory first:
//   auditpol /set /subcategory:"Filtering Platform Connection" ^
//            /success:enable /failure:enable
//
// Ctrl+C to stop.

#include "common/wfp_util.h"

#include <atomic>
#include <cstdio>
#include <string>

namespace {

std::atomic<bool> g_stop{false};
HANDLE g_stopEvent = nullptr;
unsigned long long g_count = 0;

void CALLBACK OnNetEvent(void* /*context*/, const FWPM_NET_EVENT5* ev) {
    if (!ev) return;
    const FWPM_NET_EVENT_HEADER3* h = &ev->header;

    const bool hasLPort = (h->flags & FWPM_NET_EVENT_FLAG_LOCAL_PORT_SET) != 0;
    const bool hasRPort = (h->flags & FWPM_NET_EVENT_FLAG_REMOTE_PORT_SET) != 0;
    const bool hasProto = (h->flags & FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET) != 0;

    std::string local  = ngwfp::IpToStr(h, false);
    std::string remote = ngwfp::IpToStr(h, true);
    std::string app    = ngwfp::AppIdToStr(&h->appId);
    std::string sid    = ngwfp::UserSid(h);

    SYSTEMTIME st{};
    FileTimeToSystemTime(&h->timeStamp, &st);

    printf("%02d:%02d:%02d.%03d  %-8s %-4s  %s:%u -> %s:%u  [%s]%s%s\n",
           st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
           ngwfp::TypeName(ev->type),
           hasProto ? ngwfp::ProtoName(h->ipProtocol) : "IP",
           local.c_str(),  hasLPort ? h->localPort  : 0,
           remote.c_str(), hasRPort ? h->remotePort : 0,
           app.empty() ? "(no app id)" : app.c_str(),
           sid.empty() ? "" : "  user=", sid.c_str());
    fflush(stdout);
    ++g_count;
}

BOOL WINAPI CtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stop = true;
        if (g_stopEvent) SetEvent(g_stopEvent);
        return TRUE;
    }
    return FALSE;
}

}  // namespace

int main() {
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    FWPM_SESSION0 session{};
    session.flags = 0;  // NOT dynamic (see ngd/main.cpp note re: SetOption)
    session.displayData.name = const_cast<wchar_t*>(L"ngmon");

    HANDLE engine = nullptr;
    DWORD err = FwpmEngineOpen0(nullptr, RPC_C_AUTHN_WINNT, nullptr, &session, &engine);
    if (err != ERROR_SUCCESS) {
        fprintf(stderr, "FwpmEngineOpen0 failed: 0x%08lX (are you elevated?)\n", err);
        return 1;
    }

    auto setU32 = [&](FWPM_ENGINE_OPTION opt, UINT32 val) {
        FWP_VALUE0 v{};
        v.type = FWP_UINT32;
        v.uint32 = val;
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
    sub.enumTemplate = nullptr;  // all net events

    HANDLE subHandle = nullptr;
    err = FwpmNetEventSubscribe4(engine, &sub, OnNetEvent, nullptr, &subHandle);
    if (err != ERROR_SUCCESS) {
        fprintf(stderr, "FwpmNetEventSubscribe4 failed: 0x%08lX\n", err);
        FwpmEngineClose0(engine);
        return 1;
    }

    printf("ngmon - watching WFP net events (Ctrl+C to stop)\n");
    printf("time          verdict  proto local -> remote  [image]\n");
    printf("-----------------------------------------------------------------\n");
    fflush(stdout);

    WaitForSingleObject(g_stopEvent, INFINITE);

    printf("\nStopping... (%llu events observed)\n", g_count);
    FwpmNetEventUnsubscribe0(engine, subHandle);
    FwpmEngineClose0(engine);
    WSACleanup();
    return 0;
}
