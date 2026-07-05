// Process identity: turns a raw WFP appId (an NT device path like
// \device\harddiskvolume3\...\app.exe) into a stable, de-duplicated identity -
// normalized path + SHA-256 + Authenticode signer - stored once in the
// process_identity table and cached in memory.
#pragma once

#include <mutex>
#include <string>
#include <unordered_map>

namespace ng {

class Db;

class IdentityResolver {
public:
    explicit IdentityResolver(Db& db) : db_(db) {}

    void init();   // build the \Device\.. -> drive-letter map (call once)

    // Resolve a device-path appId to its process_identity row id (cached).
    // Returns -1 on failure.
    long long resolve(const std::string& devicePath);

private:
    std::string normalize(const std::string& devPath) const;

    Db& db_;
    std::mutex cacheMutex_;
    std::unordered_map<std::string, long long> cache_;   // device path -> row id
    std::unordered_map<std::string, std::string> devMap_;  // lower(\Device\..) -> "C:"
};

}  // namespace ng
