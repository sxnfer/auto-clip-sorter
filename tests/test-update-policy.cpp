#include "update-policy.hpp"
#include "version.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

using namespace clip_atlas;

namespace {

constexpr std::int64_t kNow = 1800000000; // a Unix time in 2027
constexpr std::int64_t kHour = 60 * 60;
constexpr std::int64_t kDay = 24 * kHour;
constexpr std::int64_t kMaxTime = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kMinTime = std::numeric_limits<std::int64_t>::min();

const std::string kTrustedPrefix = "https://github.com/sxnfer/auto-clip-sorter/releases/";

// The answer GitHub gives for a published release: its tag and its page, as the release page would link them.
ReleaseInfo published(const std::string &tag)
{
	ReleaseInfo release;
	release.tag_name = tag;
	release.html_url = kTrustedPrefix + "tag/" + tag;
	return release;
}

ReleaseInfo draft_of(const std::string &tag)
{
	ReleaseInfo release = published(tag);
	release.draft = true;
	return release;
}

ReleaseInfo prerelease_of(const std::string &tag)
{
	ReleaseInfo release = published(tag);
	release.prerelease = true;
	return release;
}

void expect_failed(const UpdateResult &result, const std::string &error)
{
	CHECK(result.outcome == UpdateOutcome::failed);
	CHECK(result.error == error);
	CHECK(result.latest_version.empty());
	CHECK(result.download_url.empty());
}

void expect_no_release(const UpdateResult &result)
{
	CHECK(result.outcome == UpdateOutcome::no_release);
	CHECK(result.latest_version.empty());
	CHECK(result.download_url.empty());
	CHECK(result.error.empty());
}

void expect_up_to_date(const UpdateResult &result, const std::string &latest)
{
	CHECK(result.outcome == UpdateOutcome::up_to_date);
	CHECK(result.latest_version == latest);
	CHECK(result.download_url.empty());
	CHECK(result.error.empty());
}

void expect_update(const UpdateResult &result, const std::string &latest, const std::string &url)
{
	CHECK(result.outcome == UpdateOutcome::update_available);
	CHECK(result.latest_version == latest);
	CHECK(result.download_url == url);
	CHECK(result.error.empty());
}

} // namespace

TEST_CASE("the endpoint and the releases page are the repository's")
{
	CHECK(std::string(kLatestReleaseApiUrl) ==
	      "https://api.github.com/repos/sxnfer/auto-clip-sorter/releases/latest");
	CHECK(std::string(kReleasesPageUrl) == "https://github.com/sxnfer/auto-clip-sorter/releases");
	CHECK(kUpdateCheckIntervalSeconds == 86400);
}

TEST_CASE("update_check_due: a check is due when none was ever made")
{
	CHECK(update_check_due(std::nullopt, 0));
	CHECK(update_check_due(std::nullopt, kNow));
	CHECK(update_check_due(std::nullopt, kMinTime));
	CHECK(update_check_due(std::nullopt, kMaxTime));
}

TEST_CASE("update_check_due: a check is due once a full day has passed since the last one")
{
	CHECK_FALSE(update_check_due(kNow, kNow));
	CHECK_FALSE(update_check_due(kNow - 1, kNow));
	CHECK_FALSE(update_check_due(kNow - (kDay - 1), kNow)); // 24 h - 1 s
	CHECK(update_check_due(kNow - kDay, kNow));             // exactly 24 h
	CHECK(update_check_due(kNow - (kDay + 1), kNow));
	CHECK(update_check_due(kNow - 30 * kDay, kNow));
}

TEST_CASE("update_check_due: a stored time up to an hour ahead is kept, one more than an hour is repaired")
{
	CHECK_FALSE(update_check_due(kNow + 1, kNow));
	CHECK_FALSE(update_check_due(kNow + kHour - 1, kNow));
	CHECK_FALSE(update_check_due(kNow + kHour, kNow)); // exactly an hour is not "more than an hour"
	CHECK(update_check_due(kNow + kHour + 1, kNow));
	CHECK(update_check_due(kNow + kDay, kNow));
	CHECK(update_check_due(kNow + 10 * kDay, kNow));
}

TEST_CASE("update_check_due: the limits of the time range neither overflow nor wrap")
{
	SECTION("the first check ever at the earliest time, now at the latest")
	{
		CHECK(update_check_due(kMinTime, kMaxTime));
	}
	SECTION("a stored time at the latest value, now at the earliest: a clock set back by an enormous amount")
	{
		CHECK(update_check_due(kMaxTime, kMinTime));
		CHECK(update_check_due(kMaxTime, 0));
		CHECK(update_check_due(1000, kMinTime));
	}
	SECTION("the same instant at either end")
	{
		CHECK_FALSE(update_check_due(kMaxTime, kMaxTime));
		CHECK_FALSE(update_check_due(kMinTime, kMinTime));
	}
	SECTION("a stored time an hour ahead, at the top of the range")
	{
		CHECK_FALSE(update_check_due(kMaxTime, kMaxTime - 1));
		CHECK_FALSE(update_check_due(kMaxTime, kMaxTime - kHour));
		CHECK(update_check_due(kMaxTime, kMaxTime - kHour - 1));
	}
	SECTION("exactly a day apart, at the top of the range")
	{
		CHECK(update_check_due(kMaxTime - kDay, kMaxTime));
		CHECK(update_check_due(kMaxTime - kDay - 1, kMaxTime));
		CHECK_FALSE(update_check_due(kMaxTime - kDay + 1, kMaxTime));
	}
	SECTION("exactly a day apart, at the bottom of the range")
	{
		CHECK(update_check_due(kMinTime, kMinTime + kDay));
		CHECK_FALSE(update_check_due(kMinTime, kMinTime + kDay - 1));
	}
	SECTION("a clock set back from just above the earliest value")
	{
		CHECK_FALSE(update_check_due(kMinTime + 1, kMinTime));
		CHECK(update_check_due(kMinTime + kHour + 1, kMinTime));
	}
	SECTION("a check at the earliest time, now at zero")
	{
		CHECK(update_check_due(kMinTime, 0));
	}
}

TEST_CASE("is_trusted_release_url accepts download pages under the releases list")
{
	for (const std::string &url :
	     {kTrustedPrefix + "tag/0.2.0", kTrustedPrefix + "tag/v1.0.0-rc.1", kTrustedPrefix + "tag/v0.2.0",
	      kTrustedPrefix + "tag/a.b", kTrustedPrefix + "tag/a_b~c", kTrustedPrefix + "tag/a_b~c.d/",
	      kTrustedPrefix + "latest", kTrustedPrefix + "a"}) {
		INFO(url);
		CHECK(is_trusted_release_url(url));
	}
}

TEST_CASE("is_trusted_release_url refuses the releases list itself, the bare prefix and the empty string")
{
	CHECK_FALSE(is_trusted_release_url(kReleasesPageUrl));
	CHECK_FALSE(is_trusted_release_url(kTrustedPrefix));
	CHECK_FALSE(is_trusted_release_url(""));
	// The fallback page lacks the trailing slash, so it is opened without this check, not through it.
	CHECK(is_trusted_release_url(std::string(kReleasesPageUrl) + "/tag/1.0.0"));
}

TEST_CASE("is_trusted_release_url requires the exact prefix, as written")
{
	for (const char *url : {"http://github.com/sxnfer/auto-clip-sorter/releases/tag/1.0.0",
				"HTTPS://github.com/sxnfer/auto-clip-sorter/releases/tag/1.0.0",
				"https://GitHub.com/sxnfer/auto-clip-sorter/releases/tag/1.0.0",
				"https://github.com/SXNFER/auto-clip-sorter/releases/tag/1.0.0",
				"https://github.com/sxnfer/Auto-Clip-Sorter/releases/tag/1.0.0",
				"https://github.com.evil.example/sxnfer/auto-clip-sorter/releases/tag/1.0.0",
				"https://github.com@evil.example/sxnfer/auto-clip-sorter/releases/tag/1.0.0",
				"https://github.com:443/sxnfer/auto-clip-sorter/releases/tag/1.0.0",
				"https://evil.example/https://github.com/sxnfer/auto-clip-sorter/releases/tag/1.0.0",
				"https://github.com/sxnfer/auto-clip-sorter-evil/releases/x",
				"https://github.com/sxnfer/auto-clip-sorter/releases-evil/x",
				"https://github.com/sxnfer/auto-clip-sorter/release/x",
				" https://github.com/sxnfer/auto-clip-sorter/releases/tag/1.0.0",
				"https:/github.com/sxnfer/auto-clip-sorter/releases/tag/1.0.0"}) {
		INFO(url);
		CHECK_FALSE(is_trusted_release_url(url));
	}
}

TEST_CASE("is_trusted_release_url refuses '..' anywhere after the prefix")
{
	for (const std::string &url :
	     {kTrustedPrefix + "../../evil", kTrustedPrefix + "tag/..", kTrustedPrefix + "tag/a..b",
	      kTrustedPrefix + "tag/...", kTrustedPrefix + "..", kTrustedPrefix + "tag/1/../2"}) {
		INFO(url);
		CHECK_FALSE(is_trusted_release_url(url));
	}
}

TEST_CASE("is_trusted_release_url refuses queries, fragments, escapes, and whitespace after the prefix")
{
	for (const std::string &url :
	     {kTrustedPrefix + "tag/1?x=y", kTrustedPrefix + "tag/1#frag", kTrustedPrefix + "tag/%2e%2e/evil",
	      kTrustedPrefix + "tag/a@b", kTrustedPrefix + "tag/a\\b", kTrustedPrefix + "tag/a b",
	      kTrustedPrefix + "tag/a\tb", kTrustedPrefix + "tag/a\nb", kTrustedPrefix + "tag/a\rb",
	      kTrustedPrefix + "tag/1.0.0\n", kTrustedPrefix + "tag/1;rm", kTrustedPrefix + "tag/1&x"}) {
		INFO(url);
		CHECK_FALSE(is_trusted_release_url(url));
	}
}

TEST_CASE("is_trusted_release_url allows exactly [0-9A-Za-z._~/-] after the prefix")
{
	const std::string allowed = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._~/-";
	for (int code = 0; code < 256; ++code) {
		const char c = static_cast<char>(code);
		std::string url = kTrustedPrefix + "tag/1";
		url += c;
		INFO("character " << code);
		CHECK(is_trusted_release_url(url) == (allowed.find(c) != std::string::npos));
	}
}

TEST_CASE("is_trusted_release_url refuses an embedded NUL and non-ASCII text")
{
	std::string nul = kTrustedPrefix + "tag/1.0.0";
	nul.push_back('\0');
	CHECK_FALSE(is_trusted_release_url(nul));
	CHECK_FALSE(is_trusted_release_url(kTrustedPrefix + "tag/\xC3\xA9")); // "é" in UTF-8
}

TEST_CASE("is_trusted_release_url allows up to 300 bytes and no more")
{
	const std::string at_limit = kTrustedPrefix + std::string(300U - kTrustedPrefix.size(), 'a');
	REQUIRE(at_limit.size() == 300U);
	CHECK(is_trusted_release_url(at_limit));

	const std::string over = at_limit + "a";
	REQUIRE(over.size() == 301U);
	CHECK_FALSE(is_trusted_release_url(over));
}

TEST_CASE("UpdateResult and ReleaseInfo start out empty")
{
	const UpdateResult result{};
	CHECK(result.outcome == UpdateOutcome::failed);
	CHECK(result.latest_version.empty());
	CHECK(result.download_url.empty());
	CHECK(result.error.empty());

	const ReleaseInfo release{};
	CHECK(release.tag_name.empty());
	CHECK(release.html_url.empty());
	CHECK_FALSE(release.draft);
	CHECK_FALSE(release.prerelease);
}

TEST_CASE("evaluate_update: a transport error is the failure, whatever the status and the answer")
{
	expect_failed(evaluate_update("connection refused", 0, std::nullopt, "0.1.0"), "connection refused");
	expect_failed(evaluate_update("timed out", 404, published("0.2.0"), "0.1.0"), "timed out");
	expect_failed(evaluate_update("timed out", 200, published("0.2.0"), "0.1.0"), "timed out");
	expect_failed(evaluate_update("TLS failure", 500, std::nullopt, "dev"), "TLS failure");
}

TEST_CASE("evaluate_update: 404 means no release has been published yet, whatever the body says")
{
	expect_no_release(evaluate_update("", 404, std::nullopt, "0.1.0"));
	expect_no_release(evaluate_update("", 404, published("0.2.0"), "0.1.0"));
	expect_no_release(evaluate_update("", 404, published("banana"), "dev"));
	expect_no_release(evaluate_update("", 404, draft_of("0.2.0"), "0.1.0"));
}

TEST_CASE("evaluate_update: any other HTTP status is a failure that names the status")
{
	for (const int status : {0, 201, 204, 301, 304, 400, 403, 429, 500, 503}) {
		INFO("status " << status);
		expect_failed(evaluate_update("", status, std::nullopt, "0.1.0"),
			      "GitHub answered HTTP " + std::to_string(status));
	}
	// The status is judged before the body: a 500 with a usable release is still a 500.
	expect_failed(evaluate_update("", 500, published("0.2.0"), "0.1.0"), "GitHub answered HTTP 500");
}

TEST_CASE("evaluate_update: a 200 whose answer could not be read is a failure")
{
	expect_failed(evaluate_update("", 200, std::nullopt, "0.1.0"), "GitHub's answer could not be read");
	// The unreadable answer is reported before this build's version is looked at.
	expect_failed(evaluate_update("", 200, std::nullopt, "dev"), "GitHub's answer could not be read");
}

TEST_CASE("evaluate_update: a draft or a pre-release is never offered")
{
	expect_failed(evaluate_update("", 200, draft_of("9.9.9"), "0.1.0"), "GitHub returned an unpublished release");
	expect_failed(evaluate_update("", 200, prerelease_of("9.9.9"), "0.1.0"),
		      "GitHub returned an unpublished release");
	// The unpublished check comes before the tag and the version are read.
	expect_failed(evaluate_update("", 200, draft_of("banana"), "dev"), "GitHub returned an unpublished release");
}

TEST_CASE("evaluate_update: a tag that is not a version is a failure that names the tag")
{
	expect_failed(evaluate_update("", 200, published("banana"), "0.1.0"),
		      "the latest release has an unexpected tag: banana");
	expect_failed(evaluate_update("", 200, published("1.2"), "0.1.0"),
		      "the latest release has an unexpected tag: 1.2");
	expect_failed(evaluate_update("", 200, published("v"), "0.1.0"), "the latest release has an unexpected tag: v");
	expect_failed(evaluate_update("", 200, published("01.0.0"), "0.1.0"),
		      "the latest release has an unexpected tag: 01.0.0");
	expect_failed(evaluate_update("", 200, published("1.0.0-rc.01"), "0.1.0"),
		      "the latest release has an unexpected tag: 1.0.0-rc.01");
	expect_failed(evaluate_update("", 200, published(""), "0.1.0"), "the latest release has an unexpected tag: ");
}

TEST_CASE("evaluate_update: a build version that is not a version is a failure that names it")
{
	expect_failed(evaluate_update("", 200, published("0.2.0"), "dev"),
		      "this build's version dev is not a release version");
	expect_failed(evaluate_update("", 200, published("0.2.0"), ""),
		      "this build's version  is not a release version");
	// The tag is read first, so a bad tag is what gets reported when both are bad.
	expect_failed(evaluate_update("", 200, published("banana"), "dev"),
		      "the latest release has an unexpected tag: banana");
}

TEST_CASE("evaluate_update: a build version with a pre-release or build metadata is a version")
{
	expect_update(evaluate_update("", 200, published("0.2.0"), "0.1.0-dev+x"), "0.2.0",
		      kTrustedPrefix + "tag/0.2.0");
	// A release is newer than the pre-release of the same version.
	expect_update(evaluate_update("", 200, published("0.1.0"), "0.1.0-dev+x"), "0.1.0",
		      kTrustedPrefix + "tag/0.1.0");
	expect_up_to_date(evaluate_update("", 200, published("0.0.9"), "0.1.0-dev+x"), "0.0.9");
}

TEST_CASE("evaluate_update: a newer release is an update, with its version and its page")
{
	expect_update(evaluate_update("", 200, published("0.2.0"), "0.1.0"), "0.2.0", kTrustedPrefix + "tag/0.2.0");
	expect_update(evaluate_update("", 200, published("v0.2.0"), "0.1.0"), "0.2.0", kTrustedPrefix + "tag/v0.2.0");
	expect_update(evaluate_update("", 200, published("0.2.0"), "0.1.9"), "0.2.0", kTrustedPrefix + "tag/0.2.0");
	expect_update(evaluate_update("", 200, published("0.10.0"), "0.9.0"), "0.10.0", kTrustedPrefix + "tag/0.10.0");
	// A release outranks the pre-release of the same version.
	expect_update(evaluate_update("", 200, published("1.0.0"), "1.0.0-rc.1"), "1.0.0",
		      kTrustedPrefix + "tag/1.0.0");
	expect_update(evaluate_update("", 200, published("1.0.0-rc.2"), "1.0.0-rc.1"), "1.0.0-rc.2",
		      kTrustedPrefix + "tag/1.0.0-rc.2");
	// The version offered is written without "v", pre-release kept, build metadata dropped.
	expect_update(evaluate_update("", 200, published("V1.0.0-beta2"), "0.9.0"), "1.0.0-beta2",
		      kTrustedPrefix + "tag/V1.0.0-beta2");
	expect_update(evaluate_update("", 200, published("v1.0.0-beta2+build.5"), "0.9.0"), "1.0.0-beta2",
		      kReleasesPageUrl);
}

TEST_CASE("evaluate_update: an answer's own release page is offered when it is trusted")
{
	ReleaseInfo release = published("0.2.0");
	release.html_url = kTrustedPrefix + "tag/v0.2.0-special.page";
	expect_update(evaluate_update("", 200, release, "0.1.0"), "0.2.0", kTrustedPrefix + "tag/v0.2.0-special.page");
}

TEST_CASE("evaluate_update: an untrusted release page falls back to the releases list")
{
	for (const char *html_url :
	     {"http://evil.example/", "https://evil.example/", "",
	      "https://github.com/sxnfer/auto-clip-sorter/releases",
	      "https://github.com/sxnfer/auto-clip-sorter/releases/tag/1.0.0?x=y",
	      "https://github.com/sxnfer/auto-clip-sorter/releases/../../evil", "HTTPS://github.com/x"}) {
		INFO("html_url " << html_url);
		ReleaseInfo release = published("0.2.0");
		release.html_url = html_url;
		expect_update(evaluate_update("", 200, release, "0.1.0"), "0.2.0", kReleasesPageUrl);
	}
}

TEST_CASE("evaluate_update: up to date when the latest release is not newer than this build")
{
	expect_up_to_date(evaluate_update("", 200, published("0.1.0"), "0.1.0"), "0.1.0");
	expect_up_to_date(evaluate_update("", 200, published("0.1.0"), "0.2.0"), "0.1.0");
	expect_up_to_date(evaluate_update("", 200, published("v0.2.0+build"), "0.2.0"), "0.2.0");
	expect_up_to_date(evaluate_update("", 200, published("1.0.0-rc.1"), "1.0.0-rc.1"), "1.0.0-rc.1");
	expect_up_to_date(evaluate_update("", 200, published("1.0.0-rc.1"), "1.0.0"), "1.0.0-rc.1");
	expect_up_to_date(evaluate_update("", 200, published("0.2.0-beta2"), "0.2.0"), "0.2.0-beta2");
	expect_up_to_date(evaluate_update("", 200, published("0.1.0"), "0.1.0+build"), "0.1.0");
}

TEST_CASE("evaluate_update: the first problem found is the one reported")
{
	SECTION("a transport error, before anything else")
	{
		expect_failed(evaluate_update("x", 404, std::nullopt, "dev"), "x");
	}
	SECTION("a 404, before the body and this build's version")
	{
		expect_no_release(evaluate_update("", 404, std::nullopt, "dev"));
	}
	SECTION("the status, before the body")
	{
		expect_failed(evaluate_update("", 500, std::nullopt, "dev"), "GitHub answered HTTP 500");
	}
	SECTION("an unreadable answer, before the unpublished check")
	{
		expect_failed(evaluate_update("", 200, std::nullopt, "dev"), "GitHub's answer could not be read");
	}
	SECTION("the unpublished check, before the tag")
	{
		expect_failed(evaluate_update("", 200, draft_of("banana"), "dev"),
			      "GitHub returned an unpublished release");
	}
	SECTION("the tag, before this build's version")
	{
		expect_failed(evaluate_update("", 200, published("banana"), "dev"),
			      "the latest release has an unexpected tag: banana");
	}
	SECTION("this build's version, when everything else is fine")
	{
		expect_failed(evaluate_update("", 200, published("0.2.0"), "dev"),
			      "this build's version dev is not a release version");
	}
}

TEST_CASE("evaluate_update offers an update exactly when is_newer_version says the tag is newer")
{
	const char *const tags[] = {"0.1.0", "0.1.1",      "0.2.0",       "v1.0.0-rc.1",
				    "1.0.0", "1.0.0-rc.2", "0.1.0+build", "banana"};
	const char *const currents[] = {"0.1.0", "0.1.1-rc.1", "1.0.0-rc.1", "1.0.0", "0.0.1", "dev"};
	for (const char *tag : tags) {
		for (const char *current : currents) {
			INFO(tag << " against " << current);
			const UpdateResult result = evaluate_update("", 200, published(tag), current);
			CHECK((result.outcome == UpdateOutcome::update_available) == is_newer_version(tag, current));
		}
	}
}

namespace {

const std::string kDigestHex = "55e1b8e0e98b4526269325853d056e408a3d6643b2263f09c84197368d19693b";

// The installer asset GitHub lists for release `tag` of version `version`, as in the 1.1.2 release.
ReleaseAsset installer_asset(const std::string &tag, const std::string &version)
{
	ReleaseAsset asset;
	asset.name = "clip-atlas-" + version + "-windows-x64-installer.exe";
	asset.download_url = kTrustedPrefix + "download/" + tag + "/" + asset.name;
	asset.size = 4476588;
	asset.state = "uploaded";
	asset.digest = "sha256:" + kDigestHex;
	return asset;
}

// A published release with the files the release job uploads: the zip and source first, then the installer.
ReleaseInfo with_installer(const std::string &tag, const std::string &version)
{
	ReleaseInfo release = published(tag);
	ReleaseAsset zip = installer_asset(tag, version);
	zip.name = "clip-atlas-" + version + "-windows-x64.zip";
	zip.download_url = kTrustedPrefix + "download/" + tag + "/" + zip.name;
	release.assets.push_back(zip);
	release.assets.push_back(installer_asset(tag, version));
	return release;
}

} // namespace

TEST_CASE("installer_file_name is the name Package-Windows.ps1 gives the installer")
{
	CHECK(installer_file_name("1.2.0") == "clip-atlas-1.2.0-windows-x64-installer.exe");
	CHECK(installer_file_name("2.0.0-rc.1") == "clip-atlas-2.0.0-rc.1-windows-x64-installer.exe");
}

TEST_CASE("the installer runs without its wizard and waits for OBS")
{
	CHECK(std::string(kInstallerParameters) == "/SILENT /SP- /NORESTART /WAITOBS");
}

TEST_CASE("sha256_from_digest reads GitHub's sha256 digests, in lowercase")
{
	CHECK(sha256_from_digest("sha256:" + kDigestHex) == kDigestHex);
	CHECK(sha256_from_digest("sha256:55E1B8E0E98B4526269325853D056E408A3D6643B2263F09C84197368D19693B") ==
	      kDigestHex);
}

TEST_CASE("sha256_from_digest refuses every other form")
{
	CHECK_FALSE(sha256_from_digest(""));
	CHECK_FALSE(sha256_from_digest("sha256:"));
	CHECK_FALSE(sha256_from_digest(kDigestHex));                             // no algorithm
	CHECK_FALSE(sha256_from_digest("SHA256:" + kDigestHex));                 // the prefix is exact
	CHECK_FALSE(sha256_from_digest("sha512:" + kDigestHex));                 // another algorithm
	CHECK_FALSE(sha256_from_digest("sha256:" + kDigestHex.substr(1)));       // 63 digits
	CHECK_FALSE(sha256_from_digest("sha256:" + kDigestHex + "0"));           // 65 digits
	CHECK_FALSE(sha256_from_digest("sha256:" + kDigestHex.substr(1) + "g")); // not hex
	CHECK_FALSE(sha256_from_digest(" sha256:" + kDigestHex));                // whitespace
	CHECK_FALSE(sha256_from_digest("sha256:" + kDigestHex.substr(1) + " ")); // whitespace
	CHECK_FALSE(sha256_from_digest("sha256:" + kDigestHex.substr(1) + std::string(1, '\0')));
}

TEST_CASE("find_installer picks the release's installer and builds its address")
{
	const ReleaseInfo release = with_installer("1.2.0", "1.2.0");
	const std::optional<InstallerDownload> installer = find_installer(release, "1.2.0");
	REQUIRE(installer);
	CHECK(installer->url == kTrustedPrefix + "download/1.2.0/clip-atlas-1.2.0-windows-x64-installer.exe");
	CHECK(installer->file_name == "clip-atlas-1.2.0-windows-x64-installer.exe");
	CHECK(installer->size == 4476588);
	CHECK(installer->sha256 == kDigestHex);
}

TEST_CASE("find_installer works for a tag with a leading v")
{
	const std::optional<InstallerDownload> installer = find_installer(with_installer("v1.2.0", "1.2.0"), "1.2.0");
	REQUIRE(installer);
	CHECK(installer->url == kTrustedPrefix + "download/v1.2.0/clip-atlas-1.2.0-windows-x64-installer.exe");
}

TEST_CASE("find_installer finds nothing without a usable installer")
{
	SECTION("no assets")
	{
		CHECK_FALSE(find_installer(published("1.2.0"), "1.2.0"));
	}
	SECTION("the installer of another version")
	{
		CHECK_FALSE(find_installer(with_installer("1.2.0", "1.1.9"), "1.2.0"));
	}
	SECTION("an empty version")
	{
		CHECK_FALSE(find_installer(with_installer("1.2.0", "1.2.0"), ""));
	}
	SECTION("a file still uploading")
	{
		ReleaseInfo release = with_installer("1.2.0", "1.2.0");
		release.assets.back().state = "starter";
		CHECK_FALSE(find_installer(release, "1.2.0"));
	}
	SECTION("no digest, or one that is not sha256")
	{
		ReleaseInfo release = with_installer("1.2.0", "1.2.0");
		release.assets.back().digest = "";
		CHECK_FALSE(find_installer(release, "1.2.0"));
		release.assets.back().digest = "md5:0123456789abcdef0123456789abcdef";
		CHECK_FALSE(find_installer(release, "1.2.0"));
	}
	SECTION("a size of zero, below zero, or over the cap")
	{
		ReleaseInfo release = with_installer("1.2.0", "1.2.0");
		release.assets.back().size = 0;
		CHECK_FALSE(find_installer(release, "1.2.0"));
		release.assets.back().size = -1;
		CHECK_FALSE(find_installer(release, "1.2.0"));
		release.assets.back().size = kMaxInstallerBytes + 1;
		CHECK_FALSE(find_installer(release, "1.2.0"));
		release.assets.back().size = kMaxInstallerBytes;
		CHECK(find_installer(release, "1.2.0"));
	}
}

TEST_CASE("find_installer only accepts the repository's own download address for the file")
{
	const std::string name = "clip-atlas-1.2.0-windows-x64-installer.exe";
	for (const std::string &url : {
		     std::string("https://evil.example/") + name,
		     "http://github.com/sxnfer/auto-clip-sorter/releases/download/1.2.0/" + name,
		     "https://github.com/someone-else/auto-clip-sorter/releases/download/1.2.0/" + name,
		     kTrustedPrefix + "download/1.1.0/" + name,
		     kTrustedPrefix + "download/1.2.0/" + name + "?x=1",
		     kTrustedPrefix + "download/1.2.0/other/" + name,
		     std::string(""),
	     }) {
		ReleaseInfo release = with_installer("1.2.0", "1.2.0");
		release.assets.back().download_url = url;
		CAPTURE(url);
		CHECK_FALSE(find_installer(release, "1.2.0"));
	}
}

TEST_CASE("find_installer refuses a tag that could leave its path segment")
{
	for (const char *tag : {"", "1.2.0/..", "..", "a/b", "1.2.0?x", "1.2.0#x", "1.2 0", "1.2.0%2F"}) {
		ReleaseInfo release = with_installer("1.2.0", "1.2.0");
		release.tag_name = tag;
		release.assets.back().download_url =
			kTrustedPrefix + "download/" + tag + "/" + release.assets.back().name;
		CAPTURE(tag);
		CHECK_FALSE(find_installer(release, "1.2.0"));
	}
}

TEST_CASE("evaluate_update attaches the installer to an update, and only to an update")
{
	SECTION("a newer release with an installer")
	{
		const UpdateResult result = evaluate_update("", 200, with_installer("1.2.0", "1.2.0"), "1.1.2");
		expect_update(result, "1.2.0", kTrustedPrefix + "tag/1.2.0");
		REQUIRE(result.installer);
		CHECK(result.installer->file_name == "clip-atlas-1.2.0-windows-x64-installer.exe");
	}
	SECTION("a v-prefixed tag still finds the installer named after the bare version")
	{
		const UpdateResult result = evaluate_update("", 200, with_installer("v1.2.0", "1.2.0"), "1.1.2");
		REQUIRE(result.installer);
		CHECK(result.installer->url ==
		      kTrustedPrefix + "download/v1.2.0/clip-atlas-1.2.0-windows-x64-installer.exe");
	}
	SECTION("a newer release without one still offers its page")
	{
		const UpdateResult result = evaluate_update("", 200, published("1.2.0"), "1.1.2");
		expect_update(result, "1.2.0", kTrustedPrefix + "tag/1.2.0");
		CHECK_FALSE(result.installer);
	}
	SECTION("the same release is not an update")
	{
		const UpdateResult result = evaluate_update("", 200, with_installer("1.1.2", "1.1.2"), "1.1.2");
		expect_up_to_date(result, "1.1.2");
		CHECK_FALSE(result.installer);
	}
}

TEST_CASE("is_installer_location accepts the plugin where the installer puts it")
{
	const std::string dll = "C:\\ProgramData\\obs-studio\\plugins\\clip-atlas\\bin\\64bit\\clip-atlas.dll";
	CHECK(is_installer_location(dll, "C:\\ProgramData"));
	CHECK(is_installer_location(dll, "C:\\ProgramData\\"));
	CHECK(is_installer_location(dll, "c:/programdata/"));
	CHECK(is_installer_location("C:/ProgramData/obs-studio/plugins/clip-atlas/bin/64bit/clip-atlas.dll",
				    "C:\\ProgramData"));
	CHECK(is_installer_location("D:\\Data\\OBS-Studio\\Plugins\\Clip-Atlas\\bin\\64bit\\clip-atlas.dll",
				    "D:\\Data"));
}

TEST_CASE("is_installer_location refuses every other place")
{
	const std::string program_data = "C:\\ProgramData";
	CHECK_FALSE(is_installer_location("C:\\Program Files\\obs-studio\\obs-plugins\\64bit\\clip-atlas.dll",
					  program_data));
	CHECK_FALSE(is_installer_location(
		"C:\\Users\\San\\AppData\\Roaming\\obs-studio\\plugins\\clip-atlas\\bin\\64bit\\clip-atlas.dll",
		program_data));
	CHECK_FALSE(is_installer_location("C:\\ProgramData\\obs-studio\\plugins\\clip-atlas-old\\bin\\clip-atlas.dll",
					  program_data));
	CHECK_FALSE(is_installer_location("C:\\ProgramData\\obs-studio\\plugins\\clip-atlas\\", program_data));
	CHECK_FALSE(is_installer_location(
		"C:\\ProgramData\\obs-studio\\plugins\\clip-atlas\\..\\other\\bin\\64bit\\other.dll", program_data));
	CHECK_FALSE(is_installer_location("C:\\ProgramDataX\\obs-studio\\plugins\\clip-atlas\\bin\\clip-atlas.dll",
					  program_data));
	CHECK_FALSE(is_installer_location("", program_data));
	CHECK_FALSE(is_installer_location("C:\\ProgramData\\obs-studio\\plugins\\clip-atlas\\bin\\clip-atlas.dll", ""));
	CHECK_FALSE(is_installer_location("\\obs-studio\\plugins\\clip-atlas\\bin\\clip-atlas.dll", "\\"));
}
