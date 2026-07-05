// Shared helpers for reading WFP net events. Header-only (inline) so both the
// ngmon printer and the ngd recorder can use the same formatting logic.
#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <fwpmu.h>
#include <sddl.h>

#include <string>

namespace ngwfp {

// WFP net-event header addresses/ports are in host byte order.
inline std::string IpToStr(const FWPM_NET_EVENT_HEADER3* h, bool remote) {
    char buf[INET6_ADDRSTRLEN] = {};
    if (h->ipVersion == FWP_IP_VERSION_V4) {
        in_addr a{};
        a.s_addr = htonl(remote ? h->remoteAddrV4 : h->localAddrV4);
        InetNtopA(AF_INET, &a, buf, sizeof(buf));
    } else if (h->ipVersion == FWP_IP_VERSION_V6) {
        in6_addr a{};
        memcpy(&a, remote ? h->remoteAddrV6.byteArray16
                          : h->localAddrV6.byteArray16, 16);
        InetNtopA(AF_INET6, &a, buf, sizeof(buf));
    } else {
        return "?";
    }
    return buf;
}

inline const char* ProtoName(UINT8 p) {
    switch (p) {
        case IPPROTO_TCP:    return "TCP";
        case IPPROTO_UDP:    return "UDP";
        case IPPROTO_ICMP:   return "ICMP";
        case IPPROTO_ICMPV6: return "ICMPv6";
        default:             return "IP";
    }
}

inline const char* TypeName(FWPM_NET_EVENT_TYPE t) {
    switch (t) {
        case FWPM_NET_EVENT_TYPE_CLASSIFY_DROP:      return "DROP";
        case FWPM_NET_EVENT_TYPE_CLASSIFY_ALLOW:     return "ALLOW";
        case FWPM_NET_EVENT_TYPE_CAPABILITY_DROP:    return "CAPDROP";
        case FWPM_NET_EVENT_TYPE_CAPABILITY_ALLOW:   return "CAPALLOW";
        default:                                     return "OTHER";
    }
}

// appId is the NT device path of the image (e.g. \device\harddiskvolume3\...).
// Returned raw here; normalization to C:\ paths + signer/hash is the recorder's job.
inline std::string AppIdToStr(const FWP_BYTE_BLOB* blob) {
    if (!blob || !blob->data || blob->size == 0) return "";
    const wchar_t* w = reinterpret_cast<const wchar_t*>(blob->data);
    size_t wlen = blob->size / sizeof(wchar_t);
    while (wlen > 0 && w[wlen - 1] == L'\0') --wlen;
    if (wlen == 0) return "";
    int need = WideCharToMultiByte(CP_UTF8, 0, w, (int)wlen, nullptr, 0, nullptr, nullptr);
    std::string s(need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, (int)wlen, s.data(), need, nullptr, nullptr);
    return s;
}

inline std::string UserSid(const FWPM_NET_EVENT_HEADER3* h) {
    if (!(h->flags & FWPM_NET_EVENT_FLAG_USER_ID_SET) || !h->userId) return "";
    LPSTR str = nullptr;
    std::string out;
    if (ConvertSidToStringSidA(h->userId, &str) && str) { out = str; LocalFree(str); }
    return out;
}

}  // namespace ngwfp
