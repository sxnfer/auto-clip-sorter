#include "clip-index.hpp"
#include "clip-layout.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <ctime>
#include <fstream>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <vector>

using namespace clip_atlas;
namespace fs = std::filesystem;

namespace {

using SystemClock = std::chrono::system_clock;
using Names = std::vector<std::string>;

// A fresh, empty folder under the system temp directory, removed when the test ends.
class TempDir {
public:
	TempDir()
	{
		std::random_device rd;
		// Two statements: the order of two rd() calls in one expression is unspecified.
		const unsigned long long hi = rd();
		const unsigned long long lo = rd();
		path_ = fs::temp_directory_path() /
			path_from_utf8("clip-atlas-index-" + std::to_string((hi << 32) | lo));
		std::error_code ec;
		fs::create_directories(path_, ec);
		REQUIRE(!ec);
	}
	~TempDir()
	{
		std::error_code ec;
		fs::remove_all(path_, ec);
	}
	TempDir(const TempDir &) = delete;
	TempDir &operator=(const TempDir &) = delete;

	const fs::path &path() const { return path_; }

private:
	fs::path path_;
};

// Writes a whole file, making its folders first. The stream is closed before returning,
// because Windows cannot remove a file that is still open.
void write_file(const fs::path &p, const std::string &content = "clip")
{
	std::error_code ec;
	fs::create_directories(p.parent_path(), ec);
	std::ofstream out(p, std::ios::binary | std::ios::trunc);
	REQUIRE(out.is_open());
	out << content;
	out.close();
	REQUIRE(!out.fail());
}

// The file time that to_system_time() maps back to `t`. The two clocks are read at
// different moments, so the round trip is only accurate to well under a second.
fs::file_time_type file_time_at(SystemClock::time_point t)
{
	return fs::file_time_type::clock::now() +
	       std::chrono::duration_cast<fs::file_time_type::duration>(t - SystemClock::now());
}

void set_mtime(const fs::path &p, SystemClock::time_point t)
{
	std::error_code ec;
	fs::last_write_time(p, file_time_at(t), ec);
	REQUIRE(!ec);
}

// Sets an exact file time, so several files can share one stamp.
void set_mtime(const fs::path &p, fs::file_time_type stamp)
{
	std::error_code ec;
	fs::last_write_time(p, stamp, ec);
	REQUIRE(!ec);
}

// Fixed times, so the ClipIndex tests never depend on the wall clock: at(n) is n minutes
// after an arbitrary epoch.
SystemClock::time_point at(long long minutes_after)
{
	return SystemClock::time_point(std::chrono::seconds(1'800'000'000LL)) + std::chrono::minutes(minutes_after);
}

// The path make_entry gives a clip: <clips>/<game>/<file>, or <clips>/<file> when unfiled.
fs::path entry_path(const std::string &game, const std::string &file_name)
{
	const fs::path clips = "clips";
	if (game.empty())
		return clips / path_from_utf8(file_name);
	return clips / path_from_utf8(game) / path_from_utf8(file_name);
}

// A clip for the ClipIndex tests: the fields the index reads, with a fixed size.
ClipEntry make_entry(const std::string &game, const std::string &file_name, SystemClock::time_point modified)
{
	ClipEntry entry;
	entry.path = entry_path(game, file_name);
	entry.file_name = file_name;
	entry.game_folder = game;
	entry.modified = modified;
	entry.size = 1;
	return entry;
}

// The file names of `clips`, in order.
Names names_of(const std::vector<ClipEntry> &clips)
{
	Names names;
	for (const ClipEntry &clip : clips)
		names.push_back(clip.file_name);
	return names;
}

// The game folder as text, or "<none>" for nullopt, so a CHECK can compare plain strings.
std::string game_or_none(const std::optional<std::string> &game)
{
	return game ? *game : "<none>";
}

// The new file name as text, or "<rejected>". Lets a CHECK compare plain strings.
std::string renamed(const fs::path &old_file, const std::string &stem)
{
	const std::optional<std::string> name = renamed_file_name(old_file, stem);
	return name ? *name : "<rejected>";
}

} // namespace

TEST_CASE("path_to_utf8 round-trips non-ASCII names", "[clip-index]")
{
	const std::string name = "Ελληνικά é 日本.mkv";
	CHECK(path_to_utf8(path_from_utf8(name)) == name);
	CHECK(path_to_utf8(path_from_utf8("plain.mp4")) == "plain.mp4");
	CHECK(path_to_utf8(fs::path()) == "");
}

TEST_CASE("is_clip_file accepts every OBS video and image extension, in any case", "[clip-index]")
{
	const Names names = {"a.mkv", "a.mp4", "a.mov",  "a.flv", "a.ts",  "a.webm",
			     "a.png", "a.jpg", "a.jpeg", "a.bmp", "a.webp"};
	for (const std::string &name : names)
		CHECK(is_clip_file(name));
	// The extension is compared ignoring ASCII case.
	CHECK(is_clip_file("LOUD.MKV"));
	CHECK(is_clip_file("Mixed.Mp4"));
	CHECK(is_clip_file("shot.JPEG"));
	CHECK(is_clip_file("shot.WebP"));
	// Only the last extension counts, so a name with a clip extension in the middle is a clip.
	CHECK(is_clip_file("x.tar.mkv"));
}

TEST_CASE("is_clip_file rejects other files, partial copies and names without an extension", "[clip-index]")
{
	CHECK_FALSE(is_clip_file("notes.txt"));
	CHECK_FALSE(is_clip_file("archive.zip"));
	CHECK_FALSE(is_clip_file("clip.avi")); // not an extension OBS saves
	CHECK_FALSE(is_clip_file("x.mkv.txt"));
	// file-mover's copy in progress is never a clip.
	CHECK_FALSE(is_clip_file("a.mp4.clip-atlas-partial"));
	CHECK_FALSE(is_clip_file("a.mkv.clip-atlas-partial"));
	// No extension at all.
	CHECK_FALSE(is_clip_file("recording"));
	CHECK_FALSE(is_clip_file(""));
	// A name that is only ".mkv" has no extension: the leading dot belongs to the name, so
	// it is not a clip. The scan skips such hidden names anyway.
	CHECK_FALSE(is_clip_file(".mkv"));
	CHECK_FALSE(is_clip_file(".mp4"));
	// A trailing dot is not an extension.
	CHECK_FALSE(is_clip_file("a.mkv."));
}

TEST_CASE("kind_of_clip tells screenshots, replays and recordings apart", "[clip-index]")
{
	const ScanOptions options{}; // replay_prefix "Replay"
	CHECK(kind_of_clip("Replay 2026-10-21 18-04-12.mkv", options) == ClipKind::replay);
	CHECK(kind_of_clip("replay clip.mp4", options) == ClipKind::replay); // the prefix ignores ASCII case
	CHECK(kind_of_clip("REPLAY.MKV", options) == ClipKind::replay);
	CHECK(kind_of_clip("2026-10-21 17-50-03.mkv", options) == ClipKind::recording);
	CHECK(kind_of_clip("My Replay.mkv", options) == ClipKind::recording); // the prefix must start the name
	// Images are screenshots whatever their name says.
	CHECK(kind_of_clip("Replay 1.png", options) == ClipKind::screenshot);
	CHECK(kind_of_clip("shot.JPG", options) == ClipKind::screenshot);
}

TEST_CASE("kind_of_clip with an empty or custom replay prefix", "[clip-index]")
{
	ScanOptions none;
	none.replay_prefix.clear();
	// An empty prefix means no replays at all, rather than every name matching "".
	CHECK(kind_of_clip("Replay 1.mkv", none) == ClipKind::recording);

	ScanOptions custom;
	custom.replay_prefix = "Buffer_";
	CHECK(kind_of_clip("buffer_0001.mkv", custom) == ClipKind::replay);
	CHECK(kind_of_clip("Replay 1.mkv", custom) == ClipKind::recording);
	// A prefix longer than the name never matches.
	CHECK(kind_of_clip("Re.mkv", ScanOptions{}) == ClipKind::recording);
}

TEST_CASE("game_folder_of names the first folder below base, up to three levels", "[clip-index]")
{
	const fs::path base = "D:/Clips";
	CHECK(game_or_none(game_folder_of(base, "D:/Clips/x.mkv")) == "");                          // in base
	CHECK(game_or_none(game_folder_of(base, "D:/Clips/Game/x.mkv")) == "Game");                 // level 2
	CHECK(game_or_none(game_folder_of(base, "D:/Clips/Game/2026-10/x.mkv")) == "Game");         // level 3
	CHECK(game_or_none(game_folder_of(base, "D:/Clips/Game/2026-10/extra/x.mkv")) == "<none>"); // level 4
	CHECK(game_or_none(game_folder_of(base, "D:/Clips/a/b/c/d/x.mkv")) == "<none>");
}

TEST_CASE("game_folder_of treats a trailing separator on base like no separator", "[clip-index]")
{
	const fs::path with_sep = "D:/Clips/";
	CHECK(game_or_none(game_folder_of(with_sep, "D:/Clips/x.mkv")) == "");
	CHECK(game_or_none(game_folder_of(with_sep, "D:/Clips/Game/x.mkv")) == "Game");
	CHECK(game_or_none(game_folder_of(with_sep, "D:/Clips/Game/2026-10/y.mp4")) == "Game");
}

TEST_CASE("game_folder_of gives nullopt for base itself, outside files and look-alike siblings", "[clip-index]")
{
	const fs::path base = "D:/Clips";
	CHECK(game_or_none(game_folder_of(base, "D:/Clips")) == "<none>");
	CHECK(game_or_none(game_folder_of(base, "D:/Clips/")) == "<none>");
	CHECK(game_or_none(game_folder_of(base, "D:/Other/x.mkv")) == "<none>");
	CHECK(game_or_none(game_folder_of(base, "E:/Clips/x.mkv")) == "<none>");
	// Components are compared whole: "Clips2" is not inside "Clips".
	CHECK(game_or_none(game_folder_of(base, "D:/Clips2/x.mkv")) == "<none>");
	CHECK(game_or_none(game_folder_of(base, "D:/Clips2/Game/x.mkv")) == "<none>");
	// Exact, not case-folded: both paths come from the same settings string.
	CHECK(game_or_none(game_folder_of(base, "D:/clips/Game/x.mkv")) == "<none>");
}

TEST_CASE("game_folder_of normalises . and .. before it compares", "[clip-index]")
{
	CHECK(game_or_none(game_folder_of("clips", "clips/../clips/Game/x.mkv")) == "Game");
	CHECK(game_or_none(game_folder_of("clips", "clips/./Game/x.mkv")) == "Game");
	CHECK(game_or_none(game_folder_of("clips", "elsewhere/../clips/x.mkv")) == "");
	// An empty base names no folder at all.
	CHECK(game_or_none(game_folder_of(fs::path(), "x.mkv")) == "<none>");
}

TEST_CASE("game_folder_of returns non-ASCII folder names as UTF-8", "[clip-index]")
{
	const fs::path base = path_from_utf8("クリップ");
	const fs::path file = base / path_from_utf8("ゲーム") / "2026-10" / path_from_utf8("Ελληνικά é 日本.mkv");
	CHECK(game_or_none(game_folder_of(base, file)) == "ゲーム");
}

TEST_CASE("to_system_time maps the current file time to the current system time", "[clip-index]")
{
	const SystemClock::time_point before = SystemClock::now();
	const SystemClock::time_point mapped = to_system_time(fs::file_time_type::clock::now());
	const SystemClock::time_point after = SystemClock::now();
	// The two clocks are read at different moments, so allow a second either way.
	CHECK(mapped >= before - std::chrono::seconds(1));
	CHECK(mapped <= after + std::chrono::seconds(1));
}

TEST_CASE("describe_clip reads a clip's game, kind, size and time", "[clip-index]")
{
	TempDir tmp;
	const fs::path base = tmp.path();
	const fs::path file = base / "Game" / "Replay 2026-10-21 18-04-12.mkv";
	write_file(file, "0123456789");
	const SystemClock::time_point when = SystemClock::now() - std::chrono::hours(3);
	set_mtime(file, when);

	const std::optional<ClipEntry> entry = describe_clip(base, file, ScanOptions{});
	REQUIRE(entry.has_value());
	CHECK(entry->path == file);
	CHECK(entry->file_name == "Replay 2026-10-21 18-04-12.mkv");
	CHECK(entry->game_folder == "Game");
	CHECK(entry->kind == ClipKind::replay);
	CHECK(entry->size == 10);
	// The time maps back to the one that was set, to within a second.
	CHECK(std::chrono::abs(entry->modified - when) < std::chrono::seconds(1));
}

TEST_CASE("describe_clip gives nullopt for anything a scan would not list", "[clip-index]")
{
	TempDir tmp;
	const fs::path base = tmp.path();
	const ScanOptions options{};

	CHECK_FALSE(describe_clip(base, base / "Game" / "missing.mkv", options).has_value());
	fs::create_directories(base / "Game" / "Folder.mkv");
	CHECK_FALSE(describe_clip(base, base / "Game" / "Folder.mkv", options).has_value()); // a folder
	write_file(base / "Game" / "notes.txt");
	CHECK_FALSE(describe_clip(base, base / "Game" / "notes.txt", options).has_value()); // not a clip
	write_file(base / "Game" / "copy.mkv.clip-atlas-partial");
	CHECK_FALSE(describe_clip(base, base / "Game" / "copy.mkv.clip-atlas-partial", options).has_value());
	write_file(base / "A" / "B" / "C" / "deep.mkv");
	CHECK_FALSE(describe_clip(base, base / "A" / "B" / "C" / "deep.mkv", options).has_value()); // too deep
	write_file(base / ".hidden.mkv");
	CHECK_FALSE(describe_clip(base, base / ".hidden.mkv", options).has_value());
	write_file(base / ".Secret" / "x.mkv");
	CHECK_FALSE(describe_clip(base, base / ".Secret" / "x.mkv", options).has_value()); // in a hidden folder
	CHECK_FALSE(describe_clip(base, base, options).has_value());                       // base itself
}

TEST_CASE("describe_clip lists a file directly in base as unsorted, and keeps UTF-8 names", "[clip-index]")
{
	TempDir tmp;
	const fs::path base = tmp.path();
	const fs::path direct = base / "2026-10-21 17-50-03.mp4";
	write_file(direct);
	const std::optional<ClipEntry> unsorted = describe_clip(base, direct, ScanOptions{});
	REQUIRE(unsorted.has_value());
	CHECK(unsorted->game_folder == "");
	CHECK(unsorted->kind == ClipKind::recording);

	const std::string name = "Ελληνικά é 日本.mkv";
	const fs::path unicode = base / path_from_utf8("ゲーム") / path_from_utf8(name);
	write_file(unicode, "uu");
	const std::optional<ClipEntry> entry = describe_clip(base, unicode, ScanOptions{});
	REQUIRE(entry.has_value());
	CHECK(entry->file_name == name);
	CHECK(entry->game_folder == "ゲーム");
	CHECK(entry->size == 2);
}

TEST_CASE("describe_clip never follows a symlink", "[clip-index]")
{
	TempDir tmp;
	const fs::path base = tmp.path();
	const fs::path target = base / "Game" / "real.mkv";
	write_file(target);
	const fs::path link = base / "Game" / "link.mkv";
	std::error_code ec;
	fs::create_symlink(target, link, ec);
	if (ec) {
		SKIP("symbolic links are not available on this system");
		return;
	}
	CHECK_FALSE(describe_clip(base, link, ScanOptions{}).has_value());
	CHECK(describe_clip(base, target, ScanOptions{}).has_value());
}

TEST_CASE("scan_clips_folder lists clips up to three levels deep, newest first", "[clip-index]")
{
	TempDir tmp;
	const fs::path base = tmp.path() / "Clips";
	const SystemClock::time_point t0 = SystemClock::now() - std::chrono::hours(48);
	// Writes a file under base with a time `minutes_after` past t0, which sets its place in the order.
	const auto put = [&base, &t0](const fs::path &rel, long long minutes_after) {
		write_file(base / rel);
		set_mtime(base / rel, t0 + std::chrono::minutes(minutes_after));
	};
	// What a scan lists: one file for each place it looks.
	put("direct.mkv", 10);                                                     // directly in base: unsorted
	put(fs::path("Game") / "a.mkv", 20);                                       // a game folder
	put(fs::path("Game") / "2026-10" / "b.mp4", 30);                           // a game and a month folder
	put(fs::path("Game") / "2026-10" / path_from_utf8("Replay 1.mkv"), 40);    // a replay
	put(fs::path("Other") / "shot.png", 5);                                    // a screenshot
	put(path_from_utf8("ゲーム") / path_from_utf8("Ελληνικά é 日本.mkv"), 50); // non-ASCII names
	// What it must leave out.
	put("notes.txt", 60);
	put(fs::path("Game") / "readme.md", 61);
	put(fs::path("Game") / "2026-10" / "half.mkv.clip-atlas-partial", 62);
	put(fs::path("Game") / "2026-10" / "extra" / "deep.mkv", 63); // a fourth level
	put(".hidden.mkv", 64);
	put(fs::path(".Secret") / "x.mkv", 65);   // inside a hidden folder
	put(fs::path("Game") / ".thumb.png", 66); // a hidden file

	const ScanResult result = scan_clips_folder(base, ScanOptions{});
	CHECK(result.base_exists);
	CHECK_FALSE(result.truncated);
	const Names expected_names = {"Ελληνικά é 日本.mkv", "Replay 1.mkv", "b.mp4", "a.mkv",
				      "direct.mkv",          "shot.png"};
	const Names expected_games = {"ゲーム", "Game", "Game", "Game", "", "Other"};
	CHECK(names_of(result.clips) == expected_names);
	REQUIRE(result.clips.size() == expected_games.size());
	for (std::size_t i = 0; i < expected_games.size(); ++i)
		CHECK(result.clips[i].game_folder == expected_games[i]);
	CHECK(result.clips[1].kind == ClipKind::replay);
	CHECK(result.clips[4].kind == ClipKind::recording);
	CHECK(result.clips[5].kind == ClipKind::screenshot);
}

TEST_CASE("scan_clips_folder breaks equal modification times by path, descending", "[clip-index]")
{
	TempDir tmp;
	const fs::path base = tmp.path();
	// One stamp for all three files, so their file times are equal to the last tick.
	const fs::file_time_type same = file_time_at(SystemClock::now() - std::chrono::hours(5));
	for (const char *rel : {"a/z.mkv", "z/a.mkv", "m.mkv"}) {
		write_file(base / rel);
		set_mtime(base / rel, same);
	}
	const ScanResult result = scan_clips_folder(base, ScanOptions{});
	REQUIRE(result.clips.size() == 3);
	// Path order, descending: z/a.mkv, then m.mkv, then a/z.mkv.
	CHECK(result.clips[0].path == base / "z" / "a.mkv");
	CHECK(result.clips[1].path == base / "m.mkv");
	CHECK(result.clips[2].path == base / "a" / "z.mkv");
}

TEST_CASE("scan_clips_folder keeps the newest max_clips and says when it cut some", "[clip-index]")
{
	TempDir tmp;
	const SystemClock::time_point t0 = SystemClock::now() - std::chrono::hours(5);
	for (int i = 0; i < 5; ++i) {
		const fs::path file = tmp.path() / ("clip " + std::to_string(i) + ".mkv");
		write_file(file);
		set_mtime(file, t0 + std::chrono::minutes(i));
	}
	ScanOptions options;
	options.max_clips = 3;
	const ScanResult cut = scan_clips_folder(tmp.path(), options);
	const Names newest_three = {"clip 4.mkv", "clip 3.mkv", "clip 2.mkv"};
	CHECK(cut.truncated);
	CHECK(names_of(cut.clips) == newest_three);

	options.max_clips = 5;
	const ScanResult whole = scan_clips_folder(tmp.path(), options);
	CHECK_FALSE(whole.truncated);
	CHECK(whole.clips.size() == 5);

	options.max_clips = 0;
	const ScanResult none = scan_clips_folder(tmp.path(), options);
	CHECK(none.base_exists);
	CHECK(none.clips.empty());
	CHECK(none.truncated);
}

TEST_CASE("scan_clips_folder reports a missing, empty or non-folder base", "[clip-index]")
{
	TempDir tmp;
	const ScanOptions options{};

	const ScanResult empty = scan_clips_folder(tmp.path(), options);
	CHECK(empty.base_exists);
	CHECK(empty.clips.empty());

	const ScanResult missing = scan_clips_folder(tmp.path() / "gone", options);
	CHECK_FALSE(missing.base_exists);
	CHECK(missing.clips.empty());
	CHECK_FALSE(missing.truncated);

	const ScanResult no_path = scan_clips_folder(fs::path(), options);
	CHECK_FALSE(no_path.base_exists);
	CHECK(no_path.clips.empty());

	write_file(tmp.path() / "plain.mkv");
	const ScanResult as_file = scan_clips_folder(tmp.path() / "plain.mkv", options);
	CHECK_FALSE(as_file.base_exists);
	CHECK(as_file.clips.empty());
}

TEST_CASE("scan_clips_folder skips symlinks to files and to folders", "[clip-index]")
{
	TempDir tmp;
	const fs::path base = tmp.path();
	write_file(base / "Real" / "clip.mkv");
	std::error_code ec;
	fs::create_directory_symlink(base / "Real", base / "Linked", ec);
	if (ec) {
		SKIP("symbolic links are not available on this system");
		return;
	}
	fs::create_symlink(base / "Real" / "clip.mkv", base / "Real" / "alias.mkv", ec);
	if (ec) {
		SKIP("symbolic links are not available on this system");
		return;
	}
	const ScanResult result = scan_clips_folder(base, ScanOptions{});
	REQUIRE(result.clips.size() == 1);
	CHECK(result.clips[0].file_name == "clip.mkv");
	CHECK(result.clips[0].game_folder == "Real");
}

TEST_CASE("ClipIndex holds the capacity it was given", "[clip-index]")
{
	CHECK(ClipIndex().max_clips() == 5000);
	CHECK(ClipIndex(3).max_clips() == 3);
	CHECK(ClipIndex(0).clips().empty());
}

TEST_CASE("ClipIndex::reset sorts newest first and keeps the first of duplicate paths", "[clip-index]")
{
	ClipIndex index;
	ClipEntry duplicate = make_entry("Game", "a.mkv", at(9)); // same path, newer, and later in the input
	duplicate.size = 99;
	index.reset({make_entry("Game", "a.mkv", at(1)), make_entry("Game", "b.mkv", at(3)), duplicate,
		     make_entry("", "c.mkv", at(2))});
	const Names expected = {"b.mkv", "c.mkv", "a.mkv"};
	CHECK(names_of(index.clips()) == expected);
	const ClipEntry *a = index.find(entry_path("Game", "a.mkv"));
	REQUIRE(a != nullptr);
	CHECK(a->size == 1);
	CHECK(a->modified == at(1));
}

TEST_CASE("ClipIndex::reset keeps only the newest max_clips", "[clip-index]")
{
	ClipIndex index(2);
	index.reset({make_entry("Game", "a.mkv", at(1)), make_entry("Game", "b.mkv", at(3)),
		     make_entry("Game", "c.mkv", at(2))});
	const Names expected = {"b.mkv", "c.mkv"};
	CHECK(names_of(index.clips()) == expected);
	CHECK(index.find(entry_path("Game", "a.mkv")) == nullptr);
	CHECK(index.max_clips() == 2);
}

TEST_CASE("ClipIndex::upsert inserts in order and replaces an entry with the same path", "[clip-index]")
{
	ClipIndex index;
	CHECK(index.upsert(make_entry("Game", "mid.mkv", at(5))));
	CHECK(index.upsert(make_entry("Game", "old.mkv", at(1))));
	CHECK(index.upsert(make_entry("Game", "new.mkv", at(9))));
	const Names in_order = {"new.mkv", "mid.mkv", "old.mkv"};
	CHECK(names_of(index.clips()) == in_order);

	// The same path again, now the newest: it moves to the front and is not duplicated.
	ClipEntry again = make_entry("Game", "old.mkv", at(10));
	again.size = 42;
	CHECK(index.upsert(again));
	const Names moved_front = {"old.mkv", "new.mkv", "mid.mkv"};
	CHECK(names_of(index.clips()) == moved_front);
	REQUIRE(index.find(again.path) != nullptr);
	CHECK(index.find(again.path)->size == 42);

	// Replaced with an older time: it moves to the back.
	CHECK(index.upsert(make_entry("Game", "old.mkv", at(0))));
	const Names moved_back = {"new.mkv", "mid.mkv", "old.mkv"};
	CHECK(names_of(index.clips()) == moved_back);
	CHECK(index.clips().size() == 3);
}

TEST_CASE("ClipIndex::upsert drops the oldest past capacity, and reports a dropped new clip", "[clip-index]")
{
	ClipIndex index(2);
	CHECK(index.upsert(make_entry("Game", "a.mkv", at(3))));
	CHECK(index.upsert(make_entry("Game", "b.mkv", at(2))));

	// Older than both: the new clip is the one dropped.
	CHECK_FALSE(index.upsert(make_entry("Game", "c.mkv", at(1))));
	const Names kept = {"a.mkv", "b.mkv"};
	CHECK(names_of(index.clips()) == kept);
	CHECK(index.find(entry_path("Game", "c.mkv")) == nullptr);

	// Newer than both: the oldest, b.mkv, goes, and the new clip stays.
	CHECK(index.upsert(make_entry("Game", "d.mkv", at(4))));
	const Names evicted = {"d.mkv", "a.mkv"};
	CHECK(names_of(index.clips()) == evicted);
	CHECK(index.find(entry_path("Game", "b.mkv")) == nullptr);

	// Replacing an entry never evicts anything: the old copy leaves before the new one goes in.
	CHECK(index.upsert(make_entry("Game", "a.mkv", at(0))));
	const Names replaced = {"d.mkv", "a.mkv"};
	CHECK(names_of(index.clips()) == replaced);
	CHECK(index.clips().size() == 2);
}

TEST_CASE("ClipIndex with zero capacity keeps nothing", "[clip-index]")
{
	ClipIndex index(0);
	CHECK_FALSE(index.upsert(make_entry("Game", "a.mkv", at(1))));
	CHECK(index.clips().empty());
	index.reset({make_entry("Game", "b.mkv", at(2))});
	CHECK(index.clips().empty());
}

TEST_CASE("ClipIndex::remove and find match the exact path only", "[clip-index]")
{
	ClipIndex index;
	index.upsert(make_entry("Game", "a.mkv", at(1)));
	index.upsert(make_entry("Other", "a.mkv", at(2))); // the same file name in another folder is another path
	REQUIRE(index.find(entry_path("Game", "a.mkv")) != nullptr);
	CHECK(index.find(entry_path("Game", "missing.mkv")) == nullptr);

	CHECK_FALSE(index.remove(entry_path("Game", "missing.mkv")));
	CHECK(index.clips().size() == 2);
	CHECK(index.remove(entry_path("Game", "a.mkv")));
	CHECK(index.clips().size() == 1);
	CHECK(index.find(entry_path("Game", "a.mkv")) == nullptr);
	CHECK(index.find(entry_path("Other", "a.mkv")) != nullptr);
	CHECK_FALSE(index.remove(entry_path("Game", "a.mkv"))); // already gone
}

TEST_CASE("ClipIndex::games lists each folder once, sorted ignoring ASCII case", "[clip-index]")
{
	ClipIndex index;
	index.upsert(make_entry("beta", "1.mkv", at(1)));
	index.upsert(make_entry("Gamma", "2.mkv", at(2)));
	index.upsert(make_entry("Gamma", "3.mkv", at(3))); // a second clip in a folder adds no entry
	index.upsert(make_entry("Alpha", "4.mkv", at(4)));
	index.upsert(make_entry("alpha", "5.mkv", at(5))); // the same letters in other case: a folder of its own
	index.upsert(make_entry("Beta", "6.mkv", at(6)));
	index.upsert(make_entry("", "7.mkv", at(7))); // unfiled: not a game
	const Names expected = {"Alpha", "alpha", "Beta", "beta", "Gamma"};
	CHECK(index.games() == expected);
}

TEST_CASE("ClipIndex::has_unfiled is true only while a clip sits directly in base", "[clip-index]")
{
	ClipIndex index;
	CHECK_FALSE(index.has_unfiled());
	index.upsert(make_entry("Game", "a.mkv", at(1)));
	CHECK_FALSE(index.has_unfiled());
	index.upsert(make_entry("", "u.mkv", at(2)));
	CHECK(index.has_unfiled());
	CHECK(index.remove(entry_path("", "u.mkv")));
	CHECK_FALSE(index.has_unfiled());
}

TEST_CASE("ClipIndex::query filters by game folder, and an empty folder means unfiled only", "[clip-index]")
{
	ClipIndex index;
	index.upsert(make_entry("Game", "g1.mkv", at(4)));
	index.upsert(make_entry("Other", "o1.mkv", at(3)));
	index.upsert(make_entry("", "u1.mkv", at(2)));
	index.upsert(make_entry("Game", "g2.mkv", at(1)));

	ClipQuery everything;
	const Names all = {"g1.mkv", "o1.mkv", "u1.mkv", "g2.mkv"};
	CHECK(names_of(index.query(everything)) == all);

	ClipQuery game;
	game.game_folder = "Game";
	const Names game_names = {"g1.mkv", "g2.mkv"};
	CHECK(names_of(index.query(game)) == game_names);

	ClipQuery unfiled;
	unfiled.game_folder = "";
	const Names unfiled_names = {"u1.mkv"};
	CHECK(names_of(index.query(unfiled)) == unfiled_names);

	ClipQuery lower_case;
	lower_case.game_folder = "game"; // the folder filter is exact, unlike the search
	CHECK(index.query(lower_case).empty());

	ClipQuery missing;
	missing.game_folder = "Nope";
	CHECK(index.query(missing).empty());
}

TEST_CASE("ClipIndex::query search needs every term to match the name or the folder", "[clip-index]")
{
	ClipIndex index;
	index.upsert(make_entry("CS2", "Replay 2026-10-21 18-04-12.mkv", at(4)));
	index.upsert(make_entry("Fortnite", "Replay 2026-10-20.mkv", at(3)));
	index.upsert(make_entry("CS2", "2026-10-19 Recording.mp4", at(2)));
	index.upsert(make_entry("", "Desktop Replay.png", at(1)));

	ClipQuery q;
	q.search = "REPLAY"; // ignores ASCII case
	const Names replays = {"Replay 2026-10-21 18-04-12.mkv", "Replay 2026-10-20.mkv", "Desktop Replay.png"};
	CHECK(names_of(index.query(q)) == replays);

	q.search = "replay cs2"; // "replay" matches the name and "cs2" the folder
	const Names one = {"Replay 2026-10-21 18-04-12.mkv"};
	CHECK(names_of(index.query(q)) == one);

	q.search = "fortnite"; // a match on the folder alone
	const Names fortnite = {"Replay 2026-10-20.mkv"};
	CHECK(names_of(index.query(q)) == fortnite);

	q.search = "replay recording"; // every term has to match somewhere
	CHECK(index.query(q).empty());

	q.search = "  cs2 \t "; // blanks around the terms are not terms
	const Names cs2 = {"Replay 2026-10-21 18-04-12.mkv", "2026-10-19 Recording.mp4"};
	CHECK(names_of(index.query(q)) == cs2);

	q.search = " \t "; // blanks only: no terms, so everything matches
	CHECK(index.query(q).size() == 4);

	// The game filter and the search together.
	ClipQuery both;
	both.game_folder = "CS2";
	both.search = "replay";
	const Names cs2_replays = {"Replay 2026-10-21 18-04-12.mkv"};
	CHECK(names_of(index.query(both)) == cs2_replays);
	both.game_folder = "";
	const Names unfiled_replays = {"Desktop Replay.png"};
	CHECK(names_of(index.query(both)) == unfiled_replays);
}

namespace {

// True when `age` has this unit and count.
bool is_age(const Age &age, AgeUnit unit, long long count)
{
	return age.unit == unit && age.count == count;
}

} // namespace

TEST_CASE("describe_age switches unit at each boundary", "[clip-index]")
{
	using std::chrono::seconds;
	const SystemClock::time_point now = at(0);
	const auto age_of = [&now](seconds ago) {
		return describe_age(now - ago, now);
	};
	CHECK(is_age(age_of(seconds(0)), AgeUnit::just_now, 0));
	CHECK(is_age(age_of(seconds(59)), AgeUnit::just_now, 0));
	CHECK(is_age(age_of(seconds(60)), AgeUnit::minutes, 1));
	CHECK(is_age(age_of(seconds(119)), AgeUnit::minutes, 1));   // rounds down
	CHECK(is_age(age_of(seconds(3540)), AgeUnit::minutes, 59)); // 59 min
	CHECK(is_age(age_of(seconds(3599)), AgeUnit::minutes, 59)); // 59 min 59 s
	CHECK(is_age(age_of(seconds(3600)), AgeUnit::hours, 1));    // 60 min
	CHECK(is_age(age_of(seconds(82800)), AgeUnit::hours, 23));  // 23 h
	CHECK(is_age(age_of(seconds(86399)), AgeUnit::hours, 23));  // 23 h 59 min 59 s
	CHECK(is_age(age_of(seconds(86400)), AgeUnit::days, 1));    // 24 h
	CHECK(is_age(age_of(seconds(2 * 86400 + 5 * 3600)), AgeUnit::days, 2));
	CHECK(is_age(age_of(seconds(518400)), AgeUnit::days, 6)); // 6 d
	CHECK(is_age(age_of(seconds(604799)), AgeUnit::days, 6)); // 6 d 23 h 59 min 59 s
	CHECK(is_age(age_of(seconds(604800)), AgeUnit::date, 0)); // 7 d
	CHECK(is_age(age_of(seconds(400 * 86400)), AgeUnit::date, 0));
}

TEST_CASE("describe_age reads a time after now as just now", "[clip-index]")
{
	const SystemClock::time_point now = at(0);
	CHECK(is_age(describe_age(now, now), AgeUnit::just_now, 0));
	CHECK(is_age(describe_age(now + std::chrono::minutes(5), now), AgeUnit::just_now, 0)); // clock set back
	CHECK(is_age(describe_age(now + std::chrono::hours(48), now), AgeUnit::just_now, 0));
}

TEST_CASE("format_local_date gives the local calendar date as YYYY-MM-DD", "[clip-index]")
{
	// Built with mktime, so the expected dates are local, as the function's are.
	const auto local_time = [](int year, int month, int day, int hour, int minute) {
		std::tm tm{};
		tm.tm_year = year - 1900;
		tm.tm_mon = month - 1;
		tm.tm_mday = day;
		tm.tm_hour = hour;
		tm.tm_min = minute;
		tm.tm_isdst = -1;
		return SystemClock::from_time_t(std::mktime(&tm));
	};
	CHECK(format_local_date(local_time(2026, 10, 21, 18, 4)) == "2026-10-21");
	CHECK(format_local_date(local_time(2026, 9, 9, 12, 0)) == "2026-09-09"); // zero-padded
	CHECK(format_local_date(local_time(2027, 1, 1, 0, 30)) == "2027-01-01"); // just after midnight
	CHECK(format_local_date(local_time(2026, 12, 31, 23, 59)) == "2026-12-31");
}

TEST_CASE("format_size rounds down, with one decimal only below 10", "[clip-index]")
{
	CHECK(format_size(0) == "0 B");
	CHECK(format_size(999) == "999 B");
	CHECK(format_size(1023) == "1023 B");
	CHECK(format_size(1024) == "1.0 KB");
	CHECK(format_size(1536) == "1.5 KB");
	CHECK(format_size(10239) == "9.9 KB");
	CHECK(format_size(10240) == "10 KB");
	CHECK(format_size(1048575) == "1023 KB");
	CHECK(format_size(1048576) == "1.0 MB");
	CHECK(format_size(88080384) == "84 MB");
	CHECK(format_size(1073741824) == "1.0 GB");
	CHECK(format_size(10737418239ULL) == "9.9 GB"); // 10 GiB less one byte rounds down, not up to 10
	// 1288490188 bytes is 1.19999... GiB. Rounded down to a tenth it reads 1.1, not 1.2.
	CHECK(format_size(1288490188ULL) == "1.1 GB");
	CHECK(format_size(1ULL << 40) == "1.0 TB");
	CHECK(format_size(5ULL << 40) == "5.0 TB");
	CHECK(format_size(2048ULL << 40) == "2048 TB");
	// The largest size stays in TB instead of running out of units.
	CHECK(format_size(std::numeric_limits<std::uintmax_t>::max()) == "16777215 TB");
}

TEST_CASE("renamed_file_name keeps the extension and trims the stem", "[clip-index]")
{
	const fs::path old_file = "clips/Replay 1.mkv";
	CHECK(renamed(old_file, "Highlight") == "Highlight.mkv");
	CHECK(renamed(old_file, "  My clip \t") == "My clip.mkv");
	CHECK(renamed(old_file, "\tTab edges\t") == "Tab edges.mkv");
	CHECK(renamed(old_file, "two  inner   spaces") == "two  inner   spaces.mkv");
	CHECK(renamed(old_file, "v1.2 final") == "v1.2 final.mkv"); // dots inside the stem are fine
	// The extension comes back as it was, case included.
	CHECK(renamed(fs::path("clips/x.MKV"), "y") == "y.MKV");
	// Only the file name matters, not the folders above it.
	CHECK(renamed(fs::path("dir/sub/a.mkv"), "b") == "b.mkv");
}

TEST_CASE("renamed_file_name accepts non-ASCII stems", "[clip-index]")
{
	const fs::path old_file = "clips/a.mkv";
	CHECK(renamed(old_file, "ポケモン 2026") == "ポケモン 2026.mkv");
	CHECK(renamed(old_file, "Ελληνικά é 日本") == "Ελληνικά é 日本.mkv");
}

TEST_CASE("renamed_file_name rejects blank stems", "[clip-index]")
{
	const fs::path old_file = "clips/a.mkv";
	CHECK(renamed(old_file, "") == "<rejected>");
	CHECK(renamed(old_file, "   ") == "<rejected>");
	CHECK(renamed(old_file, " \t \t") == "<rejected>");
}

TEST_CASE("renamed_file_name rejects characters Windows forbids and control characters", "[clip-index]")
{
	const fs::path old_file = "clips/a.mkv";
	for (const char *bad : {"a<b", "a>b", "a:b", "a\"b", "a/b", "a\\b", "a|b", "a?b", "a*b"})
		CHECK(renamed(old_file, bad) == "<rejected>");
	// Control characters are refused anywhere in the stem. Only the ends are trimmed, so an inner tab counts.
	CHECK(renamed(old_file, std::string("a") + static_cast<char>(0x01) + "b") == "<rejected>");
	CHECK(renamed(old_file, std::string("a") + static_cast<char>(0x1f) + "b") == "<rejected>");
	CHECK(renamed(old_file, "a\tb") == "<rejected>");
	CHECK(renamed(old_file, "a\nb") == "<rejected>");
	CHECK(renamed(old_file, std::string("a\0b", 3)) == "<rejected>");
}

TEST_CASE("renamed_file_name rejects a stem that ends in a dot", "[clip-index]")
{
	const fs::path old_file = "clips/a.mkv";
	CHECK(renamed(old_file, "name.") == "<rejected>");
	CHECK(renamed(old_file, "name. ") == "<rejected>"); // trimmed first, then the dot shows
	CHECK(renamed(old_file, "name .") == "<rejected>");
	CHECK(renamed(old_file, ".") == "<rejected>");
}

TEST_CASE("renamed_file_name rejects Windows device names, ignoring case", "[clip-index]")
{
	const fs::path old_file = "clips/a.mkv";
	for (const char *device :
	     {"CON", "con", "Prn", "AUX", "nul", "COM1", "com9", "LPT1", "lpt9", "CON.backup", "nul .txt", "Com5 .log"})
		CHECK(renamed(old_file, device) == "<rejected>");
	// Names that merely begin with a device name are fine.
	CHECK(renamed(old_file, "CONSOLE") == "CONSOLE.mkv");
	CHECK(renamed(old_file, "NULL") == "NULL.mkv");
	CHECK(renamed(old_file, "COM10") == "COM10.mkv");
	CHECK(renamed(old_file, "LPT0") == "LPT0.mkv");
	CHECK(renamed(old_file, "my CON") == "my CON.mkv");
}

TEST_CASE("renamed_file_name limits the whole name to 255 bytes", "[clip-index]")
{
	const fs::path old_file = "clips/a.mkv";
	// 251 bytes of stem and ".mkv" make exactly 255. One more byte is too long.
	const std::string longest(251, 'a');
	CHECK(renamed(old_file, longest) == longest + ".mkv");
	CHECK(renamed(old_file, longest + "a") == "<rejected>");

	// Bytes count, not characters: 125 two-byte letters are 250 bytes, which with ".mkv" makes 254.
	std::string two_byte;
	for (int i = 0; i < 125; ++i)
		two_byte += "é";
	CHECK(renamed(old_file, two_byte) == two_byte + ".mkv");
	CHECK(renamed(old_file, two_byte + "é") == "<rejected>"); // 252 bytes plus ".mkv" is 256

	// The extension counts too: 251 bytes of extension leave room for a 4-byte stem only.
	const fs::path long_ext = std::string("clips/a.") + std::string(250, 'x');
	CHECK(renamed(long_ext, "abcd") == "abcd." + std::string(250, 'x'));
	CHECK(renamed(long_ext, "abcde") == "<rejected>");
}

TEST_CASE("renamed_file_name handles names without an extension, and refuses the same name", "[clip-index]")
{
	CHECK(renamed(fs::path("clips/notes"), "renamed") == "renamed");
	CHECK(renamed(fs::path("clips/notes"), "notes") == "<rejected>");
	CHECK(renamed(fs::path("clips/a.mkv"), "a") == "<rejected>");     // the same name
	CHECK(renamed(fs::path("clips/a.mkv"), "  a  ") == "<rejected>"); // the same name once trimmed
	CHECK(renamed(fs::path("clips/a.mkv"), "A") == "A.mkv");          // a change of case is a new name
}
