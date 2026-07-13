#pragma once
#include <string>
#include <functional>

namespace ng {

// Result of an update check == the manifest for the latest published release.
struct UpdateInfo {
    bool        available = false;   // latest > current
    std::string currentVersion;      // NG_VERSION baked into this build
    std::string latestVersion;       // manifest "version"
    std::string url;                 // installer download URL (https)
    std::string sha256;              // expected installer hash, lowercase hex ("" = unknown)
    long long   size = 0;            // expected installer size in bytes (0 = unknown)
    std::string notes;               // release-notes URL
    std::string error;               // non-empty => the check/step failed
};

enum class UpdateStage { Checking, Downloading, Verifying, Launching, Done, Failed };

// (stage, percent 0-100 or -1 for indeterminate, human-readable message)
using ProgressFn = std::function<void(UpdateStage, int, const std::string&)>;

// In-app self-updater. Reads a tokenless JSON manifest published as an asset on
// the latest GitHub Release, downloads the Inno Setup installer, verifies it,
// and hands off to the installer (which replaces the files + optionally restarts).
//
// Shared by the CLI (ngcore -> `ngd update`) and the WinUI dashboard (the same
// .cpp is compiled straight into the gui project). Depends only on WinHTTP +
// BCrypt + std, so it builds standalone in either target with no ngcore/Db pull-in.
//
// SCAFFOLD: the network / verify / hand-off path has not been exercised against
// a real release yet - see docs/UPDATER.md for the remaining TODOs before ship.
class Updater {
public:
    explicit Updater(std::string owner = "harrisb415",
                     std::string repo  = "NeuralGuard");

    // 1. Fetch the manifest and compare to NG_VERSION. Never throws; on failure
    //    returns info with .error set and .available == false.
    UpdateInfo check();

    // 2. Download info.url to %TEMP%, verifying size + sha256 when the manifest
    //    provides them. Returns the local installer path, or "" on failure.
    std::string download(const UpdateInfo& info, const ProgressFn& progress = {});

    // 3. Launch the installer (silent), then the CALLER must exit so the
    //    installer can replace the running files. Returns false on launch failure.
    bool apply(const std::string& installerPath, const ProgressFn& progress = {});

    // check -> download -> apply. Blocks; on true the caller should exit promptly.
    bool run(const ProgressFn& progress = {});

    // https://github.com/<owner>/<repo>/releases/latest/download/<kManifestName>
    std::string manifestUrl() const;

    // semver-ish compare, tolerant of a leading 'v'. >0 if a>b, 0 equal, <0 a<b.
    static int compareVersions(const std::string& a, const std::string& b);

    static constexpr const char* kManifestName = "update-manifest.json";

private:
    std::string m_owner, m_repo;
};

} // namespace ng
