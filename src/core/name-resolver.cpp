#include "name-resolver.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace clip_atlas {

namespace {

// Non-ASCII bytes are spelled as escapes so the source stays ASCII and no compiler's source
// charset can change them.
constexpr std::string_view kEnDash = "\xE2\x80\x93";
constexpr std::string_view kEmDash = "\xE2\x80\x94";
constexpr std::string_view kExeSuffix = ".exe";

// Separators between a title and a launcher name, as in "Celeste - Steam".
constexpr std::array<std::string_view, 4> kLauncherSeparators = {" - ", " | ", " \xE2\x80\x93 ", " \xE2\x80\x94 "};

// Launcher names that get appended to window titles. Only a match at the very end is removed.
constexpr std::array<std::string_view, 9> kLauncherNames = {
	"Steam",           "Epic Games", "Epic Games Store", "GOG Galaxy",      "Xbox",
	"Microsoft Store", "Battle.net", "EA app",           "Ubisoft Connect",
};

// Product names that describe the engine or runtime rather than the game. The (R) signs are UTF-8.
constexpr std::array<std::string_view, 8> kGenericProductNames = {
	"Unreal Engine",
	"Unity",
	"Java(TM) Platform SE binary",
	"Java Platform SE binary",
	"OpenJDK Platform binary",
	"Microsoft\xC2\xAE Windows\xC2\xAE Operating System",
	"Electron",
	"Godot Engine",
};

bool is_ascii_space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

bool is_ascii_digit(char c)
{
	return c >= '0' && c <= '9';
}

bool is_ascii_alpha(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// Every byte of a multi-byte UTF-8 sequence is >= 0x80, so this covers all non-ASCII text.
bool is_non_ascii(char c)
{
	return static_cast<unsigned char>(c) >= 0x80;
}

char to_lower_ascii(char c)
{
	return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equals_ascii_ci(std::string_view a, std::string_view b)
{
	if (a.size() != b.size())
		return false;
	for (std::size_t i = 0; i < a.size(); ++i) {
		if (to_lower_ascii(a[i]) != to_lower_ascii(b[i]))
			return false;
	}
	return true;
}

bool starts_with(std::string_view s, std::string_view prefix)
{
	return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

bool ends_with(std::string_view s, std::string_view suffix)
{
	return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

bool ends_with_ci(std::string_view s, std::string_view suffix)
{
	return s.size() >= suffix.size() && equals_ascii_ci(s.substr(s.size() - suffix.size()), suffix);
}

std::string_view trim_ascii_space(std::string_view s)
{
	while (!s.empty() && is_ascii_space(s.front()))
		s.remove_prefix(1);
	while (!s.empty() && is_ascii_space(s.back()))
		s.remove_suffix(1);
	return s;
}

// The part after the last '\' or '/', trimmed. "unknown" and blank input give an empty view.
// OBS sends a bare file name, so the split is defensive; the second trim keeps "dir\\ game.exe"
// from leaving a leading space in the key.
std::string_view bare_file_name(const std::string &executable)
{
	std::string_view name = trim_ascii_space(executable);
	const std::size_t sep = name.find_last_of("\\/");
	if (sep != std::string_view::npos)
		name = trim_ascii_space(name.substr(sep + 1));
	if (name.empty() || equals_ascii_ci(name, "unknown"))
		return {};
	return name;
}

// Removes one launcher suffix such as " - Steam" from the end of `title`. Trailing whitespace is
// ignored when matching, so "Celeste - Steam  " still matches. Leading space is kept: the separator
// starts with one, so a title that is only " - Steam" must still match.
bool remove_launcher_suffix(std::string_view &title)
{
	while (!title.empty() && is_ascii_space(title.back()))
		title.remove_suffix(1);
	for (const std::string_view launcher : kLauncherNames) {
		if (!ends_with_ci(title, launcher))
			continue;
		const std::string_view before_name = title.substr(0, title.size() - launcher.size());
		for (const std::string_view separator : kLauncherSeparators) {
			if (ends_with(before_name, separator)) {
				title.remove_suffix(launcher.size() + separator.size());
				return true;
			}
		}
	}
	return false;
}

std::size_t skip_digits(std::string_view s, std::size_t i)
{
	while (i < s.size() && is_ascii_digit(s[i]))
		++i;
	return i;
}

// "1.21.4", "v1.2", "(1.0.3)", "[2.0.1b]", "1.0-beta". A dot is required, so "Doom 3" keeps its number.
bool is_version_token(std::string_view token)
{
	// Only one pair of brackets is removed, so "((1.0))" is not a version.
	if (token.size() >= 2) {
		const char first = token.front();
		const char last = token.back();
		if ((first == '(' && last == ')') || (first == '[' && last == ']'))
			token = token.substr(1, token.size() - 2);
	}

	std::size_t i = 0;
	if (i < token.size() && (token[i] == 'v' || token[i] == 'V'))
		++i;

	const std::size_t digits_begin = i;
	i = skip_digits(token, i);
	if (i == digits_begin)
		return false;

	std::size_t groups = 0;
	while (i < token.size() && token[i] == '.') {
		const std::size_t group_begin = i + 1;
		i = skip_digits(token, group_begin);
		if (i == group_begin)
			return false; // "1." and "1.x" are not versions
		++groups;
	}
	if (groups == 0)
		return false;

	// The tail may not hold another dot, so "1.0-beta.2" is left alone.
	for (; i < token.size(); ++i) {
		if (!is_ascii_alpha(token[i]) && !is_ascii_digit(token[i]) && token[i] != '-')
			return false;
	}
	return true;
}

// Drops version tokens and rejoins the rest with single spaces. Empty tokens come from runs of
// spaces and are dropped too, so the result never has a double space.
std::string drop_version_tokens(std::string_view title)
{
	std::string out;
	out.reserve(title.size());
	std::size_t begin = 0;
	while (begin <= title.size()) {
		std::size_t end = title.find(' ', begin);
		if (end == std::string_view::npos)
			end = title.size();
		const std::string_view token = title.substr(begin, end - begin);
		if (!token.empty() && !is_version_token(token)) {
			if (!out.empty())
				out.push_back(' ');
			out.append(token.data(), token.size());
		}
		begin = end + 1;
	}
	return out;
}

// ASCII punctuation left over from a removed version or launcher, such as "Game: " or "- Game".
bool is_separator_char(char c)
{
	return is_ascii_space(c) || c == '-' || c == '|' || c == ':';
}

std::size_t leading_separator_length(std::string_view s)
{
	if (!s.empty() && is_separator_char(s.front()))
		return 1;
	if (starts_with(s, kEnDash) || starts_with(s, kEmDash))
		return kEnDash.size();
	return 0;
}

std::size_t trailing_separator_length(std::string_view s)
{
	if (!s.empty() && is_separator_char(s.back()))
		return 1;
	if (ends_with(s, kEnDash) || ends_with(s, kEmDash))
		return kEnDash.size();
	return 0;
}

// Dashes are three bytes and are removed whole, so no UTF-8 sequence is left cut in half.
std::string_view trim_separators(std::string_view s)
{
	bool trimmed = true;
	while (trimmed) {
		trimmed = false;
		if (const std::size_t lead = leading_separator_length(s)) {
			s.remove_prefix(lead);
			trimmed = true;
		}
		if (const std::size_t trail = trailing_separator_length(s)) {
			s.remove_suffix(trail);
			trimmed = true;
		}
	}
	return s;
}

// Punctuation-only leftovers such as "***" or "[]" name nothing. Non-ASCII text counts as a name
// because titles in other scripts have no ASCII letters at all.
bool has_name_character(std::string_view s)
{
	for (const char c : s) {
		if (is_ascii_alpha(c) || is_ascii_digit(c) || is_non_ascii(c))
			return true;
	}
	return false;
}

} // namespace

std::string normalize_executable(const std::string &executable)
{
	std::string name(bare_file_name(executable));
	for (char &c : name)
		c = to_lower_ascii(c);
	return name;
}

std::string executable_stem(const std::string &executable)
{
	std::string_view stem = bare_file_name(executable);
	if (ends_with_ci(stem, kExeSuffix))
		stem.remove_suffix(kExeSuffix.size());
	return std::string(stem);
}

bool is_generic_product_name(const std::string &product_name)
{
	const std::string_view name = trim_ascii_space(product_name);
	if (name.empty())
		return true;
	for (const std::string_view generic : kGenericProductNames) {
		if (equals_ascii_ci(name, generic))
			return true;
	}
	return false;
}

std::string clean_window_title(const std::string &title)
{
	std::string_view rest = title;
	bool removed = true;
	while (removed)
		removed = remove_launcher_suffix(rest);

	const std::string joined = drop_version_tokens(rest);
	const std::string_view core = trim_separators(joined);
	if (!has_name_character(core))
		return {};
	return std::string(core);
}

ResolvedGame resolve_game(const GameIdentityInput &input, const UserGameRules &rules, const BundledGameNames &bundled)
{
	ResolvedGame game;
	game.folder_key = normalize_executable(input.executable);

	// Rules and bundled names are keyed by a real executable. A blank key has none, so the lookups
	// are skipped rather than risking a match on an empty key.
	std::string user_name;
	std::string bundled_name;
	if (!game.folder_key.empty()) {
		const auto rule = rules.find(game.folder_key);
		if (rule != rules.end()) {
			game.ignored = rule->second.ignore;
			user_name = rule->second.display_name;
		}
		const auto bundled_entry = bundled.find(game.folder_key);
		if (bundled_entry != bundled.end())
			bundled_name = bundled_entry->second;
	}

	// A generic product name is blanked here, so it falls through exactly as an empty name would.
	const std::string product = is_generic_product_name(input.product_name) ? std::string() : input.product_name;

	// The candidates are in resolution order; the first one that is non-empty after trimming wins.
	struct Candidate {
		std::string name;
		NameSource source;
	};
	const Candidate candidates[] = {
		{user_name, NameSource::user_rule},
		{bundled_name, NameSource::bundled},
		{product, NameSource::product_name},
		{clean_window_title(input.window_title), NameSource::window_title},
		{executable_stem(input.executable), NameSource::executable},
	};
	for (const Candidate &candidate : candidates) {
		const std::string_view name = trim_ascii_space(candidate.name);
		if (!name.empty()) {
			game.display_name = std::string(name);
			game.source = candidate.source;
			break;
		}
	}
	return game;
}

} // namespace clip_atlas
