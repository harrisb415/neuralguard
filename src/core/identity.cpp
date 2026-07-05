#include "core/identity.h"
#include "core/db.h"
#include "core/signer.h"
#include "core/util.h"

#include <windows.h>
#include <bcrypt.h>

#include <vector>

namespace ng {
namespace {

// SHA-256 of a file's contents, hex-encoded. "" if the file can't be read.
std::string Sha256File(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "";
    std::string result;
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        DWORD objLen = 0, hashLen = 0, cb = 0;
        BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objLen, sizeof(objLen), &cb, 0);
        BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, (PUCHAR)&hashLen, sizeof(hashLen), &cb, 0);
        std::vector<unsigned char> obj(objLen), hash(hashLen), buf(65536);
        BCRYPT_HASH_HANDLE h = nullptr;
        if (BCryptCreateHash(alg, &h, obj.data(), objLen, nullptr, 0, 0) == 0) {
            DWORD rd = 0; bool ok = true;
            while (ReadFile(f, buf.data(), (DWORD)buf.size(), &rd, nullptr) && rd > 0)
                if (BCryptHashData(h, buf.data(), rd, 0) != 0) { ok = false; break; }
            if (ok && BCryptFinishHash(h, hash.data(), hashLen, 0) == 0)
                result = util::ToHex(hash.data(), hashLen);
            BCryptDestroyHash(h);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    CloseHandle(f);
    return result;
}

}  // namespace

void IdentityResolver::init() {
    for (wchar_t c = L'A'; c <= L'Z'; ++c) {
        wchar_t drive[3] = {c, L':', 0};
        wchar_t target[1024];
        if (QueryDosDeviceW(drive, target, 1024) > 0) {
            std::string letter; letter += (char)c; letter += ':';
            devMap_[util::ToLower(util::Narrow(std::wstring(target)))] = letter;
        }
    }
}

std::string IdentityResolver::normalize(const std::string& appId) const {
    std::string low = util::ToLower(appId);
    for (const auto& kv : devMap_) {
        const std::string& dev = kv.first;  // already lowercase
        if (low.size() >= dev.size() && low.compare(0, dev.size(), dev) == 0 &&
            (low.size() == dev.size() || low[dev.size()] == '\\')) {
            return kv.second + appId.substr(dev.size());
        }
    }
    return appId;  // unmapped (e.g. \device\mup\.. network path) - leave as-is
}

Identity IdentityResolver::resolve(const std::string& devPath) {
    {
        std::lock_guard<std::mutex> lk(cacheMutex_);
        auto it = cache_.find(devPath);
        if (it != cache_.end()) return it->second;
    }
    // Heavy work (file hash + signer) done without holding any lock.
    std::string dos = normalize(devPath);
    std::wstring wdos = util::Widen(dos);
    std::string sha = Sha256File(wdos);
    std::string signer, thumb;
    bool isSigned = GetSigner(wdos, signer, thumb);
    std::string now = util::IsoNow();

    Identity idn;
    idn.path = dos;
    // Stable habit key: prefer signer thumbprint (survives binary updates), then
    // content hash, then the raw path as a last resort.
    if (isSigned)          idn.key = "sig:" + thumb;
    else if (!sha.empty()) idn.key = "sha:" + sha;
    else                   idn.key = "dev:" + devPath;
    if (isSigned) {
        idn.label = signer;
    } else {
        size_t p = dos.find_last_of('\\');
        idn.label = (p == std::string::npos) ? dos : dos.substr(p + 1);
    }

    {
        std::lock_guard<std::mutex> lk(db_.mutex());
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db_.handle(),
            "INSERT INTO process_identity"
            "(device_path,image_path,sha256,signer,signer_thumbprint,signed,first_seen,last_seen)"
            " VALUES(?,?,?,?,?,?,?,?)"
            " ON CONFLICT(device_path) DO UPDATE SET last_seen=excluded.last_seen"
            " RETURNING id;", -1, &s, nullptr);
        bindText(s, 1, devPath);
        bindText(s, 2, dos);
        if (sha.empty()) sqlite3_bind_null(s, 3); else bindText(s, 3, sha);
        if (isSigned) { bindText(s, 4, signer); bindText(s, 5, thumb); }
        else { sqlite3_bind_null(s, 4); sqlite3_bind_null(s, 5); }
        sqlite3_bind_int(s, 6, isSigned ? 1 : 0);
        bindText(s, 7, now);
        bindText(s, 8, now);
        if (sqlite3_step(s) == SQLITE_ROW) idn.id = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    {
        std::lock_guard<std::mutex> lk(cacheMutex_);
        cache_[devPath] = idn;
    }
    return idn;
}

}  // namespace ng
