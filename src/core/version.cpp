#include "version.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace clip_atlas {

namespace {

// The largest MAJOR, MINOR or PATCH. Well inside 32 bits, which is all Windows' unsigned long has, and
// parse_number checks before it multiplies, so nothing can wrap on the way to a rejected value.
constexpr unsigned long kMaxNumber = 999999999UL;

bool is_digit(char c)
{
	return c >= '0' && c <= '9';
}

// The characters a pre-release or build identifier may use (semver 2.0 §9 and §10).
bool is_identifier_char(char c)
{
	return is_digit(c) || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-';
}

// One of MAJOR, MINOR or PATCH: digits only, no leading zero unless the number is "0".
std::optional<unsigned long> parse_number(const std::string &text, std::size_t begin, std::size_t end)
{
	if (begin == end)
		return std::nullopt;
	if (end - begin > 1 && text[begin] == '0')
		return std::nullopt;
	unsigned long value = 0;
	for (std::size_t i = begin; i < end; ++i) {
		if (!is_digit(text[i]))
			return std::nullopt;
		const unsigned long digit = static_cast<unsigned long>(text[i] - '0');
		// Tested before the multiplication: an overflow there would wrap and could let a huge number through.
		if (value > (kMaxNumber - digit) / 10)
			return std::nullopt;
		value = value * 10 + digit;
	}
	return value;
}

// One identifier, text[begin, end): non-empty and made of identifier characters only. Semver forbids leading
// zeros only in numeric pre-release identifiers, so "1.0.0-rc.01" is not a version but "1.0.0+build.01" is.
bool is_valid_identifier(const std::string &text, std::size_t begin, std::size_t end, bool pre_release)
{
	if (begin == end)
		return false;
	bool all_digits = true;
	for (std::size_t i = begin; i < end; ++i) {
		if (!is_identifier_char(text[i]))
			return false;
		all_digits = all_digits && is_digit(text[i]);
	}
	return !(pre_release && all_digits && end - begin > 1 && text[begin] == '0');
}

// The dot-separated identifiers in text[begin, end). An empty identifier, such as in "rc..1", makes the whole
// list invalid, and so does an empty list.
bool is_valid_identifier_list(const std::string &text, std::size_t begin, std::size_t end, bool pre_release)
{
	std::size_t start = begin;
	for (;;) {
		std::size_t dot = text.find('.', start);
		if (dot == std::string::npos || dot > end)
			dot = end;
		if (!is_valid_identifier(text, start, dot, pre_release))
			return false;
		if (dot == end)
			return true;
		start = dot + 1;
	}
}

int sign_of(int c)
{
	return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

int compare_numbers(unsigned long a, unsigned long b)
{
	return a < b ? -1 : (b < a ? 1 : 0);
}

bool is_numeric(const std::string &identifier)
{
	return !identifier.empty() &&
	       std::all_of(identifier.begin(), identifier.end(), [](char c) { return is_digit(c); });
}

std::string strip_leading_zeros(const std::string &digits)
{
	const std::size_t first = digits.find_first_not_of('0');
	return first == std::string::npos ? std::string() : digits.substr(first);
}

// Digit strings compared as numbers, without converting them: a pre-release number may have more digits than
// any integer type holds. A leading zero can only reach here from a Version built by hand, and "01" is still 1.
int compare_digit_strings(const std::string &a, const std::string &b)
{
	const std::string x = strip_leading_zeros(a);
	const std::string y = strip_leading_zeros(b);
	if (x.size() != y.size())
		return x.size() < y.size() ? -1 : 1;
	return sign_of(x.compare(y));
}

// Semver 2.0 §11.4: numeric identifiers compare as numbers and rank below every other identifier, which
// compare in ASCII order.
int compare_identifiers(const std::string &a, const std::string &b)
{
	const bool a_numeric = is_numeric(a);
	const bool b_numeric = is_numeric(b);
	if (a_numeric && b_numeric)
		return compare_digit_strings(a, b);
	if (a_numeric != b_numeric)
		return a_numeric ? -1 : 1;
	return sign_of(a.compare(b));
}

std::vector<std::string> split_identifiers(const std::string &list)
{
	std::vector<std::string> identifiers;
	std::size_t start = 0;
	for (;;) {
		const std::size_t dot = list.find('.', start);
		identifiers.push_back(list.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
		if (dot == std::string::npos)
			return identifiers;
		start = dot + 1;
	}
}

int compare_prerelease(const std::string &a, const std::string &b)
{
	const std::vector<std::string> x = split_identifiers(a);
	const std::vector<std::string> y = split_identifiers(b);
	for (std::size_t i = 0; i < x.size() && i < y.size(); ++i) {
		const int c = compare_identifiers(x[i], y[i]);
		if (c != 0)
			return c;
	}
	// The shared identifiers are equal, so the longer list is the greater one: "rc" < "rc.1".
	if (x.size() == y.size())
		return 0;
	return x.size() < y.size() ? -1 : 1;
}

} // namespace

std::optional<Version> parse_version(const std::string &text)
{
	std::size_t pos = 0;
	// The "v" that GitHub's tags often carry is not part of the version.
	if (pos < text.size() && (text[pos] == 'v' || text[pos] == 'V'))
		++pos;

	const std::size_t first_dot = text.find('.', pos);
	if (first_dot == std::string::npos)
		return std::nullopt;
	const std::size_t second_dot = text.find('.', first_dot + 1);
	if (second_dot == std::string::npos)
		return std::nullopt;
	// The patch number ends at the first "-" or "+", or at the end of the text.
	std::size_t core_end = text.find_first_of("-+", second_dot + 1);
	if (core_end == std::string::npos)
		core_end = text.size();

	const std::optional<unsigned long> major = parse_number(text, pos, first_dot);
	const std::optional<unsigned long> minor = parse_number(text, first_dot + 1, second_dot);
	const std::optional<unsigned long> patch = parse_number(text, second_dot + 1, core_end);
	if (!major || !minor || !patch)
		return std::nullopt;

	Version version;
	version.major = *major;
	version.minor = *minor;
	version.patch = *patch;

	std::size_t rest = core_end;
	if (rest < text.size() && text[rest] == '-') {
		// A pre-release may contain "-" itself, so it runs up to the "+" that starts the build metadata.
		const std::size_t begin = rest + 1;
		std::size_t end = text.find('+', begin);
		if (end == std::string::npos)
			end = text.size();
		if (!is_valid_identifier_list(text, begin, end, true))
			return std::nullopt;
		version.prerelease = text.substr(begin, end - begin);
		rest = end;
	}
	// The build metadata is checked and then dropped: it never changes which release a version is.
	if (rest < text.size() && text[rest] == '+' && !is_valid_identifier_list(text, rest + 1, text.size(), false))
		return std::nullopt;
	return version;
}

int compare_versions(const Version &a, const Version &b)
{
	if (const int c = compare_numbers(a.major, b.major))
		return c;
	if (const int c = compare_numbers(a.minor, b.minor))
		return c;
	if (const int c = compare_numbers(a.patch, b.patch))
		return c;
	// A release is greater than any pre-release of the same MAJOR.MINOR.PATCH (semver 2.0 §11.3).
	if (a.prerelease.empty() && b.prerelease.empty())
		return 0;
	if (a.prerelease.empty())
		return 1;
	if (b.prerelease.empty())
		return -1;
	return compare_prerelease(a.prerelease, b.prerelease);
}

bool is_newer_version(const std::string &candidate, const std::string &current)
{
	const std::optional<Version> newest = parse_version(candidate);
	const std::optional<Version> installed = parse_version(current);
	// An unreadable version is never reported as newer: a bad tag must not trigger an update prompt.
	if (!newest || !installed)
		return false;
	return compare_versions(*newest, *installed) > 0;
}

std::string format_version(const Version &version)
{
	std::string out = std::to_string(version.major);
	out += '.';
	out += std::to_string(version.minor);
	out += '.';
	out += std::to_string(version.patch);
	if (!version.prerelease.empty()) {
		out += '-';
		out += version.prerelease;
	}
	return out;
}

} // namespace clip_atlas
