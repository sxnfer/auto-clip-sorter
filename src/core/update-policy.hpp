// Clip Atlas — when to check GitHub for a new release, and what the answer
// means.
//
// The plugin asks GitHub's REST API for the latest published release:
//   GET https://api.github.com/repos/sxnfer/auto-clip-sorter/releases/latest
// That endpoint returns only published releases that are not pre-releases
// (drafts and pre-releases are never offered), as JSON with, among others,
// "tag_name", "html_url", "draft" and "prerelease"; it answers 404 while no
// release has been published. Parsing the JSON is done by the OBS glue (with
// obs_data); this file decides what the fields mean.
//
// Pure C++17 with no OBS dependency.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

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

// The fields of the API's answer the plugin uses.
struct ReleaseInfo {
	std::string tag_name;
	std::string html_url;
	bool draft = false;
	bool prerelease = false;
};

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
//   - tag newer than current → update_available; otherwise up_to_date.
UpdateResult evaluate_update(const std::string &transport_error, int status, const std::optional<ReleaseInfo> &release,
			     const std::string &current_version);

} // namespace clip_atlas
