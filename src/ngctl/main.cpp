// ngctl - NeuralGuard control tool. For Phase 2a this drives the enforcement
// primitive: install/remove WFP filters and, most importantly, PANIC (delete
// every NeuralGuard filter so the machine fails open). Full policy control and
// the tray front-end come later; this is the safety-first foundation.
//
// Usage:
//   ngctl panic                 Delete ALL NeuralGuard filters (fail open).
//   ngctl status                Show installed NeuralGuard filter count.
//   ngctl block <ipv4> [port]   Add a block filter for a remote IPv4[:port] (TCP).
//   ngctl allow <ipv4> [port]   Add a permit filter for a remote IPv4[:port] (TCP).
//   ngctl -h | --help | /?      This help.
//
// Requires an elevated (Administrator) prompt.

#include "core/enforcer.h"
#include "core/db.h"
#include "core/util.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

bool IsElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION elev{}; DWORD cb = 0;
    bool elevated = GetTokenInformation(tok, TokenElevation, &elev, sizeof(elev), &cb) &&
                    elev.TokenIsElevated;
    CloseHandle(tok);
    return elevated;
}

void PrintUsage() {
    printf(
        "NeuralGuard ngctl - firewall control\n\n"
        "Usage:\n"
        "  ngctl panic                 Delete ALL NeuralGuard filters (fail open).\n"
        "  ngctl status                Show installed NeuralGuard filter count.\n"
        "  ngctl block <ipv4> [port]   Add a block filter for a remote IPv4[:port] (TCP).\n"
        "  ngctl allow <ipv4> [port]   Add a permit filter for a remote IPv4[:port] (TCP).\n"
        "  ngctl enforce <seconds>     Default-deny outbound IPv4 for <seconds>, then\n"
        "                              auto-revert (Tier-0 exempt; inbound untouched).\n"
        "  ngctl enforce-baseline <db> <seconds> [min-conns]\n"
        "                              Permit STABLE (app, port) pairs from <db> (seen on\n"
        "                              >= min-conns connections, default 3), default-deny\n"
        "                              the rest for <seconds>, then auto-revert.\n"
        "  ngctl -h | --help | /?      This help.\n\n"
        "Requires an elevated (Administrator) prompt.\n");
}

// Parse dotted IPv4 into host byte order (what WFP IP_REMOTE_ADDRESS wants).
bool ParseIpv4Host(const char* s, uint32_t& out) {
    in_addr a{};
    if (InetPtonA(AF_INET, s, &a) != 1) return false;
    out = ntohl(a.s_addr);
    return true;
}

int Rule(int argc, char** argv, bool block) {
    if (argc < 3) { fprintf(stderr, "usage: ngctl %s <ipv4> [port]\n", argv[1]); return 2; }
    uint32_t ip = 0;
    if (!ParseIpv4Host(argv[2], ip)) { fprintf(stderr, "bad IPv4: %s\n", argv[2]); return 2; }
    uint16_t port = (argc >= 4) ? (uint16_t)atoi(argv[3]) : 0;

    ng::Enforcer enf;
    if (!enf.open()) return 1;
    bool ok = enf.addRemoteIpv4Rule(ip, port, IPPROTO_TCP, block);
    if (ok)
        printf("%s %s%s%s\n", block ? "BLOCK" : "PERMIT", argv[2],
               port ? ":" : "", port ? argv[3] : "");
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { PrintUsage(); return 2; }
    const char* cmd = argv[1];
    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "/?") == 0) {
        PrintUsage();
        return 0;
    }

    if (!IsElevated()) {
        fprintf(stderr,
            "ngctl must be run as Administrator (it manages Windows Filtering\n"
            "Platform filters). Right-click PowerShell -> Run as administrator.\n");
        return 1;
    }

    if (strcmp(cmd, "panic") == 0) {
        ng::Enforcer enf;
        if (!enf.open()) return 1;
        int n = enf.panic();
        printf("PANIC: removed %d NeuralGuard filter(s). Failing open.\n", n);
        return 0;
    }
    if (strcmp(cmd, "status") == 0) {
        ng::Enforcer enf;
        if (!enf.open()) return 1;
        printf("NeuralGuard filters installed: %d\n", enf.countRules());
        return 0;
    }
    if (strcmp(cmd, "enforce") == 0) {
        // Mandatory duration = a dead-man switch: enforcement always auto-reverts,
        // so an unattended test can never leave the machine stuck blocked.
        int secs = (argc >= 3) ? atoi(argv[2]) : 0;
        if (secs <= 0) {
            fprintf(stderr, "usage: ngctl enforce <seconds>  (duration required; auto-reverts)\n");
            return 2;
        }
        ng::Enforcer enf;
        if (!enf.open()) return 1;
        if (!enf.enableDefaultDeny()) {
            fprintf(stderr, "enforce failed; reverting.\n");
            enf.panic();
            return 1;
        }
        printf("ENFORCE: default-deny outbound IPv4 ON (%d filters). "
               "Tier-0 exempt; inbound untouched.\n", enf.countRules());
        printf("Auto-reverting in %d s...\n", secs);
        fflush(stdout);
        Sleep((DWORD)secs * 1000);
        int n = enf.panic();
        printf("Reverted (removed %d filter(s)). Failing open.\n", n);
        return 0;
    }
    if (strcmp(cmd, "enforce-baseline") == 0) {
        if (argc < 4) {
            fprintf(stderr, "usage: ngctl enforce-baseline <db> <seconds> [min-connections]\n");
            return 2;
        }
        const char* dbPath = argv[2];
        int secs = atoi(argv[3]);
        int minConns = (argc >= 5) ? atoi(argv[4]) : 3;  // stability threshold
        if (secs <= 0) { fprintf(stderr, "seconds must be > 0 (auto-reverts)\n"); return 2; }
        if (minConns < 1) minConns = 1;

        ng::Db db;
        if (!db.open(dbPath)) return 1;
        ng::Enforcer enf;
        if (!enf.open()) return 1;

        // Permit only STABLE (application, remote port) pairs - ones seen on at
        // least min-connections distinct connections. Provisional (rarely seen)
        // pairs are left to default-deny (a prompt, once the tray flow lands).
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db.handle(),
            "SELECT pi.image_path, fe.protocol, fe.remote_port,"
            " COUNT(DISTINCT fe.local_port || '|' || fe.remote_addr) AS conns"
            " FROM flow_events fe JOIN process_identity pi ON fe.image_id = pi.id"
            " WHERE fe.remote_port > 0 AND fe.remote_port < 49152 AND pi.image_path LIKE '_:\\%'"
            "   AND fe.verdict IN ('ALLOW','CAPALLOW')"
            " GROUP BY pi.image_path, fe.protocol, fe.remote_port"
            " HAVING conns >= ?;", -1, &s, nullptr);
        sqlite3_bind_int(s, 1, minConns);
        int permits = 0;
        while (sqlite3_step(s) == SQLITE_ROW) {
            const char* path = (const char*)sqlite3_column_text(s, 0);
            int proto = sqlite3_column_int(s, 1);
            int port = sqlite3_column_int(s, 2);
            if (path && enf.addPermitAppId(ng::util::Widen(path).c_str(),
                                           (uint16_t)port, (uint8_t)proto))
                ++permits;
        }
        sqlite3_finalize(s);

        if (!enf.enableDefaultDeny()) { enf.panic(); return 1; }
        printf("ENFORCE-BASELINE: %d stable app permits (>=%d conns) + default-deny"
               " (%d filters total).\n", permits, minConns, enf.countRules());
        printf("Auto-reverting in %d s...\n", secs);
        fflush(stdout);
        Sleep((DWORD)secs * 1000);
        int n = enf.panic();
        printf("Reverted (removed %d filter(s)).\n", n);
        return 0;
    }
    if (strcmp(cmd, "block") == 0) return Rule(argc, argv, true);
    if (strcmp(cmd, "allow") == 0) return Rule(argc, argv, false);

    fprintf(stderr, "unknown command: %s\n", cmd);
    PrintUsage();
    return 2;
}
