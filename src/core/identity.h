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

// A resolved process identity. `key` is the stable habit key for the process:
// "sig:<thumbprint>" if signed (survives binary updates), else "sha:<hash>",
// else "dev:<path>". `label` is a human-readable name (signer or image basename).
struct Identity {
    long long   id = -1;   // process_identity row id (-1 if unresolved)
    std::string key;
    std::string label;
    std::string path;      // normalized C:\... image path (for WFP app-id permits)
};

class IdentityResolver {
public:
    explicit IdentityResolver(Db& db) : db_(db) {}

    void init();   // build the \Device\.. -> drive-letter map (call once)

    // Resolve a device-path appId to its identity (cached).
    Identity resolve(const std::string& devicePath);

private:
    std::string normalize(const std::string& devPath) const;

    Db& db_;
    std::mutex cacheMutex_;
    std::unordered_map<std::string, Identity> cache_;      // device path -> identity
    std::unordered_map<std::string, std::string> devMap_;  // lower(\Device\..) -> "C:"
};

}  // namespace ng
