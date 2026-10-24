// Clip Atlas — files one save's clips into a game folder and reports each file once. The steps are in
// clip-filer.hpp; this file runs them on the plugin's threads.
//
// Deferred work (the wait and move callbacks, and the run_on_ui() tasks) captures a std::weak_ptr to Shared and plain
// values, never `this`. The weak_ptr is locked and released only on the UI thread. A callback that held the last
// reference would destroy Shared, and the waits mover inside it, on a mover's worker thread, which would then try to
// join its own thread.
#include "clip-filer.hpp"

#include "clip-layout.hpp"
#include "ui-thread.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/config-file.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace clip_atlas {

namespace fs = std::filesystem;

namespace {

// A file that OBS will not remux is complete when the event fires. The short wait only guards against an unfinished
// write. A timeout still moves the file, because OBS has finished writing by then.
constexpr std::chrono::milliseconds kPlainStableFor{1000};
constexpr std::chrono::milliseconds kPlainStableTimeout{30000};

// With a remux, OBS reads the original while it writes the copy, so the original waits for the copy to settle.
constexpr std::chrono::milliseconds kRemuxStableFor{3000};

// Shared by every ClipFiler, so a save id is unique across the session. Starts at 1, because 0 means "no save".
std::atomic<std::uint64_t> g_next_save_id{1};

// What every file of one save has in common.
struct Save {
	ClipKind kind = ClipKind::replay;
	std::uint64_t id = 0;
	fs::path dest;
	ClipGame game;
};

// The word the log uses for a kind of clip: "replay sorted to ...".
const char *noun_of(ClipKind kind)
{
	switch (kind) {
	case ClipKind::replay:
		return "replay";
	case ClipKind::recording:
		return "recording";
	case ClipKind::screenshot:
		return "screenshot";
	}
	return "clip";
}

// UTF-8 text of a path, for log lines. u8string() is std::string in C++17 and std::u8string in C++20, so the bytes are
// copied across either way.
std::string utf8_of(const fs::path &path)
{
	const auto text = path.u8string();
	return std::string(text.begin(), text.end());
}

char lower_ascii(char c)
{
	return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// ASCII case-insensitive comparison, which is how OBS matches these config values.
bool equals_ignore_case(const char *a, const char *b)
{
	for (;; ++a, ++b) {
		if (lower_ascii(*a) != lower_ascii(*b))
			return false;
		if (*a == '\0')
			return true;
	}
}

} // namespace

std::chrono::milliseconds remux_timeout(ClipKind kind)
{
	// A long recording on a slow disk can take more than ten minutes to remux. Replays and screenshots are short.
	if (kind == ClipKind::recording)
		return std::chrono::minutes(60);
	return std::chrono::minutes(10);
}

// The profile settings that decide whether OBS remuxes a saved clip (OBSBasic::AutoRemux, OBS 31.0). Like OBS, only a
// missing Mode or exactly "Simple" counts as simple mode; anything else reads the advanced keys.
RemuxConfig read_remux_config()
{
	RemuxConfig remux;
	config_t *profile = obs_frontend_get_profile_config(); // owned by OBS; not freed here
	if (!profile)
		return remux;
	remux.auto_remux = config_get_bool(profile, "Video", "AutoRemux");
	const char *mode = config_get_string(profile, "Output", "Mode");
	remux.simple_mode = !mode || std::strcmp(mode, "Simple") == 0;
	const char *rec_type = config_get_string(profile, "AdvOut", "RecType");
	remux.advanced_ffmpeg = rec_type && equals_ignore_case(rec_type, "FFmpeg");
	const char *format = config_get_string(profile, remux.simple_mode ? "SimpleOutput" : "AdvOut", "RecFormat2");
	remux.recording_format = format ? format : "";
	return remux;
}

// What deferred work may reach. ClipFiler owns it through a shared_ptr, and deferred work holds only weak_ptrs to it.
// The helpers are members rather than free functions because ClipFiler::Shared is private to ClipFiler.
struct ClipFiler::Shared {
	Shared(SettingsStore &store, FileMover &main_mover) : settings(store), mover(main_mover) {}

	SettingsStore &settings;
	FileMover &mover; // moves the clips; owned by the plugin, not here
	// Holds only wait_stable jobs. A remux wait can last minutes, and this keeps it from delaying moves.
	FileMover waits;
	ClipSortedCallback on_clip_sorted;
	ActivityCallback on_activity;
	// Set on the UI thread by stop(). Nothing is logged, reported or queued after that.
	bool stopped = false;

	// Observers cannot interrupt filing, including when a listener replaces itself.
	void notify(const ActivityEvent &event)
	{
		try {
			const ActivityCallback listener = on_activity;
			if (listener)
				listener(event);
		} catch (...) {
			obs_log(LOG_WARNING, "activity listener failed");
		}
	}

	void phase(const Save &save, std::size_t index, ActivityPhase value, bool stable = false,
		   ActivityNotice notice = ActivityNotice::none, const std::string &detail = {})
	{
		ActivityEvent event;
		event.save_id = save.id;
		event.file_index = index;
		event.phase = value;
		event.stability_confirmed = stable;
		event.notice = notice;
		event.detail = detail;
		notify(event);
	}

	// Runs `fn` with the live Shared, on the UI thread, if the filer still exists and has not stopped. The lock is taken
	// and released here, so the last reference is never dropped on a worker thread.
	template<typename Fn> static void if_running(const std::weak_ptr<Shared> &self, Fn fn)
	{
		const std::shared_ptr<Shared> shared = self.lock();
		if (shared && !shared->stopped)
			fn(*shared);
	}

	// A file OBS will not remux: the wait only guards against an unfinished write.
	void queue_plain(const std::weak_ptr<Shared> &self, const Save &save, const fs::path &file, std::size_t index)
	{
		waits.wait_stable(file, kPlainStableFor, kPlainStableTimeout,
				  [self, save, file, index](const StableResult &result) {
					  // Cancelled means the filer is stopping, so nothing moves. Stable and timed out both move the file,
					  // because OBS has finished writing it by then.
					  if (result.status == StableStatus::cancelled)
						  return;
					  const bool stable = result.status == StableStatus::stable;
					  run_on_ui([self, save, file, index, stable]() {
						  if_running(self, [&](Shared &running) {
							  running.queue_move(self, save, file, index, stable);
						  });
					  });
				  });
	}

	// OBS is remuxing, so the original must not move until the copy has settled.
	void queue_remux(const std::weak_ptr<Shared> &self, const Save &save, const fs::path &file,
			 const fs::path &remuxed, std::size_t index)
	{
		waits.wait_stable(
			remuxed, kRemuxStableFor, remux_timeout(save.kind),
			[self, save, file, remuxed, index](const StableResult &result) {
				if (result.status == StableStatus::cancelled)
					return;
				const bool settled = result.status == StableStatus::stable;
				run_on_ui([self, save, file, remuxed, settled, index]() {
					if_running(self, [&](Shared &running) {
						if (!settled) {
							obs_log(LOG_WARNING,
								"remuxed file %s did not settle or never appeared; "
								"moving only the original",
								utf8_of(remuxed).c_str());
						}
						if (!settled)
							running.phase(save, index + 1, ActivityPhase::unavailable,
								      false, ActivityNotice::remux_unavailable);
						running.queue_move(self, save, file, index, settled);
						if (settled)
							running.queue_move(self, save, remuxed, index + 1, true);
					});
				});
			});
	}

	// Queues `file` on the plugin's mover, into the save's folder. The result comes back to the UI thread.
	void queue_move(const std::weak_ptr<Shared> &self, const Save &save, const fs::path &file, std::size_t index,
			bool stable)
	{
		phase(save, index, ActivityPhase::queued, stable,
		      stable ? ActivityNotice::none : ActivityNotice::stability_timeout);
		mover.move(
			file, save.dest,
			[self, save, index](const MoveResult &result) {
				// Runs on the mover's worker thread, or on this thread if the mover has already shut down. Only plain
				// values are copied out of the result.
				ClipSorted clip;
				clip.kind = save.kind;
				clip.status = result.status;
				clip.original_path = result.source;
				if (result.status == MoveStatus::moved ||
				    result.status == MoveStatus::copied_source_kept)
					clip.final_path = result.final_path;
				clip.game_key = save.game.key;
				clip.game_name = save.game.name;
				clip.error = result.error;
				clip.save_id = save.id;
				// Where the clip is now, for the log. After a failed move that kept a partial copy, that is the partial.
				const fs::path where = result.final_path.empty() ? result.source : result.final_path;
				run_on_ui([self, save, index, clip = std::move(clip), where]() {
					if_running(self, [&](Shared &running) {
						const ActivityPhase value =
							clip.status == MoveStatus::cancelled ? ActivityPhase::cancelled
							: clip.status == MoveStatus::failed  ? ActivityPhase::failed
											     : ActivityPhase::completed;
						running.phase(save, index, value, false,
							      clip.status == MoveStatus::copied_source_kept
								      ? ActivityNotice::original_kept
								      : ActivityNotice::none,
							      clip.error);
						running.report(clip, where);
					});
				});
			},
			[self, save, index] {
				run_on_ui([self, save, index] {
					if_running(self, [&](Shared &running) {
						running.phase(save, index, ActivityPhase::moving);
					});
				});
			});
	}

	// Logs one file's outcome and passes it to the listener. UI thread.
	void report(const ClipSorted &clip, const fs::path &where)
	{
		const char *noun = noun_of(clip.kind);
		const std::string where_text = utf8_of(where);
		switch (clip.status) {
		case MoveStatus::moved:
			obs_log(LOG_INFO, "%s sorted to %s", noun, where_text.c_str());
			break;
		case MoveStatus::copied_source_kept:
			obs_log(LOG_INFO, "%s copied to %s; the original was kept at %s (%s)", noun, where_text.c_str(),
				utf8_of(clip.original_path).c_str(), clip.error.c_str());
			break;
		case MoveStatus::failed:
		case MoveStatus::cancelled:
			obs_log(LOG_WARNING, "%s not sorted (%s); the clip was left at %s", noun, clip.error.c_str(),
				where_text.c_str());
			break;
		}
		if (!on_clip_sorted)
			return;
		// A copy, so a listener that replaces the callback does not destroy the one that is running.
		const ClipSortedCallback listener = on_clip_sorted;
		try {
			listener(clip);
		} catch (const std::exception &e) {
			obs_log(LOG_ERROR, "clip sorted listener failed: %s", e.what());
		} catch (...) {
			obs_log(LOG_ERROR, "clip sorted listener failed: unknown exception");
		}
	}
};

ClipFiler::ClipFiler(SettingsStore &settings, FileMover &mover) : shared_(std::make_shared<Shared>(settings, mover)) {}

ClipFiler::~ClipFiler()
{
	stop();
}

bool ClipFiler::file(ClipKind kind, const std::vector<ClipFile> &files, const ClipGame &game)
{
	try {
		Shared &shared = *shared_;
		// After stop() a save is dropped silently: nothing is reported during shutdown.
		if (shared.stopped || files.empty())
			return false;
		const std::string base = shared.settings.effective_clips_folder();
		if (base.empty()) {
			for (const ClipFile &clip : files) {
				obs_log(LOG_WARNING, "no clips folder is known, %s not sorted; it was left at %s",
					noun_of(kind), utf8_of(clip.file).c_str());
			}
			return false;
		}
		// A copy: get() returns a reference that a later set() replaces.
		const Settings settings = shared.settings.get();
		Save save;
		save.kind = kind;
		save.game = game;
		save.dest = destination_dir(path_from_utf8(base), game.name,
					    year_month_of(std::chrono::system_clock::now()),
					    LayoutOptions{settings.monthly_folders, settings.unsorted_folder});
		save.id = g_next_save_id.fetch_add(1);

		ActivityEvent accepted;
		accepted.save_id = save.id;
		accepted.accepted = true;
		accepted.kind = kind;
		accepted.game = game.name;
		for (const ClipFile &clip : files) {
			ActivityFile original;
			original.path = utf8_of(clip.file);
			accepted.files.push_back(original);
			if (!clip.remuxed.empty()) {
				ActivityFile remuxed;
				remuxed.path = utf8_of(clip.remuxed);
				remuxed.remuxed = true;
				accepted.files.push_back(remuxed);
			}
		}
		shared.notify(accepted);
		const std::weak_ptr<Shared> self = shared_;
		std::size_t index = 0;
		for (const ClipFile &clip : files) {
			if (clip.remuxed.empty())
				shared.queue_plain(self, save, clip.file, index);
			else
				shared.queue_remux(self, save, clip.file, clip.remuxed, index);
			index += clip.remuxed.empty() ? 1 : 2;
		}
		return true;
	} catch (const std::exception &e) {
		// Files queued before the exception stay queued and are still moved and reported.
		obs_log(LOG_ERROR, "%s sorting failed: %s", noun_of(kind), e.what());
		return false;
	} catch (...) {
		obs_log(LOG_ERROR, "%s sorting failed: unknown exception", noun_of(kind));
		return false;
	}
}

void ClipFiler::stop()
{
	if (shared_->stopped)
		return;
	// Set first, so a wait that the shutdown cancels (and that calls back on this thread) sees it.
	shared_->stopped = true;
	shared_->waits.shutdown(false);
}

void ClipFiler::set_clip_sorted_callback(ClipSortedCallback callback)
{
	shared_->on_clip_sorted = std::move(callback);
}

void ClipFiler::set_activity_callback(ActivityCallback callback)
{
	shared_->on_activity = std::move(callback);
}

} // namespace clip_atlas
