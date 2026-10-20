// Clip Atlas — sorts finished recordings into game folders.
//
// OBS 31.0 facts this relies on (UI/window-basic-main*.cpp, UI/api-interface.cpp):
//   - StartRecording() names the first file and stores it in lastRecordingPath
//     before the output starts, so obs_frontend_get_last_recording() (char *,
//     free with bfree()) returns the file being written from
//     OBS_FRONTEND_EVENT_RECORDING_STARTED on. At RECORDING_STARTING it is
//     still the previous recording's path.
//   - A split recording: the recording output emits
//     "void file_changed(string next_file)" on its own thread (not the UI
//     thread) each time it starts a new file; "next_file" is the NEW file.
//     It is not emitted for the first file. OBS remuxes each finished part
//     right away (AutoRemux(old_path, true)) and the last part after STOPPED.
//   - obs_frontend_get_recording_output() returns a new reference
//     (obs_output_get_ref), released with obs_output_release(). Outputs are
//     recreated when the user changes output settings or profile, which OBS
//     only allows while no output is active, so the output must be fetched at
//     each RECORDING_STARTED.
//   - RecordingStop() emits RECORDING_STOPPED, then calls AutoRemux(last
//     path). A failed recording also emits STOPPED.
//   - Advanced output mode with the "Custom Output (FFmpeg)" type
//     ([AdvOut] RecType "FFmpeg") writes a file only when
//     config_get_bool(cfg, "AdvOut", "FFOutputToFile") is true; otherwise it
//     sends to a URL and lastRecordingPath is not updated.
//
// Session (all on the UI thread unless noted):
//   RECORDING_STARTED:
//     - If the profile is Advanced + FFmpeg and FFOutputToFile is false, log
//       at INFO that the recording is not a file and track nothing.
//     - first = obs_frontend_get_last_recording().
//     - id = tracker.start(first, clip_game_for(state.game_for_save(now)),
//       clip_game_for(state.current()), now).
//     - output = obs_frontend_get_recording_output(); if non-NULL, connect
//       "file_changed" on obs_output_get_signal_handler(output) and keep the
//       output reference until it is disconnected (at STOPPED, at stop(), or
//       before a new STARTED).
//   file_changed (output thread): copy calldata_string(cd, "next_file") and
//     the session id, then run_on_ui() → tracker.file_changed(id, next).
//   on_game_changed(): tracker.game_changed(clip_game_for(state.current()), now).
//   RECORDING_STOPPED:
//     - disconnect "file_changed" and release the output.
//     - last = obs_frontend_get_last_recording() (the tracker uses it only if
//       the session has no file, or when STOPPED arrives without STARTED).
//     - finished = tracker.stop(last, settings.recording_dominant_game,
//       clip_game_for(state.game_for_save(now)), now).
//     - Skip (log at INFO) unless sorting_enabled(settings, ClipKind::recording).
//     - remux = read_remux_config(); filer.file(ClipKind::recording, one
//       ClipFile per finished.files entry with remuxed =
//       expected_remux_path(file, remux), finished.game).
//   Settings are read when each event arrives.
//
// The frontend callback is never removed (OBS dispatches by index; a removal
// during EXIT makes another callback run twice or be skipped); after stop() it
// does nothing. The private data of the frontend callback and of the signal
// is a handle that outlives this object.
//
// UI thread only (all public functions).
#pragma once

#include "clip-filer.hpp"
#include "file-mover.hpp"
#include "game-state.hpp"
#include "settings-store.hpp"

#include <memory>

namespace clip_atlas {

class RecordingEvents {
public:
	// Called on the UI thread once per file: every part of a split recording
	// and every remuxed copy, all with the same save_id.
	using ClipSortedCallback = clip_atlas::ClipSortedCallback;

	RecordingEvents(const GameState &state, SettingsStore &settings, FileMover &mover);
	~RecordingEvents(); // calls stop()

	RecordingEvents(const RecordingEvents &) = delete;
	RecordingEvents &operator=(const RecordingEvents &) = delete;

	// obs_frontend_add_event_callback, once. Touches no output: OBS creates
	// its outputs after modules load. A start() after stop() does nothing.
	void start();

	// Idempotent. Disconnects "file_changed" and releases the output if a
	// recording is being tracked, makes the frontend callback inert, and stops
	// the filer. A recording in progress is not filed.
	void stop();

	// The current game may have changed (SourceTracker's callback). Feeds the
	// dominant-game clock of a recording in progress. Does nothing after
	// stop() or when no recording is tracked.
	void on_game_changed();

	void set_clip_sorted_callback(ClipSortedCallback callback);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace clip_atlas
