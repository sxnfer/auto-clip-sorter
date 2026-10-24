#include "clip-index.hpp"

#include "clip-layout.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <set>
#include <string_view>
#include <system_error>

namespace clip_atlas {

namespace fs = std::filesystem;

namespace {

// Lowercases A-Z only. A locale-aware tolower() could fold bytes inside UTF-8
// sequences, and every name here is UTF-8.
char ascii_lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string ascii_lower(std::string s)
{
	for (char &c : s)
		c = ascii_lower(c);
	return s;
}

bool starts_with_fold(const std::string &name, const std::string &prefix)
{
	return name.size() >= prefix.size() && ascii_lower(name.substr(0, prefix.size())) == ascii_lower(prefix);
}

bool ends_with(const std::string &name, const std::string &suffix)
{
	return name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// A leading dot hides an entry from the scan. Windows marks such files hidden in
// some tools, but the rule here is the name alone.
bool is_hidden_name(const std::string &name)
{
	return !name.empty() && name[0] == '.';
}

// The extension in lowercase ASCII, "" when there is none. A name that is only a
// dot-prefixed word (".mkv") has no extension, so it is never a clip.
std::string lower_extension(const fs::path &file)
{
	return ascii_lower(path_to_utf8(file.extension()));
}

bool is_video_extension(const std::string &ext)
{
	static const std::array<const char *, 6> kVideo = {".mkv", ".mp4", ".mov", ".flv", ".ts", ".webm"};
	return std::any_of(kVideo.begin(), kVideo.end(), [&ext](const char *e) { return ext == e; });
}

bool is_image_extension(const std::string &ext)
{
	static const std::array<const char *, 5> kImage = {".png", ".jpg", ".jpeg", ".bmp", ".webp"};
	return std::any_of(kImage.begin(), kImage.end(), [&ext](const char *e) { return ext == e; });
}

// The components of `p` after lexically_normal(). The empty element a trailing
// separator leaves behind is dropped, so "D:/Clips" and "D:/Clips/" compare equal.
std::vector<fs::path> components_of(const fs::path &p)
{
	std::vector<fs::path> parts;
	for (const fs::path &part : p.lexically_normal()) {
		if (!part.empty())
			parts.push_back(part);
	}
	return parts;
}

// Whether any folder or the file itself, below `base`, is hidden. A scan never
// enters a hidden folder, so describe_clip refuses a file inside one as well.
bool hidden_below(const fs::path &base, const fs::path &file)
{
	const std::vector<fs::path> base_parts = components_of(base);
	const std::vector<fs::path> file_parts = components_of(file);
	for (std::size_t i = base_parts.size(); i < file_parts.size(); ++i) {
		if (is_hidden_name(path_to_utf8(file_parts[i])))
			return true;
	}
	return false;
}

// Newest first. The path breaks ties, so the order is total and the same from one
// scan to the next.
bool newer_first(const ClipEntry &a, const ClipEntry &b)
{
	if (a.modified != b.modified)
		return a.modified > b.modified;
	return b.path < a.path;
}

// The two clocks read at one moment. Two reads differ by tens of nanoseconds, so converting
// each file against fresh reads would give equal file times slightly different results, and
// the same file a different `modified` in every scan. One reading per process keeps every
// conversion consistent: equal file times stay equal (the path tie-break in newer_first()
// applies), and a filed clip's entry matches the one a later scan builds. A clock change
// while OBS runs shifts every converted time alike, which only moves the displayed ages.
struct ClockReading {
	std::chrono::system_clock::time_point system;
	fs::file_time_type file;
};

ClockReading read_clocks()
{
	// A function-local static is initialised once, thread-safely, on first use.
	static const ClockReading reading{std::chrono::system_clock::now(), fs::file_time_type::clock::now()};
	return reading;
}

// The to_system_time() formula, against a reading taken earlier.
std::chrono::system_clock::time_point convert(fs::file_time_type t, const ClockReading &clocks)
{
	return clocks.system + std::chrono::duration_cast<std::chrono::system_clock::duration>(t - clocks.file);
}

// describe_clip() with the clocks already read. A scan passes one reading for all of its files.
std::optional<ClipEntry> describe_clip_with(const fs::path &base, const fs::path &file, const ScanOptions &options,
					    const ClockReading &clocks)
{
	std::error_code ec;
	// symlink_status, not status: a symlink is not a regular file, so it is never followed.
	const fs::file_status status = fs::symlink_status(file, ec);
	if (ec || !fs::is_regular_file(status))
		return std::nullopt;
	if (!is_clip_file(file))
		return std::nullopt;
	const std::optional<std::string> game = game_folder_of(base, file);
	if (!game || hidden_below(base, file))
		return std::nullopt;

	std::error_code size_ec;
	std::error_code time_ec;
	const std::uintmax_t size = fs::file_size(file, size_ec);
	const fs::file_time_type mtime = fs::last_write_time(file, time_ec);
	if (size_ec || time_ec)
		return std::nullopt;

	ClipEntry entry;
	entry.path = file;
	entry.file_name = path_to_utf8(file.filename());
	entry.game_folder = *game;
	entry.kind = kind_of_clip(file, options);
	entry.size = size;
	entry.modified = convert(mtime, clocks);
	return entry;
}

// Folder depth below the scanned base: 0 is base, 1 a game folder, 2 a month
// folder. Folders deeper than this are never entered, so nothing below
// <base>/<a>/<b>/<c> is listed.
constexpr int kLastListedDepth = 2;

// Lists the clips in `dir` into `found`, then recurses into its folders. An
// unreadable folder is skipped, so one locked folder never ends the scan.
void walk_folder(const fs::path &base, const fs::path &dir, int depth, const ScanOptions &options,
		 const ClockReading &clocks, std::vector<ClipEntry> &found)
{
	try {
		std::error_code ec;
		// skip_permission_denied: a folder the user cannot read is skipped, not reported.
		fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
		const fs::directory_iterator end;
		// An error on increment ends this folder early; the entries read so far stay listed.
		for (; !ec && it != end; it.increment(ec)) {
			const fs::directory_entry &entry = *it;
			const fs::path path = entry.path();
			if (is_hidden_name(path_to_utf8(path.filename())))
				continue;
			// symlink_status: a symlink is never followed, and is neither a file nor a folder here.
			std::error_code status_ec;
			const fs::file_status status = entry.symlink_status(status_ec);
			if (status_ec)
				continue;
			if (fs::is_regular_file(status)) {
				if (std::optional<ClipEntry> clip = describe_clip_with(base, path, options, clocks))
					found.push_back(std::move(*clip));
			} else if (fs::is_directory(status) && depth < kLastListedDepth) {
				walk_folder(base, path, depth + 1, options, clocks, found);
			}
		}
	} catch (const fs::filesystem_error &) {
		// Only reached if a library call throws despite the error_code overloads. The
		// folder is skipped, which is what an unreadable folder gets anyway.
	}
}

// Trims ASCII space and tab from both ends. Nothing else is trimmed, so a stem that
// ends in a dot still reaches the dot check.
std::string trim_spaces(const std::string &s)
{
	const std::string_view blanks = " \t";
	const std::size_t first = s.find_first_not_of(blanks);
	if (first == std::string::npos)
		return {};
	const std::size_t last = s.find_last_not_of(blanks);
	return s.substr(first, last - first + 1);
}

bool is_forbidden_char(char c)
{
	return std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos;
}

// Windows reserves these names whatever follows the first dot ("NUL.txt"). Trailing
// spaces before that dot are ignored, as clip-layout's device check does; a rename
// to "NUL .txt" is refused for the same reason.
bool is_device_stem(const std::string &stem)
{
	const std::size_t dot = stem.find('.');
	std::string base = ascii_lower(stem.substr(0, dot));
	while (!base.empty() && base.back() == ' ')
		base.pop_back();
	if (base == "con" || base == "prn" || base == "aux" || base == "nul")
		return true;
	const bool com_or_lpt = starts_with_fold(base, "com") || starts_with_fold(base, "lpt");
	return com_or_lpt && base.size() == 4 && base[3] >= '1' && base[3] <= '9';
}

// Longest file name Windows allows, in bytes, extension included.
constexpr std::size_t kMaxNameBytes = 255;

} // namespace

std::string path_to_utf8(const fs::path &path)
{
	// u8string() is std::string in C++17 and std::u8string in C++20: copy the bytes either way.
	const auto u8 = path.u8string();
	return std::string(u8.begin(), u8.end());
}

bool is_clip_file(const fs::path &file)
{
	// file-mover's copy in progress. Kept out of the list even if its extension were ever
	// added, so a half-written copy never shows up as a clip.
	const std::string name = path_to_utf8(file.filename());
	if (ends_with(name, ".clip-atlas-partial"))
		return false;
	const std::string ext = lower_extension(file);
	return is_video_extension(ext) || is_image_extension(ext);
}

ClipKind kind_of_clip(const fs::path &file, const ScanOptions &options)
{
	if (is_image_extension(lower_extension(file)))
		return ClipKind::screenshot;
	// An empty prefix would match every name, so it means "no replays", not "all replays".
	const std::string name = path_to_utf8(file.filename());
	if (!options.replay_prefix.empty() && starts_with_fold(name, options.replay_prefix))
		return ClipKind::replay;
	return ClipKind::recording;
}

std::optional<std::string> game_folder_of(const fs::path &base, const fs::path &file)
{
	const std::vector<fs::path> base_parts = components_of(base);
	const std::vector<fs::path> file_parts = components_of(file);
	// An empty base names no place a clip could be in. A file no deeper than base is base
	// itself or lies outside it.
	if (base_parts.empty() || file_parts.size() <= base_parts.size())
		return std::nullopt;
	// Exact comparison: both paths come from the same settings string, so no case folding.
	for (std::size_t i = 0; i < base_parts.size(); ++i) {
		if (file_parts[i].native() != base_parts[i].native())
			return std::nullopt;
	}
	const std::size_t depth = file_parts.size() - base_parts.size();
	if (depth == 1)
		return std::string(); // directly in base: not sorted
	if (depth == 2 || depth == 3)
		return path_to_utf8(file_parts[base_parts.size()]);
	return std::nullopt; // four or more levels: a scan never lists those
}

std::chrono::system_clock::time_point to_system_time(fs::file_time_type t)
{
	return convert(t, read_clocks());
}

std::optional<ClipEntry> describe_clip(const fs::path &base, const fs::path &file, const ScanOptions &options)
{
	return describe_clip_with(base, file, options, read_clocks());
}

ScanResult scan_clips_folder(const fs::path &base, const ScanOptions &options)
{
	ScanResult result;
	if (base.empty())
		return result;
	std::error_code ec;
	result.base_exists = fs::is_directory(base, ec);
	if (!result.base_exists)
		return result;

	std::vector<ClipEntry> found;
	// One clock reading for the whole scan, so equal file times still convert to equal times.
	walk_folder(base, base, 0, options, read_clocks(), found);
	std::sort(found.begin(), found.end(), newer_first);
	if (found.size() > options.max_clips) {
		found.erase(found.begin() + static_cast<std::ptrdiff_t>(options.max_clips), found.end());
		result.truncated = true;
	}
	result.clips = std::move(found);
	return result;
}

ClipIndex::ClipIndex(std::size_t max_clips) : max_clips_(max_clips) {}

void ClipIndex::reset(std::vector<ClipEntry> clips)
{
	// The first entry for a path wins, so a rescan's earlier listing takes precedence over a
	// duplicate later in the same input.
	std::set<fs::path> seen;
	std::vector<ClipEntry> unique;
	unique.reserve(clips.size());
	for (ClipEntry &clip : clips) {
		if (seen.insert(clip.path).second)
			unique.push_back(std::move(clip));
	}
	std::sort(unique.begin(), unique.end(), newer_first);
	if (unique.size() > max_clips_)
		unique.erase(unique.begin() + static_cast<std::ptrdiff_t>(max_clips_), unique.end());
	clips_ = std::move(unique);
}

bool ClipIndex::upsert(ClipEntry clip)
{
	// The old entry goes first: after this, the path appears at most once, and the insert
	// below finds its place among the other entries only.
	const auto old =
		std::find_if(clips_.begin(), clips_.end(), [&clip](const ClipEntry &e) { return e.path == clip.path; });
	if (old != clips_.end())
		clips_.erase(old);

	const auto pos = std::upper_bound(clips_.begin(), clips_.end(), clip, newer_first);
	const auto inserted = clips_.insert(pos, std::move(clip));
	// The new entry's index among the newest. Anything at max_clips_ or beyond is dropped below.
	const std::size_t index = static_cast<std::size_t>(inserted - clips_.begin());
	if (clips_.size() > max_clips_)
		clips_.erase(clips_.begin() + static_cast<std::ptrdiff_t>(max_clips_), clips_.end());
	return index < max_clips_;
}

bool ClipIndex::remove(const fs::path &path)
{
	const auto it =
		std::find_if(clips_.begin(), clips_.end(), [&path](const ClipEntry &e) { return e.path == path; });
	if (it == clips_.end())
		return false;
	clips_.erase(it);
	return true;
}

const std::vector<ClipEntry> &ClipIndex::clips() const
{
	return clips_;
}

const ClipEntry *ClipIndex::find(const fs::path &path) const
{
	const auto it =
		std::find_if(clips_.begin(), clips_.end(), [&path](const ClipEntry &e) { return e.path == path; });
	return it == clips_.end() ? nullptr : &*it;
}

std::vector<std::string> ClipIndex::games() const
{
	std::set<std::string> distinct;
	for (const ClipEntry &clip : clips_) {
		if (!clip.game_folder.empty())
			distinct.insert(clip.game_folder);
	}
	std::vector<std::string> out(distinct.begin(), distinct.end());
	// Folded text first, so "alpha" and "Beta" sort together. Exact text breaks the tie.
	std::sort(out.begin(), out.end(), [](const std::string &a, const std::string &b) {
		const std::string la = ascii_lower(a);
		const std::string lb = ascii_lower(b);
		return la != lb ? la < lb : a < b;
	});
	return out;
}

bool ClipIndex::has_unfiled() const
{
	return std::any_of(clips_.begin(), clips_.end(), [](const ClipEntry &c) { return c.game_folder.empty(); });
}

std::vector<ClipEntry> ClipIndex::query(const ClipQuery &query) const
{
	// Terms are split on ASCII whitespace and folded once, not per clip.
	const std::string search = ascii_lower(query.search);
	std::vector<std::string> terms;
	std::string current;
	for (char c : search) {
		const bool blank = c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
		if (!blank) {
			current.push_back(c);
			continue;
		}
		if (!current.empty())
			terms.push_back(current);
		current.clear();
	}
	if (!current.empty())
		terms.push_back(current);

	std::vector<ClipEntry> out;
	for (const ClipEntry &clip : clips_) {
		if (query.game_folder && clip.game_folder != *query.game_folder)
			continue;
		if (query.kind && clip.kind != *query.kind)
			continue;
		const std::string name = ascii_lower(clip.file_name);
		const std::string folder = ascii_lower(clip.game_folder);
		// Each term may match either field: "replay cs2" finds a file named "Replay ..." in folder "CS2".
		const bool all_terms =
			std::all_of(terms.begin(), terms.end(), [&name, &folder](const std::string &term) {
				return name.find(term) != std::string::npos || folder.find(term) != std::string::npos;
			});
		if (all_terms)
			out.push_back(clip);
	}
	return out;
}

std::size_t ClipIndex::max_clips() const
{
	return max_clips_;
}

Age describe_age(std::chrono::system_clock::time_point then, std::chrono::system_clock::time_point now)
{
	using std::chrono::hours;
	using std::chrono::minutes;
	// A clip time after `now` (the clock was set back) reads as just now. Checking first keeps
	// the subtraction from going negative.
	if (then >= now)
		return {AgeUnit::just_now, 0};
	const auto age = now - then;
	if (age < minutes(1))
		return {AgeUnit::just_now, 0};
	if (age < hours(1))
		return {AgeUnit::minutes, static_cast<long long>(std::chrono::duration_cast<minutes>(age).count())};
	if (age < hours(24))
		return {AgeUnit::hours, static_cast<long long>(std::chrono::duration_cast<hours>(age).count())};
	if (age < hours(24 * 7))
		return {AgeUnit::days, static_cast<long long>(std::chrono::duration_cast<hours>(age).count() / 24)};
	return {AgeUnit::date, 0};
}

std::string format_local_date(std::chrono::system_clock::time_point t)
{
	std::time_t tt = std::chrono::system_clock::to_time_t(t);
	std::tm local{};
#ifdef _WIN32
	localtime_s(&local, &tt);
#else
	localtime_r(&tt, &local);
#endif
	// Large enough for the widest possible int in each field, so the compiler cannot
	// suspect truncation.
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
	return buf;
}

std::string format_size(std::uintmax_t bytes)
{
	if (bytes < 1024)
		return std::to_string(bytes) + " B";

	static const char *const kUnits[] = {"KB", "MB", "GB", "TB"};
	constexpr std::size_t kLastUnit = 3;
	// The largest unit not above the size. Sizes beyond TB stay in TB.
	std::uintmax_t unit = 1024;
	std::size_t index = 0;
	while (index < kLastUnit && bytes / unit >= 1024) {
		unit *= 1024;
		++index;
	}

	const std::uintmax_t whole = bytes / unit;
	if (whole >= 10)
		return std::to_string(whole) + " " + kUnits[index];
	// One decimal, rounded down. The remainder is below `unit`, so times ten it cannot overflow.
	const std::uintmax_t tenths = (bytes % unit) * 10 / unit;
	return std::to_string(whole) + "." + std::to_string(tenths) + " " + kUnits[index];
}

std::optional<std::string> renamed_file_name(const fs::path &old_file, const std::string &new_stem)
{
	const std::string stem = trim_spaces(new_stem);
	if (stem.empty())
		return std::nullopt;
	for (char c : stem) {
		if (static_cast<unsigned char>(c) < 0x20 || is_forbidden_char(c))
			return std::nullopt;
	}
	if (stem.back() == '.')
		return std::nullopt;
	if (is_device_stem(stem))
		return std::nullopt;

	// Only the stem changes: the extension comes back byte for byte, with its case kept.
	const std::string name = stem + path_to_utf8(old_file.extension());
	if (name.size() > kMaxNameBytes)
		return std::nullopt;
	if (name == path_to_utf8(old_file.filename()))
		return std::nullopt;
	return name;
}

} // namespace clip_atlas
