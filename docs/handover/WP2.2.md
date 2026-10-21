# WP2.2 hand-over: obs/recording-events + wiring

## Summary

- `src/obs/recording-events.cpp` (new, 362 lines) implements `RecordingEvents` as `recording-events.hpp` specifies. It follows each recording from `OBS_FRONTEND_EVENT_RECORDING_STARTED` to `RECORDING_STOPPED` with `RecordingTracker`: the first file, every split part from the recording output's `file_changed` signal (connected only for `ffmpeg_muxer` and `mp4_output`), and the game (the start game, or the dominant game when that option is on). At STOPPED each part goes to one `ClipFiler` with its expected auto-remux copy, which waits for the copy and moves the files, all with one `save_id`.
- `src/plugin-main.cpp` creates, starts and stops `RecordingEvents` with the other components, and forwards game changes and settings changes to its dominant-game clock. The replay code is unchanged.
- Fix round 1 (the coordinator's review) is applied: (1) a session still active at STARTED, whose STOPPED was lost, is ended without filing, with a WARNING; (2) the last part of a split gets an expected copy only when OBS's last recording path is that part; (3) the output type is read the way OBS 31.0.0 creates the output (case-insensitive `Advanced`); (4) a settings change reaches the recording clock before the tracker and the replay controller.
- No header changed. Nothing was committed or pushed.
- On the fixed sources every local check passes: the CI-preset build has zero warnings, the unit tests pass 234 of 234, `clang-format-19` is clean, the harness passes 204 of 204 under plain, ASan+UBSan+LSan and TSan (no sanitizer or ThreadSanitizer reports), and the lifecycle harness passes 37 of 37 under ASan+UBSan. MSVC was not run.
- The OBS facts in this note were checked by the orchestrator against OBS 31.0.0 source. This worker did not check them against any source, and nothing was run against OBS 32.x. The user runs OBS 32.x (plan.md decision log: M1 was accepted on OBS 32.2.2), so the facts must be re-checked against 32.x.

## Base

STEP 0: `git merge --ff-only claude/m2-recordings-screenshots` fast-forwarded `77f7bcd..5527f16`. `git log -1 --oneline` printed `5527f16 WP2.1: core/recording-tracker`. Fix round 1 was made on the same worktree; the base did not change.

## Files changed

| File | Change |
|---|---|
| `src/obs/recording-events.cpp` | New, 362 lines. `RecordingEvents`: the frontend handler, the signal callback, the connection and its lifetimes. |
| `src/plugin-main.cpp` | 22 insertions, 1 deletion: the include, a `recordings` member after `events`, creation and start in `start_plugin()`, a separately guarded block in `on_game_changed()`, the recordings call first in `apply_settings_change()`, and `stop()` in `stop_all()`. |
| `docs/handover/WP2.2.md` | This note. |

`git status --short` shows only these three files (the build directories are git-ignored).

## Design

- **Lifetimes** follow `frontend-events.cpp`. `Impl` owns a `shared_ptr<Shared>`. The frontend callback's private data is a `weak_ptr<Shared>` in `recording_handles()`, a function-local deque. It is registered once and never removed, and it does nothing once `stop()` has run or the object is gone.
- **Signal.** The connection is made at STARTED, and only for `ffmpeg_muxer` and `mp4_output`. Its data is a `SignalContext` (a weak_ptr and the session id) in `signal_contexts()`. The output thread copies the new part's path and session id and posts `run_on_ui()`. Only the UI-thread task locks the weak_ptr and calls `tracker.file_changed(session, path)`. A part refused because its session has ended gets the WARNING.
- **Output reference.** `obs_frontend_get_recording_output()` gives a reference that `Shared::output` holds only while the output is connected. It is released at STOPPED, at `stop()`, before a new connection, and at once for an output that does not split.
- **STARTED.** Disconnect any previous connection. If a session is still active, its STOPPED was lost: end it without filing, with a WARNING (fix 1). If the recording goes to a URL (below), log INFO and track nothing. Otherwise start the tracker with OBS's first file and the start game, and connect the output.
- **Output type.** A recording goes to a URL only for Advanced + Custom FFmpeg + no file, each compared case-insensitively. A missing `[Output] Mode` is Simple. The values are read with `config_get_string` in this file, not with `read_remux_config()`, because OBS creates the output with a case-insensitive "Advanced" test (UI/window-basic-main.cpp:1997 in OBS 31.0.0), while AutoRemux uses an exact "Simple" test.
- **STOPPED.** Disconnect and release first. With no session: INFO, and nothing else is read. With one: stop the tracker with OBS's last path (used only when the session has no file of its own), with the dominant option from the settings. Expected copies: each earlier part keeps its copy, because OBS remuxed it when it split. The last part gets a copy only when OBS's last path is that part, because OBS remuxes only that path at STOPPED (window-basic-main.cpp:7155 in OBS 31.0.0; a multitrack dump can overwrite the path). Then one `filer.file()` call.
- **Game clock.** `on_game_changed()` feeds the tracker only while a session is active and nothing has stopped. The plugin calls it from SourceTracker's callback, and after a settings change (before the tracker and replay calls). It does not depend on the Replay Buffer controller having started.

## How it was tested

Commands run from the worktree root, `/home/user/auto-clip-sorter/.claude/worktrees/agent-aaeb2671787be8559`. Logs and harness outputs are in `/tmp/claude-0/-home-user-auto-clip-sorter/6c995450-a095-5948-9d9e-9af008631cf2/scratchpad/wp22/`.

1. **CI-preset build, warnings are errors.** `cmake --preset ubuntu-ci-x86_64 && cmake --build --preset ubuntu-ci-x86_64`: exit 0 and zero warnings on the fixed sources (log `wp22/fix-build-ci.log`).
2. **Unresolved symbols.** `nm -C build_x86_64/clip-atlas.so | grep " U " | grep clip_atlas` prints nothing.
3. **Format.** `clang-format-19 --dry-run --Werror src/obs/recording-events.cpp src/plugin-main.cpp`: exit 0.
4. **Unit tests.** `cmake -S tests -B build/tests && cmake --build build/tests && ctest --test-dir build/tests --output-on-failure`: 234 of 234 passed.
5. **Strict warnings on the real glue.** The harness builds compile `recording-events.cpp`, `clip-filer.cpp`, `ui-thread.cpp`, `settings-store.cpp` and the core files they need with `g++ 13.3 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wformat=2`: zero warnings. The lifecycle build compiles `plugin-main.cpp` with the same flags: no warning points into `src/`. MSVC was not run.
6. **Scratch harness, 204 checks (not committed).** `harness/harness.cpp`, with `harness/fixround.inc`, links the real `recording-events.cpp`, `clip-filer.cpp`, `ui-thread.cpp` and `settings-store.cpp`, with the core files. The OBS frontend API, the output accessors and `obs_log` are stubbed. A fake output stands in for `obs_output_t`, and its `file_changed` is a real libobs signal emitted from a separate thread. Results on the fixed sources:
   - plain: 204 passed, 0 failed, 0 build warnings;
   - ASan+UBSan+LSan: 204 passed, 0 failed, no sanitizer report;
   - TSan: 204 passed, 0 failed, 0 ThreadSanitizer reports.

   New in this round: T16 also checks the stale-session WARNING. T22a (the reviewer's R1, file second): A's STOPPED is lost, B is filed, and A's parts stay where OBS wrote them. T22b (R1, URL second): nothing is filed, every part stays, and the URL output is never referenced. T23 (R3, a multitrack dump overwrites OBS's last path): the recording is filed without waiting for a copy. T24 (R4, `Mode=simple` in lower case): tracked and filed. T25 (mode rules): `ADVANCED` with `ffmpeg` and no file is a URL, and a profile with no Mode key is a file. T26 (last-part copy rule): part 1's copy is awaited, and part 2 is filed without waiting for a copy.
7. **Lifecycle harness, 37 checks, ASan+UBSan (not committed).** `harness/lifecycle.cpp` drives the real `src/plugin-main.cpp` with the real recording glue, filer, frontend glue, settings store, name resolver and sound player. The source tracker and the replay controller are stubs that record their calls and expose the game state. One process per scenario: `gate` (14 checks: a game change before FINISHED_LOADING reaches the recording clock but not the replay controller; the recording is filed under Game B, which was current longer; the replay controller starts once with the current game; EXIT stops it), `exit` (7: EXIT releases the output, and nothing is filed after it), `unload` (6: unload without EXIT releases the output and leaves the callbacks inert), `settings` (5: a settings change that makes the game being recorded ignored reaches the recording clock, so Game A is filed), and `settings-throw` (5: the tracker's `apply_settings` fails first, and the recording still sees the change).
8. **Mutation checks, re-run on the fixed sources.** The fix round changed code that some mutants target, so every mutant was re-run.
   - The 15 original mutants of `recording-events.cpp` (each applied to a copy, each must fail a named check): all 15 killed on the fixed sources, each by 1 to 12 failing checks (the patterns of m08 and m11 were updated for the new code; the others are unchanged).
   - Three fix mutants, which put back the behaviour a fix removed: f1 (no end of a stale session): 6 checks fail; f2 (every part expects a copy): 4 checks fail; f3 (the exact "Simple" rule for the output type): 4 checks fail.
   - Lifecycle mutants: L1 (the recording call after the tracker): `settings-throw` fails; L2 (no recording call on a settings change): `settings` and `settings-throw` both fail; G (the game clock fed only after the replay controller starts): killed; `gate` fails 2 checks, and the recording is filed under Game A where Game B is expected.

Harness files (not committed), in `wp22/harness/`: `harness.cpp`, `fixround.inc`, `lifecycle.cpp`, `build.sh`, `build-lifecycle.sh`, `mutate.py`, `mutate_fix.py`, `mutate_lc.py`, `mutate_lc2.py` and `run-all.sh`. The reviewer's R1, R3 and R4 (`review-wp22/harness/review-tests.inc`) are reproduced in this harness as T22, T23 and T24.

## Deviations from the contract, and why

1. **The handlers take the handle.** `recording_started()` and `connect_output()` take `const std::weak_ptr<Shared> &self`, because `SignalContext` needs a weak_ptr and `if_running()` passes only `Shared &`. The contract did not say how the weak_ptr reaches STARTED. Behaviour is unchanged.
2. **The output reference is held by an RAII guard** (`unique_ptr` with `obs_output_release` as the deleter) from the get until it is kept in `Shared`. If `push_back` throws, the reference is still released. The contract did not say.
3. **`stop()` and `on_game_changed()` catch and log.** The contract's rule for OBS callbacks is applied to them too. `stop()` runs from the destructor, so it must not throw.
4. **STARTED log text.** An empty first path is logged as "no file name yet" instead of an empty `%s`. The rest of the format is as specified.
5. **Plugin lifecycle.** `on_game_changed()` keeps the replay block as it was and adds a separate try block for recordings. `apply_settings_change()` calls `recordings->on_game_changed()` before the tracker and replay calls (fix 4), so that an exception from them cannot skip it. `RecordingEvents::on_game_changed()` catches its own failures, so that call needs no try block.
6. **Member name.** `Shared::context` holds the connected `SignalContext *`. This is the contract's wording, not a header change.
7. **Output type (fix 3).** The contract's rule (not a file = `!simple_mode && advanced_ffmpeg && !FFOutputToFile`, from `read_remux_config()`) uses AutoRemux's exact "Simple" test. OBS creates the output with a case-insensitive "Advanced" test. The two differ only for a hand-edited Mode such as `simple`. Each follows OBS: the output type uses OBS's output test here, and the expected copy keeps `read_remux_config()`, which mirrors OBS's AutoRemux.
8. **Expected copy of the last part (fix 2).** The contract gives every part `expected_remux_path`. Here the last part gets one only when OBS's last path is that part, because OBS remuxes only that path at STOPPED.
9. **A session still active at STARTED (fix 1).** The contract does not cover it. It is ended without filing, with a WARNING.

## Known gaps and risks

1. **Not run in OBS 31 or 32, or on Windows.** The compile used the Ubuntu libobs 30.0.2 headers, and the harnesses stub OBS. Every API used is declared in those headers. The OBS facts were checked by the orchestrator against OBS 31.0.0 only, and the user runs OBS 32.x.
2. **Which outputs split (review question 1, answered).** OBS 31.0.0 creates only `ffmpeg_output`, `ffmpeg_muxer` and `mp4_output` for recording (window-basic-main-outputs.cpp:517, 696, 1521, 1550). Only the last two declare `file_changed`, so the list in `connect_output()` is complete for 31.0.0. libobs logs a warning when a listed output does not declare the signal, which is why the list is exact. Any split-capable recording output with another id, in a later OBS, would be silently unsorted after its first part. Re-check the list against 32.x.
3. **Copies that never appear.** The filer's waits run FIFO on one mover. A copy that never appears holds every later wait, including this recording's moves, for up to 60 minutes. Three cases produce one: (a) a remux that cannot open its input never creates the copy; (b) a remux that fails after creating the copy leaves a partial copy, which is moved along with the original; (c) a multitrack stream dump overwrites OBS's last path, which is now handled for the last part (deviation 8). Nothing is lost in any case: the files stay where OBS wrote them until the wait ends, or the partial copy is moved with its original.
4. **Quitting right after a recording.** EXIT calls `stop()`, which cancels pending remux waits. A recording stopped just before OBS quits stays unsorted in the OBS folder. This matches M1's replays.
5. **AutoRemux changed during a recording.** The expected copies follow the profile at STOPPED. An earlier part that OBS remuxed under the old setting may then get no copy to wait for, or wait for a copy that never comes (up to 60 minutes; see gap 3). Rare.
6. **A lost STOPPED (fixed in this round).** OBS drops every frontend event except EXIT and scene-collection cleanup while it switches scene collections (UI/api-interface.cpp:618-622; `ClearSceneData()` pumps the posted events, window-basic-main.cpp:4674-4732). A queued RECORDING_STOPPED can therefore be lost. Its session then stays active until the next STARTED, which ends it with a WARNING. Its files stay where OBS wrote them. If no recording starts again, nothing is filed. Until then the game clock keeps feeding that session, which has no effect on any file.
7. **A failed start.** If OBS sends STARTED for a start that then fails, the first file never appears. The filer waits 30 s and logs one WARNING. Not verified against OBS.
8. **Never freed.** `signal_contexts()` gets one small entry per connected recording, and `recording_handles()` one per load, as the contract asked.
9. **The 60-minute remux timeout is not reached in a test.** The harness runs the moves with the real 1 s and 3 s stability waits. The 60-minute value is the `remux_timeout()` constant, as in WP2.3.
10. **MSVC /W3 not run.** Reasoned: `%zu` for `size_t`, explicit `static_cast`s where needed, no unused parameters (callbacks name only what they use), no name hiding (`context`, `shared` and `session` never shadow an outer name), and UTF-8 paths through `path_from_utf8`.

## Questions for the orchestrator

1. Review question 1 is answered in gap 2. Please check the recording output ids of OBS 32.x, the version the user runs, before the release. The list is complete only for 31.0.0.
2. A recording stopped just before quitting is left unsorted (gap 4). Should `stop_all()` let a pending remux finish first? This is the same question as for replays in M1.
3. Should the remux waits get their own mover, so that a copy that never appears delays only later recordings? WP2.3 raised the same question for replays (gap 3).
4. Should AutoRemux be read when the recording starts, rather than at STOPPED (gap 5)?
5. Review question 5 (ending an old session that never got STOPPED) is handled by fix 1: a session still active at every STARTED is ended, not only for a URL recording. No question is left open.

## Orchestrator integration notes

- Read `recording-events.cpp` and the `plugin-main.cpp` diff in full; changes stay within WP2.2's ownership.
- ★ Reviewer verdict: accept with fixes. Fix round 1 applied four fixes, and the orchestrator re-read each:
  - the stale session is ended at STARTED;
  - the last part's copy is expected only when OBS's last path is that part;
  - OBS's case-insensitive "Advanced" output test is used;
  - recordings come first in `apply_settings_change()`.

  The OBS claims behind them were verified by the orchestrator against 31.0.0: frontend events are dropped while `disableSaving` is set (`UI/api-interface.cpp:618-622`), and the output type uses `astrcmpi(mode, "Advanced")` (`window-basic-main.cpp:1997`).
- **OBS 32.2.2 check (the version the user runs).** The orchestrator cloned tag 32.2.2 and re-checked the recording facts:
  - `lastRecordingPath` is still set in `GetRecordingFilename` and replaced by `next_file` in `OBSRecordFileChanged` (`frontend/utility/BasicOutputHandler.cpp:112, 449`);
  - `AutoRemux` follows the same rules (`frontend/widgets/OBSBasic_Recording.cpp:47-110, 244, 254`);
  - frontend events are dropped under `disableSaving` (`frontend/OBSStudioAPI.cpp:747`);
  - the screenshot path is set before `SCREENSHOT_TAKEN` (`frontend/utility/ScreenshotObj.cpp:359-360`).

  **One difference:** OBS 32 adds `mov_output` (Hybrid MOV; `plugins/obs-outputs/mp4-output.c:613`), which shares `mp4_output_create_internal` and so declares `file_changed` (:214). The orchestrator added `"mov_output"` to `connect_output()`. Its `.mov` is auto-remuxed to `<stem>.mp4`, which `expected_remux_path` already predicts.
- Known risk added: on OBS 32, Hybrid MOV with a ProRes encoder makes OBS remux `x.mov` to `x.mov`, which libobs refuses. `expected_remux_path` would then expect `x.mp4`, and the recording would wait up to 60 minutes before the original moves alone. ProRes encoding is not available on Windows in OBS by default.
- Known gap carried from the review (not fixed): a remux copy that stays 0 bytes for 3 s counts as settled. The original then moves while OBS still reads it. On Windows this fails to rename, falls back to a copy, and leaves a `copied_source_kept` duplicate. No file is lost.
- Answers: (1) OBS 32.2.2 ids checked; `mov_output` added; (2)–(4) left as known gaps for M2 and listed in the M2 risks (each leaves files in place, never loses them).
- Gates in the main checkout: Ubuntu CI-preset build OK with no undefined `clip_atlas` symbols, 234 of 234 unit tests, `build-aux/check-format` clean.
