#include "core/enforcer.h"

#include <winsock2.h>
#include <windows.h>
#include <fwpmu.h>

#include <cstdio>
#include <cstring>

namespace ng {
namespace {

// NeuralGuard's own WFP provider + sublayer identities.
// {b8d0f1a2-3c4d-4e5f-9a0b-1c2d3e4f5061} / ...5062
const GUID kProviderGuid =
    {0xb8d0f1a2, 0x3c4d, 0x4e5f, {0x9a, 0x0b, 0x1c, 0x2d, 0x3e, 0x4f, 0x50, 0x61}};
const GUID kSubLayerGuid =
    {0xb8d0f1a2, 0x3c4d, 0x4e5f, {0x9a, 0x0b, 0x1c, 0x2d, 0x3e, 0x4f, 0x50, 0x62}};

// The ALE layers we install filters at (outbound connect + inbound accept).
const GUID kLayers[] = {
    FWPM_LAYER_ALE_AUTH_CONNECT_V4,
    FWPM_LAYER_ALE_AUTH_CONNECT_V6,
    FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4,
    FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6,
};

int DeleteOurFiltersInLayer(HANDLE eng, const GUID& layer) {
    HANDLE h = nullptr;
    FWPM_FILTER_ENUM_TEMPLATE0 t{};
    t.layerKey = layer;
    t.enumType = FWP_FILTER_ENUM_OVERLAPPING;
    t.flags = FWP_FILTER_ENUM_FLAG_INCLUDE_BOOTTIME | FWP_FILTER_ENUM_FLAG_INCLUDE_DISABLED;
    t.actionMask = 0xFFFFFFFF;
    if (FwpmFilterCreateEnumHandle0(eng, &t, &h) != ERROR_SUCCESS) return 0;

    int deleted = 0;
    FWPM_FILTER0** arr = nullptr;
    UINT32 n = 0;
    while (FwpmFilterEnum0(eng, h, 64, &arr, &n) == ERROR_SUCCESS && n > 0) {
        for (UINT32 i = 0; i < n; ++i) {
            if (arr[i]->providerKey && IsEqualGUID(*arr[i]->providerKey, kProviderGuid)) {
                if (FwpmFilterDeleteById0(eng, arr[i]->filterId) == ERROR_SUCCESS) ++deleted;
            }
        }
        FwpmFreeMemory0((void**)&arr);
        if (n < 64) break;
    }
    FwpmFilterDestroyEnumHandle0(eng, h);
    return deleted;
}

int CountOurFiltersInLayer(HANDLE eng, const GUID& layer) {
    HANDLE h = nullptr;
    FWPM_FILTER_ENUM_TEMPLATE0 t{};
    t.layerKey = layer;
    t.enumType = FWP_FILTER_ENUM_OVERLAPPING;
    t.flags = FWP_FILTER_ENUM_FLAG_INCLUDE_BOOTTIME | FWP_FILTER_ENUM_FLAG_INCLUDE_DISABLED;
    t.actionMask = 0xFFFFFFFF;
    if (FwpmFilterCreateEnumHandle0(eng, &t, &h) != ERROR_SUCCESS) return 0;

    int count = 0;
    FWPM_FILTER0** arr = nullptr;
    UINT32 n = 0;
    while (FwpmFilterEnum0(eng, h, 64, &arr, &n) == ERROR_SUCCESS && n > 0) {
        for (UINT32 i = 0; i < n; ++i)
            if (arr[i]->providerKey && IsEqualGUID(*arr[i]->providerKey, kProviderGuid)) ++count;
        FwpmFreeMemory0((void**)&arr);
        if (n < 64) break;
    }
    FwpmFilterDestroyEnumHandle0(eng, h);
    return count;
}

}  // namespace

Enforcer::~Enforcer() { close(); }

bool Enforcer::ensureObjects() {
    HANDLE eng = (HANDLE)engine_;

    FWPM_PROVIDER0 provider{};
    provider.providerKey = kProviderGuid;
    provider.displayData.name = const_cast<wchar_t*>(L"NeuralGuard");
    provider.displayData.description = const_cast<wchar_t*>(L"NeuralGuard firewall provider");
    DWORD e = FwpmProviderAdd0(eng, &provider, nullptr);
    if (e != ERROR_SUCCESS && e != FWP_E_ALREADY_EXISTS) {
        fprintf(stderr, "FwpmProviderAdd0 failed: 0x%08lX\n", e);
        return false;
    }

    FWPM_SUBLAYER0 sub{};
    sub.subLayerKey = kSubLayerGuid;
    sub.displayData.name = const_cast<wchar_t*>(L"NeuralGuard");
    sub.providerKey = const_cast<GUID*>(&kProviderGuid);
    sub.weight = 0xFFFF;  // above Windows Defender Firewall's sublayers
    e = FwpmSubLayerAdd0(eng, &sub, nullptr);
    if (e != ERROR_SUCCESS && e != FWP_E_ALREADY_EXISTS) {
        fprintf(stderr, "FwpmSubLayerAdd0 failed: 0x%08lX\n", e);
        return false;
    }
    return true;
}

bool Enforcer::open() {
    HANDLE eng = nullptr;
    // Dynamic session: every object we add (provider, sublayer, filters) is
    // auto-deleted by BFE when this session ends - including if the process
    // dies unexpectedly. That IS our fail-open-on-death guarantee: a crashed
    // enforcer can never leave the machine locked down.
    FWPM_SESSION0 session{};
    session.flags = FWPM_SESSION_FLAG_DYNAMIC;
    session.displayData.name = const_cast<wchar_t*>(L"NeuralGuard enforcer");
    DWORD e = FwpmEngineOpen0(nullptr, RPC_C_AUTHN_WINNT, nullptr, &session, &eng);
    if (e != ERROR_SUCCESS) {
        fprintf(stderr, "FwpmEngineOpen0 failed: 0x%08lX (are you elevated?)\n", e);
        return false;
    }
    engine_ = eng;
    return ensureObjects();
}

void Enforcer::close() {
    if (engine_) { FwpmEngineClose0((HANDLE)engine_); engine_ = nullptr; }
}

bool Enforcer::addRemoteIpv4Rule(uint32_t ipv4Host, uint16_t port, uint8_t proto, bool block) {
    HANDLE eng = (HANDLE)engine_;

    FWPM_FILTER_CONDITION0 conds[3];
    UINT32 nc = 0;
    conds[nc].fieldKey = FWPM_CONDITION_IP_REMOTE_ADDRESS;
    conds[nc].matchType = FWP_MATCH_EQUAL;
    conds[nc].conditionValue.type = FWP_UINT32;
    conds[nc].conditionValue.uint32 = ipv4Host;
    ++nc;
    if (port) {
        conds[nc].fieldKey = FWPM_CONDITION_IP_REMOTE_PORT;
        conds[nc].matchType = FWP_MATCH_EQUAL;
        conds[nc].conditionValue.type = FWP_UINT16;
        conds[nc].conditionValue.uint16 = port;
        ++nc;
    }
    if (proto) {
        conds[nc].fieldKey = FWPM_CONDITION_IP_PROTOCOL;
        conds[nc].matchType = FWP_MATCH_EQUAL;
        conds[nc].conditionValue.type = FWP_UINT8;
        conds[nc].conditionValue.uint8 = proto;
        ++nc;
    }

    FWPM_FILTER0 filter{};
    filter.layerKey = FWPM_LAYER_ALE_AUTH_CONNECT_V4;
    filter.subLayerKey = kSubLayerGuid;
    filter.providerKey = const_cast<GUID*>(&kProviderGuid);
    filter.displayData.name = const_cast<wchar_t*>(block ? L"NeuralGuard block"
                                                         : L"NeuralGuard permit");
    filter.action.type = block ? FWP_ACTION_BLOCK : FWP_ACTION_PERMIT;
    filter.weight.type = FWP_EMPTY;  // let BFE assign a weight
    filter.numFilterConditions = nc;
    filter.filterCondition = conds;

    UINT64 id = 0;
    DWORD e = FwpmFilterAdd0(eng, &filter, nullptr, &id);
    if (e != ERROR_SUCCESS) {
        fprintf(stderr, "FwpmFilterAdd0 failed: 0x%08lX\n", e);
        return false;
    }
    return true;
}

bool Enforcer::addV4(bool block, void* condsV, unsigned nc, unsigned char weight,
                     const wchar_t* name) {
    FWPM_FILTER0 filter{};
    filter.layerKey = FWPM_LAYER_ALE_AUTH_CONNECT_V4;
    filter.subLayerKey = kSubLayerGuid;
    filter.providerKey = const_cast<GUID*>(&kProviderGuid);
    filter.displayData.name = const_cast<wchar_t*>(name);
    filter.action.type = block ? FWP_ACTION_BLOCK : FWP_ACTION_PERMIT;
    filter.weight.type = FWP_UINT8;
    filter.weight.uint8 = weight;
    filter.numFilterConditions = nc;
    filter.filterCondition = static_cast<FWPM_FILTER_CONDITION0*>(condsV);
    UINT64 id = 0;
    DWORD e = FwpmFilterAdd0((HANDLE)engine_, &filter, nullptr, &id);
    if (e != ERROR_SUCCESS) {
        fprintf(stderr, "FwpmFilterAdd0(%ls) failed: 0x%08lX\n", name, e);
        return false;
    }
    return true;
}

bool Enforcer::addV6(bool block, void* condsV, unsigned nc, unsigned char weight,
                     const wchar_t* name) {
    FWPM_FILTER0 filter{};
    filter.layerKey = FWPM_LAYER_ALE_AUTH_CONNECT_V6;
    filter.subLayerKey = kSubLayerGuid;
    filter.providerKey = const_cast<GUID*>(&kProviderGuid);
    filter.displayData.name = const_cast<wchar_t*>(name);
    filter.action.type = block ? FWP_ACTION_BLOCK : FWP_ACTION_PERMIT;
    filter.weight.type = FWP_UINT8;
    filter.weight.uint8 = weight;
    filter.numFilterConditions = nc;
    filter.filterCondition = static_cast<FWPM_FILTER_CONDITION0*>(condsV);
    UINT64 id = 0;
    DWORD e = FwpmFilterAdd0((HANDLE)engine_, &filter, nullptr, &id);
    if (e != ERROR_SUCCESS) {
        fprintf(stderr, "FwpmFilterAdd0 v6(%ls) failed: 0x%08lX\n", name, e);
        return false;
    }
    return true;
}

bool Enforcer::addPermitCidrV4(uint32_t addrHost, uint32_t maskHost) {
    FWP_V4_ADDR_AND_MASK am{}; am.addr = addrHost; am.mask = maskHost;
    FWPM_FILTER_CONDITION0 c{};
    c.fieldKey = FWPM_CONDITION_IP_REMOTE_ADDRESS;
    c.matchType = FWP_MATCH_EQUAL;
    c.conditionValue.type = FWP_V4_ADDR_MASK;
    c.conditionValue.v4AddrMask = &am;
    return addV4(false, &c, 1, 15, L"NeuralGuard exempt (subnet)");
}

bool Enforcer::addPermitRemotePortV4(uint16_t port, uint8_t proto) {
    FWPM_FILTER_CONDITION0 c[2]{};
    c[0].fieldKey = FWPM_CONDITION_IP_PROTOCOL;
    c[0].matchType = FWP_MATCH_EQUAL;
    c[0].conditionValue.type = FWP_UINT8;
    c[0].conditionValue.uint8 = proto;
    c[1].fieldKey = FWPM_CONDITION_IP_REMOTE_PORT;
    c[1].matchType = FWP_MATCH_EQUAL;
    c[1].conditionValue.type = FWP_UINT16;
    c[1].conditionValue.uint16 = port;
    return addV4(false, c, 2, 15, L"NeuralGuard exempt (port)");
}

bool Enforcer::addPermitCidrV6(const unsigned char addr[16], unsigned char prefixLen) {
    FWP_V6_ADDR_AND_MASK am{};
    memcpy(am.addr, addr, 16);
    am.prefixLength = prefixLen;
    FWPM_FILTER_CONDITION0 c{};
    c.fieldKey = FWPM_CONDITION_IP_REMOTE_ADDRESS;
    c.matchType = FWP_MATCH_EQUAL;
    c.conditionValue.type = FWP_V6_ADDR_MASK;
    c.conditionValue.v6AddrMask = &am;
    return addV6(false, &c, 1, 15, L"NeuralGuard exempt v6 (subnet)");
}

bool Enforcer::addPermitRemotePortV6(uint16_t port, uint8_t proto) {
    // Protocol + remote-port conditions are IP-version-agnostic; installing them
    // at CONNECT_V6 exempts the same service over IPv6.
    FWPM_FILTER_CONDITION0 c[2]{};
    c[0].fieldKey = FWPM_CONDITION_IP_PROTOCOL;
    c[0].matchType = FWP_MATCH_EQUAL;
    c[0].conditionValue.type = FWP_UINT8;
    c[0].conditionValue.uint8 = proto;
    c[1].fieldKey = FWPM_CONDITION_IP_REMOTE_PORT;
    c[1].matchType = FWP_MATCH_EQUAL;
    c[1].conditionValue.type = FWP_UINT16;
    c[1].conditionValue.uint16 = port;
    return addV6(false, c, 2, 15, L"NeuralGuard exempt v6 (port)");
}

bool Enforcer::addPermitAppId(const wchar_t* dosPath, uint16_t port, uint8_t proto) {
    FWP_BYTE_BLOB* appId = nullptr;
    if (FwpmGetAppIdFromFileName0(dosPath, &appId) != ERROR_SUCCESS || !appId) return false;

    FWPM_FILTER_CONDITION0 c[3]{};
    UINT32 nc = 0;
    c[nc].fieldKey = FWPM_CONDITION_ALE_APP_ID;
    c[nc].matchType = FWP_MATCH_EQUAL;
    c[nc].conditionValue.type = FWP_BYTE_BLOB_TYPE;
    c[nc].conditionValue.byteBlob = appId;
    ++nc;
    if (proto) {
        c[nc].fieldKey = FWPM_CONDITION_IP_PROTOCOL;
        c[nc].matchType = FWP_MATCH_EQUAL;
        c[nc].conditionValue.type = FWP_UINT8;
        c[nc].conditionValue.uint8 = proto;
        ++nc;
    }
    if (port) {
        c[nc].fieldKey = FWPM_CONDITION_IP_REMOTE_PORT;
        c[nc].matchType = FWP_MATCH_EQUAL;
        c[nc].conditionValue.type = FWP_UINT16;
        c[nc].conditionValue.uint16 = port;
        ++nc;
    }
    // App-id / proto / port conditions are version-agnostic - permit the app over
    // BOTH IPv4 and IPv6, else the v6 default-deny would block a baseline app's
    // IPv6 traffic (a wrong block).
    bool ok = addV4(false, c, nc, 12, L"NeuralGuard baseline permit");
    ok = addV6(false, c, nc, 12, L"NeuralGuard baseline permit (v6)") && ok;
    FwpmFreeMemory0((void**)&appId);
    return ok;
}

bool Enforcer::applyUserRule(const wchar_t* appPath, uint32_t remoteIpv4Host,
                             uint16_t port, uint8_t proto, bool block) {
    FWP_BYTE_BLOB* appId = nullptr;
    if (appPath && *appPath &&
        FwpmGetAppIdFromFileName0(appPath, &appId) != ERROR_SUCCESS)
        appId = nullptr;

    FWPM_FILTER_CONDITION0 c[4]{};
    UINT32 nc = 0;
    if (appId) {
        c[nc].fieldKey = FWPM_CONDITION_ALE_APP_ID;
        c[nc].matchType = FWP_MATCH_EQUAL;
        c[nc].conditionValue.type = FWP_BYTE_BLOB_TYPE;
        c[nc].conditionValue.byteBlob = appId; ++nc;
    }
    if (remoteIpv4Host) {
        c[nc].fieldKey = FWPM_CONDITION_IP_REMOTE_ADDRESS;
        c[nc].matchType = FWP_MATCH_EQUAL;
        c[nc].conditionValue.type = FWP_UINT32;
        c[nc].conditionValue.uint32 = remoteIpv4Host; ++nc;
    }
    if (proto) {
        c[nc].fieldKey = FWPM_CONDITION_IP_PROTOCOL;
        c[nc].matchType = FWP_MATCH_EQUAL;
        c[nc].conditionValue.type = FWP_UINT8;
        c[nc].conditionValue.uint8 = proto; ++nc;
    }
    if (port) {
        c[nc].fieldKey = FWPM_CONDITION_IP_REMOTE_PORT;
        c[nc].matchType = FWP_MATCH_EQUAL;
        c[nc].conditionValue.type = FWP_UINT16;
        c[nc].conditionValue.uint16 = port; ++nc;
    }
    // WFP FWP_UINT8 filter weights are 0-15. User permit = 13, user block = 14
    // (both above the baseline's 12, so a block beats a permit which beats the
    // baseline); both stay below tier-0's 15 so loopback/DNS/the SSH subnet can
    // never be blocked by a user rule.
    bool ok = addV4(block, nc ? c : nullptr, nc, block ? 14 : 13,
                    block ? L"NeuralGuard user block" : L"NeuralGuard user permit");
    // A rule WITHOUT a v4 address is version-agnostic (app / port / proto) - also
    // apply it at CONNECT_V6 so a user's app-level permit/block covers IPv6. A
    // rule pinned to a v4 address is inherently IPv4-only (skip v6).
    if (!remoteIpv4Host) {
        ok = addV6(block, nc ? c : nullptr, nc, block ? 14 : 13,
                   block ? L"NeuralGuard user block (v6)" : L"NeuralGuard user permit (v6)") && ok;
    }
    if (appId) FwpmFreeMemory0((void**)&appId);
    return ok;
}

bool Enforcer::enableDefaultDeny() {
    // Tier-0 always-exempt permits (weight 15, above the catch-all block).
    bool ok = true;
    ok &= addPermitCidrV4(0x7F000000, 0xFF000000);  // 127.0.0.0/8   loopback
    ok &= addPermitCidrV4(0x0A000000, 0xFF000000);  // 10.0.0.0/8    private
    ok &= addPermitCidrV4(0xAC100000, 0xFFF00000);  // 172.16.0.0/12 private
    ok &= addPermitCidrV4(0xC0A80000, 0xFFFF0000);  // 192.168.0.0/16 private (LAN/SSH peer)
    ok &= addPermitCidrV4(0xA9FE0000, 0xFFFF0000);  // 169.254.0.0/16 link-local
    ok &= addPermitRemotePortV4(53, IPPROTO_UDP);   // DNS
    ok &= addPermitRemotePortV4(53, IPPROTO_TCP);   // DNS/TCP
    ok &= addPermitRemotePortV4(67, IPPROTO_UDP);   // DHCP
    ok &= addPermitRemotePortV4(123, IPPROTO_UDP);  // NTP
    // Catch-all outbound block (weight 0, lowest -> everything above wins).
    ok &= addV4(true, nullptr, 0, 0, L"NeuralGuard default-deny (outbound v4)");

    // --- IPv6 outbound (Phase B): mirror the Tier-0 exempts + catch-all onto
    // CONNECT_V6. The multicast + link-local exempts are load-bearing: IPv6
    // Neighbor Discovery / Router Discovery / MLD run over ff00::/8 and fe80::/10,
    // and IPv6 stops working entirely if those are blocked.
    static const unsigned char kLoopback6[16]  = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};  // ::1
    static const unsigned char kLinkLocal6[16] = {0xfe, 0x80};                        // fe80::/10
    static const unsigned char kUla6[16]       = {0xfc};                              // fc00::/7  (ULA)
    static const unsigned char kMcast6[16]     = {0xff};                              // ff00::/8  (multicast)
    ok &= addPermitCidrV6(kLoopback6, 128);
    ok &= addPermitCidrV6(kLinkLocal6, 10);
    ok &= addPermitCidrV6(kUla6, 7);
    ok &= addPermitCidrV6(kMcast6, 8);
    ok &= addPermitRemotePortV6(53, IPPROTO_UDP);   // DNS
    ok &= addPermitRemotePortV6(53, IPPROTO_TCP);   // DNS/TCP
    ok &= addPermitRemotePortV6(547, IPPROTO_UDP);  // DHCPv6 (client -> server)
    ok &= addPermitRemotePortV6(123, IPPROTO_UDP);  // NTP
    ok &= addV6(true, nullptr, 0, 0, L"NeuralGuard default-deny (outbound v6)");
    return ok;
}

int Enforcer::countRules() {
    int n = 0;
    for (const GUID& layer : kLayers) n += CountOurFiltersInLayer((HANDLE)engine_, layer);
    return n;
}

int Enforcer::clearFilters() {
    HANDLE eng = (HANDLE)engine_;
    int deleted = 0;
    for (const GUID& layer : kLayers) deleted += DeleteOurFiltersInLayer(eng, layer);
    return deleted;
}

int Enforcer::panic() {
    HANDLE eng = (HANDLE)engine_;
    int deleted = clearFilters();
    // Now that no filters reference it, drop the sublayer (best effort).
    FwpmSubLayerDeleteByKey0(eng, &kSubLayerGuid);
    FwpmProviderDeleteByKey0(eng, &kProviderGuid);
    return deleted;
}

}  // namespace ng
