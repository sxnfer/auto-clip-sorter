// Clip Atlas — sorts saved replays into game folders.
//
// On REPLAY_BUFFER_SAVED (UI thread) the replay is filed by a move on the plugin's FileMover. When OBS
// will remux it, the move waits for the remuxed copy to settle first, because OBS reads the original
// while it writes the copy. Results come back to the UI thread through run_on_ui().
//
// Deferred work (the wait and move callbacks, and the run_on_ui() tasks) captures a std::weak_ptr to
// Shared and plain values, never `this` or a reference to a plugin object, for two reasons:
//   - the work can run after FrontendEvents is gone, and then the weak_ptr has expired;
//   - a capture that held the last reference would destroy Shared, and the waits mover inside it, on a
//     mover's worker thread, which would then join its own thread. So a weak_ptr is locked only on the
//     UI thread, and that lock is released there too.
//
// The plugin never removes a frontend callback. OBS dispatches them by index, so a removal while an event
// (EXIT in particular) is being dispatched can make another plugin's callback run twice or be skipped. Stopping
// therefore sets a flag that makes the callback do nothing, and the handle OBS holds stays valid for the rest of
// the process (see frontend_handles()).
#include "frontend-events.hpp"

#include "clip-layout.hpp"
#include "name-resolver.hpp"
#include "remux-rules.hpp"
#include "ui-thread.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/bmem.h>
#include <util/config-file.h>

#include <chrono>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clip_atlas {

namespace fs = std::filesystem;

namespace {

// A replay that OBS does not remux is complete when the event fires. The short wait only guards against an
// unfinished write. A timeout still moves it, because OBS has finished writing by then.
constexpr std::chrono::milliseconds kPlainStableFor{1000};
constexpr std::chrono::milliseconds kPlainStableTimeout{30000};

// With remux, OBS reads the original while it writes the .mp4, so the original waits for the copy. Ten
// minutes covers a long remux. If the copy never settles, only the original is moved.
constexpr std::chrono::milliseconds kRemuxStableFor{3000};
constexpr std::chrono::milliseconds kRemuxStableTimeout = std::chrono::minutes(10);

// obs_frontend_get_last_replay() returns a bstrdup'd copy. This frees it on every path.
using BmemString = std::unique_ptr<char, void (*)(void *)>;

// What deferred work may reach. FrontendEvents::Impl owns it through a shared_ptr, and deferred work holds
// only weak_ptrs to it (see the file comment).
struct Shared {
	Shared(const GameState &game_state, SettingsStore &store, FileMover &main_mover)
		: state(game_state),
		  settings(store),
		  mover(main_mover)
	{
	}

	const GameState &state;
	SettingsStore &settings;
	FileMover &mover; // moves the clips; owned by the plugin, not here
	// Holds only wait_stable jobs. A remux wait can last minutes, and this keeps it from delaying moves.
	FileMover waits;
	FrontendEvents::ClipSortedCallback on_clip_sorted;
	// Set on the UI thread by stop(). Nothing is logged, reported or queued after that.
	bool stopped = false;
};

// Runs `fn` with the live Shared, on the UI thread, if FrontendEvents still exists and has not stopped. The
// lock is taken and released here, so the last reference is never dropped on a worker thread.
template<typename Fn> void if_running(const std::weak_ptr<Shared> &self, Fn fn)
{
	const std::shared_ptr<Shared> shared = self.lock();
	if (shared && !shared->stopped)
		fn(*shared);
}

// UTF-8 text of a path, for log lines. u8string() is std::string in C++17 and std::u8string in C++20, so the
// bytes are copied across either way.
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

// Settings::game_rules as the resolver's map. validated() has already lowercased the executables and removed
// duplicates; emplace keeps the first rule for an executable in any case.
UserGameRules user_rules(const std::vector<GameRuleSetting> &rules)
{
	UserGameRules out;
	for (const GameRuleSetting &rule : rules)
		out.emplace(rule.executable, UserGameRule{rule.display_name, rule.ignore});
	return out;
}

// The folder a replay saved now goes in. Both fields empty means Unsorted.
struct ClipGame {
	std::string key;  // ResolvedGame::folder_key
	std::string name; // ResolvedGame::display_name
};

ClipGame game_for_replay(const GameState &state, const Settings &settings)
{
	const std::optional<CurrentGame> current = state.game_for_save(SteadyClock::now());
	if (!current)
		return {};
	const ResolvedGame game = resolve_game({current->info.executable, current->info.title, ""},
					       user_rules(settings.game_rules), BundledGameNames{});
	// An ignored game counts as no game: its clips go to the unsorted folder.
	if (game.ignored || game.display_name.empty())
		return {};
	return {game.folder_key, game.display_name};
}

// The profile settings that decide whether OBS remuxes a replay (OBSBasic::AutoRemux, OBS 31.0). Like OBS,
// only a missing Mode or exactly "Simple" counts as simple mode; anything else reads the advanced keys.
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

// Logs one file's outcome and passes it to the listener. UI thread.
void report_result(Shared &shared, const ClipSorted &clip, const fs::path &where)
{
	const std::string where_text = utf8_of(where);
	switch (clip.status) {
	case MoveStatus::moved:
		obs_log(LOG_INFO, "replay sorted to %s", where_text.c_str());
		break;
	case MoveStatus::copied_source_kept:
		obs_log(LOG_INFO, "replay copied to %s; the original was kept at %s (%s)", where_text.c_str(),
			utf8_of(clip.original_path).c_str(), clip.error.c_str());
		break;
	case MoveStatus::failed:
	case MoveStatus::cancelled:
		obs_log(LOG_WARNING, "replay not sorted (%s); the clip was left at %s", clip.error.c_str(),
			where_text.c_str());
		break;
	}
	if (!shared.on_clip_sorted)
		return;
	// A copy, so a listener that replaces the callback does not destroy the one that is running.
	const FrontendEvents::ClipSortedCallback listener = shared.on_clip_sorted;
	try {
		listener(clip);
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "clip sorted listener failed: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "clip sorted listener failed: unknown exception");
	}
}

// Queues one file on the plugin's mover. The result comes back to the UI thread through run_on_ui().
void queue_move(Shared &shared, const std::weak_ptr<Shared> &self, const fs::path &file, const fs::path &dest,
		const ClipGame &game)
{
	shared.mover.move(file, dest, [self, game](const MoveResult &result) {
		// Runs on the mover's worker thread, or on this thread if the mover has already shut down. Only
		// plain values are copied out of the result.
		ClipSorted clip;
		clip.kind = ClipKind::replay;
		clip.status = result.status;
		clip.original_path = result.source;
		if (result.status == MoveStatus::moved || result.status == MoveStatus::copied_source_kept)
			clip.final_path = result.final_path;
		clip.game_key = game.key;
		clip.game_name = game.name;
		clip.error = result.error;
		// Where the clip is now, for the log. After a failed move that kept a partial copy, that is the partial.
		const fs::path where = result.final_path.empty() ? result.source : result.final_path;
		run_on_ui([self, clip = std::move(clip), where]() {
			if_running(self, [&clip, &where](Shared &running) { report_result(running, clip, where); });
		});
	});
}

// Handles one saved replay. UI thread. `self` is the weak pointer that `shared` was locked from.
void sort_saved_replay(Shared &shared, const std::weak_ptr<Shared> &self)
{
	BmemString last(obs_frontend_get_last_replay(), bfree);
	if (!last || *last == '\0') {
		obs_log(LOG_WARNING, "replay saved, but OBS gave no file path; nothing was sorted");
		return;
	}
	const std::string path_utf8 = last.get();
	const fs::path source = path_from_utf8(path_utf8);

	// A copy: get() returns a reference that a later set() replaces.
	const Settings settings = shared.settings.get();
	if (!settings.auto_sort || !settings.sort_replays) {
		obs_log(LOG_INFO, "sorting off, replay left at %s", path_utf8.c_str());
		return;
	}

	const std::string base = shared.settings.effective_clips_folder();
	if (base.empty()) {
		obs_log(LOG_WARNING, "no clips folder is known, replay not sorted; it was left at %s",
			path_utf8.c_str());
		return;
	}

	const ClipGame game = game_for_replay(shared.state, settings);
	const fs::path dest = destination_dir(path_from_utf8(base), game.name,
					      year_month_of(std::chrono::system_clock::now()),
					      LayoutOptions{settings.monthly_folders, settings.unsorted_folder});

	const fs::path remuxed = expected_remux_path(source, read_remux_config());
	if (remuxed.empty()) {
		// No remux is coming, so the wait only guards against an unfinished write.
		shared.waits.wait_stable(source, kPlainStableFor, kPlainStableTimeout,
					 [self, source, dest, game](const StableResult &result) {
						 // Cancelled means the plugin is stopping, so nothing moves. Stable and timed
						 // out both move the file, since OBS has finished writing it.
						 if (result.status == StableStatus::cancelled)
							 return;
						 run_on_ui([self, source, dest, game]() {
							 if_running(self, [&](Shared &running) {
								 queue_move(running, self, source, dest, game);
							 });
						 });
					 });
		return;
	}

	// OBS is remuxing, so the original must not move until the copy has settled.
	shared.waits.wait_stable(
		remuxed, kRemuxStableFor, kRemuxStableTimeout,
		[self, source, remuxed, dest, game](const StableResult &result) {
			if (result.status == StableStatus::cancelled)
				return;
			const bool settled = result.status == StableStatus::stable;
			run_on_ui([self, source, remuxed, dest, game, settled]() {
				if_running(self, [&](Shared &running) {
					if (!settled) {
						obs_log(LOG_WARNING,
							"remuxed file %s did not settle or never appeared; "
							"moving only the original",
							utf8_of(remuxed).c_str());
					}
					queue_move(running, self, source, dest, game);
					if (settled)
						queue_move(running, self, remuxed, dest, game);
				});
			});
		});
}

// Every handle given to OBS. OBS keeps a callback and its private data until the process exits, because the plugin
// never removes one (see the file comment). So a handle must outlive FrontendEvents: once FrontendEvents is gone it
// is an expired weak_ptr, and the callback that finds it does nothing. A deque keeps the addresses stable as it
// grows. It gets one entry per FrontendEvents, which is one per plugin load.
std::deque<std::weak_ptr<Shared>> &frontend_handles()
{
	static std::deque<std::weak_ptr<Shared>> handles;
	return handles;
}

// Called by OBS on the UI thread for every frontend event. The private data is one of frontend_handles(), so it
// is always valid to read. Once FrontendEvents is gone, or stopped, the weak_ptr does nothing.
void on_frontend_event(obs_frontend_event event, void *private_data)
{
	try {
		if (event != OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED)
			return;
		const std::weak_ptr<Shared> &self = *static_cast<const std::weak_ptr<Shared> *>(private_data);
		if_running(self, [&self](Shared &shared) { sort_saved_replay(shared, self); });
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "replay sorting failed: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "replay sorting failed: unknown exception");
	}
}

} // namespace

struct FrontendEvents::Impl {
	Impl(const GameState &state, SettingsStore &settings, FileMover &mover)
		: shared(std::make_shared<Shared>(state, settings, mover)),
		  handle(&frontend_handles().emplace_back(shared))
	{
	}

	std::shared_ptr<Shared> shared;
	// The private data OBS hands back with each event. It lives in frontend_handles(), so it stays valid.
	std::weak_ptr<Shared> *handle;
	bool registered = false; // UI thread only
};

FrontendEvents::FrontendEvents(const GameState &state, SettingsStore &settings, FileMover &mover)
	: impl_(std::make_unique<Impl>(state, settings, mover))
{
}

FrontendEvents::~FrontendEvents()
{
	stop();
}

void FrontendEvents::start()
{
	// stop() is final, because the waits mover cannot be restarted. A start after it does nothing.
	if (impl_->registered || impl_->shared->stopped)
		return;
	obs_frontend_add_event_callback(on_frontend_event, impl_->handle);
	impl_->registered = true;
}

void FrontendEvents::stop()
{
	if (impl_->shared->stopped)
		return;
	// The callback stays registered. The plugin never removes a frontend callback, because OBS dispatches them by
	// index and a removal during EXIT can make another plugin's callback run twice or be skipped. The flag below
	// makes the callback do nothing instead.
	// Set first, so a wait that the shutdown below cancels (and that calls back on this thread) sees it.
	impl_->shared->stopped = true;
	impl_->shared->waits.shutdown(false);
}

void FrontendEvents::set_clip_sorted_callback(ClipSortedCallback callback)
{
	impl_->shared->on_clip_sorted = std::move(callback);
}

} // namespace clip_atlas
