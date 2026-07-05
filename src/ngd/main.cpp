// ngd - NeuralGuard daemon (Phase 1: learning mode, recorder).
//
// Subscribes to WFP net events (same source as ngmon) and RECORDS each one into
// a SQLite database instead of printing. This is the passive baseline collector
// the enforcement design later stands on. No enforcement, no ML - just observe
// and persist. Runs as a console app for now; the Windows-service wrapper comes
// in Phase 2. See docs/DESIGN.md and docs/ROADMAP.md (Phase 1).
//
// Usage:
//   ngd [record] [dbpath]   record net events into dbpath (default ngpolicy.db)
//   ngd dump      [dbpath]   print event count + the most recent 20 rows
//
// Requires elevation and the "Filtering Platform Connection" audit subcategory
// (see ngmon). Ctrl+C to stop recording.

#include "common/wfp_util.h"
#include "sqlite3.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace {

sqlite3*      g_db   = nullptr;
sqlite3_stmt* g_ins  = nullptr;
std::mutex    g_dbMutex;
std::atomic<unsigned long long> g_count{0};
HANDLE        g_stopEvent = nullptr;

const char* kSchema =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA synchronous=NORMAL;"
    "CREATE TABLE IF NOT EXISTS meta(k TEXT PRIMARY KEY, v TEXT);"
    "INSERT OR IGNORE INTO meta(k,v) VALUES('schema_version','1');"
    "CREATE TABLE IF NOT EXISTS flow_events("
    "  id INTEGER PRIMARY KEY,"
    "  ts_utc      TEXT NOT NULL,"
    "  verdict     TEXT NOT NULL,"
    "  protocol    INTEGER,"
    "  local_addr  TEXT,"
    "  local_port  INTEGER,"
    "  remote_addr TEXT,"
    "  remote_port INTEGER,"
    "  image_path  TEXT,"
    "  user_sid    TEXT);";

std::string IsoTime(const FILETIME& ft) {
    SYSTEMTIME st{};
    FileTimeToSystemTime(&ft, &st);
    char buf[40];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

void bindText(sqlite3_stmt* s, int i, const std::string& v) {
    sqlite3_bind_text(s, i, v.c_str(), (int)v.size(), SQLITE_TRANSIENT);
}

void CALLBACK OnNetEvent(void* /*context*/, const FWPM_NET_EVENT5* ev) {
    if (!ev) return;
    const FWPM_NET_EVENT_HEADER3* h = &ev->header;

    const bool hasProto = (h->flags & FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET) != 0;
    const bool hasLPort = (h->flags & FWPM_NET_EVENT_FLAG_LOCAL_PORT_SET) != 0;
    const bool hasRPort = (h->flags & FWPM_NET_EVENT_FLAG_REMOTE_PORT_SET) != 0;

    std::string ts     = IsoTime(h->timeStamp);
    std::string local  = ngwfp::IpToStr(h, false);
    std::string remote = ngwfp::IpToStr(h, true);
    std::string app    = ngwfp::AppIdToStr(&h->appId);
    std::string sid    = ngwfp::UserSid(h);

    std::lock_guard<std::mutex> lk(g_dbMutex);
    if (!g_ins) return;
    sqlite3_reset(g_ins);
    sqlite3_clear_bindings(g_ins);
    bindText(g_ins, 1, ts);
    bindText(g_ins, 2, ngwfp::TypeName(ev->type));
    if (hasProto) sqlite3_bind_int(g_ins, 3, h->ipProtocol); else sqlite3_bind_null(g_ins, 3);
    bindText(g_ins, 4, local);
    if (hasLPort) sqlite3_bind_int(g_ins, 5, h->localPort); else sqlite3_bind_null(g_ins, 5);
    bindText(g_ins, 6, remote);
    if (hasRPort) sqlite3_bind_int(g_ins, 7, h->remotePort); else sqlite3_bind_null(g_ins, 7);
    bindText(g_ins, 8, app);
    bindText(g_ins, 9, sid);
    if (sqlite3_step(g_ins) == SQLITE_DONE) ++g_count;
}

BOOL WINAPI CtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        if (g_stopEvent) SetEvent(g_stopEvent);
        return TRUE;
    }
    return FALSE;
}

int OpenDb(const char* path) {
    if (sqlite3_open(path, &g_db) != SQLITE_OK) {
        fprintf(stderr, "sqlite3_open(%s) failed: %s\n", path, sqlite3_errmsg(g_db));
        return 1;
    }
    char* errmsg = nullptr;
    if (sqlite3_exec(g_db, kSchema, nullptr, nullptr, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "schema init failed: %s\n", errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
        return 1;
    }
    return 0;
}

int RunDump(const char* path) {
    if (OpenDb(path) != 0) return 1;
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(g_db, "SELECT count(*) FROM flow_events;", -1, &s, nullptr);
    if (sqlite3_step(s) == SQLITE_ROW)
        printf("flow_events rows: %lld\n", sqlite3_column_int64(s, 0));
    sqlite3_finalize(s);

    printf("--- most recent 20 ---\n");
    const char* q =
        "SELECT ts_utc,verdict,protocol,local_addr,local_port,remote_addr,remote_port,image_path,user_sid"
        " FROM flow_events ORDER BY id DESC LIMIT 20;";
    sqlite3_prepare_v2(g_db, q, -1, &s, nullptr);
    while (sqlite3_step(s) == SQLITE_ROW) {
        printf("%s  %-8s proto=%d  %s:%d -> %s:%d  [%s]  %s\n",
               sqlite3_column_text(s, 0), sqlite3_column_text(s, 1),
               sqlite3_column_int(s, 2),
               sqlite3_column_text(s, 3), sqlite3_column_int(s, 4),
               sqlite3_column_text(s, 5), sqlite3_column_int(s, 6),
               sqlite3_column_text(s, 7) ? (const char*)sqlite3_column_text(s, 7) : "",
               sqlite3_column_text(s, 8) ? (const char*)sqlite3_column_text(s, 8) : "");
    }
    sqlite3_finalize(s);
    sqlite3_close(g_db);
    return 0;
}

int RunRecord(const char* path) {
    if (OpenDb(path) != 0) return 1;
    if (sqlite3_prepare_v2(g_db,
            "INSERT INTO flow_events"
            "(ts_utc,verdict,protocol,local_addr,local_port,remote_addr,remote_port,image_path,user_sid)"
            " VALUES(?,?,?,?,?,?,?,?,?);",
            -1, &g_ins, nullptr) != SQLITE_OK) {
        fprintf(stderr, "prepare insert failed: %s\n", sqlite3_errmsg(g_db));
        return 1;
    }

    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    FWPM_SESSION0 session{};
    session.flags = 0;  // NOT dynamic: engine options can't be set from a dynamic
                        // session (FWP_E_DYNAMIC_SESSION_IN_PROGRESS).
    session.displayData.name = const_cast<wchar_t*>(L"ngd");

    HANDLE engine = nullptr;
    DWORD err = FwpmEngineOpen0(nullptr, RPC_C_AUTHN_WINNT, nullptr, &session, &engine);
    if (err != ERROR_SUCCESS) {
        fprintf(stderr, "FwpmEngineOpen0 failed: 0x%08lX (are you elevated?)\n", err);
        return 1;
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
    err = FwpmNetEventSubscribe4(engine, &sub, OnNetEvent, nullptr, &subHandle);
    if (err != ERROR_SUCCESS) {
        fprintf(stderr, "FwpmNetEventSubscribe4 failed: 0x%08lX\n", err);
        FwpmEngineClose0(engine);
        return 1;
    }

    printf("ngd - recording WFP net events to %s (Ctrl+C to stop)\n", path);
    fflush(stdout);
    WaitForSingleObject(g_stopEvent, INFINITE);

    {
        std::lock_guard<std::mutex> lk(g_dbMutex);
        FwpmNetEventUnsubscribe0(engine, subHandle);
        FwpmEngineClose0(engine);
        sqlite3_finalize(g_ins);
        g_ins = nullptr;
        sqlite3_close(g_db);
    }
    printf("\nStopped. %llu events recorded to %s\n", g_count.load(), path);
    WSACleanup();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const char* mode = "record";
    const char* db   = "ngpolicy.db";
    if (argc >= 2 && (strcmp(argv[1], "dump") == 0 || strcmp(argv[1], "record") == 0)) {
        mode = argv[1];
        if (argc >= 3) db = argv[2];
    } else if (argc >= 2) {
        db = argv[1];  // treat a lone arg as the db path
    }
    return (strcmp(mode, "dump") == 0) ? RunDump(db) : RunRecord(db);
}
