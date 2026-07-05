// Authenticode signer identity extraction. Identity only - reports who signed a
// PE image (embedded signature, or via the security catalog that vouches for
// it). Does NOT verify the trust chain; that is a separate, later concern.
#pragma once

#include <string>

namespace ng {

// On success, fills `subject` (signer display name) and `thumb` (SHA1 thumbprint
// hex) and returns true. Returns false if the file is unsigned or parsing fails.
bool GetSigner(const std::wstring& path, std::string& subject, std::string& thumb);

}  // namespace ng
