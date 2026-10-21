#include "update-policy.hpp"

#include "version.hpp"

#include <cstddef>
#include <limits>
#include <utility>

namespace clip_atlas {

const char *const kLatestReleaseApiUrl = "https://api.github.com/repos/sxnfer/auto-clip-sorter/releases/latest";
const char *const kReleasesPageUrl = "https://github.com/sxnfer/auto-clip-sorter/releases";

namespace {

constexpr std::int64_t kLargestTime = std::numeric_limits<std::int64_t>::max();

// A stored check time this far ahead of now can only come from a clock that was set forward and then back.
// Treating it as due replaces the stored time with a true one; waiting instead would skip checks until the
// clock caught up, which could take days.
constexpr std::int64_t kFutureToleranceSeconds = 60 * 60;

// Every download page the plugin may open starts with this, the release list's path with its trailing slash.
constexpr char kReleasePrefix[] = "https://github.com/sxnfer/auto-clip-sorter/releases/";
constexpr std::size_t kReleasePrefixBytes = sizeof(kReleasePrefix) - 1;

// Real release pages are much shorter; a longer answer is not one the plugin should send the user to.
constexpr std::size_t kMaxUrlBytes = 300;

// What may follow the prefix: the characters GitHub uses in tag names and paths. Nothing that can start a
// query or fragment, escape the path, or hide a space or control character.
bool is_url_path_char(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.' || c == '_' ||
	       c == '~' || c == '/' || c == '-';
}

UpdateResult failure(std::string error)
{
	UpdateResult result;
	result.outcome = UpdateOutcome::failed;
	result.error = std::move(error);
	return result;
}

} // namespace

bool update_check_due(std::optional<std::int64_t> last_check, std::int64_t now)
{
	if (!last_check)
		return true;
	const std::int64_t last = *last_check;
	if (last > now) {
		// Near the top of the range nothing can be an hour ahead of now, and now + the tolerance would
		// overflow there, so the answer is "not due".
		return now <= kLargestTime - kFutureToleranceSeconds && last > now + kFutureToleranceSeconds;
	}
	// last + the interval can only overflow near the top of the range, where it is beyond any `now`.
	return last <= kLargestTime - kUpdateCheckIntervalSeconds && now >= last + kUpdateCheckIntervalSeconds;
}

bool is_trusted_release_url(const std::string &url)
{
	// Nothing after the prefix means the release list, not a release, so it is not a download page.
	if (url.size() <= kReleasePrefixBytes || url.size() > kMaxUrlBytes)
		return false;
	if (url.compare(0, kReleasePrefixBytes, kReleasePrefix) != 0)
		return false;
	for (std::size_t i = kReleasePrefixBytes; i < url.size(); ++i) {
		if (!is_url_path_char(url[i]))
			return false;
	}
	// ".." would climb out of the releases directory on any server that resolves dot segments.
	return url.find("..", kReleasePrefixBytes) == std::string::npos;
}

UpdateResult evaluate_update(const std::string &transport_error, int status, const std::optional<ReleaseInfo> &release,
			     const std::string &current_version)
{
	// The order decides which problem is reported when several apply: with no answer there is no status to
	// read, and a 404 means "no release yet" whatever the body holds.
	if (!transport_error.empty())
		return failure(transport_error);
	if (status == 404) {
		UpdateResult result;
		result.outcome = UpdateOutcome::no_release;
		return result;
	}
	if (status != 200)
		return failure("GitHub answered HTTP " + std::to_string(status));
	if (!release)
		return failure("GitHub's answer could not be read");
	// The endpoint never returns these; if one arrives anyway, offering it would announce an unpublished build.
	if (release->draft || release->prerelease)
		return failure("GitHub returned an unpublished release");
	const std::optional<Version> latest = parse_version(release->tag_name);
	if (!latest)
		return failure("the latest release has an unexpected tag: " + release->tag_name);
	const std::optional<Version> installed = parse_version(current_version);
	if (!installed)
		return failure("this build's version " + current_version + " is not a release version");

	UpdateResult result;
	result.latest_version = format_version(*latest);
	if (compare_versions(*latest, *installed) > 0) {
		result.outcome = UpdateOutcome::update_available;
		// Only an answer's own page that passes the trust check is offered; anything else falls back to the
		// releases list, a constant that no answer can change.
		result.download_url = is_trusted_release_url(release->html_url) ? release->html_url
										: std::string(kReleasesPageUrl);
	} else {
		result.outcome = UpdateOutcome::up_to_date;
	}
	return result;
}

} // namespace clip_atlas
