// Clip Atlas — sorts saved replays and screenshots into game folders.
//
// One frontend callback (UI thread), registered by start() and never removed:
//
// OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED:
//   1. path = obs_frontend_get_last_replay() (char *, free with bfree()). OBS sets it before the event, then calls
//      AutoRemux(path).
//   2. Skip (log at INFO) unless sorting_enabled(settings, ClipKind::replay).
//   3. game = clip_game_for(GameState::game_for_save(now), settings).
//   4. filer.file(ClipKind::replay, {{path, expected_remux_path(path, read_remux_config())}}, game).
//
// OBS_FRONTEND_EVENT_SCREENSHOT_TAKEN:
//   1. path = obs_frontend_get_last_screenshot() (char *, free with bfree()). OBS joins its writer thread and sets
//      the path before the event (UI/window-basic-main-screenshot.cpp), so the file is complete.
//   2. Skip (log at INFO) unless sorting_enabled(settings, ClipKind::screenshot).
//   3. game = clip_game_for(GameState::game_for_save(now), settings).
//   4. filer.file(ClipKind::screenshot, {{path, {}}}, game). OBS never remuxes a screenshot.
//
// A missing or empty path is logged as a warning and nothing is filed. The filing itself (the waits, the remux wait,
// the moves, the log lines and the ClipSorted callback) is ClipFiler's, the same for every kind. Replays and
// screenshots share one ClipFiler, so their waits run in order.
//
// The callback's private data is a handle that outlives FrontendEvents: OBS keeps every callback until the process
// exits, and the plugin never removes one (OBS dispatches by index, so a removal during EXIT can make another
// plugin's callback run twice or be skipped). Once FrontendEvents is stopped or gone, the callback does nothing.
//
// UI thread only.
#include "frontend-events.hpp"

#include "clip-filer.hpp"
#include "clip-layout.hpp"
#include "clip-routing.hpp"
#include "remux-rules.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/bmem.h>

#include <deque>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace clip_atlas {

namespace fs = std::filesystem;

namespace {

// obs_frontend_get_last_replay() and obs_frontend_get_last_screenshot() return a bstrdup'd copy. This frees it on
// every path.
using BmemString = std::unique_ptr<char, void (*)(void *)>;

// What the frontend callback may reach. FrontendEvents::Impl owns it through a shared_ptr, and the callback holds only
// a weak_ptr to it.
struct Shared {
	Shared(const GameState &game_state, SettingsStore &store, FileMover &mover)
		: state(game_state),
		  settings(store),
		  filer(store, mover)
	{
	}

	const GameState &state;
	SettingsStore &settings;
	ClipFiler filer; // files every save; owns the waits mover and the listener
	// Set on the UI thread by stop(). The callback does nothing after that.
	bool stopped = false;
};

// Runs `fn` with the live Shared, on the UI thread, if FrontendEvents still exists and has not stopped. The lock is
// taken and released here, so the last reference is never dropped on another thread.
template<typename Fn> void if_running(const std::weak_ptr<Shared> &self, Fn fn)
{
	const std::shared_ptr<Shared> shared = self.lock();
	if (shared && !shared->stopped)
		fn(*shared);
}

// Takes OBS's copy of a path. Returns "" and logs `missing` when OBS gave none. UI thread.
std::string take_obs_path(char *obs_path, const char *missing)
{
	const BmemString path(obs_path, bfree);
	if (!path || *path == '\0') {
		obs_log(LOG_WARNING, "%s", missing);
		return {};
	}
	return path.get();
}

// Files the replay OBS has just saved. UI thread.
void sort_saved_replay(Shared &shared)
{
	const std::string path = take_obs_path(obs_frontend_get_last_replay(),
					       "replay saved, but OBS gave no file path; nothing was sorted");
	if (path.empty())
		return;
	// A copy: get() returns a reference that a later set() replaces.
	const Settings settings = shared.settings.get();
	if (!sorting_enabled(settings, ClipKind::replay)) {
		obs_log(LOG_INFO, "sorting off, replay left at %s", path.c_str());
		return;
	}
	const fs::path source = path_from_utf8(path);
	const ClipGame game = clip_game_for(shared.state.game_for_save(SteadyClock::now()), settings);
	const std::vector<ClipFile> files{ClipFile{source, expected_remux_path(source, read_remux_config())}};
	shared.filer.file(ClipKind::replay, files, game);
}

// Files the screenshot OBS has just taken. UI thread.
void sort_taken_screenshot(Shared &shared)
{
	const std::string path = take_obs_path(obs_frontend_get_last_screenshot(),
					       "screenshot taken, but OBS gave no file path; nothing was sorted");
	if (path.empty())
		return;
	const Settings settings = shared.settings.get();
	if (!sorting_enabled(settings, ClipKind::screenshot)) {
		obs_log(LOG_INFO, "sorting off, screenshot left at %s", path.c_str());
		return;
	}
	const fs::path source = path_from_utf8(path);
	const ClipGame game = clip_game_for(shared.state.game_for_save(SteadyClock::now()), settings);
	// OBS never remuxes a screenshot, so there is no copy to wait for.
	const std::vector<ClipFile> files{ClipFile{source, {}}};
	shared.filer.file(ClipKind::screenshot, files, game);
}

// Every handle given to OBS. OBS keeps a callback and its private data until the process exits, because the plugin
// never removes one (see the file comment). So a handle must outlive FrontendEvents: once FrontendEvents is gone it is
// an expired weak_ptr, and the callback that finds it does nothing. A deque keeps the addresses stable as it grows. It
// gets one entry per FrontendEvents, which is one per plugin load.
std::deque<std::weak_ptr<Shared>> &frontend_handles()
{
	static std::deque<std::weak_ptr<Shared>> handles;
	return handles;
}

// Called by OBS on the UI thread for every frontend event. The private data is one of frontend_handles(), so it is
// always valid to read. Once FrontendEvents is gone, or stopped, the weak_ptr does nothing.
void on_frontend_event(obs_frontend_event event, void *private_data)
{
	try {
		if (event != OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED && event != OBS_FRONTEND_EVENT_SCREENSHOT_TAKEN)
			return;
		const std::weak_ptr<Shared> &self = *static_cast<const std::weak_ptr<Shared> *>(private_data);
		if (event == OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED)
			if_running(self, sort_saved_replay);
		else
			if_running(self, sort_taken_screenshot);
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "clip sorting failed: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "clip sorting failed: unknown exception");
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
	// stop() is final, because the filer's waits mover cannot be restarted. A start after it does nothing.
	if (impl_->registered || impl_->shared->stopped)
		return;
	obs_frontend_add_event_callback(on_frontend_event, impl_->handle);
	impl_->registered = true;
}

void FrontendEvents::stop()
{
	if (impl_->shared->stopped)
		return;
	// The callback stays registered (see the file comment); the flag makes it do nothing from here on. Set before
	// the filer stops, so no save can reach the filer after that.
	impl_->shared->stopped = true;
	impl_->shared->filer.stop();
}

void FrontendEvents::set_clip_sorted_callback(ClipSortedCallback callback)
{
	impl_->shared->filer.set_clip_sorted_callback(std::move(callback));
}

} // namespace clip_atlas
