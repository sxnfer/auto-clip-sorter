# WP2.0 hand-over: M2 interface headers (orchestrator)

## Summary

The orchestrator wrote the fixed contracts for M2 and implemented the small core pieces that several WPs share, so the group-A WPs never touch the same file.

| File | Kind | Implemented by |
|---|---|---|
| `src/core/clip-routing.hpp/.cpp`, `tests/test-clip-routing.cpp` | new, implemented | WP2.0 |
| `src/core/settings-model.*`, `tests/test-settings-model.cpp` | `play_save_sound`, `save_sound_path`, `save_sound_volume` added, implemented | WP2.0 |
| `src/core/recording-tracker.hpp` | new header | WP2.1 ★ |
| `src/obs/recording-events.hpp` | new header | WP2.2 ★ |
| `src/obs/clip-filer.hpp` | new header (`ClipFiler`, `ClipSorted` moved here with `save_id`, `read_remux_config`, `remux_timeout`) | WP2.3 |
| `src/obs/frontend-events.hpp` | screenshots added; `ClipKind` moved to core, `ClipSorted` to clip-filer | WP2.3 |
| `src/ui/settings-dialog.hpp` | doc comment specifies the M2 General page | WP2.4 |
| `src/core/wav-volume.hpp` | new header | WP2.5 |
| `src/platform/sound-player.hpp` | new header | WP2.5 |
| `CMakeLists.txt` | links `winmm` on WIN32 | WP2.0 |

The existing `frontend-events.cpp` still compiles unchanged against the new header, and nothing references an undefined symbol, so this commit is safe to push (MSVC links).

## Verified against OBS 31.0.0 source (tag `31.0.0`)

Beyond the facts in `M2-kickoff.md` (re-checked):
- `BasicOutputHandler::GetRecordingFilename()` (`UI/window-basic-main-outputs.cpp:2379-2387`) sets `lastRecordingPath` when a recording **starts** (Simple: `ConfigureRecording(false)`, :1301; Advanced: `StartRecording()`, :2221). `OBS_FRONTEND_EVENT_RECORDING_STARTING` is emitted before that (`window-basic-main.cpp`, `StartRecording`), `RECORDING_STARTED` after the output starts (`RecordingStart`, :7078-7093). So `obs_frontend_get_last_recording()` names the first file from `RECORDING_STARTED` on.
- `file_changed` is emitted only from `prepare_split_file()` (`plugins/obs-ffmpeg/obs-ffmpeg-mux.c:735-752`) and `change_file()` (`plugins/obs-outputs/mp4-output.c:365-404`), both on a split, never for the first file. Calldata: string `next_file`, the new file.
- `OBSRecordFileChanged` (:104-114) queues `RecordingFileChanged(old)` → `AutoRemux(old, true)`, so each finished part is remuxed while recording continues.
- Advanced + FFmpeg output: `ffmpegRecording = ffmpegOutput && config_get_bool(cfg, "AdvOut", "FFOutputToFile")` (:1513). Only then is a file path set (`"url"` setting). Otherwise `lastRecordingPath` keeps the previous recording's path, so `RecordingEvents` must not track it.
- The multitrack stream dump (`window-basic-main-outputs.cpp:2520`) also calls `GetRecordingFilename()`, which overwrites `lastRecordingPath` when an Enhanced Broadcasting stream with dump starts. The tracker therefore keeps its own file list and uses OBS's last path at `STOPPED` only when the session has no file (or there was no session).
- `media_remux_job_create` (`libobs/media-io/media-remux.c:145`) refuses input == output, which confirms `expected_remux_path()` returning empty for an `.mp4` (hybrid MP4).
- APIs and signatures quoted in the headers: `obs_frontend_get_recording_output`, `obs_frontend_get_last_recording`, `obs_frontend_get_last_screenshot`, `obs_frontend_get_profile_config` (`UI/obs-frontend-api/obs-frontend-api.h:191-235`); `obs_output_get_signal_handler`, `obs_output_release` (`libobs/obs.h:1803-1884`); `signal_handler_connect/disconnect` (`libobs/callback/signal.h:57-61`); `calldata_string` (`calldata.h:156`); `OBS_FRONTEND_EVENT_RECORDING_STARTED/STOPPED`, `OBS_FRONTEND_EVENT_SCREENSHOT_TAKEN`.

## Checks

- Each new header compiles on its own (`g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -fsyntax-only`).
- Unit tests: 175 of 175 pass (5 new: clip-routing, save-sound clamping).
- `build-aux/check-format` clean; the Ubuntu CI-preset build is OK.

## Decisions

In the plan.md decision log: the shared `ClipFiler` (one per event class), `core/clip-routing` and `save_id`, the recording file list and the FFmpeg-to-URL exclusion, dominant-game rules, remux timeouts, the save sound for replays only, and the volume curve.
