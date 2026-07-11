#include "core/flowstats.h"

#include "core/db.h"
#include "core/dns.h"
#include "core/identity.h"
#include "core/util.h"

#include <mutex>

// Winsock headers must precede <windows.h>; NG_DEFS defines WIN32_LEAN_AND_MEAN.
#include <winsock2.h>
#include <ws2ipdef.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <tcpestats.h>
#include <psapi.h>
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "psapi.lib")

namespace ng {

namespace {

constexpr int kRetentionDays = 30;   // auto-purge window
constexpr DWORD kPollMs = 2000;      // TCP-table poll interval

std::string IpStr(DWORD addrBE) {
    in_addr a; a.S_un.S_addr = addrBE;
    char buf[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &a, buf, sizeof(buf));
    return buf;
}

// Enabling ESTATS data collection is idempotent and needs admin.
void EnableEstats(const MIB_TCPROW& row) {
    TCP_ESTATS_DATA_RW_v0 rw{}; rw.EnableCollection = TRUE;
    SetPerTcpConnectionEStats(const_cast<PMIB_TCPROW>(&row), TcpConnectionEstatsData,
                              reinterpret_cast<PUCHAR>(&rw), 0, sizeof(rw), 0);
}

bool ReadEstats(const MIB_TCPROW& row, unsigned long long& in, unsigned long long& out) {
    TCP_ESTATS_DATA_ROD_v0 rod{};
    if (GetPerTcpConnectionEStats(const_cast<PMIB_TCPROW>(&row), TcpConnectionEstatsData,
            nullptr, 0, 0, nullptr, 0, 0, reinterpret_cast<PUCHAR>(&rod), 0, sizeof(rod)) != NO_ERROR)
        return false;
    in = rod.DataBytesIn; out = rod.DataBytesOut;
    return true;
}

// PID -> process identity. GetProcessImageFileNameW returns the NT device-path
// form (\Device\HarddiskVolumeN\...), which is exactly what IdentityResolver
// expects; lowercased to match the WFP app-id form so the identity cache hits.
Identity ResolvePid(IdentityResolver& id, DWORD pid) {
    if (!pid) return Identity{};
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return Identity{};
    wchar_t dev[MAX_PATH]{};
    DWORD len = GetProcessImageFileNameW(h, dev, MAX_PATH);
    CloseHandle(h);
    if (!len) return Identity{};
    return id.resolve(util::ToLower(util::Narrow(std::wstring(dev, len))));
}

struct ActiveFlow {
    double      firstEpoch = 0;
    std::string remoteIp;
    unsigned    remotePort = 0;
    unsigned    localPort = 0;
    std::string procKey, procLabel, procPath;
    unsigned long long bytesIn = 0, bytesOut = 0;
};

}  // namespace

long long PurgeFlowFeatures(Db& db, int days) {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    u.QuadPart -= (ULONGLONG)days * 24 * 3600 * 10000000ULL;
    FILETIME cf; cf.dwLowDateTime = u.LowPart; cf.dwHighDateTime = u.HighPart;
    std::string cutoff = util::IsoTime(cf);

    std::lock_guard<std::mutex> lk(db.mutex());
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db.handle(), "DELETE FROM flow_features WHERE ts_utc < ?;",
                           -1, &s, nullptr) != SQLITE_OK)
        return -1;
    bindText(s, 1, cutoff);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) return -1;
    return sqlite3_changes(db.handle());
}

void FlowCollector::stop() {
    if (stopEvent_) SetEvent(static_cast<HANDLE>(stopEvent_));
}

bool FlowCollector::run(int seconds) {
    long long purged = PurgeFlowFeatures(db_, kRetentionDays);
    if (purged > 0) printf("purged %lld flow feature row(s) older than %d days\n", purged, kRetentionDays);

    sqlite3_stmt* ins = nullptr;
    if (sqlite3_prepare_v2(db_.handle(),
            "INSERT INTO flow_features"
            "(ts_utc,process_key,process_label,dest,remote_port,protocol,duration_ms,bytes_in,bytes_out,local_port,anomaly_score,malicious_score)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,?,?);",
            -1, &ins, nullptr) != SQLITE_OK) {
        fprintf(stderr, "prepare flow_features insert failed: %s\n", sqlite3_errmsg(db_.handle()));
        return false;
    }

    // Optional shadow-mode scoring: load whichever models were configured
    // (no-op if a model or onnxruntime.dll is missing - that scorer stays off).
    if (!anomalyPath_.empty() && anomaly_.load(anomalyPath_))
        printf("anomaly model loaded (shadow mode).\n");
    if (!supervisedPath_.empty() && supervised_.load(supervisedPath_))
        printf("supervised model loaded (shadow mode).\n");

    // Phase 4d: in active mode, high scores over their confidence gate become
    // ml_flags (demote/review). Read the gates once; prepare the writer.
    double malThresh = 0.9, anomThresh = -0.15;
    sqlite3_stmt* mlIns = nullptr;
    if (active_) {
        auto readThresh = [&](const char* k, double dflt) {
            std::lock_guard<std::mutex> lk(db_.mutex());
            sqlite3_stmt* s = nullptr; double v = dflt;
            if (sqlite3_prepare_v2(db_.handle(), "SELECT v FROM meta WHERE k=?;", -1, &s, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(s, 1, k, -1, SQLITE_TRANSIENT);
                if (sqlite3_step(s) == SQLITE_ROW) { const char* t = (const char*)sqlite3_column_text(s, 0); if (t) v = atof(t); }
                sqlite3_finalize(s);
            }
            return v;
        };
        malThresh = readThresh("ml_malicious_threshold", 0.9);
        anomThresh = readThresh("ml_anomaly_threshold", -0.15);
        sqlite3_prepare_v2(db_.handle(),
            "INSERT OR IGNORE INTO ml_flags(ts_utc,kind,process_key,process_label,app_path,dest,remote_port,protocol,score)"
            " VALUES(?,?,?,?,?,?,?,?,?);", -1, &mlIns, nullptr);
        printf("ML ACTIVE: demote at P(malicious) >= %.2f, review at anomaly <= %.2f.\n", malThresh, anomThresh);
    }

    WSADATA wsa{}; WSAStartup(MAKEWORD(2, 2), &wsa);
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const ULONGLONG deadline = seconds > 0 ? GetTickCount64() + (ULONGLONG)seconds * 1000 : 0;

    std::map<std::string, ActiveFlow> active;
    std::string tblBuf;

    auto writeFlow = [&](const ActiveFlow& f, double nowEpoch) {
        int durMs = (int)((nowEpoch - f.firstEpoch) * 1000.0);
        if (durMs < 0) durMs = 0;
        // Resolve the destination to a domain at completion time - by now DNS
        // (which lags the connect by ~1s) has almost always caught up.
        std::string dest = f.remoteIp;
        if (dns_) { std::string d = dns_->lookup(f.remoteIp); if (!d.empty()) dest = d; }

        // Shadow-mode scores (computed off the DB lock - ML inference, not a DB
        // op). NULL for whichever model isn't loaded. The supervised classifier
        // uses only the 6 network features (a wire-captured IDS dataset has no
        // host context); the anomaly model adds is_signed + hour.
        bool haveAnom = false, haveMal = false;
        double anomScore = 0.0, malScore = 0.0;
        if (anomaly_.loaded() || supervised_.loaded()) {
            SYSTEMTIME st{}; GetSystemTime(&st);   // UTC hour, matches the ISO ts_utc
            long long total = (long long)f.bytesIn + (long long)f.bytesOut;
            std::vector<float> sup = {
                std::log1p((float)durMs),
                std::log1p((float)(long long)f.bytesIn),
                std::log1p((float)(long long)f.bytesOut),
                (float)((double)(long long)f.bytesOut / (double)(total + 1)),
                f.remotePort == 443 ? 1.0f : 0.0f,
                f.remotePort == 80 ? 1.0f : 0.0f,
            };
            std::vector<float> anom = sup;
            anom.push_back(f.procKey.rfind("sig:", 0) == 0 ? 1.0f : 0.0f);
            anom.push_back((float)st.wHour);
            if (anomaly_.loaded()) {
                auto o = anomaly_.run(anom);
                if (!o.empty()) { anomScore = o[0]; haveAnom = true; }
            }
            if (supervised_.loaded()) {
                auto o = supervised_.run(sup);            // [P(benign), P(malicious)]
                if (o.size() >= 2) { malScore = o[1]; haveMal = true; }
                else if (!o.empty()) { malScore = o[0]; haveMal = true; }
            }
        }

        // Serialize with the recorder, which writes flow_events from WFP threads.
        std::lock_guard<std::mutex> lk(db_.mutex());
        sqlite3_reset(ins);
        sqlite3_clear_bindings(ins);
        bindText(ins, 1, util::IsoNow());
        bindText(ins, 2, f.procKey);
        bindText(ins, 3, f.procLabel);
        bindText(ins, 4, dest);
        sqlite3_bind_int(ins, 5, (int)f.remotePort);
        sqlite3_bind_int(ins, 6, IPPROTO_TCP);
        sqlite3_bind_int(ins, 7, durMs);
        sqlite3_bind_int64(ins, 8, (sqlite3_int64)f.bytesIn);
        sqlite3_bind_int64(ins, 9, (sqlite3_int64)f.bytesOut);
        sqlite3_bind_int(ins, 10, (int)f.localPort);
        if (haveAnom) sqlite3_bind_double(ins, 11, anomScore); else sqlite3_bind_null(ins, 11);
        if (haveMal)  sqlite3_bind_double(ins, 12, malScore);  else sqlite3_bind_null(ins, 12);
        if (sqlite3_step(ins) == SQLITE_DONE) ++written_;

        // Phase 4d (active mode, same lock): scores over their gate -> ml_flags.
        // 'demote' removes the (app,port) permit next reapply; 'review' is advisory.
        if (active_ && mlIns) {
            bool demote = haveMal && malScore >= malThresh && !f.procPath.empty();
            bool review = haveAnom && anomScore <= anomThresh;
            auto flag = [&](const char* kind, double sc) {
                sqlite3_reset(mlIns); sqlite3_clear_bindings(mlIns);
                bindText(mlIns, 1, util::IsoNow());
                bindText(mlIns, 2, kind);
                bindText(mlIns, 3, f.procKey);
                bindText(mlIns, 4, f.procLabel);
                bindText(mlIns, 5, f.procPath);
                bindText(mlIns, 6, dest);
                sqlite3_bind_int(mlIns, 7, (int)f.remotePort);
                sqlite3_bind_int(mlIns, 8, IPPROTO_TCP);
                sqlite3_bind_double(mlIns, 9, sc);
                sqlite3_step(mlIns);
            };
            if (demote) flag("demote", malScore);
            if (review) flag("review", anomScore);
            if (demote)   // baseline changed -> nudge a running enforce daemon to reapply
                sqlite3_exec(db_.handle(),
                    "UPDATE meta SET v=CAST(v AS INTEGER)+1 WHERE k='rules_gen';", nullptr, nullptr, nullptr);
        }
    };

    for (;;) {
        ULONG size = 0;
        GetTcpTable2(nullptr, &size, FALSE);
        tblBuf.resize(size);
        auto* tbl = reinterpret_cast<MIB_TCPTABLE2*>(tblBuf.data());
        double nowEpoch = util::UnixEpoch([] { FILETIME ft; GetSystemTimeAsFileTime(&ft); return ft; }());

        if (size && GetTcpTable2(tbl, &size, FALSE) == NO_ERROR) {
            std::set<std::string> present;
            for (DWORD i = 0; i < tbl->dwNumEntries; ++i) {
                const MIB_TCPROW2& r2 = tbl->table[i];
                if (r2.dwState != MIB_TCP_STATE_ESTAB || r2.dwRemoteAddr == 0) continue;

                MIB_TCPROW row{};
                row.dwState      = r2.dwState;
                row.dwLocalAddr  = r2.dwLocalAddr;
                row.dwLocalPort  = r2.dwLocalPort;
                row.dwRemoteAddr = r2.dwRemoteAddr;
                row.dwRemotePort = r2.dwRemotePort;

                unsigned lport = ntohs((u_short)r2.dwLocalPort);
                unsigned rport = ntohs((u_short)r2.dwRemotePort);
                std::string rip = IpStr(r2.dwRemoteAddr);
                std::string key = std::to_string(lport) + "|" + rip + "|" + std::to_string(rport);
                present.insert(key);

                auto it = active.find(key);
                if (it == active.end()) {
                    EnableEstats(row);
                    ActiveFlow f;
                    f.firstEpoch = nowEpoch;
                    f.remoteIp = rip;
                    f.remotePort = rport;
                    f.localPort = lport;
                    Identity idn = ResolvePid(id_, r2.dwOwningPid);
                    f.procKey = idn.key;
                    f.procLabel = idn.label;
                    f.procPath = idn.path;   // baseline exclusion key for 4d demotions
                    ReadEstats(row, f.bytesIn, f.bytesOut);
                    active.emplace(key, std::move(f));
                } else {
                    ReadEstats(row, it->second.bytesIn, it->second.bytesOut);
                }
            }
            // Completion: anything we tracked that's no longer in the table closed.
            for (auto it = active.begin(); it != active.end();) {
                if (!present.count(it->first)) {
                    writeFlow(it->second, nowEpoch);
                    it = active.erase(it);
                } else {
                    ++it;
                }
            }
        }

        if (seconds > 0 && GetTickCount64() >= deadline) break;
        if (WaitForSingleObject(static_cast<HANDLE>(stopEvent_), kPollMs) == WAIT_OBJECT_0) break;
    }

    sqlite3_finalize(ins);
    if (mlIns) sqlite3_finalize(mlIns);
    CloseHandle(static_cast<HANDLE>(stopEvent_));
    stopEvent_ = nullptr;
    WSACleanup();
    return true;
}

}  // namespace ng
