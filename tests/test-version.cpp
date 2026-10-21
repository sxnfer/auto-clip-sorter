#include "version.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <iterator>
#include <optional>
#include <string>

using namespace clip_atlas;

namespace {

// The characters an identifier may use, semver 2.0 §9 and §10. Written out here so the test does not depend on the
// code it checks.
const std::string kIdentifierChars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-";

// The parsed form of a text the test expects to be a valid version.
Version must_parse(const std::string &text)
{
	const std::optional<Version> version = parse_version(text);
	REQUIRE(version.has_value());
	return *version;
}

int sign(int x)
{
	return (x > 0) - (x < 0);
}

// How `a` ranks against `b`: -1, 0 or 1. Also checks that swapping them gives the opposite answer, so every
// precedence test checks antisymmetry too.
int precedence(const std::string &a, const std::string &b)
{
	const int forward = sign(compare_versions(must_parse(a), must_parse(b)));
	const int backward = sign(compare_versions(must_parse(b), must_parse(a)));
	CHECK(backward == -forward);
	return forward;
}

} // namespace

TEST_CASE("parse_version reads MAJOR.MINOR.PATCH")
{
	const Version v = must_parse("1.2.3");
	CHECK(v.major == 1UL);
	CHECK(v.minor == 2UL);
	CHECK(v.patch == 3UL);
	CHECK(v.prerelease.empty());

	const Version zero = must_parse("0.0.0");
	CHECK(zero.major == 0UL);
	CHECK(zero.minor == 0UL);
	CHECK(zero.patch == 0UL);

	const Version largest = must_parse("999999999.999999999.999999999");
	CHECK(largest.major == 999999999UL);
	CHECK(largest.minor == 999999999UL);
	CHECK(largest.patch == 999999999UL);
}

TEST_CASE("parse_version takes one leading v or V, and drops it")
{
	CHECK(must_parse("v0.2.0").minor == 2UL);
	CHECK(must_parse("V0.2.0").minor == 2UL);
	CHECK(must_parse("v1.0.0-rc.1").prerelease == "rc.1");
	CHECK(precedence("v1.2.3", "1.2.3") == 0);
	CHECK(precedence("V1.2.3", "v1.2.3") == 0);
}

TEST_CASE("parse_version keeps the pre-release and drops the build metadata")
{
	const Version rc = must_parse("1.2.3-rc.1");
	CHECK(rc.prerelease == "rc.1");
	CHECK(rc.patch == 3UL);

	const Version beta = must_parse("0.2.0-beta2+x");
	CHECK(beta.prerelease == "beta2");
	CHECK(beta.minor == 2UL);
	CHECK(must_parse("1.2.3-beta2+x").prerelease == "beta2");

	CHECK(must_parse("1.2.3+build.5").prerelease.empty());
	CHECK(must_parse("1.2.3-rc.1+build.5").prerelease == "rc.1");

	// Examples from semver.org, §9 and §10.
	CHECK(must_parse("1.0.0-alpha").prerelease == "alpha");
	CHECK(must_parse("1.0.0-alpha+001").prerelease == "alpha");
	CHECK(must_parse("1.0.0+20130313144700").prerelease.empty());
	CHECK(must_parse("1.0.0-beta+exp.sha.5114f85").prerelease == "beta");
	CHECK(must_parse("1.0.0+21AF26D3----117B344092BD").prerelease.empty());
	CHECK(must_parse("1.0.0-x.7.z.92").prerelease == "x.7.z.92");
	CHECK(must_parse("1.0.0-x-y-z.--").prerelease == "x-y-z.--");
}

TEST_CASE("parse_version refuses anything that is not MAJOR.MINOR.PATCH with optional parts")
{
	for (const char *text : {"",
				 "v",
				 "V",
				 "vv1.2.3",
				 "vV1.2.3",
				 "banana",
				 "dev",
				 "1",
				 "1.2",
				 "1.2.3.4",
				 "1..3",
				 ".1.2",
				 "1.2.",
				 "1.2.3.",
				 "1.2.3a",
				 "a.b.c",
				 "-1.2.3",
				 "+1.2.3",
				 "1.-2.3",
				 "1.2.-3",
				 "1.2.3.-rc",
				 "1.2.3-",
				 "1.2.3+",
				 "1.2.3-+",
				 "1.2.3-rc+",
				 "1.2.3-rc..1",
				 "1.2.3-rc.",
				 "1.2.3-.rc",
				 "1.2.3-rc_1",
				 "1.2.3-rc 1",
				 "1.2.3+build..5",
				 "1.2.3+.",
				 "1.2.3+b+c",
				 "1.2.3-a+b+c"}) {
		INFO(text);
		CHECK_FALSE(parse_version(text).has_value());
	}
}

TEST_CASE("parse_version refuses surrounding and inner whitespace")
{
	for (const char *text : {" 1.2.3", "1.2.3 ", "\t1.2.3", "1.2.3\n", "1.2.3\r", " v1.2.3", "v 1.2.3", "1 .2.3",
				 "1.2 .3", "1.2.3-rc 1", "1.2.3+b c"}) {
		INFO(text);
		CHECK_FALSE(parse_version(text).has_value());
	}
}

TEST_CASE("parse_version refuses a number with a leading zero")
{
	for (const char *text : {"01.2.3", "1.02.3", "1.2.03", "00.0.0", "v01.2.3", "1.0.0-rc.01", "1.0.0-rc.00",
				 "1.0.0-00", "1.0.0-0.01"}) {
		INFO(text);
		CHECK_FALSE(parse_version(text).has_value());
	}
	// A lone "0" is a number, and a leading zero is only a problem in a number that has more digits.
	CHECK(must_parse("0.0.0").patch == 0UL);
	CHECK(must_parse("10.20.30").minor == 20UL);
}

TEST_CASE("parse_version allows leading zeros where semver does")
{
	// Only a numeric pre-release identifier may not start with a zero (semver 2.0 §9).
	CHECK(must_parse("1.0.0-0").prerelease == "0");
	CHECK(must_parse("1.0.0-0.3.7").prerelease == "0.3.7");
	CHECK(must_parse("1.0.0-01a").prerelease == "01a");
	CHECK(must_parse("1.0.0-0A").prerelease == "0A");
	// Build metadata is never compared, so its numbers may start with zeros (semver 2.0 §10).
	CHECK(must_parse("1.0.0+01").prerelease.empty());
	CHECK(must_parse("1.0.0+001.02").prerelease.empty());
}

TEST_CASE("parse_version caps MAJOR, MINOR and PATCH at 999999999")
{
	CHECK(must_parse("999999999.0.0").major == 999999999UL);
	CHECK(must_parse("0.999999999.0").minor == 999999999UL);
	CHECK(must_parse("0.0.999999999").patch == 999999999UL);

	for (const char *text : {"1000000000.0.0", "0.1000000000.0", "0.0.1000000000", "4294967295.0.0",
				 "4294967296.0.0", "18446744073709551616.0.0", "99999999999999999999999999999999.0.0",
				 "1234567890123456789012345678901234567890.1.1"}) {
		INFO(text);
		// 4294967296 wraps to 0 in a 32-bit unsigned long, which Windows uses; it must still be refused.
		CHECK_FALSE(parse_version(text).has_value());
	}
}

TEST_CASE("parse_version does not cap the digits of a pre-release identifier")
{
	const Version v = must_parse("1.0.0-99999999999999999999999999999999");
	CHECK(v.prerelease == "99999999999999999999999999999999");
}

TEST_CASE("parse_version accepts exactly the identifier characters, with the separators in their places")
{
	for (int code = 0; code < 256; ++code) {
		const char c = static_cast<char>(code);
		INFO("character " << code);

		// "." separates identifiers and "+" starts the build metadata, so both are valid here.
		std::string pre = "1.0.0-a";
		pre += c;
		pre += 'b';
		const bool pre_ok = kIdentifierChars.find(c) != std::string::npos || c == '.' || c == '+';
		CHECK(parse_version(pre).has_value() == pre_ok);

		// Build metadata has no further separator, so "+" is refused inside it.
		std::string build = "1.0.0+a";
		build += c;
		build += 'b';
		const bool build_ok = kIdentifierChars.find(c) != std::string::npos || c == '.';
		CHECK(parse_version(build).has_value() == build_ok);
	}
}

TEST_CASE("compare_versions orders MAJOR, MINOR and PATCH as numbers")
{
	CHECK(precedence("1.0.0", "2.0.0") == -1);
	CHECK(precedence("0.9.9", "1.0.0") == -1);
	CHECK(precedence("1.2.0", "1.10.0") == -1); // as numbers, not as text
	CHECK(precedence("1.0.9", "1.0.10") == -1);
	CHECK(precedence("1.0.0", "1.0.0") == 0);
	CHECK(precedence("999999999.0.0", "1.0.0") == 1);
	CHECK(precedence("999999999.999999999.999999999", "999999999.999999999.999999998") == 1);
}

TEST_CASE("compare_versions ranks a release above its own pre-releases")
{
	CHECK(precedence("1.0.0-rc.1", "1.0.0") == -1);
	CHECK(precedence("1.0.0-alpha", "1.0.0") == -1);
	CHECK(precedence("1.0.0", "1.0.0-rc.1") == 1);
	// The core is compared first, so a pre-release of a newer release is still the newer one.
	CHECK(precedence("1.0.1-alpha", "1.0.0") == 1);
	CHECK(precedence("1.0.0-rc.1", "0.9.9") == 1);
}

TEST_CASE("compare_versions follows the semver.org precedence chain")
{
	// Each entry is greater than every entry before it.
	const char *const chain[] = {"1.0.0-alpha",  "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
				     "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1",       "1.0.0"};
	const std::size_t count = std::size(chain);
	for (std::size_t i = 0; i < count; ++i) {
		CHECK(precedence(chain[i], chain[i]) == 0);
		for (std::size_t j = i + 1; j < count; ++j) {
			INFO(chain[i] << " against " << chain[j]);
			CHECK(precedence(chain[i], chain[j]) == -1);
		}
	}
}

TEST_CASE("compare_versions ranks a shorter identifier list lower when the shared identifiers are equal")
{
	CHECK(precedence("1.0.0-rc", "1.0.0-rc.1") == -1);
	CHECK(precedence("1.0.0-rc.1", "1.0.0-rc.2") == -1);
	CHECK(precedence("1.0.0-rc.2", "1.0.0-rc.10") == -1);
	CHECK(precedence("1.0.0-rc.1", "1.0.0-rc.1.1") == -1);
	// An earlier identifier that differs decides, whatever the list lengths.
	CHECK(precedence("1.0.0-rc.1.1", "1.0.0-rc.2") == -1);
}

TEST_CASE("compare_versions ranks numeric identifiers below alphanumeric ones")
{
	CHECK(precedence("1.0.0-1", "1.0.0-alpha") == -1);
	CHECK(precedence("1.0.0-99", "1.0.0-a") == -1);
	CHECK(precedence("1.0.0-alpha.1", "1.0.0-alpha.beta") == -1);
	// "1a" has a letter, so it is alphanumeric and ranks above the number 2.
	CHECK(precedence("1.0.0-1a", "1.0.0-2") == 1);
}

TEST_CASE("compare_versions compares alphanumeric identifiers in ASCII order")
{
	CHECK(precedence("1.0.0-Alpha", "1.0.0-alpha") == -1); // 'A' is before 'a'
	CHECK(precedence("1.0.0-alpha", "1.0.0-alpha1") == -1);
	CHECK(precedence("1.0.0-alpha1", "1.0.0-alpha2") == -1);
	CHECK(precedence("1.0.0-alpha2", "1.0.0-beta1") == -1);
	CHECK(precedence("1.0.0-beta", "1.0.0-rc") == -1);
	CHECK(precedence("1.0.0-rc-1", "1.0.0-rc-2") == -1);
	// A hyphen is an ordinary character: "rc-2" is one identifier, which is above "rc".
	CHECK(precedence("1.0.0-rc-2", "1.0.0-rc.1") == 1);
}

TEST_CASE("compare_versions compares numeric identifiers as numbers, however many digits they have")
{
	CHECK(precedence("1.0.0-rc.99999999999999999999999", "1.0.0-rc.100000000000000000000000") == -1);
	CHECK(precedence("1.0.0-rc.100000000000000000000000", "1.0.0-rc.99999999999999999999999") == 1);
	CHECK(precedence("1.0.0-rc.99999999999999999999999", "1.0.0-rc.99999999999999999999999") == 0);
	CHECK(precedence("1.0.0-rc.18446744073709551615", "1.0.0-rc.18446744073709551616") == -1);
	CHECK(precedence("1.0.0-rc.9999999999999999999999", "1.0.0-rc.99999999999999999999999") == -1);
}

TEST_CASE("compare_versions ignores build metadata and the v prefix")
{
	CHECK(precedence("1.0.0+a", "1.0.0+b") == 0);
	CHECK(precedence("1.0.0+a", "1.0.0") == 0);
	CHECK(precedence("1.0.0-rc.1+build.5", "1.0.0-rc.1") == 0);
	CHECK(precedence("v1.0.0", "1.0.0") == 0);
	CHECK(precedence("V1.0.0+x", "v1.0.0") == 0);
	// Build metadata does not turn a pre-release into a release.
	CHECK(precedence("1.0.0+1", "1.0.0-1") == 1);
}

TEST_CASE("compare_versions reads a hand-built Version with leading zeros as the same number")
{
	const Version padded{1UL, 0UL, 0UL, "rc.010"};
	const Version plain{1UL, 0UL, 0UL, "rc.10"};
	CHECK(compare_versions(padded, plain) == 0);
	CHECK(compare_versions(plain, padded) == 0);
}

TEST_CASE("compare_versions does not throw on pre-release text that parse_version would refuse")
{
	const Version empty_identifier{1UL, 0UL, 0UL, "rc..1"};
	const Version alone{1UL, 0UL, 0UL, "rc"};
	CHECK_NOTHROW(compare_versions(empty_identifier, alone));
	CHECK(sign(compare_versions(empty_identifier, alone)) == -sign(compare_versions(alone, empty_identifier)));
}

TEST_CASE("is_newer_version is true only for a candidate that is greater than the current version")
{
	CHECK(is_newer_version("0.2.0", "0.1.0"));
	CHECK(is_newer_version("v0.2.0", "0.1.9"));
	CHECK(is_newer_version("0.10.0", "0.9.0"));
	CHECK(is_newer_version("1.0.0", "1.0.0-rc.1"));
	CHECK(is_newer_version("1.0.0-rc.2", "1.0.0-rc.1"));
	CHECK(is_newer_version("0.1.0", "0.1.0-dev+x"));
	CHECK(is_newer_version("0.1.1", "0.1.0-dev+x"));

	CHECK_FALSE(is_newer_version("0.1.0", "0.1.0"));
	CHECK_FALSE(is_newer_version("0.1.0", "0.2.0"));
	CHECK_FALSE(is_newer_version("1.0.0-rc.1", "1.0.0"));
	CHECK_FALSE(is_newer_version("1.0.0+build", "1.0.0"));
	CHECK_FALSE(is_newer_version("V1.0.0", "v1.0.0+x"));
}

TEST_CASE("is_newer_version is false when either version does not parse")
{
	CHECK_FALSE(is_newer_version("banana", "0.1.0"));
	CHECK_FALSE(is_newer_version("0.2.0", "dev"));
	CHECK_FALSE(is_newer_version("dev", "dev"));
	CHECK_FALSE(is_newer_version("", "0.1.0"));
	CHECK_FALSE(is_newer_version("0.2.0", ""));
	CHECK_FALSE(is_newer_version("1.2", "0.1.0"));
	CHECK_FALSE(is_newer_version("0.2.0", "0.1.0-"));
	CHECK_FALSE(is_newer_version("1000000000.0.0", "0.1.0"));
	CHECK_FALSE(is_newer_version("0.2.0", "01.1.0"));
}

TEST_CASE("format_version writes MAJOR.MINOR.PATCH and the pre-release")
{
	CHECK(format_version(Version{}) == "0.0.0");
	CHECK(format_version(Version{1UL, 2UL, 3UL, ""}) == "1.2.3");
	CHECK(format_version(Version{1UL, 2UL, 3UL, "rc.1"}) == "1.2.3-rc.1");
	CHECK(format_version(Version{0UL, 2UL, 0UL, "beta2"}) == "0.2.0-beta2");
	CHECK(format_version(Version{999999999UL, 999999999UL, 999999999UL, ""}) == "999999999.999999999.999999999");
}

TEST_CASE("format_version drops the v and the build metadata")
{
	CHECK(format_version(must_parse("v0.2.0")) == "0.2.0");
	CHECK(format_version(must_parse("V1.0.0-rc.1+build.5")) == "1.0.0-rc.1");
	CHECK(format_version(must_parse("1.2.3+x")) == "1.2.3");
}

TEST_CASE("format_version writes a canonical version back unchanged")
{
	for (const char *text :
	     {"0.0.0", "0.2.0", "1.0.0-rc.1", "10.20.30-alpha.beta.1", "999999999.0.999999999-x", "1.0.0-0.3.7"}) {
		INFO(text);
		CHECK(format_version(must_parse(text)) == text);
	}
}
