// NeuralGuard in-app updater.
//
// Flow:  check() -> download() -> apply().  Reads a tokenless JSON manifest
// published on the latest GitHub Release, downloads the Inno Setup installer,
// verifies size + SHA-256, then launches it silently and returns so the caller
// can exit and let the installer replace the running files.
//
// SCAFFOLD STATUS: the pure logic (version compare, manifest parse, URL build)
// is complete; the WinHTTP fetch, hash verify, and installer hand-off are wired
// but have NOT been exercised end-to-end against a real release. Validate on the
// VM before shipping. See docs/UPDATER.md.

#include "core/updater.h"
#include "core/version.h"

#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")

namespace ng {

// ---- small helpers ---------------------------------------------------------

static std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
static std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
static bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

// Minimal flat-JSON field read: the value for "key" as a string ("...") or a
// bare number/bool token. Adequate for our own manifest; NOT a general parser.
// TODO(scaffold): if the manifest ever nests, swap this for a real JSON reader.
static std::string jsonField(const std::string& j, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t k = j.find(needle);
    if (k == std::string::npos) return {};
    size_t c = j.find(':', k + needle.size());
    if (c == std::string::npos) return {};
    size_t i = c + 1;
    while (i < j.size() && (j[i] == ' ' || j[i] == '\t' || j[i] == '\n' || j[i] == '\r')) ++i;
    if (i >= j.size()) return {};
    if (j[i] == '"') {                                   // string value
        size_t e = j.find('"', i + 1);
        return e == std::string::npos ? std::string{} : j.substr(i + 1, e - i - 1);
    }
    size_t e = i;                                        // bare token
    while (e < j.size() && j[e] != ',' && j[e] != '}' &&
           j[e] != ' ' && j[e] != '\n' && j[e] != '\r') ++e;
    return j.substr(i, e - i);
}

// SHA-256 of a file as lowercase hex, via BCrypt. "" on any failure.
static std::string sha256File(const std::wstring& path) {
    HANDLE fh = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return {};

    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr;
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<unsigned char> buf(1 << 16);
        DWORD read = 0;
        while (ReadFile(fh, buf.data(), (DWORD)buf.size(), &read, nullptr) && read)
            BCryptHashData(h, buf.data(), read, 0);
        unsigned char digest[32];
        if (BCryptFinishHash(h, digest, sizeof(digest), 0) == 0) {
            static const char* hx = "0123456789abcdef";
            for (unsigned char b : digest) { out += hx[b >> 4]; out += hx[b & 0xF]; }
        }
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(fh);
    return out;
}

// WinHTTP GET. If dest is empty the body is returned in *body; otherwise the
// response is streamed to the file at dest (progress against Content-Length).
// Redirects are followed (WinHTTP default) so GitHub's latest/download 302 chain
// resolves to the CDN asset.
static bool httpFetch(const std::wstring& url, std::string* body,
                      const std::wstring& dest, const ProgressFn& progress,
                      UpdateStage stage, std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };

    URL_COMPONENTS uc{}; uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[4096] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath  = path; uc.dwUrlPathLength  = _countof(path);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return fail("bad URL");

    HINTERNET sess = WinHttpOpen(L"NeuralGuard-Updater/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!sess) return fail("WinHttpOpen failed");

    HINTERNET conn = WinHttpConnect(sess, host, uc.nPort, 0);
    HINTERNET req  = nullptr;
    HANDLE    fh   = INVALID_HANDLE_VALUE;
    bool ok = false;
    std::string localErr;

    if (conn) {
        DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        req = WinHttpOpenRequest(conn, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    }
    if (req &&
        WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr)) {

        DWORD status = 0, sl = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sl, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            long long total = 0;
            DWORD cl = 0, cll = sizeof(cl);
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &cl, &cll, WINHTTP_NO_HEADER_INDEX))
                total = cl;

            if (!dest.empty()) {
                fh = CreateFileW(dest.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            }
            if (dest.empty() || fh != INVALID_HANDLE_VALUE) {
                ok = true;
                long long got = 0; DWORD avail = 0;
                do {
                    if (!WinHttpQueryDataAvailable(req, &avail)) { ok = false; break; }
                    if (!avail) break;
                    std::vector<char> buf(avail);
                    DWORD read = 0;
                    if (!WinHttpReadData(req, buf.data(), avail, &read) || read == 0) break;
                    if (fh != INVALID_HANDLE_VALUE) { DWORD wr = 0; WriteFile(fh, buf.data(), read, &wr, nullptr); }
                    else if (body) body->append(buf.data(), read);
                    got += read;
                    if (progress) progress(stage, total > 0 ? (int)(got * 100 / total) : -1, "");
                } while (avail > 0);
            } else {
                localErr = "cannot open destination file";
            }
        } else {
            localErr = "HTTP status != 200";
        }
    } else {
        localErr = "request failed";
    }

    if (fh != INVALID_HANDLE_VALUE) CloseHandle(fh);
    if (req)  WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    if (sess) WinHttpCloseHandle(sess);
    if (!ok && err && err->empty()) *err = localErr.empty() ? "fetch failed" : localErr;
    return ok;
}

// ---- Updater ---------------------------------------------------------------

Updater::Updater(std::string owner, std::string repo)
    : m_owner(std::move(owner)), m_repo(std::move(repo)) {}

std::string Updater::manifestUrl() const {
    return "https://github.com/" + m_owner + "/" + m_repo +
           "/releases/latest/download/" + kManifestName;
}

int Updater::compareVersions(const std::string& a, const std::string& b) {
    auto parse = [](std::string s) {
        if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
        std::vector<int> v; std::stringstream ss(s); std::string tok;
        while (std::getline(ss, tok, '.')) {
            try { v.push_back(std::stoi(tok)); } catch (...) { v.push_back(0); }
        }
        return v;
    };
    std::vector<int> va = parse(a), vb = parse(b);
    size_t n = (std::max)(va.size(), vb.size());
    for (size_t i = 0; i < n; ++i) {
        int x = i < va.size() ? va[i] : 0;
        int y = i < vb.size() ? vb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

UpdateInfo Updater::check() {
    UpdateInfo info;
    info.currentVersion = NG_VERSION;

    std::string body, err;
    if (!httpFetch(widen(manifestUrl()), &body, L"", {}, UpdateStage::Checking, &err)) {
        info.error = err.empty() ? "manifest fetch failed" : err;
        return info;
    }
    info.latestVersion = jsonField(body, "version");
    info.url    = jsonField(body, "url");
    info.sha256 = jsonField(body, "sha256");
    info.notes  = jsonField(body, "notes");
    std::string sz = jsonField(body, "size");
    if (!sz.empty()) { try { info.size = std::stoll(sz); } catch (...) {} }

    if (info.latestVersion.empty()) { info.error = "manifest missing version"; return info; }
    info.available = compareVersions(info.latestVersion, info.currentVersion) > 0;
    return info;
}

std::string Updater::download(const UpdateInfo& info, const ProgressFn& progress) {
    if (info.url.empty()) return {};

    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dest = std::wstring(tmp) + L"NeuralGuard-Setup-" +
                        widen(info.latestVersion) + L".exe";

    if (progress) progress(UpdateStage::Downloading, 0, "Downloading installer...");
    std::string err;
    if (!httpFetch(widen(info.url), nullptr, dest, progress, UpdateStage::Downloading, &err)) {
        if (progress) progress(UpdateStage::Failed, -1, "Download failed");
        return {};
    }

    // Verify size when known.
    if (info.size > 0) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(dest.c_str(), GetFileExInfoStandard, &fa)) {
            long long actual = ((long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
            if (actual != info.size) {
                DeleteFileW(dest.c_str());
                if (progress) progress(UpdateStage::Failed, -1, "Size mismatch");
                return {};
            }
        }
    }
    // Verify SHA-256 when known.
    if (!info.sha256.empty()) {
        if (progress) progress(UpdateStage::Verifying, -1, "Verifying...");
        std::string got = sha256File(dest);
        if (got.empty() || !iequals(got, info.sha256)) {
            DeleteFileW(dest.c_str());
            if (progress) progress(UpdateStage::Failed, -1, "Checksum mismatch");
            return {};
        }
    }
    return narrow(dest);
}

bool Updater::apply(const std::string& installerPath, const ProgressFn& progress) {
    if (progress) progress(UpdateStage::Launching, -1, "Launching installer...");

    // The installer is per-user (PrivilegesRequired=lowest) and elevates its own
    // service/uninstall steps via ShellExec, so we launch it WITHOUT forcing UAC
    // here. /VERYSILENT skips the wizard; /SUPPRESSMSGBOXES answers any prompt.
    // TODO(scaffold): Inno's [Setup] needs CloseApplications / restart-manager so
    // the running dashboard's files unlock the instant we exit (avoid a race).
    SHELLEXECUTEINFOW sei{}; sei.cbSize = sizeof(sei);
    sei.fMask  = SEE_MASK_NOASYNC;
    sei.lpVerb = nullptr;                     // default "open" - no forced elevation
    std::wstring file = widen(installerPath);
    sei.lpFile = file.c_str();
    sei.lpParameters = L"/VERYSILENT /NORESTART /SUPPRESSMSGBOXES";
    sei.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        if (progress) progress(UpdateStage::Failed, -1, "Could not launch installer");
        return false;
    }
    if (progress) progress(UpdateStage::Done, 100, "Installer launched; exit to finish updating.");
    return true;
}

bool Updater::run(const ProgressFn& progress) {
    UpdateInfo info = check();
    if (!info.error.empty() || !info.available) return false;
    std::string path = download(info, progress);
    if (path.empty()) return false;
    return apply(path, progress);
}

} // namespace ng
