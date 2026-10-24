// Clip Atlas — sorts finished recordings into game folders.
//
// The steps are in recording-events.hpp. This file runs them on OBS's threads with the lifetimes of frontend-events.cpp:
// every handle given to OBS outlives RecordingEvents, and a callback that finds its object gone or stopped does nothing.
//   - The frontend callback's private data is a weak_ptr to Shared, in recording_handles(). It is registered once.
//   - The output's "file_changed" data is a SignalContext, in signal_contexts(): one per connected recording.
// The output thread copies a new part's path and session id, and hands them to the UI thread with run_on_ui(). Only the
// UI thread locks a weak_ptr, so the last reference to Shared is never dropped on another thread.
#include "recording-events.hpp"

#include "clip-filer.hpp"
#include "clip-layout.hpp"
#include "clip-routing.hpp"
#include "recording-tracker.hpp"
#include "remux-rules.hpp"
#include "ui-thread.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/bmem.h>
#include <util/config-file.h>

#include <cstddef>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace clip_atlas {

namespace fs = std::filesystem;

namespace {

// obs_frontend_get_last_recording() returns a bstrdup'd copy. This frees it on every path.
using BmemString = std::unique_ptr<char, void (*)(void *)>;

// A reference from obs_frontend_get_recording_output(), released with obs_output_release(). The deleter runs only for a
// non-NULL output.
using OutputRef = std::unique_ptr<obs_output_t, void (*)(obs_output_t *)>;

struct Shared;

// What the output's "file_changed" callback needs, given to the signal as its data. It is made on the UI thread when a
// recording's output connects. It is never freed: the signal refers to it until the disconnect, and the deque keeps its
// address stable.
struct SignalContext {
	std::weak_ptr<Shared> self;
	RecordingSessionId session = 0;
};

// What the UI thread may reach. RecordingEvents::Impl owns it through a shared_ptr. Callbacks hold only weak_ptrs.
struct Shared {
	Shared(const GameState &game_state, SettingsStore &store, FileMover &mover)
		: state(game_state),
		  settings(store),
		  filer(store, mover)
	{
	}

	const GameState &state;
	SettingsStore &settings;
	ClipFiler filer;                  // files every part of every recording; owns the waits mover and the listener
	RecordingTracker tracker;         // one session per recording
	obs_output_t *output = nullptr;   // the output with "file_changed" connected: a strong reference, or nullptr
	SignalContext *context = nullptr; // what that connection was given; nullptr exactly when output is nullptr
	// Set on the UI thread by stop(). The callbacks do nothing after that.
	bool stopped = false;
};

// Runs `fn` with the live Shared, on the UI thread, if RecordingEvents still exists and has not stopped. The lock is
// taken and released here, so the last reference is never dropped on another thread.
template<typename Fn> void if_running(const std::weak_ptr<Shared> &self, Fn fn)
{
	const std::shared_ptr<Shared> shared = self.lock();
	if (shared && !shared->stopped)
		fn(*shared);
}

// Every handle given to OBS. OBS keeps a callback and its private data until the process exits, because the plugin never
// removes one. A deque keeps the addresses stable as it grows. It gets one entry per RecordingEvents, so one per load.
std::deque<std::weak_ptr<Shared>> &recording_handles()
{
	static std::deque<std::weak_ptr<Shared>> handles;
	return handles;
}

// One entry per connected recording, never freed: each is small, and a recording's signal refers to its entry.
std::deque<SignalContext> &signal_contexts()
{
	static std::deque<SignalContext> contexts;
	return contexts;
}

// obs_frontend_get_last_recording() returns "" when OBS has no recording path. UI thread.
std::string take_last_recording()
{
	const BmemString path(obs_frontend_get_last_recording(), bfree);
	return path ? std::string(path.get()) : std::string();
}

// Lower-cases one ASCII letter. OBS compares these config values ignoring ASCII case, so this does too.
char lower_ascii(char c)
{
	return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// ASCII case-insensitive equality, as OBS's astrcmpi. Both strings must be non-NULL.
bool equals_ignore_case(const char *a, const char *b)
{
	for (;; ++a, ++b) {
		if (lower_ascii(*a) != lower_ascii(*b))
			return false;
		if (*a == '\0')
			return true;
	}
}

// False only for an Advanced "Custom Output (FFmpeg)" recording sent to a URL. OBS writes no file then and leaves its last
// recording path alone, so there is nothing to sort. OBS picks Advanced with a case-insensitive match on [Output] Mode
// (UI/window-basic-main.cpp, OBS 31.0.0). That is not the exact "Simple" test AutoRemux uses (read_remux_config), so the
// mode is read here. A missing Mode is Simple. UI thread.
bool recording_writes_file()
{
	config_t *profile = obs_frontend_get_profile_config(); // owned by OBS; not freed here
	if (!profile)
		return true;
	const char *mode = config_get_string(profile, "Output", "Mode");
	const char *rec_type = config_get_string(profile, "AdvOut", "RecType");
	const bool advanced = mode && equals_ignore_case(mode, "Advanced");
	const bool custom_ffmpeg = rec_type && equals_ignore_case(rec_type, "FFmpeg");
	return !(advanced && custom_ffmpeg && !config_get_bool(profile, "AdvOut", "FFOutputToFile"));
}

// OBS has started the next part of a split recording. Runs on the output's own thread, so it copies what it needs and hops
// to the UI thread. It must not lock the weak_ptr: the last reference could then be dropped here, which destroys the
// filer's mover on this thread.
void on_file_changed(void *data, calldata_t *cd)
{
	try {
		const SignalContext *context = static_cast<const SignalContext *>(data);
		const char *next = calldata_string(cd, "next_file"); // the NEW part; NULL if OBS gave none
		if (!next || *next == '\0')
			return;
		run_on_ui([self = context->self, session = context->session, file = std::string(next)]() {
			if_running(self, [session, &file](Shared &running) {
				const bool added = running.tracker.file_changed(session, file);
				// The part of a recording that stopped before this task ran. OBS wrote it, and it stays there.
				if (!added && session != running.tracker.session()) {
					obs_log(LOG_WARNING,
						"recording part %s arrived after the recording stopped; it was left where OBS wrote it",
						file.c_str());
				}
			});
		});
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "recording part not tracked: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "recording part not tracked: unknown exception");
	}
}

// Disconnects "file_changed" and releases the output, if one is held. Once disconnect returns, no callback of this
// connection is still running: signal_handler_signal holds the signal's lock while it calls them. UI thread.
void disconnect_output(Shared &shared)
{
	if (shared.output) {
		signal_handler_disconnect(obs_output_get_signal_handler(shared.output), "file_changed", on_file_changed,
					  shared.context);
		obs_output_release(shared.output);
	}
	shared.output = nullptr;
	shared.context = nullptr;
}

// Connects "file_changed" for a recording whose output splits its file. Only "ffmpeg_muxer", "mp4_output" and, from OBS 32,
// "mov_output" (Hybrid MOV) declare that signal, and libobs logs a warning for a connection to any other output. The
// others never split, so their reference is dropped now. UI thread.
void connect_output(const std::weak_ptr<Shared> &self, Shared &shared, RecordingSessionId session)
{
	// NULL when OBS has no recording output. Every path that does not keep the reference releases it.
	OutputRef output(obs_frontend_get_recording_output(), obs_output_release);
	if (!output)
		return;
	const char *id = obs_output_get_id(output.get());
	signal_handler_t *handler = obs_output_get_signal_handler(output.get());
	const bool splits = id && handler &&
			    (std::strcmp(id, "ffmpeg_muxer") == 0 || std::strcmp(id, "mp4_output") == 0 ||
			     std::strcmp(id, "mov_output") == 0);
	if (!splits)
		return; // the reference is released by OutputRef
	signal_contexts().push_back(SignalContext{self, session});
	SignalContext *context = &signal_contexts().back();
	signal_handler_connect(handler, "file_changed", on_file_changed, context);
	shared.output = output.release();
	shared.context = context;
}

// RECORDING_STARTED. OBS names the file it is writing before the output starts, so that is the first file.
void recording_started(const std::weak_ptr<Shared> &self, Shared &shared)
{
	// Connected here only if the previous recording's STOPPED never arrived. Its connection must not outlive it.
	disconnect_output(shared);
	// While OBS switches scene collections it drops every frontend event but EXIT and scene-collection cleanup, so
	// a STOPPED can be lost. That session is ended here without filing: its parts stay where OBS wrote them.
	if (shared.tracker.active()) {
		obs_log(LOG_WARNING,
			"the previous recording's stop was never reported; its files were left where OBS wrote them");
		shared.tracker.stop(std::string(), false, ClipGame{}, SteadyClock::now());
	}
	if (!recording_writes_file()) {
		obs_log(LOG_INFO,
			"recording is not written to a file (custom FFmpeg output to a URL); it will not be sorted");
		return;
	}
	const std::string first = take_last_recording();
	// A copy: get() returns a reference that a later set() replaces.
	const Settings settings = shared.settings.get();
	const SteadyClock::time_point now = SteadyClock::now();
	const ClipGame start_game = clip_game_for(shared.state.game_for_save(now), settings);
	const ClipGame current = clip_game_for(shared.state.current(), settings);
	const RecordingSessionId session = shared.tracker.start(first, start_game, current, now);
	obs_log(LOG_INFO, "recording started: %s (game: %s)", first.empty() ? "no file name yet" : first.c_str(),
		start_game.unsorted() ? "none" : start_game.name.c_str());
	connect_output(self, shared, session);
}

// RECORDING_STOPPED. OBS's last path is used only by a session that has no file of its own (recording-tracker.hpp). With
// no session nothing is read for filing: OBS's path is not per recording.
void recording_stopped(Shared &shared)
{
	disconnect_output(shared);
	if (!shared.tracker.active()) {
		obs_log(LOG_INFO, "recording stopped, but it was not tracked; nothing was sorted");
		return;
	}
	const std::string last = take_last_recording();
	// A copy: get() returns a reference that a later set() replaces.
	const Settings settings = shared.settings.get();
	const SteadyClock::time_point now = SteadyClock::now();
	const FinishedRecording finished = shared.tracker.stop(
		last, settings.recording_dominant_game, clip_game_for(shared.state.game_for_save(now), settings), now);
	if (!sorting_enabled(settings, ClipKind::recording)) {
		for (const std::string &file : finished.files)
			obs_log(LOG_INFO, "sorting off, recording left at %s", file.c_str());
		return;
	}
	if (finished.files.empty()) {
		obs_log(LOG_WARNING, "recording stopped, but OBS gave no file path; nothing was sorted");
		return;
	}
	// Read now, as OBS does: it remuxes the last part right after STOPPED, with the settings it has then.
	const RemuxConfig remux = read_remux_config();
	std::vector<ClipFile> files;
	files.reserve(finished.files.size());
	for (std::size_t i = 0; i < finished.files.size(); ++i) {
		const fs::path source = path_from_utf8(finished.files[i]);
		// OBS remuxes only its last recording path at STOPPED, which a multitrack dump can overwrite.
		const bool copy_expected = i + 1 < finished.files.size() || last == finished.files[i];
		files.push_back(ClipFile{source, copy_expected ? expected_remux_path(source, remux) : fs::path()});
	}
	shared.filer.file(ClipKind::recording, files, finished.game);
	obs_log(LOG_INFO, "recording stopped: %zu file(s) for %s", files.size(),
		finished.game.unsorted() ? "Unsorted" : finished.game.name.c_str());
}

// Called by OBS on the UI thread for every frontend event. The private data is one of recording_handles(), so it is
// always valid to read. Once RecordingEvents is gone, or stopped, the weak_ptr does nothing.
void on_frontend_event(obs_frontend_event event, void *private_data)
{
	try {
		if (event != OBS_FRONTEND_EVENT_RECORDING_STARTED && event != OBS_FRONTEND_EVENT_RECORDING_STOPPED)
			return;
		const std::weak_ptr<Shared> &self = *static_cast<const std::weak_ptr<Shared> *>(private_data);
		if (event == OBS_FRONTEND_EVENT_RECORDING_STARTED)
			if_running(self, [&self](Shared &shared) { recording_started(self, shared); });
		else
			if_running(self, recording_stopped);
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "recording sorting failed: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "recording sorting failed: unknown exception");
	}
}

} // namespace

struct RecordingEvents::Impl {
	Impl(const GameState &state, SettingsStore &settings, FileMover &mover)
		: shared(std::make_shared<Shared>(state, settings, mover)),
		  handle(&recording_handles().emplace_back(shared))
	{
	}

	std::shared_ptr<Shared> shared;
	// The private data OBS hands back with each event. It lives in recording_handles(), so it stays valid.
	std::weak_ptr<Shared> *handle;
	bool registered = false; // UI thread only
};

RecordingEvents::RecordingEvents(const GameState &state, SettingsStore &settings, FileMover &mover)
	: impl_(std::make_unique<Impl>(state, settings, mover))
{
}

RecordingEvents::~RecordingEvents()
{
	stop();
}

void RecordingEvents::start()
{
	// stop() is final, because the filer's waits mover cannot be restarted. A start after it does nothing.
	if (impl_->registered || impl_->shared->stopped)
		return;
	// Registered without touching an output: OBS creates its outputs after modules load.
	obs_frontend_add_event_callback(on_frontend_event, impl_->handle);
	impl_->registered = true;
}

void RecordingEvents::stop()
{
	Shared &shared = *impl_->shared;
	if (shared.stopped)
		return;
	// Set first, so anything that runs during the shutdown below already sees the stop.
	shared.stopped = true;
	try {
		disconnect_output(shared);
		shared.filer.stop();
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "stopping recording sorting failed: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "stopping recording sorting failed: unknown exception");
	}
}

void RecordingEvents::on_game_changed()
{
	try {
		Shared &shared = *impl_->shared;
		if (shared.stopped || !shared.tracker.active())
			return;
		// A copy: get() returns a reference that a later set() replaces.
		const Settings settings = shared.settings.get();
		shared.tracker.game_changed(clip_game_for(shared.state.current(), settings), SteadyClock::now());
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "recording game change failed: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "recording game change failed: unknown exception");
	}
}

void RecordingEvents::set_clip_sorted_callback(ClipSortedCallback callback)
{
	impl_->shared->filer.set_clip_sorted_callback(std::move(callback));
}

void RecordingEvents::set_activity_callback(ActivityCallback callback)
{
	impl_->shared->filer.set_activity_callback(std::move(callback));
}

} // namespace clip_atlas
