// Clip Atlas — one recording session: which game it belongs to and which files it wrote.
#include "recording-tracker.hpp"

#include <algorithm>
#include <utility>

namespace clip_atlas {

namespace {

// Games are told apart by key, or by name when the key is empty. The prefixes keep a key and a name that read the
// same ("foo" as an executable, "foo" as a folder name) from merging into one game.
std::string game_identity(const ClipGame &game)
{
	return game.key.empty() ? "n:" + game.name : "k:" + game.key;
}

} // namespace

RecordingSessionId RecordingTracker::start(const std::string &first_file, const ClipGame &start_game,
					   const ClipGame &current_game, SteadyClock::time_point now)
{
	// Forget the previous session in full. next_id_ is not rewound, so its id is never handed out again.
	files_.clear();
	game_times_.clear();
	segment_game_ = ClipGame{};
	segment_start_ = SteadyClock::time_point{};

	start_game_ = start_game;
	if (!first_file.empty())
		files_.push_back(first_file);

	session_ = next_id_++;
	// 0 means "no session". The counter only gets there after 2^64 starts, but it is cheap to rule out.
	if (next_id_ == 0)
		next_id_ = 1;

	// The cleared segment has no game, so closing it adds nothing. This opens the first segment at `now`.
	game_changed(current_game, now);
	return session_;
}

bool RecordingTracker::file_changed(RecordingSessionId session_id, const std::string &next_file)
{
	if (!active() || session_id != session_ || next_file.empty())
		return false;
	// A part is listed once, even if OBS reports it again.
	if (std::find(files_.begin(), files_.end(), next_file) != files_.end())
		return false;
	files_.push_back(next_file);
	return true;
}

void RecordingTracker::game_changed(const ClipGame &current_game, SteadyClock::time_point now)
{
	if (!active())
		return;
	close_segment(now);
	segment_game_ = current_game;
	if (current_game.unsorted())
		return;
	// try_emplace keeps the first_seen of a game that was already current earlier in this recording.
	auto [it, inserted] = game_times_.try_emplace(game_identity(current_game));
	if (inserted)
		it->second.first_seen = now;
	it->second.game = current_game; // the latest key and name, which the dominant game is reported with
}

void RecordingTracker::close_segment(SteadyClock::time_point now)
{
	// Time with no game counts for nothing, and a clock that went backwards counts as zero. The next segment starts
	// at `now` either way, so closing again at the same instant adds nothing.
	if (!segment_game_.unsorted() && now > segment_start_) {
		auto it = game_times_.find(game_identity(segment_game_));
		if (it != game_times_.end())
			it->second.total += now - segment_start_;
	}
	segment_start_ = now;
}

FinishedRecording RecordingTracker::stop(const std::string &last_file, bool use_dominant, const ClipGame &fallback_game,
					 SteadyClock::time_point now)
{
	FinishedRecording out;
	if (!active()) {
		// STOPPED with no session here (the plugin loaded mid-recording, say). OBS's path is all there is.
		out.game = fallback_game;
		if (!last_file.empty())
			out.files.push_back(last_file);
		return out;
	}

	close_segment(now);
	// OBS's last path is only a fallback: another output can overwrite it, so the session's own list wins.
	if (files_.empty() && !last_file.empty())
		files_.push_back(last_file);

	out.game = start_game_;
	if (use_dominant) {
		// The largest total wins, and a tie goes to the game that was current first. Zero time never qualifies,
		// so a recording with no game time keeps the start game.
		const GameTime *best = nullptr;
		for (const auto &entry : game_times_) {
			const GameTime &record = entry.second;
			if (record.total <= SteadyClock::duration::zero())
				continue;
			if (!best || record.total > best->total ||
			    (record.total == best->total && record.first_seen < best->first_seen))
				best = &record;
		}
		if (best)
			out.game = best->game;
	}
	out.files = std::move(files_);

	// End the session. next_id_ is left alone so this id is never reused.
	files_.clear();
	game_times_.clear();
	start_game_ = ClipGame{};
	segment_game_ = ClipGame{};
	segment_start_ = SteadyClock::time_point{};
	session_ = 0;
	return out;
}

bool RecordingTracker::active() const
{
	return session_ != 0;
}

RecordingSessionId RecordingTracker::session() const
{
	return session_;
}

} // namespace clip_atlas
