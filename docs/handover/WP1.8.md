# WP1.8 hand-over: obs/frontend-events + replay sorting wiring

## Summary

- `src/obs/frontend-events.cpp` (new) implements `FrontendEvents`. On `OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED` it resolves the game (or Unsorted), picks the destination, waits for an OBS remux when one is coming, moves the replay (and its remuxed copy) with the main `FileMover`, and reports each file once: logged on the UI thread, then passed to the `ClipSorted` listener.
- `src/plugin-main.cpp` is wired for the lifecycle the orchestrator set in this round's briefs: load starts the tracker and the events, the Replay Buffer controller starts on `OBS_FRONTEND_EVENT_FINISHED_LOADING`, `OBS_FRONTEND_EVENT_EXIT` stops everything, and only `obs_module_unload()` destroys objects.
- Rules followed throughout: no frontend callback is ever removed; nothing is destroyed while OBS may still call its callback; the replay controller is not called before it has started.
- Verified: the CI preset builds with warnings as errors (0 warnings), the 170 unit tests pass, both sources pass `clang-format-19 --dry-run --Werror`, and scratch harnesses pass under ASan, UBSan, LeakSanitizer and TSan. Nothing was run inside OBS (see Known gaps).

## Files changed

| File | Change |
|---|---|
| `src/obs/frontend-events.cpp` | New. Implementation of `FrontendEvents`. |
| `src/plugin-main.cpp` | Rewritten from the stub. Module entry points, the plugin-level frontend callback, the settings listener, the Tools menu item, `start_plugin()`, `stop_all()`, `release_context()`. License header, `OBS_DECLARE_MODULE`, `OBS_MODULE_USE_DEFAULT_LOCALE`, `obs_module_name`/`obs_module_description` and the `loaded v%s` line are unchanged. |
| `docs/handover/WP1.8.md` | This note. |

No header, locale, CMake, core or `legacy/` file was touched. Nothing was committed or pushed.

## Base and merges

- STEP 0: `git merge --ff-only claude/m1-feature-parity` fast-forwarded `77f7bcd..9a1136a`. `git log --oneline -1` showed `9a1136a WP1.5: core/file-mover`, and the three files existed.
- Later, on the coordinator's notice, the worktree fast-forwarded to `d33ffae Add M1 manual test checklist` (adds `docs/testing/M1.md` only). The merge succeeded with my untracked `frontend-events.cpp` present.
- `close_settings_dialog()` on the branch: `src/ui/settings-dialog.hpp:64` declares `void close_settings_dialog();`, defined at `settings-dialog.cpp:198`. Used as declared.

## How it was tested

Commands are run from the worktree root. Scratch harnesses live in `/tmp/claude-0/-home-user-auto-clip-sorter/7156fddb-48fe-5a2e-b1b6-f895365608ba/scratchpad/wp18/harness/` and are not committed.

1. **CI preset build (warnings as errors).** `cmake --preset ubuntu-ci-x86_64 && cmake --build --preset ubuntu-ci-x86_64`. Result: exit 0, zero warnings, last rebuilt after the final sources. `nm -C build_x86_64/clip-atlas.so | grep " U " | grep clip_atlas` lists only `SourceTracker::` and `ReplayController::` members (WP1.7 and WP1.9 are not on the branch yet). Everything `FrontendEvents` and `plugin-main` need is defined, and the `obs_module_*` entry points are exported.
2. **Unit tests.** `cmake -S tests -B build/tests && cmake --build build/tests -j4 && ctest --test-dir build/tests --output-on-failure`. Result: 170 of 170 passed. Core is unchanged since this run.
3. **Format.** `clang-format-19 --dry-run --Werror src/obs/frontend-events.cpp src/plugin-main.cpp`: exit 0 on the final sources.
4. **Warnings.** `g++ 13.3 -std=c++17 -Wall -Wextra -Wpedantic -Wshadow` over the harness and the real sources: no warnings from `src/`. The only warnings are `-Wpedantic` anonymous-struct notices in OBS's own headers. An earlier build with `-Wconversion -Wsign-conversion -Wold-style-cast` was also clean for `src/`.
5. **FrontendEvents harness** (`harness/harness.cpp`). Links the real `frontend-events.cpp`, `ui-thread.cpp`, `settings-store.cpp` and the core (`file-mover`, `game-state`, `name-resolver`, `clip-layout`, `remux-rules`, `settings-model`) against the installed libobs 30.0.2. The OBS frontend API and `config_get_*` are stubbed, and a queue stands in for the UI thread. Command: `g++ -std=c++17 -g -O1 -fsanitize=address,undefined -I/usr/include/obs -Isrc -Isrc/core -Isrc/obs -Isrc/ui -Isrc/platform ... -lobs -lpthread`.
   - ASan + UBSan (LeakSanitizer on): **105 checks, 0 failed**, exit 0.
   - TSan: **105 checks, 0 failed, 0 ThreadSanitizer reports.**
   - Covered: current game into `<clips>/<Game>/<YYYY-MM>/`; no game into `Unsorted`; ignored game into `Unsorted`; user display-name rule; monthly folders off; remux with `.mkv` where the `.mp4` appears later (the original stays put until the copy settles, then both move, in order, with one result each); advanced mode with FFmpeg output does not wait for a remux; advanced mode reads `[AdvOut] RecFormat2` (a decoy in `[SimpleOutput]` would hang the test); sorting off (`auto_sort` and `sort_replays`); no replay path; no clips folder; the OBS recording folder used when the setting is empty; a name collision gets `name (2).mkv`; UTF-8 game and folder names; a game with no exe and no title goes to Unsorted; a throwing listener is logged; a missing source times out after 30 s and is reported as left in place; `stop()` leaves the callback registered and makes it inert; a late delivery after `stop()` does nothing; `stop()` during a remux wait cancels it and nothing moves; a result queued before `stop()` is dropped when handled after it; every result is handled on the UI thread; zero removals of frontend callbacks.
   - Not covered: the 10-minute remux timeout (the constant is too long for a test).
6. **Lifecycle harness** (`harness/lifecycle-v4.cpp`). Drives the real `plugin-main.cpp` through `obs_module_load`, `FINISHED_LOADING`, `EXIT`, and `obs_module_unload`. `SourceTracker`, `ReplayController` and the settings dialog are stubbed, and the stubs record their calls and destructors. Each scenario runs in its own process. ASan + UBSan + LeakSanitizer: **70 checks in six scenarios, all pass.**
   - `exit`: load starts nothing in the replay controller; `FINISHED_LOADING` starts it once, with the current settings; `EXIT` stops, destroys nothing, and removes nothing; afterwards the Tools item, the settings listener, the REPLAY callback and `run_on_ui` are all inert; `unload` then destroys the objects and logs `unloaded`.
   - `early-exit`: `EXIT` before `FINISHED_LOADING` never starts or explicitly stops the controller; a late `FINISHED_LOADING` starts nothing.
   - `unload-started` / `unload-not-started`: unload without `EXIT` stops everything and keeps the objects alive; the callbacks stay inert.
   - `failed`: a component throws during load; load returns false, nothing stays registered, the objects are stopped and kept.
   - `settings`: settings before start reach only the tracker; the controller gets the settings in force at `FINISHED_LOADING`; the ignore predicate follows the current rules; game changes are not forwarded before start.
   - The first run caught a real bug, now fixed: the Tools item still opened settings after `EXIT`, because the context now outlives `EXIT`. `open_settings_from_menu()` now checks the stopped flag.
   - The second run reported the retained context as a leak: a raw-pointer global that is only written is not a root for LeakSanitizer (most likely the optimiser removed it; not checked in the assembly). It is now held in a `std::unique_ptr` global, which the compiler must keep.

Not run: OBS itself, Windows, and MSVC. MSVC `/W3` cleanliness is reasoned, not tested: no implicit narrowing, no unused parameters (callbacks take unnamed parameters), explicit `static_cast`s, and the only `u8string()` use is copied byte by byte.

## Design

**Shared state and lifetimes (`frontend-events.cpp`).**
- The header fixes `std::unique_ptr<Impl> impl_`. So `Impl` owns a `std::shared_ptr<Shared>`, and `Shared` holds the state deferred work needs: the `GameState` and settings references, the main mover (not owned), the `waits` mover, the listener and the `stopped` flag.
- Deferred work (mover callbacks, wait callbacks, `run_on_ui` tasks) captures only a `std::weak_ptr<Shared>` and plain values. A weak pointer is locked only on the UI thread, and the lock is released there too (`if_running()`). So the last reference is never dropped on a mover's worker thread, which would destroy the `waits` mover from its own thread.
- The waits mover runs `wait_stable` jobs only, so a remux wait cannot hold up moves.
- Each wait callback hops to the UI thread with `run_on_ui` before calling `mover.move()`. Nothing calls into the plugin's mover from a worker thread.
- The OBS private data for the REPLAY callback is a `weak_ptr<Shared>` kept in `frontend_handles()`, a function-local `std::deque` (addresses stay stable). OBS keeps the callback for the life of the process, so the handle must outlive `FrontendEvents`. Once `FrontendEvents` is gone the handle is expired and the callback does nothing.

**`FrontendEvents`.**
- `start()` registers the callback once. `stop()` sets `stopped`, shuts down the waits mover (`shutdown(false)`) and does not remove the callback. A `start()` after `stop()` does nothing.
- A replay with no remux: `wait_stable(src, 1 s, 30 s)`, then move. A timeout still moves it.
- A replay with a remux (`expected_remux_path()` non-empty): `wait_stable(remuxed, 3 s, 10 min)`. If the copy settles, the original and the copy are queued in that order; if it times out, a WARNING is logged and only the original moves; a cancelled wait does nothing.
- Each move result hops to the UI thread: logged at INFO (`moved`, `copied_source_kept`) or WARNING (`failed`, `cancelled`, always with "the clip was left at"), then passed to the `ClipSorted` listener once per file. Results that arrive after `stop()` are dropped.
- Config values are read from `obs_frontend_get_profile_config()` only at save time, on the UI thread: `[Video] AutoRemux`, `[Output] Mode`, `[AdvOut] RecType` and `RecFormat2` from the section OBS uses.

**Plugin lifecycle (`plugin-main.cpp`), as changed by the orchestrator in this round.**
- `obs_module_load`: loads the settings, builds the context, calls `tracker->start` and `events->start` (signal connections and frontend callbacks only), subscribes the settings listener, adds the Tools item, and registers the plugin-level callback last. It does not start the replay controller and does not query the recording folder. It logs `loaded v%s`.
- `FINISHED_LOADING`: `start_replay_control()` calls `replay->start(settings_store().get())` once, then `replay->on_game_changed()`, then logs the effective clips folder (or "none").
- `EXIT`: sets `g_exit_seen` and calls `stop_all()`. `stop_all()` replaces the earlier `teardown()`: it is idempotent, calls `ui_tasks_shutdown()`, `close_settings_dialog()`, unsubscribes the listener, `events->stop()`, `replay->stop()` (only if started) and `tracker->stop()`, then `mover.shutdown(false)`. It destroys nothing.
- `obs_module_unload`: `stop_all()`, then `release_context()`, then logs `unloaded`. `release_context()` destroys the context if `EXIT` was seen. If not, the context is kept alive for the life of the process (see Deviations).
- Failed load: `stop_all()` runs and the context is kept.
- The plugin-level callback's private data is `nullptr`. The plugin never calls `obs_frontend_remove_event_callback`. A grep of both files finds no call.
- Callbacks reach the replay controller only after it has started: `on_game_changed` and `apply_settings` are skipped before `FINISHED_LOADING`.

## Deviations and why

1. **`Impl` owns a `shared_ptr<Shared>`; deferred work holds `weak_ptr<Shared>`, not `weak_ptr<Impl>`.** The header's `std::unique_ptr<Impl>` cannot be weak-referenced.
2. **The frontend callback's private data is a handle in `frontend_handles()`, not `this` and not an `Impl` member.** The callback can outlive `FrontendEvents` once callbacks are never removed.
3. **Wait callbacks hop to the UI thread before calling `mover.move()`.** Calling the main mover from the waits worker would need a lock on a worker thread (see Design).
4. **Cancelled move results are logged at WARNING**, and the brief listed only INFO and WARNING for moved, copied and failed. A cancelled move is still a clip left where it was.
5. **`stop_all()` and `release_context()` split teardown.** This implements the orchestrator's change #4. My addition: `release_context()` destroys only if `EXIT` was seen. Without `EXIT`, OBS may still call our objects, so the context is kept (`g_retained`). The brief says destroy at unload; this differs only when unload arrives without `EXIT`.
6. **The failed-load path keeps the stopped context** instead of destroying it, for the same reason. OBS may not call `obs_module_unload()` after a failed load.
7. **`replay->stop()` is called only if the controller was started.** Its destructor still calls `stop()` by contract (`replay-controller.hpp`), so an unstarted controller gets one `stop()` when destroyed.
8. **`replay->on_game_changed()` is also called once right after `replay->start()`.** This is my addition. Game changes before `FINISHED_LOADING` are not forwarded, so the controller learns the current game from this call. If WP1.9's `start()` already evaluates the current game, the call is redundant and harmless.
9. **`replay->apply_settings()` is skipped before start.** The brief says not to call it before start, and `start()` receives the current settings anyway, so nothing is lost.
10. **`open_settings_from_menu()` does nothing once stopped.** The lifecycle harness found that it still opened the dialog after `EXIT`.
11. **`ClipSorted::final_path` is empty for failed moves**, as the header requires. A failed move that left a partial copy is logged with the partial's path, because that is where the clip is.
12. **Unknown `[Output] Mode` values count as Simple**, and only `Advanced` (ignoring case) selects the advanced keys. This is my reading of OBS's `AutoRemux`, not checked against the OBS 31 source (see Questions).
13. **`user_rules()` is duplicated** in `plugin-main.cpp` and `frontend-events.cpp`, since neither header offers a shared helper.
14. **The Tools menu item cannot be removed** (no OBS API). After stop it does nothing.

## Known gaps and risks

1. **WP1.7 and WP1.9 are not on the branch.** The plugin links with unresolved `SourceTracker`/`ReplayController` members, as expected on Linux. The lifecycle harness stubs them, so the real behaviour of their `start`, `stop`, `on_game_changed` and destructors is not exercised.
2. **`ReplayController`'s destructor calls `stop()`** (by contract), even if the controller was never started. WP1.9's `stop()` must then be a safe no-op before start and must not remove its own callback. The orchestrator is already changing it; I rely on that.
3. **Not run in OBS.** Nothing was tested against a real OBS: frontend dispatch order, `FINISHED_LOADING` timing, the Tools menu, real replays, the remux file names, or the UI. Libobs 30.0.2 headers were used for compilation; the orchestrator verified the ordering and dispatch details against OBS 31.0.0 sources.
4. **The waits mover is FIFO.** A remux that never appears or never settles blocks every later replay's wait for up to 10 minutes. Real remux failures should be rare, but this is the worst case.
5. **The 10-minute timeout path is not tested**, nor is the same-folder no-op for a replay that OBS saves directly into its destination (the mover's behaviour, covered by WP1.5 tests).
6. **Windows and MSVC are unverified.** UTF-8 paths on Windows, the `ERROR_NOT_SAME_DEVICE` mapping and the other S-8 items from WP1.5 remain for manual testing.
7. **Retained objects.** The handles, and the context when unload comes without `EXIT`, are kept until the process exits. A later process-exit destruction of `g_retained` runs the destructors of already-stopped components. They should make no OBS calls, but this depends on WP1.7/WP1.9.
8. **Stale callbacks accumulate** in OBS's list, one per plugin load. Each is inert and costs little.

## Questions for the orchestrator

1. `release_context()` keeps the context when unload arrives without `EXIT` (deviation 5). Keep this, or destroy at unload as briefed?
2. The failed-load path keeps the stopped context until unload, and OBS may never call unload after a failed load. Acceptable?
3. `start_replay_control()` calls `replay->on_game_changed()` once after `start()` (deviation 8). Keep it, or does WP1.9's `start()` evaluate the current game itself?
4. Confirm that WP1.9's `stop()` is a safe no-op before `start()`, since its destructor calls `stop()` by contract (gap 2).
5. Should `user_rules()` move into core (`settings-model` or `name-resolver`) instead of being duplicated?
6. A stuck remux blocks the waits mover for up to 10 minutes per replay (gap 4). Accept, or give remux waits their own timer?
7. Confirm the `[Output] Mode` rule against OBS 31.0.0 `AutoRemux`: anything other than `Advanced` (ignoring case) is Simple.
8. Confirm the changes this round are as intended: stop/destroy split (#4), no callback removal anywhere (#3), replay start on `FINISHED_LOADING` (#2) and teardown on `EXIT` (#1).

## Orchestrator integration notes

- Reviewed `plugin-main.cpp` in full and `frontend-events.cpp`'s config, remux and lifetime handling. Gates: clang-format-19 clean; Ubuntu CI-preset build OK.
- **Fix by the orchestrator (Q7):** OBS 31.0.0 `OBSBasic::AutoRemux` uses `isSimpleMode = !mode || strcmp(mode, "Simple") == 0`, so anything other than exactly "Simple" counts as advanced. `read_remux_config()` now matches it. The earlier rule was inverted, though harmless for real configs, which only contain "Simple" or "Advanced".
- Answers:
  1. Accepted.
  2. Accepted.
  3. Redundant but harmless: WP1.9's `start()` records the current game and evaluates.
  4. Confirmed: WP1.9's `stop()` before `start()` only sets flags and stops timers.
  5. `user_rules()` moves into core in M4 (rules editor).
  6. Accepted. The FIFO wait is listed as a known gap on the M1 checklist.
  8. All four brief changes are as intended.
- Integrated after WP1.7, so the Windows link has every symbol.
