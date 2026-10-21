// Clip Atlas — the clips the dock lists, and how they are found and shown.
//
// The dock lists every clip under the clips folder (<base>), newest first:
//   <base>/<file>                    not sorted (OBS saved it straight there)
//   <base>/<Game>/<file>             sorted, monthly folders off
//   <base>/<Game>/<any folder>/<file> sorted, monthly folders on (YYYY-MM)
// Deeper files, hidden or system entries (a leading '.'), symlinks and files
// that aren't clips are left out. A clip's game is the name of its first
// folder under <base> ("" for a file directly in <base>), which is the folder
// clip-layout put it in: the dock groups by folder, not by exe.
//
// scan_clips_folder() and describe_clip() touch the disk and run on the dock's
// TaskQueue, never on the UI thread. ClipIndex and the display helpers do no
// I/O. Not thread-safe: the dock keeps its ClipIndex on the UI thread.
//
// Pure C++17 with no OBS dependency. Strings are UTF-8; paths are built with
// path_from_utf8() and turned back into UTF-8 with path_to_utf8().
#pragma once

#include "clip-routing.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace clip_atlas {

struct ClipEntry {
	std::filesystem::path path;
	std::string file_name;   // UTF-8 path.filename()
	std::string game_folder; // UTF-8 first folder under <base>; "" directly in <base>
	ClipKind kind = ClipKind::recording;
	std::uintmax_t size = 0;
	std::chrono::system_clock::time_point modified{};

	bool operator==(const ClipEntry &o) const
	{
		return path == o.path && file_name == o.file_name && game_folder == o.game_folder && kind == o.kind &&
		       size == o.size && modified == o.modified;
	}
	bool operator!=(const ClipEntry &o) const { return !(*this == o); }
};

struct ScanOptions {
	// OBS's replay file name prefix ([SimpleOutput] RecRBPrefix, "Replay" by
	// default). A video whose file name starts with it, ignoring ASCII case, is
	// a replay; other videos are recordings. Empty: every video is a recording.
	std::string replay_prefix = "Replay";
	// How many clips to keep: the newest by `modified`.
	std::size_t max_clips = 5000;
};

// UTF-8 text of a path (the inverse of path_from_utf8; on Windows it converts
// from UTF-16, never through the ANSI code page).
std::string path_to_utf8(const std::filesystem::path &path);

// Whether the extension, ignoring ASCII case, is one OBS saves:
//   videos: .mkv .mp4 .mov .flv .ts .webm
//   images: .png .jpg .jpeg .bmp .webp
// A file ending in ".clip-atlas-partial" (file-mover's copy in progress) is
// never a clip. No I/O.
bool is_clip_file(const std::filesystem::path &file);

// ClipKind::screenshot for an image, ClipKind::replay for a video named with
// options.replay_prefix, otherwise ClipKind::recording. No I/O.
ClipKind kind_of_clip(const std::filesystem::path &file, const ScanOptions &options);

// The game folder of `file` under `base`: its first path component below
// `base`, "" for a file directly in `base`, nullopt when `file` is not inside
// `base` within the three levels above, or is `base` itself. Compares path
// components exactly (both paths come from the same settings string). No I/O.
std::optional<std::string> game_folder_of(const std::filesystem::path &base, const std::filesystem::path &file);

// A file_time_type as system_clock time: reading + (t - file clock reading),
// where both clocks are read once per process (C++17 doesn't define the epoch
// of file_time_type, so this is how it converts). Exact to well within a
// second, and the same file time always gives the same result.
std::chrono::system_clock::time_point to_system_time(std::filesystem::file_time_type t);

// Reads one file's size and modification time and builds its entry: nullopt
// when it is missing, not a regular file (a symlink included), not a clip
// (is_clip_file), or not in a place a scan would list (game_folder_of). Uses
// std::error_code overloads; never throws. Disk I/O: the dock's TaskQueue only.
std::optional<ClipEntry> describe_clip(const std::filesystem::path &base, const std::filesystem::path &file,
				       const ScanOptions &options);

struct ScanResult {
	bool base_exists = false;     // `base` is an existing directory
	std::vector<ClipEntry> clips; // newest first (modified, then path, descending)
	bool truncated = false;       // more clips existed than options.max_clips
};

// Lists every clip under `base` as the header describes. Entries that can't
// be read (permissions, a file deleted mid-scan) are skipped, never fatal;
// directory iteration uses std::error_code and skip_permission_denied, and
// never follows symlinks. An empty or missing `base` gives base_exists =
// false and no clips. Never throws (std::bad_alloc aside). Disk I/O: the
// dock's TaskQueue only.
ScanResult scan_clips_folder(const std::filesystem::path &base, const ScanOptions &options);

struct ClipQuery {
	// nullopt: every game. "": only files directly in <base>.
	std::optional<std::string> game_folder;
	// Whitespace-separated terms; a clip matches when every term is a substring
	// of its file_name or game_folder, ignoring ASCII case. Empty: everything.
	std::string search;
};

// Newest-first list of clips with a fixed capacity.
class ClipIndex {
public:
	explicit ClipIndex(std::size_t max_clips = 5000);

	// Replaces everything (a new scan). Sorts newest first and keeps the newest
	// max_clips; duplicates by path keep the first seen.
	void reset(std::vector<ClipEntry> clips);

	// Adds a clip, or replaces the entry with the same path, keeping the order;
	// then drops the oldest beyond max_clips (which can be the new clip itself,
	// when it is older than all of them). Returns false only in that case.
	bool upsert(ClipEntry clip);

	// True if an entry with exactly this path was removed.
	bool remove(const std::filesystem::path &path);

	// Newest first: by modified descending, then by path descending, so the
	// order is total and stable across scans.
	const std::vector<ClipEntry> &clips() const;

	// The entry with exactly this path, or nullptr. Valid until the next change.
	const ClipEntry *find(const std::filesystem::path &path) const;

	// Each distinct non-empty game_folder once, sorted ignoring ASCII case
	// (ties by exact text).
	std::vector<std::string> games() const;

	// True if any clip lies directly in <base> (game_folder "").
	bool has_unfiled() const;

	// The clips matching `query`, newest first.
	std::vector<ClipEntry> query(const ClipQuery &query) const;

	std::size_t max_clips() const;

private:
	std::size_t max_clips_;
	std::vector<ClipEntry> clips_;
};

// How long ago a clip was saved, for "2 min ago". The UI turns it into text.
enum class AgeUnit {
	just_now, // under a minute, or in the future (a clock change)
	minutes,  // 1-59
	hours,    // 1-23
	days,     // 1-6
	date,     // 7 days or more: show format_local_date()
};

struct Age {
	AgeUnit unit = AgeUnit::just_now;
	long long count = 0; // whole minutes, hours or days (rounded down); 0 otherwise
};

Age describe_age(std::chrono::system_clock::time_point then, std::chrono::system_clock::time_point now);

// "2026-10-21" in local time.
std::string format_local_date(std::chrono::system_clock::time_point t);

// 1024-based, at most one decimal and only below 10: "0 B", "999 B",
// "1.0 KB", "9.9 KB", "10 KB", "84 MB", "1.2 GB", "3.0 TB". Rounds down, so a
// size is never shown larger than it is (1023 B is "1023 B", 1536 B "1.5 KB").
std::string format_size(std::uintmax_t bytes);

// The new file name for renaming `old_file` to `new_stem` (UTF-8). The
// extension of old_file is kept; the user only edits the stem. nullopt when
// the stem, after trimming spaces at both ends, is empty, contains a
// character Windows forbids (< > : " / \ | ? * or a control character below
// 0x20), ends with a dot, is a device name (CON, PRN, AUX, NUL, COM1-COM9,
// LPT1-LPT9, ignoring ASCII case), would make a name longer than 255 bytes,
// or gives back old_file's own name. No I/O.
std::optional<std::string> renamed_file_name(const std::filesystem::path &old_file, const std::string &new_stem);

} // namespace clip_atlas
