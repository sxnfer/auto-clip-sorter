#include "update-policy.hpp"

#include "version.hpp"

#include <cstddef>
#include <limits>
#include <utility>

namespace clip_atlas {

const char *const kLatestReleaseApiUrl = "https://api.github.com/repos/sxnfer/auto-clip-sorter/releases/latest";
const char *const kReleasesPageUrl = "https://github.com/sxnfer/auto-clip-sorter/releases";
const char *const kInstallerParameters = "/SILENT /SP- /NORESTART /WAITOBS";

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

// Every installer the plugin may download starts with this, then "<tag>/<file name>".
constexpr char kDownloadPrefix[] = "https://github.com/sxnfer/auto-clip-sorter/releases/download/";

constexpr char kDigestPrefix[] = "sha256:";
constexpr std::size_t kDigestPrefixBytes = sizeof(kDigestPrefix) - 1;
constexpr std::size_t kSha256HexDigits = 64;

// Where the installer puts the plugin, below ProgramData (DefaultDirName in clip-atlas.iss), in lowercase.
constexpr char kInstallerSubfolder[] = "\\obs-studio\\plugins\\clip-atlas\\";

char ascii_lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// Lowercase ASCII with every slash turned into a backslash, so two spellings of one Windows path compare equal.
std::string folded_path(const std::string &path)
{
	std::string folded;
	folded.reserve(path.size());
	for (const char c : path)
		folded.push_back(c == '/' ? '\\' : ascii_lower(c));
	return folded;
}

// A tag that can sit in one URL path segment as it is: no "/" to add a segment, no ".." to remove one.
bool is_plain_tag(const std::string &tag)
{
	if (tag.empty() || tag.find("..") != std::string::npos)
		return false;
	for (const char c : tag) {
		if (c == '/' || !is_url_path_char(c))
			return false;
	}
	return true;
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

std::string installer_file_name(const std::string &version)
{
	return "clip-atlas-" + version + "-windows-x64-installer.exe";
}

std::optional<std::string> sha256_from_digest(const std::string &digest)
{
	if (digest.size() != kDigestPrefixBytes + kSha256HexDigits ||
	    digest.compare(0, kDigestPrefixBytes, kDigestPrefix) != 0)
		return std::nullopt;
	std::string hex;
	hex.reserve(kSha256HexDigits);
	for (std::size_t i = kDigestPrefixBytes; i < digest.size(); ++i) {
		const char c = ascii_lower(digest[i]);
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
			return std::nullopt;
		hex.push_back(c);
	}
	return hex;
}

std::optional<InstallerDownload> find_installer(const ReleaseInfo &release, const std::string &version)
{
	if (!is_plain_tag(release.tag_name) || version.empty())
		return std::nullopt;
	const std::string name = installer_file_name(version);
	// Built here rather than trusted from the answer: the only address that can come out is this repository's own
	// download of this file.
	const std::string expected_url = kDownloadPrefix + release.tag_name + "/" + name;
	for (const ReleaseAsset &asset : release.assets) {
		if (asset.name != name)
			continue;
		// An asset still uploading has no final size or digest yet.
		if (asset.state != "uploaded" || asset.download_url != expected_url)
			continue;
		if (asset.size <= 0 || asset.size > kMaxInstallerBytes)
			continue;
		const std::optional<std::string> sha256 = sha256_from_digest(asset.digest);
		if (!sha256)
			continue;
		InstallerDownload installer;
		installer.url = expected_url;
		installer.file_name = name;
		installer.size = asset.size;
		installer.sha256 = *sha256;
		return installer;
	}
	return std::nullopt;
}

bool is_installer_location(const std::string &module_path, const std::string &program_data)
{
	std::string base = folded_path(program_data);
	while (!base.empty() && base.back() == '\\')
		base.pop_back();
	if (base.empty() || module_path.empty() || module_path.find("..") != std::string::npos)
		return false;
	const std::string prefix = base + kInstallerSubfolder;
	const std::string path = folded_path(module_path);
	// Something must follow the folder: the folder itself is not the plugin file.
	return path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0;
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
		result.installer = find_installer(*release, result.latest_version);
	} else {
		result.outcome = UpdateOutcome::up_to_date;
	}
	return result;
}

} // namespace clip_atlas
