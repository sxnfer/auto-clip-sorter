// Clip Atlas — version numbers, for the update check.
//
// Versions follow Semantic Versioning 2.0 (https://semver.org): MAJOR.MINOR.PATCH,
// optionally followed by "-" and a pre-release ("1.0.0-rc.1", "0.2.0-beta2") and
// "+" build metadata, which is ignored. GitHub release tags may carry a leading
// "v" ("v0.2.0"); the template's release workflow uses bare tags ("0.2.0").
//
// Pure C++17 with no OBS dependency.
#pragma once

#include <optional>
#include <string>

namespace clip_atlas {

struct Version {
	unsigned long major = 0;
	unsigned long minor = 0;
	unsigned long patch = 0;
	// Without the leading "-"; "" for a release.
	std::string prerelease;
};

// Parses "1.2.3", "v1.2.3", "V1.2.3", "1.2.3-rc.1", "1.2.3+build.5",
// "1.2.3-beta2+x". nullopt for anything else: missing parts ("1.2"), extra
// parts ("1.2.3.4"), non-digits, leading zeros in a number other than "0"
// ("01.2.3"), a number above 999999999, an empty pre-release or build part
// ("1.2.3-", "1.2.3+"), pre-release or build identifiers with characters other
// than [0-9A-Za-z-] or empty identifiers ("1.2.3-rc..1"), or surrounding
// whitespace.
std::optional<Version> parse_version(const std::string &text);

// Semantic Versioning precedence: <0, 0 or >0 like strcmp. Major, minor, patch
// numerically; then a version without a pre-release is greater than one with;
// then pre-releases by dot-separated identifiers: numeric identifiers (all
// digits) numerically, others by ASCII order, numeric lower than
// alphanumeric, and a shorter list lower when all shared ones are equal
// ("rc" < "rc.1" < "rc.2" < "rc.10").
int compare_versions(const Version &a, const Version &b);

// True if `candidate` parses and is greater than `current`. False when either
// doesn't parse.
bool is_newer_version(const std::string &candidate, const std::string &current);

// "1.2.3" or "1.2.3-rc.1": the version without a "v" or build metadata.
std::string format_version(const Version &version);

} // namespace clip_atlas
