// Clip Atlas — titles, day groups and filed folders for the dock. clip-display.hpp says what each one gives.
#include "clip-display.hpp"

#include "clip-index.hpp"

#include <cstddef>
#include <ctime>

namespace clip_atlas {

namespace fs = std::filesystem;

namespace {

// "YYYY-MM-DD HH-MM-SS": OBS's default file name format, %CCYY-%MM-%DD %hh-%mm-%ss.
constexpr std::size_t kStampLength = 19;

bool is_digit(char c)
{
	return c >= '0' && c <= '9';
}

// The number in text[at, at + count), which the caller has checked is all digits.
int number_at(const std::string &text, std::size_t at, std::size_t count)
{
	int value = 0;
	for (std::size_t i = at; i < at + count; ++i)
		value = value * 10 + (text[i] - '0');
	return value;
}

// Whether `s` (exactly kStampLength bytes) is a date and time in OBS's default format. The ranges are checked, so a
// run of digits that only looks like a stamp ("9999-99-99 99-99-99") is shown as typed.
bool is_stamp(const std::string &s)
{
	static const char kPattern[] = "dddd-dd-dd dd-dd-dd";
	for (std::size_t i = 0; i < kStampLength; ++i) {
		const char want = kPattern[i];
		if (want == 'd' ? !is_digit(s[i]) : s[i] != want)
			return false;
	}
	const int month = number_at(s, 5, 2);
	const int day = number_at(s, 8, 2);
	const int hour = number_at(s, 11, 2);
	const int minute = number_at(s, 14, 2);
	const int second = number_at(s, 17, 2);
	return month >= 1 && month <= 12 && day >= 1 && day <= 31 && hour <= 23 && minute <= 59 && second <= 59;
}

// Splits a clash number such as " (2)" off the end of `stem`. Returns "" and leaves `stem` alone when it has none.
std::string take_clash_suffix(std::string &stem)
{
	if (stem.size() < 4 || stem.back() != ')')
		return {};
	const std::size_t open = stem.rfind(" (");
	if (open == std::string::npos)
		return {};
	const std::size_t first_digit = open + 2;
	const std::size_t close = stem.size() - 1;
	if (first_digit >= close)
		return {};
	for (std::size_t i = first_digit; i < close; ++i) {
		if (!is_digit(stem[i]))
			return {};
	}
	std::string suffix = stem.substr(open);
	stem.erase(open);
	return suffix;
}

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's days_from_civil), so day differences need
// no time zone and no time_t range.
long long days_from_civil(long long year, int month, int day)
{
	year -= month <= 2 ? 1 : 0;
	const long long era = (year >= 0 ? year : year - 399) / 400;
	const long long year_of_era = year - era * 400;
	const int shifted_month = month > 2 ? month - 3 : month + 9;
	const long long day_of_year = (153 * shifted_month + 2) / 5 + day - 1;
	const long long day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
	return era * 146097 + day_of_era - 719468;
}

// The components of `p` after lexically_normal(), without the empty one a trailing separator leaves, as in
// clip-index.cpp.
std::vector<fs::path> components_of(const fs::path &p)
{
	std::vector<fs::path> parts;
	for (const fs::path &part : p.lexically_normal()) {
		if (!part.empty())
			parts.push_back(part);
	}
	return parts;
}

} // namespace

ClipTitle parse_clip_title(const std::string &file_name)
{
	ClipTitle title;
	// The extension starts at the last dot, unless that dot starts the name.
	const std::size_t dot = file_name.rfind('.');
	title.stem = dot == std::string::npos || dot == 0 ? file_name : file_name.substr(0, dot);

	std::string rest = title.stem;
	const std::string suffix = take_clash_suffix(rest);
	if (rest.size() < kStampLength || !is_stamp(rest.substr(rest.size() - kStampLength)))
		return title;
	std::string prefix = rest.substr(0, rest.size() - kStampLength);
	// OBS puts a space between the prefix and the stamp. Digits run straight into a longer name are not a stamp.
	if (!prefix.empty() && prefix.back() != ' ')
		return title;
	const std::size_t first = prefix.find_first_not_of(' ');
	const std::size_t last = prefix.find_last_not_of(' ');
	prefix = first == std::string::npos ? std::string() : prefix.substr(first, last - first + 1);

	const std::string stamp = rest.substr(rest.size() - kStampLength);
	title.stamped = true;
	title.prefix = prefix;
	title.time = stamp.substr(11, 2) + ":" + stamp.substr(14, 2) + ":" + stamp.substr(17, 2);
	title.suffix = suffix;
	return title;
}

LocalDate local_date_of(std::chrono::system_clock::time_point t)
{
	const std::time_t tt = std::chrono::system_clock::to_time_t(t);
	std::tm local{};
#ifdef _WIN32
	localtime_s(&local, &tt);
#else
	localtime_r(&tt, &local);
#endif
	return LocalDate{local.tm_year + 1900, local.tm_mon + 1, local.tm_mday};
}

long long days_between(const LocalDate &from, const LocalDate &to)
{
	return days_from_civil(to.year, to.month, to.day) - days_from_civil(from.year, from.month, from.day);
}

DayGroup day_group_of(const LocalDate &clip_day, const LocalDate &today)
{
	const long long age = days_between(clip_day, today);
	DayGroup group;
	if (age <= 0)
		group.kind = DayGroupKind::today;
	else if (age == 1)
		group.kind = DayGroupKind::yesterday;
	else if (age <= 6)
		group.kind = DayGroupKind::recent;
	else {
		group.kind = DayGroupKind::month;
		group.month = YearMonth{clip_day.year, clip_day.month};
	}
	return group;
}

std::vector<std::string> folders_below(const fs::path &base, const fs::path &file)
{
	const std::vector<fs::path> base_parts = components_of(base);
	const std::vector<fs::path> file_parts = components_of(file);
	if (base_parts.empty() || file_parts.size() <= base_parts.size())
		return {};
	for (std::size_t i = 0; i < base_parts.size(); ++i) {
		if (file_parts[i].native() != base_parts[i].native())
			return {};
	}
	// Every component after base except the last one, which is the file itself.
	std::vector<std::string> folders;
	for (std::size_t i = base_parts.size(); i + 1 < file_parts.size(); ++i)
		folders.push_back(path_to_utf8(file_parts[i]));
	return folders;
}

} // namespace clip_atlas
