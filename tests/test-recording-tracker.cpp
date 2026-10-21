#include "recording-tracker.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

using namespace clip_atlas;

namespace {

// Fixed origin, so no test reads the real clock.
const SteadyClock::time_point kT0{};

SteadyClock::time_point at(int s)
{
	return kT0 + std::chrono::seconds(s);
}

// Games as the resolver reports them: `key` is the lowercased executable, `name` the folder name. An empty name is no
// game, whatever the key.
const ClipGame kNoGame{};
const ClipGame kCeleste{"celeste.exe", "Celeste"};
const ClipGame kHades{"hades.exe", "Hades"};
const ClipGame kFallback{"fallback.exe", "Fallback"};

} // namespace

TEST_CASE("a single-file recording is filed under its start game")
{
	RecordingTracker tracker;
	const RecordingSessionId id = tracker.start("rec.mkv", kCeleste, kCeleste, at(0));
	CHECK(id != 0);
	CHECK(tracker.active());
	CHECK(tracker.session() == id);

	const FinishedRecording done = tracker.stop("", false, kFallback, at(60));
	const std::vector<std::string> expected{"rec.mkv"};
	CHECK(done.files == expected);
	CHECK(done.game == kCeleste);
	CHECK_FALSE(tracker.active());
	CHECK(tracker.session() == 0);
}

TEST_CASE("a split recording keeps every part in the order OBS wrote it")
{
	RecordingTracker tracker;
	const RecordingSessionId id = tracker.start("1.mkv", kCeleste, kCeleste, at(0));
	CHECK(tracker.file_changed(id, "2.mkv"));
	CHECK(tracker.file_changed(id, "3.mkv"));

	const FinishedRecording done = tracker.stop("3.mkv", false, kFallback, at(90));
	const std::vector<std::string> expected{"1.mkv", "2.mkv", "3.mkv"};
	CHECK(done.files == expected);
	CHECK(done.game == kCeleste);
}

TEST_CASE("a repeated or empty file is not added to the session")
{
	RecordingTracker tracker;
	const RecordingSessionId id = tracker.start("1.mkv", kCeleste, kCeleste, at(0));
	CHECK_FALSE(tracker.file_changed(id, "1.mkv"));
	CHECK_FALSE(tracker.file_changed(id, ""));
	CHECK(tracker.file_changed(id, "2.mkv"));
	CHECK_FALSE(tracker.file_changed(id, "2.mkv"));

	const FinishedRecording done = tracker.stop("", false, kFallback, at(30));
	const std::vector<std::string> expected{"1.mkv", "2.mkv"};
	CHECK(done.files == expected);
}

TEST_CASE("an empty first file is not stored, but the next file is")
{
	RecordingTracker tracker;
	const RecordingSessionId id = tracker.start("", kCeleste, kCeleste, at(0));
	CHECK_FALSE(tracker.file_changed(id, ""));
	CHECK(tracker.file_changed(id, "1.mkv"));

	const FinishedRecording done = tracker.stop("", false, kFallback, at(30));
	const std::vector<std::string> expected{"1.mkv"};
	CHECK(done.files == expected);
}

TEST_CASE("file_changed does nothing without an active session")
{
	RecordingTracker tracker;
	CHECK_FALSE(tracker.file_changed(0, "a.mkv"));
	// The first start hands out id 1, but no session has started yet, so it is not active.
	CHECK_FALSE(tracker.file_changed(1, "a.mkv"));
	CHECK_FALSE(tracker.active());
	CHECK(tracker.session() == 0);
}

TEST_CASE("a late file_changed after stop is ignored")
{
	RecordingTracker tracker;
	const RecordingSessionId id = tracker.start("1.mkv", kCeleste, kCeleste, at(0));
	tracker.stop("1.mkv", false, kFallback, at(10));

	CHECK_FALSE(tracker.file_changed(id, "2.mkv"));
	// The stopped session's files are gone, so a stop with no session and no last file has none.
	const FinishedRecording done = tracker.stop("", false, kFallback, at(20));
	CHECK(done.game == kFallback);
	CHECK(done.files.empty());
}

TEST_CASE("a stale session id is ignored after a restart")
{
	RecordingTracker tracker;
	const RecordingSessionId old_id = tracker.start("old.mkv", kCeleste, kCeleste, at(0));
	const RecordingSessionId new_id = tracker.start("new.mkv", kHades, kHades, at(100));
	CHECK(new_id > old_id);
	CHECK_FALSE(tracker.file_changed(old_id, "old-2.mkv"));
	CHECK(tracker.file_changed(new_id, "new-2.mkv"));

	const FinishedRecording done = tracker.stop("", false, kFallback, at(200));
	const std::vector<std::string> expected{"new.mkv", "new-2.mkv"};
	CHECK(done.files == expected);
	CHECK(done.game == kHades);
}

TEST_CASE("start while active replaces the session and forgets its files and game time")
{
	RecordingTracker tracker;
	const RecordingSessionId old_id = tracker.start("old-1.mkv", kCeleste, kCeleste, at(0));
	CHECK(tracker.file_changed(old_id, "old-2.mkv"));
	// Celeste has 100 s in the old session. It would win below if that time survived the restart.
	tracker.game_changed(kHades, at(100));

	const RecordingSessionId new_id = tracker.start("new-1.mkv", kHades, kHades, at(200));
	CHECK(new_id > old_id);
	CHECK(tracker.session() == new_id);
	CHECK_FALSE(tracker.file_changed(old_id, "old-3.mkv"));

	const FinishedRecording done = tracker.stop("", true, kFallback, at(210));
	const std::vector<std::string> expected{"new-1.mkv"};
	CHECK(done.files == expected);
	CHECK(done.game == kHades); // Hades has 10 s in the new session
}

TEST_CASE("session ids start at 1 and increase with every start")
{
	RecordingTracker tracker;
	const RecordingSessionId first = tracker.start("", kCeleste, kCeleste, at(0));
	CHECK(first == 1);
	tracker.stop("", false, kFallback, at(1));

	const RecordingSessionId second = tracker.start("", kCeleste, kCeleste, at(2));
	const RecordingSessionId third = tracker.start("", kCeleste, kCeleste, at(3)); // replaces the second
	CHECK(second > first);
	CHECK(third > second);
}

TEST_CASE("active() and session() follow the session from start to stop")
{
	RecordingTracker tracker;
	CHECK_FALSE(tracker.active());
	CHECK(tracker.session() == 0);

	const RecordingSessionId id = tracker.start("1.mkv", kCeleste, kCeleste, at(0));
	CHECK(tracker.active());
	CHECK(tracker.session() == id);

	tracker.stop("", false, kFallback, at(10));
	CHECK_FALSE(tracker.active());
	CHECK(tracker.session() == 0);
}

TEST_CASE("stop without a session returns the fallback game and last_file")
{
	RecordingTracker tracker;
	const FinishedRecording none = tracker.stop("", true, kFallback, at(0));
	CHECK(none.game == kFallback);
	CHECK(none.files.empty());

	const FinishedRecording last = tracker.stop("last.mkv", false, kFallback, at(1));
	CHECK(last.game == kFallback);
	const std::vector<std::string> expected{"last.mkv"};
	CHECK(last.files == expected);
	CHECK_FALSE(tracker.active());
}

TEST_CASE("last_file is used only when the session has no files of its own")
{
	RecordingTracker tracker;
	// No first file was known, so OBS's last path is the recording's only file.
	tracker.start("", kCeleste, kCeleste, at(0));
	const FinishedRecording blank_start = tracker.stop("rec.mkv", false, kFallback, at(10));
	const std::vector<std::string> only_last{"rec.mkv"};
	CHECK(blank_start.files == only_last);

	// The session has its own files, so a different path from OBS (say, a multitrack dump) is not added.
	const RecordingSessionId id = tracker.start("rec.mkv", kCeleste, kCeleste, at(20));
	CHECK(tracker.file_changed(id, "rec-2.mkv"));
	const FinishedRecording with_files = tracker.stop("dump.mp4", false, kFallback, at(30));
	const std::vector<std::string> own_files{"rec.mkv", "rec-2.mkv"};
	CHECK(with_files.files == own_files);

	// The same path as the first file is not listed twice.
	tracker.start("rec.mkv", kCeleste, kCeleste, at(40));
	const FinishedRecording same = tracker.stop("rec.mkv", false, kFallback, at(50));
	const std::vector<std::string> once{"rec.mkv"};
	CHECK(same.files == once);
}

TEST_CASE("without dominant game the start game is used even when another game played longer")
{
	RecordingTracker tracker;
	tracker.start("", kCeleste, kCeleste, at(0));
	tracker.game_changed(kHades, at(5));
	const FinishedRecording done = tracker.stop("", false, kFallback, at(100)); // Hades: 95 s
	CHECK(done.game == kCeleste);
}

TEST_CASE("the dominant game adds up every segment it was current for")
{
	RecordingTracker tracker;
	tracker.start("", kCeleste, kCeleste, at(0)); // Celeste 0-10
	tracker.game_changed(kHades, at(10));         // Hades 10-25
	tracker.game_changed(kCeleste, at(25));       // Celeste 25-35
	const FinishedRecording done = tracker.stop("", true, kFallback, at(35));
	// Celeste's 20 s beats Hades' 15 s. Its last segment alone (10 s) would lose.
	CHECK(done.game == kCeleste);
}

TEST_CASE("the game still running when the recording stops counts toward the dominant game")
{
	RecordingTracker tracker;
	tracker.start("", kCeleste, kCeleste, at(0)); // Celeste 0-20
	tracker.game_changed(kHades, at(20));         // Hades 20-45, still running at the stop
	const FinishedRecording done = tracker.stop("", true, kFallback, at(45));
	CHECK(done.game == kHades);
}

TEST_CASE("a game current only at the instant of the start does not win the dominant game")
{
	RecordingTracker tracker;
	tracker.start("", kHades, kCeleste, at(0)); // Celeste is current only at the instant of the start
	const FinishedRecording done = tracker.stop("", true, kFallback, at(0));
	CHECK(done.game == kHades);
}

TEST_CASE("a tie in total goes to the game that was current first")
{
	// Alpha sorts before Zeta by key, so only first_seen can pick Zeta.
	const ClipGame zeta{"zeta.exe", "Zeta"};
	const ClipGame alpha{"alpha.exe", "Alpha"};
	RecordingTracker tracker;
	tracker.start("", kHades, zeta, at(0)); // Zeta is first current at 0 s
	tracker.game_changed(alpha, at(10));    // Alpha is first current at 10 s
	tracker.game_changed(zeta, at(20));
	const FinishedRecording done = tracker.stop("", true, kFallback, at(20)); // 10 s each
	CHECK(done.game == zeta);
}

TEST_CASE("a tie in total goes to the game current first, not the one that sorts last by key")
{
	const ClipGame zeta{"zeta.exe", "Zeta"};
	const ClipGame alpha{"alpha.exe", "Alpha"};
	RecordingTracker tracker;
	tracker.start("", kHades, alpha, at(0)); // Alpha first, 0-10 s
	tracker.game_changed(zeta, at(10));      // Zeta 10-20 s
	const FinishedRecording done = tracker.stop("", true, kFallback, at(20));
	CHECK(done.game == alpha);
}

TEST_CASE("an exact tie in total and first_seen goes to the identity that sorts first")
{
	// Equal total and first_seen fall to map order (the identity that sorts first): deterministic, but arbitrary.
	const ClipGame zeta{"zeta.exe", "Zeta"};
	const ClipGame alpha{"alpha.exe", "Alpha"};
	RecordingTracker tracker;
	// Both are current at 0 s, and each then gets 10 s: alpha 0-10 s, zeta 10-20 s.
	tracker.start("", kHades, zeta, at(0));
	tracker.game_changed(alpha, at(0));
	tracker.game_changed(zeta, at(10));
	tracker.game_changed(alpha, at(20));
	const FinishedRecording done = tracker.stop("", true, kFallback, at(20));
	CHECK(done.game == alpha);
}

TEST_CASE("time with no game never counts toward the dominant game")
{
	RecordingTracker tracker;
	// The desktop is current for an hour, Celeste for a minute.
	tracker.start("", kNoGame, kNoGame, at(0));
	tracker.game_changed(kCeleste, at(3600));
	// A key with no name is no game either, so its two minutes do not count.
	tracker.game_changed(ClipGame{"ghost.exe", ""}, at(3660));
	tracker.game_changed(kNoGame, at(3780));
	const FinishedRecording done = tracker.stop("", true, kFallback, at(3800));
	CHECK(done.game == kCeleste);
}

TEST_CASE("time with no game does not count toward a game with the same key")
{
	RecordingTracker tracker;
	tracker.start("", kCeleste, kCeleste, at(0));              // Celeste 0-10
	tracker.game_changed(ClipGame{"celeste.exe", ""}, at(10)); // a key with no name is no game, 10-60
	tracker.game_changed(kHades, at(60));                      // Hades 60-80 (20 s)
	const FinishedRecording done = tracker.stop("", true, kFallback, at(80));
	CHECK(done.game == kHades);
}

TEST_CASE("with no game time the dominant game is the start game")
{
	RecordingTracker tracker;
	tracker.start("", kHades, kNoGame, at(0)); // the recording starts on the desktop
	const FinishedRecording done = tracker.stop("", true, kFallback, at(60));
	CHECK(done.game == kHades);

	// An unsorted start game stays unsorted. The fallback only applies to a stop with no session.
	tracker.start("", kNoGame, kNoGame, at(100));
	const FinishedRecording desktop = tracker.stop("", true, kFallback, at(160));
	CHECK(desktop.game.unsorted());
}

TEST_CASE("the dominant game is reported with the latest name seen for its key")
{
	const ClipGame old_name{"celeste.exe", "Celeste (Beta)"};
	RecordingTracker tracker;
	tracker.start("", old_name, old_name, at(0));
	tracker.game_changed(kCeleste, at(10)); // the same executable, renamed while recording
	const FinishedRecording done = tracker.stop("", true, kFallback, at(20));
	CHECK(done.game == kCeleste);
}

TEST_CASE("a segment with no game name does not overwrite the name a game is reported with")
{
	const ClipGame old_name{"celeste.exe", "Celeste (Beta)"};
	RecordingTracker tracker;
	tracker.start("", old_name, old_name, at(0));
	tracker.game_changed(ClipGame{"celeste.exe", ""}, at(10)); // the same executable, but no name
	const FinishedRecording done = tracker.stop("", true, kFallback, at(20));
	CHECK(done.game == old_name);
}

TEST_CASE("a game with no key is told apart by its name")
{
	const ClipGame no_key_celeste{"", "Celeste"};
	const ClipGame no_key_hades{"", "Hades"};
	RecordingTracker tracker;
	tracker.start("", no_key_celeste, no_key_celeste, at(0)); // Celeste 0-10
	tracker.game_changed(no_key_hades, at(10));               // Hades 10-12
	tracker.game_changed(no_key_celeste, at(12));             // Celeste again, 12-13
	const FinishedRecording done = tracker.stop("", true, kFallback, at(13));
	// Celeste's 11 s beats Hades' 2 s. A second record for Celeste would give 1 s and lose.
	CHECK(done.game == no_key_celeste);
}

TEST_CASE("a key and a name that read the same are different games")
{
	const ClipGame keyed{"foo", "Bar"};
	const ClipGame named{"", "foo"};
	RecordingTracker tracker;
	tracker.start("", named, named, at(0)); // "foo" by name: 6 s
	tracker.game_changed(keyed, at(6));     // the keyed game: 5 s
	const FinishedRecording done = tracker.stop("", true, kFallback, at(11));
	// Merged, the two would be 11 s and report the keyed game (seen last). Kept apart, the name-only game wins.
	CHECK(done.game == named);
}

TEST_CASE("two different games with no key are not merged")
{
	const ClipGame alpha{"", "Alpha"};
	const ClipGame beta{"", "Beta"};
	RecordingTracker tracker;
	tracker.start("", alpha, alpha, at(0)); // Alpha 0-10
	tracker.game_changed(beta, at(10));     // Beta 10-30 (20 s)
	tracker.game_changed(alpha, at(30));    // Alpha 30-35 (15 s in all)
	const FinishedRecording done = tracker.stop("", true, kFallback, at(35));
	CHECK(done.game == beta);
}

TEST_CASE("the same display name under two executables is two games")
{
	const ClipGame shared_a{"a.exe", "Shared"};
	const ClipGame shared_b{"b.exe", "Shared"};
	RecordingTracker tracker;
	tracker.start("", kHades, shared_a, at(0)); // a 0-10 (10 s)
	tracker.game_changed(shared_b, at(10));     // b 10-25 (15 s)
	tracker.game_changed(shared_a, at(25));     // a 25-27 (12 s in all)
	const FinishedRecording done = tracker.stop("", true, kFallback, at(27));
	CHECK(done.game == shared_b);
}

TEST_CASE("a change to the same game keeps its time without losing or double-counting it")
{
	// Celeste 0-5 and 5-8 is 8 s. Hades then has 6 s, so Celeste wins only if none of its time was lost.
	RecordingTracker tracker;
	tracker.start("", kCeleste, kCeleste, at(0));
	tracker.game_changed(kCeleste, at(5));
	tracker.game_changed(kHades, at(8));
	const FinishedRecording first = tracker.stop("", true, kFallback, at(14));
	CHECK(first.game == kCeleste);

	// Celeste again has 8 s against Hades' 10 s. Counted twice, Celeste would have 13 s and win.
	tracker.start("", kCeleste, kCeleste, at(100));
	tracker.game_changed(kCeleste, at(105));
	tracker.game_changed(kHades, at(108));
	const FinishedRecording second = tracker.stop("", true, kFallback, at(118));
	CHECK(second.game == kHades);
}

TEST_CASE("time going backwards counts as zero")
{
	RecordingTracker tracker;
	tracker.start("", kCeleste, kCeleste, at(0)); // Celeste 0-10
	tracker.game_changed(kHades, at(10));         // Hades 10-17
	tracker.game_changed(kCeleste, at(17));       // Celeste from 17
	// The clock is back at 12. Celeste gains 0 s, not -5 s, and Hades restarts from 12.
	tracker.game_changed(kHades, at(12));
	const FinishedRecording done = tracker.stop("", true, kFallback, at(14)); // Hades: 7 + 2 = 9 s
	// Celeste's 10 s beats Hades' 9 s. A negative 5 s for Celeste would hand the game to Hades.
	CHECK(done.game == kCeleste);
}

TEST_CASE("a clock that goes backwards restarts the running segment at the earlier time")
{
	// Pins the rule we keep: the plugin passes steady_clock::now(), which cannot go backwards.
	RecordingTracker tracker;
	tracker.start("", kCeleste, kCeleste, at(0)); // Celeste 0-10
	tracker.game_changed(kHades, at(10));         // Hades 10-17
	tracker.game_changed(kCeleste, at(17));       // Celeste from 17
	tracker.game_changed(kHades, at(12));         // the clock went back to 12
	const FinishedRecording done = tracker.stop("", true, kFallback, at(20));
	CHECK(done.game == kHades); // Hades 7 + (20 - 12) = 15 s, against Celeste's 10 s
}

TEST_CASE("game_changed without an active session does nothing")
{
	RecordingTracker tracker;
	tracker.game_changed(kCeleste, at(0));
	tracker.game_changed(kHades, at(50));
	CHECK_FALSE(tracker.active());
	CHECK(tracker.session() == 0);

	tracker.start("", kFallback, kFallback, at(100));
	const FinishedRecording done = tracker.stop("", true, kNoGame, at(110));
	CHECK(done.game == kFallback); // only the session's own 10 s count
}
