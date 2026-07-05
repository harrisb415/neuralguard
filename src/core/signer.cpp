#include "core/signer.h"
#include "core/util.h"

#include <windows.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <mscat.h>

#include <vector>

namespace ng {
namespace {

// Extract the signer subject + SHA1 thumbprint from an EMBEDDED PKCS#7 signature
// in `path` (a PE image or a .cat catalog file).
bool GetSignerEmbedded(const std::wstring& path, std::string& subject, std::string& thumb) {
    HCERTSTORE store = nullptr; HCRYPTMSG msg = nullptr;
    DWORD enc = 0, ct = 0, ft = 0;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, path.c_str(),
                          CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                          CERT_QUERY_FORMAT_FLAG_BINARY, 0,
                          &enc, &ct, &ft, &store, &msg, nullptr))
        return false;
    bool ok = false;
    DWORD cb = 0;
    if (CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &cb) && cb) {
        std::vector<unsigned char> si(cb);
        if (CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, si.data(), &cb)) {
            CMSG_SIGNER_INFO* info = (CMSG_SIGNER_INFO*)si.data();
            CERT_INFO ci{}; ci.Issuer = info->Issuer; ci.SerialNumber = info->SerialNumber;
            PCCERT_CONTEXT cert = CertFindCertificateInStore(
                store, enc, 0, CERT_FIND_SUBJECT_CERT, &ci, nullptr);
            if (cert) {
                char name[256] = {};
                CertGetNameStringA(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr,
                                   name, sizeof(name));
                subject = name;
                unsigned char hash[20]; DWORD hn = sizeof(hash);
                if (CertGetCertificateContextProperty(cert, CERT_HASH_PROP_ID, hash, &hn))
                    thumb = util::ToHex(hash, hn);
                ok = true;
                CertFreeCertificateContext(cert);
            }
        }
    }
    if (msg) CryptMsgClose(msg);
    if (store) CertCloseStore(store, 0);
    return ok;
}

// Most in-box Windows binaries have no embedded signature - they are signed by a
// security *catalog*. Find the catalog that vouches for this file's hash and read
// the signer from it (catalogs are themselves embedded-signed .cat files).
bool GetSignerCatalog(const std::wstring& path, std::string& subject, std::string& thumb) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool ok = false;
    HCATADMIN admin = nullptr;
    // Modern Win11 catalogs use SHA-256.
    if (CryptCATAdminAcquireContext2(&admin, nullptr, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) {
        DWORD hlen = 0;
        CryptCATAdminCalcHashFromFileHandle2(admin, file, &hlen, nullptr, 0);
        if (hlen) {
            std::vector<BYTE> hash(hlen);
            if (CryptCATAdminCalcHashFromFileHandle2(admin, file, &hlen, hash.data(), 0)) {
                HCATINFO cat = CryptCATAdminEnumCatalogFromHash(admin, hash.data(), hlen, 0, nullptr);
                if (cat) {
                    CATALOG_INFO ci{}; ci.cbStruct = sizeof(ci);
                    if (CryptCATCatalogInfoFromContext(cat, &ci, 0))
                        ok = GetSignerEmbedded(ci.wszCatalogFile, subject, thumb);
                    CryptCATAdminReleaseCatalogContext(admin, cat, 0);
                }
            }
        }
        CryptCATAdminReleaseContext(admin, 0);
    }
    CloseHandle(file);
    return ok;
}

}  // namespace

bool GetSigner(const std::wstring& path, std::string& subject, std::string& thumb) {
    if (GetSignerEmbedded(path, subject, thumb)) return true;
    return GetSignerCatalog(path, subject, thumb);
}

}  // namespace ng
