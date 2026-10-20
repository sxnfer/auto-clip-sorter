# WP0.4 hand-over: M1 interface headers (orchestrator)

## Summary

The orchestrator wrote these headers. They are the fixed contracts for M1's work packages:

| Header | Implemented by |
|---|---|
| `src/core/name-resolver.hpp` | WP1.1 |
| `src/core/game-state.hpp` | WP1.2 ★ |
| `src/core/replay-policy.hpp`, `src/core/remux-rules.hpp` | WP1.3 |
| `src/core/settings-model.hpp` | WP1.4 |
| `src/core/file-mover.hpp` | WP1.5 ★ |
| `src/obs/ui-thread.hpp`, `src/obs/settings-store.hpp` | WP1.6 |
| `src/obs/source-tracker.hpp` | WP1.7 ★ |
| `src/obs/frontend-events.hpp` | WP1.8 |
| `src/obs/replay-controller.hpp` | WP1.9 |
| `src/platform/process-info.hpp` | WP4.2 (M4) |

The headers are declaration-only. No `.cpp` exists yet, so nothing links against them.

## Verified against OBS 31.0.0 source (tag `31.0.0`)

- `plugins/win-capture/game-capture.c` and `window-capture.c` both declare these. **This answers WP1.7's open question: `window_capture` emits the hook signals in 31.0.**
  - `void hooked(ptr source, string title, string class, string executable)`
  - `void unhooked(ptr source)`
  - the proc `void get_hooked(out bool hooked, out string title, out string class, out string executable)`
- Both fire from `video_tick`, on the graphics thread.
- `executable` is the image file name, for example `game.exe`, or the literal `"unknown"` when the process can't be opened (`libobs/util/windows/window-helpers.c`, `ms_get_window_exe`).
- Global signals (`libobs/obs.c`): `source_create(ptr source)`, `source_destroy(ptr source)` and `source_rename(ptr source, string new_name, string prev_name)`. `source_destroy` fires from the destruction task thread (`obs_source_destroy_defer`). `source_create` fires after the source's `create()`, so its own signals are declared by then.
- `signal_handler_signal` holds the signal's mutex while it calls callbacks, and `signal_handler_disconnect` takes the same mutex. Once disconnect returns, no callback for that signal is still running.
- Frontend (`UI/obs-frontend-api/obs-frontend-api.h`, `UI/api-interface.cpp`, `UI/window-basic-main.cpp`):
  - `obs_frontend_get_last_replay()` returns a `bstrdup` string, so free it with `bfree`. OBS sets it before firing `REPLAY_BUFFER_SAVED`, then calls `AutoRemux(path)`.
  - `obs_frontend_replay_buffer_start()` and `obs_frontend_replay_buffer_stop()` queue a `QMetaObject::invokeMethod`. Start can silently do nothing: no sources, an invalid output path, or low disk.
  - `obs_frontend_get_replay_buffer_output()` returns NULL when the Replay Buffer is disabled in Output settings, because no output is created. A non-NULL result needs `obs_output_release`.
  - `obs_frontend_get_current_record_output_path()` returns `bstrdup(GetCurrentOutputPath())`.
- The `AutoRemux()` rules in `remux-rules.hpp` come from `OBSBasic::AutoRemux` (`UI/window-basic-main.cpp`). It reads these keys:
  - `[Video] AutoRemux`
  - `[Output] Mode`
  - `[AdvOut] RecType`
  - `[SimpleOutput|AdvOut] RecFormat2`
  - Output naming: `.mp4` normally, `<stem>.remuxed.<ext>` for the fragmented formats, and avi skipped.
- `obs_queue_task(enum obs_task_type, obs_task_t, void *, bool)`, `obs_in_task_thread`, `obs_data_save_json_safe`, `os_mkdirs` and the weak-source API all exist with the signatures quoted in the headers.

## Checks

- Each header compiles on its own: `g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -fsyntax-only`.
- `build-aux/check-format` is clean.
- The Ubuntu CI-preset build is OK, with the headers globbed into the target.

## Decisions

These are in the plan.md decision log: UUID as source id, ignore semantics, Replay Buffer ownership and suppression, remux-aware replays, settings I/O on the UI thread, and `copied_source_kept`.
