// Clip Atlas — starts and stops OBS's Replay Buffer as games come and go.
//
// Asks replay-policy what to do after every game-state change, settings
// change and Replay Buffer event, and turns the answer into
// obs_frontend_replay_buffer_start() / obs_frontend_replay_buffer_stop().
// Both calls are asynchronous in OBS 31 and start can silently do nothing
// (no sources, invalid output path, low disk), so "we started it" is only
// recorded when OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED arrives while our
// start request is pending. A STARTED without our request means the user
// started it.
//
// Suppression: if REPLAY_BUFFER_STOPPED arrives without our stop request
// while a game is current, the user stopped it on purpose; don't restart
// until the current game changes (GameState::current() source or info).
//
// "Replay Buffer disabled in Output settings" is detected with
// obs_frontend_get_replay_buffer_output() returning NULL (release a non-NULL
// result with obs_output_release()). Re-checked on
// OBS_FRONTEND_EVENT_PROFILE_CHANGED and OBS_FRONTEND_EVENT_FINISHED_LOADING
// and before each start.
//
// Re-checks for a pending stop use a QTimer::singleShot on the UI thread at
// ReplayDecision::recheck_at.
//
// UI thread only.
#pragma once

#include "game-state.hpp"
#include "settings-model.hpp"

#include <functional>
#include <memory>

namespace clip_atlas {

enum class ReplayBufferStatus {
	disabled_in_obs, // Output settings have the Replay Buffer turned off
	inactive,
	active_by_us,
	active_by_user,
};

class ReplayController {
public:
	using StatusCallback = std::function<void(ReplayBufferStatus)>;

	explicit ReplayController(const GameState &state);
	~ReplayController(); // calls stop()

	ReplayController(const ReplayController &) = delete;
	ReplayController &operator=(const ReplayController &) = delete;

	// Registers the frontend event callback (obs_frontend_add_event_callback).
	void start(const Settings &settings);
	// Removes it and cancels any pending re-check. Never stops the Replay
	// Buffer itself. Idempotent.
	void stop();

	void apply_settings(const Settings &settings);
	// Call after GameState::current() changed.
	void on_game_changed();

	ReplayBufferStatus status() const;
	// Called on the UI thread whenever status() changes (for the M3 dock).
	void set_status_callback(StatusCallback callback);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace clip_atlas
