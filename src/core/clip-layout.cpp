#include "clip-layout.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <ctime>

namespace clip_atlas {

namespace {

constexpr size_t kMaxFolderBytes = 100;

bool is_reserved_char(unsigned char c)
{
	switch (c) {
	case '<':
	case '>':
	case ':':
	case '"':
	case '/':
	case '\\':
	case '|':
	case '?':
	case '*':
		return true;
	default:
		return c < 0x20 || c == 0x7f;
	}
}

std::string upper_ascii(std::string s)
{
	for (char &c : s)
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	return s;
}

// Windows refuses these as file names, even with an extension ("NUL.txt").
bool is_device_name(const std::string &name)
{
	static const std::array<const char *, 22> kDevices = {
		"CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
		"COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
	};
	std::string stem = upper_ascii(name.substr(0, name.find('.')));
	while (!stem.empty() && stem.back() == ' ')
		stem.pop_back();
	for (const char *device : kDevices) {
		if (stem == device)
			return true;
	}
	return false;
}

void trim(std::string &s)
{
	auto strip = [](char c) {
		return c == ' ' || c == '.';
	};
	while (!s.empty() && strip(s.back()))
		s.pop_back();
	size_t start = 0;
	while (start < s.size() && s[start] == ' ')
		++start;
	s.erase(0, start);
}

// Cuts to at most `max_bytes` without leaving a partial UTF-8 sequence.
void truncate_utf8(std::string &s, size_t max_bytes)
{
	if (s.size() <= max_bytes)
		return;
	size_t cut = max_bytes;
	while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
		--cut;
	s.resize(cut);
}

} // namespace

YearMonth year_month_of(std::chrono::system_clock::time_point t)
{
	std::time_t tt = std::chrono::system_clock::to_time_t(t);
	std::tm local{};
#ifdef _WIN32
	localtime_s(&local, &tt);
#else
	localtime_r(&tt, &local);
#endif
	return {local.tm_year + 1900, local.tm_mon + 1};
}

std::string month_folder_name(YearMonth ym)
{
	char buf[16];
	std::snprintf(buf, sizeof(buf), "%04d-%02d", ym.year, ym.month);
	return buf;
}

std::string sanitize_folder_name(const std::string &name, const std::string &fallback)
{
	std::string out;
	out.reserve(name.size());
	for (char c : name)
		out.push_back(is_reserved_char(static_cast<unsigned char>(c)) ? '_' : c);

	trim(out);
	truncate_utf8(out, kMaxFolderBytes);
	trim(out); // truncation may expose trailing spaces/dots

	if (out.empty() || out.find_first_not_of('_') == std::string::npos)
		return fallback;
	// Windows reserves the device name whatever follows the first dot ("COM1.exe"), so the
	// underscore goes on the stem. Appending it at the end would still be reserved and would
	// grow by one more underscore every time the name was sanitised again.
	if (is_device_name(out))
		out.insert(std::min(out.find('.'), out.size()), 1, '_');
	return out;
}

std::filesystem::path path_from_utf8(const std::string &utf8)
{
#if defined(__cpp_char8_t)
	return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
#else
	return std::filesystem::u8path(utf8);
#endif
}

std::filesystem::path destination_dir(const std::filesystem::path &base, const std::string &game, YearMonth when,
				      const LayoutOptions &options)
{
	// Months are only grouped inside a game folder: a <YYYY-MM> folder directly in <base> would be taken for a game.
	if (!options.game_folders)
		return base;
	const std::string unsorted = sanitize_folder_name(options.unsorted_folder, "Unsorted");
	std::filesystem::path dir = base / path_from_utf8(sanitize_folder_name(game, unsorted));
	if (options.monthly_folders)
		dir /= month_folder_name(when);
	return dir;
}

std::filesystem::path unique_destination(const std::filesystem::path &dir, const std::filesystem::path &file_name,
					 const ExistsFn &exists)
{
	auto taken = [&](const std::filesystem::path &p) {
		if (exists)
			return exists(p);
		std::error_code ec;
		return std::filesystem::exists(p, ec);
	};

	std::filesystem::path candidate = dir / file_name;
	if (!taken(candidate))
		return candidate;

	const std::filesystem::path stem = file_name.stem();
	const std::filesystem::path ext = file_name.extension();
	for (int n = 2; n < 10000; ++n) {
		std::filesystem::path numbered = stem;
		numbered += " (" + std::to_string(n) + ")";
		numbered += ext;
		candidate = dir / numbered;
		if (!taken(candidate))
			return candidate;
	}
	return {}; // caller treats empty as failure and leaves the clip in place
}

} // namespace clip_atlas
