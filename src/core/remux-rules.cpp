#include "remux-rules.hpp"

#include "clip-layout.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace clip_atlas {

namespace {

// OBS matches this with strncmp, so the match is case-sensitive. OBS's own format names are lower case.
constexpr std::string_view kFragmentedPrefix = "fragmented";

// Folds A-Z only. Locale-aware folding would also match names OBS does not, and the extensions
// OBS checks are ASCII. Works for char (POSIX) and wchar_t (Windows) alike.
template<typename Char> Char ascii_lower(Char c)
{
	return (c >= Char('A') && c <= Char('Z')) ? static_cast<Char>(c + ('a' - 'A')) : c;
}

// Compares the native code units, so a non-ASCII name never goes through the ANSI code page.
bool extension_is(const std::filesystem::path &ext, const char *ascii_word)
{
	const std::filesystem::path::string_type word = path_from_utf8(ascii_word).native();
	const std::filesystem::path::string_type &name = ext.native();
	if (name.size() != word.size())
		return false;
	for (std::size_t i = 0; i < name.size(); ++i) {
		if (ascii_lower(name[i]) != ascii_lower(word[i]))
			return false;
	}
	return true;
}

// Prefix test with the case-sensitive semantics of strncmp.
bool starts_with(const std::string &text, std::string_view prefix)
{
	return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

} // namespace

std::filesystem::path expected_remux_path(const std::filesystem::path &saved, const RemuxConfig &config)
{
	if (!config.auto_remux)
		return {};

	// Simple mode has no output type setting, so the advanced flag only applies in advanced mode.
	if (!config.simple_mode && config.advanced_ffmpeg)
		return {};

	// Without an extension there is nothing to swap. Lossless AVI is never remuxed.
	const std::filesystem::path ext = saved.extension();
	if (ext.empty() || extension_is(ext, ".avi"))
		return {};

	if (starts_with(config.recording_format, kFragmentedPrefix)) {
		// The fragmented original keeps its name, so the remuxed copy needs one of its own:
		// "Replay.mp4" -> "Replay.remuxed.mp4".
		std::filesystem::path remuxed_ext = path_from_utf8(".remuxed");
		remuxed_ext += ext;
		std::filesystem::path out = saved;
		out.replace_extension(remuxed_ext);
		return out;
	}

	// A .mp4 would be remuxed onto itself, which libobs refuses, so OBS skips it.
	if (extension_is(ext, ".mp4"))
		return {};

	std::filesystem::path out = saved;
	out.replace_extension(path_from_utf8(".mp4"));
	return out;
}

} // namespace clip_atlas
