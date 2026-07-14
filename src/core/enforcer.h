// Enforcer: manages NeuralGuard's WFP objects - our provider, a high-weight
// sublayer above Windows Defender Firewall, and permit/block filters at the ALE
// connect/accept layers. Also implements PANIC: delete every NeuralGuard filter
// (and our sublayer) so the machine fails open. Per docs/DESIGN.md, panic is the
// first thing built and the last line of defense against a bad rule locking you out.
#pragma once

#include <cstdint>

namespace ng {

class Enforcer {
public:
    ~Enforcer();

    bool open();    // open the WFP engine; ensure our provider + sublayer exist
    void close();

    // Add a filter matching a remote IPv4 (host byte order) with optional port
    // (0 = any) and protocol (0 = any). block=false adds a permit.
    bool addRemoteIpv4Rule(uint32_t ipv4Host, uint16_t port, uint8_t proto, bool block);

    // Turn on default-deny for OUTBOUND IPv4 AND IPv6 (ALE_AUTH_CONNECT_V4/V6):
    // install the Tier-0 always-exempt permits (loopback, private/ULA/link-local
    // ranges, IPv6 multicast, DNS, DHCP/DHCPv6, NTP) plus a catch-all block at
    // each layer. Inbound is deliberately left untouched so an inbound-initiated
    // session (e.g. SSH) can't be cut off. Call panic() to revert. Returns true
    // on success.
    bool enableDefaultDeny();

    // Permit a specific application (by its on-disk path) to make outbound IPv4
    // connections, optionally restricted to a remote port / protocol (0 = any).
    // Used to auto-permit the observed baseline before default-deny.
    bool addPermitAppId(const wchar_t* dosPath, uint16_t port, uint8_t proto);

    // Apply one user-editable rule as a WFP filter. appPath (NULL/empty = any
    // app), remoteIpv4Host (0 = any, host byte order), port (0 = any), proto
    // (0 = any). block=false permits. User permits sit just above the baseline;
    // user blocks sit above everything so an explicit block always wins.
    bool applyUserRule(const wchar_t* appPath, uint32_t remoteIpv4Host,
                       uint16_t port, uint8_t proto, bool block);

    int  countRules();   // NeuralGuard filters currently installed

    // Delete just our filters (keep the sublayer/provider), for a live re-apply.
    int  clearFilters();

    // Delete ALL NeuralGuard filters and our sublayer/provider. Returns the
    // number of filters removed. This is the panic / fail-open path.
    int  panic();

private:
    bool ensureObjects();
    // Low-level outbound filter helpers (weight: higher wins; block uses 0).
    // addV4/addV6 install the same conditions at CONNECT_V4 / CONNECT_V6.
    bool addV4(bool block, void* conds, unsigned nc, unsigned char weight,
               const wchar_t* name);
    bool addV6(bool block, void* conds, unsigned nc, unsigned char weight,
               const wchar_t* name);
    bool addPermitCidrV4(uint32_t addrHost, uint32_t maskHost);
    bool addPermitRemotePortV4(uint16_t port, uint8_t proto);
    bool addPermitCidrV6(const unsigned char addr[16], unsigned char prefixLen);
    bool addPermitRemotePortV6(uint16_t port, uint8_t proto);
    void* engine_ = nullptr;  // HANDLE
};

}  // namespace ng
