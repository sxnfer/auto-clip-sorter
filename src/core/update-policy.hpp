// Clip Atlas — when to check GitHub for a new release, and what the answer
// means.
//
// The plugin asks GitHub's REST API for the latest published release:
//   GET https://api.github.com/repos/sxnfer/auto-clip-sorter/releases/latest
// That endpoint returns only published releases that are not pre-releases
// (drafts and pre-releases are never offered), as JSON with, among others,
// "tag_name", "html_url", "draft", "prerelease" and "assets"; it answers 404
// while no release has been published. Parsing the JSON is done by the OBS
// glue (with obs_data); this file decides what the fields mean.
//
// Each entry of "assets" is one file of the release, with "name",
// "browser_download_url", "size", "state" ("uploaded" once complete) and
// "digest" ("sha256:<hex>", which GitHub computes when the file is uploaded).
// The Windows installer among them is what the plugin downloads, checks
// against that digest, and runs once OBS has closed.
//
// Pure C++17 with no OBS dependency.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace clip_atlas {

// The API address above.
extern const char *const kLatestReleaseApiUrl;
// "https://github.com/sxnfer/auto-clip-sorter/releases"; opened when a release
// has no usable html_url.
extern const char *const kReleasesPageUrl;

// How often the automatic check runs.
constexpr std::int64_t kUpdateCheckIntervalSeconds = 24 * 60 * 60;

// Whether an automatic check is due at `now` (Unix seconds): no check yet
// (`last_check` nullopt), or at least kUpdateCheckIntervalSeconds since the
// last one, or the last one is more than an hour in the future (the clock was
// set back, so the stored time can't be trusted).
bool update_check_due(std::optional<std::int64_t> last_check, std::int64_t now);

// Whether `url` may be opened as a release's download page: it starts with
// "https://github.com/sxnfer/auto-clip-sorter/releases/" (exact, ASCII case
// as written), has nothing but [0-9A-Za-z._~/-] after that prefix (no "?",
// "#", "%", "@", "\", "..", whitespace or control characters), and is at most
// 300 bytes. A tampered or unexpected answer must never make the plugin open
// some other site.
bool is_trusted_release_url(const std::string &url);

// One entry of the answer's "assets".
struct ReleaseAsset {
	std::string name;
	std::string download_url; // "browser_download_url"
	std::int64_t size = 0;
	std::string state;  // "uploaded" once the file is complete
	std::string digest; // "sha256:<64 hex digits>"; "" when GitHub gave none
};

// The fields of the API's answer the plugin uses.
struct ReleaseInfo {
	std::string tag_name;
	std::string html_url;
	bool draft = false;
	bool prerelease = false;
	std::vector<ReleaseAsset> assets;
};

// A release's Windows installer, checked and ready to download.
struct InstallerDownload {
	std::string url;       // https://github.com/sxnfer/auto-clip-sorter/releases/download/<tag>/<file_name>
	std::string file_name; // installer_file_name() of the release's version
	std::int64_t size = 0; // bytes, in (0, kMaxInstallerBytes]
	std::string sha256;    // 64 lowercase hex digits
};

// The installer is a few MB. A size far beyond that is not one the plugin should download.
constexpr std::int64_t kMaxInstallerBytes = 256 * 1024 * 1024;

// What Package-Windows.ps1 names the installer: "clip-atlas-<version>-windows-x64-installer.exe".
std::string installer_file_name(const std::string &version);

// The 64 hex digits of a "sha256:<hex>" digest, in lowercase. nullopt for any other form: another algorithm, the wrong
// length, a non-hex character, or surrounding whitespace.
std::optional<std::string> sha256_from_digest(const std::string &digest);

// The installer for `version` among `release`'s assets: the one named installer_file_name(version), in state
// "uploaded", with a size in (0, kMaxInstallerBytes], a sha256 digest, and a download URL that is exactly
// "https://github.com/sxnfer/auto-clip-sorter/releases/download/<tag_name>/<name>". The tag must be made of
// [0-9A-Za-z._~-] with no "..", so it can't leave its path segment. nullopt when no asset passes: the plugin then
// offers the release page instead.
std::optional<InstallerDownload> find_installer(const ReleaseInfo &release, const std::string &version);

// Whether the plugin file at `module_path` is where the installer puts it:
// "<program_data>\obs-studio\plugins\clip-atlas\...". Compared without regard to ASCII case or the kind of slash,
// with trailing slashes on `program_data` ignored. False when either is empty or the module path holds "..". A copy
// installed any other way (the zip in OBS's own folder, or in the user's profile) must not be updated by the
// installer, which would leave OBS loading two copies.
bool is_installer_location(const std::string &module_path, const std::string &program_data);

// The command line the plugin runs the installer with (see installer/clip-atlas.iss): no wizard pages, only the
// progress window, no restart, and /WAITOBS so it waits for OBS to finish closing first.
extern const char *const kInstallerParameters;

enum class UpdateOutcome {
	up_to_date,       // the latest release is not newer than this version
	update_available, // a newer release exists
	no_release,       // nothing published yet (HTTP 404)
	failed,           // no answer, an HTTP error, or an answer we can't use
};

struct UpdateResult {
	UpdateOutcome outcome = UpdateOutcome::failed;
	std::string latest_version; // format_version() of the tag; "" unless up_to_date/update_available
	std::string download_url;   // html_url if trusted, else kReleasesPageUrl; "" unless update_available
	std::string error;          // English, for the log and a message box; "" unless failed
	// find_installer() of the release; only ever set for update_available.
	std::optional<InstallerDownload> installer;
};

// Decides the outcome from the HTTP answer:
//   - transport_error not empty → failed with that error.
//   - status 404 → no_release.
//   - status other than 200 → failed ("GitHub answered HTTP <status>").
//   - status 200 but `release` nullopt (the JSON couldn't be read) → failed
//     ("GitHub's answer could not be read").
//   - a draft or pre-release (the endpoint shouldn't return them) → failed
//     ("GitHub returned an unpublished release").
//   - tag_name doesn't parse (parse_version) → failed ("the latest release
//     has an unexpected tag: <tag>").
//   - current_version doesn't parse → failed ("this build's version <v> is
//     not a release version").
//   - tag newer than current → update_available, with `installer` from
//     find_installer(release, latest_version); otherwise up_to_date.
UpdateResult evaluate_update(const std::string &transport_error, int status, const std::optional<ReleaseInfo> &release,
			     const std::string &current_version);

} // namespace clip_atlas
