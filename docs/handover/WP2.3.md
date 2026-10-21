# WP2.3 hand-over: obs/clip-filer + screenshot sorting

## Summary

- `src/obs/clip-filer.cpp` (new) implements `ClipFiler`, `remux_timeout()` and `read_remux_config()`. This is M1's filing pipeline made generic over `ClipKind`: wait for OBS to finish writing (and remuxing) each file, move it into its game folder on the plugin's `FileMover`, log each outcome, and call the `ClipSorted` listener once per file. The files of one save share a `save_id`.
- `src/obs/frontend-events.cpp` is now only the OBS glue. It handles `OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED` (as before) and `OBS_FRONTEND_EVENT_SCREENSHOT_TAKEN` (new), decides whether sorting is on and which game the clip belongs to, and hands the files to one `ClipFiler`. The callback lifetime design is unchanged: registered once, never removed, inert after `stop()`.
- Replays behave as in M1: same waits and timeouts, original-then-remuxed order, same log lines, one listener call per file. This is checked by diffing a replay transcript from M1's own `frontend-events.cpp` (commit `bc31ea2`) against the new code, with and without shortened timeouts: the two are identical. The only addition is that `save_id` is now set.
- Every local acceptance check in the brief passes (see "How it was tested"). The MSVC /W3 build runs only in CI and was not run here (see Known gaps). Only the two owned files changed. Nothing was committed or pushed.

## Base

STEP 0: `git merge --ff-only claude/m2-recordings-screenshots` fast-forwarded `77f7bcd..bc31ea2`. `git log -1 --oneline` printed `bc31ea2 WP2.0: tracker uses OBS's last recording path only as a fallback`.

## Files changed

| File | Change |
|---|---|
| `src/obs/clip-filer.cpp` | New, 305 lines. `remux_timeout`, `read_remux_config` (moved from M1 unchanged), `ClipFiler` and `ClipFiler::Shared`. |
| `src/obs/frontend-events.cpp` | Rewritten, 363 to 210 lines (85 insertions, 238 deletions). The replay code moved into `ClipFiler`. Screenshot handling is new. |
| `docs/handover/WP2.3.md` | This note. |

No header needed a change. `CMakeLists.txt`, `plugin-main.cpp`, `src/core/`, `legacy/` and the other headers are untouched. `git status --short` shows only `M src/obs/frontend-events.cpp`, `?? src/obs/clip-filer.cpp` and `?? docs/handover/WP2.3.md` (this note). Build directories are git-ignored.

## How it was tested

All commands ran from the worktree root, `/home/user/auto-clip-sorter/.claude/worktrees/agent-a2408d5ef986f574a`.

1. **CI-preset build (warnings are errors).** `cmake --preset ubuntu-ci-x86_64 && cmake --build --preset ubuntu-ci-x86_64`: exit 0, zero warnings. The same command also passed on the untouched base (baseline). The final rerun reported "ninja: no work to do", so the built `.so` matches the current sources.
2. **Unresolved symbols.** `nm -C build_x86_64/clip-atlas.so | grep " U " | grep clip_atlas` prints nothing, so no `clip_atlas` symbol is left unresolved. The `ClipFiler` symbols show as local (`t`) because the plugin is built with `-fvisibility=hidden`. They are defined.
3. **Format.** `clang-format-19 --dry-run --Werror src/obs/clip-filer.cpp src/obs/frontend-events.cpp`: exit 0.
4. **Unit tests.** `cmake -S tests -B build/tests && cmake --build build/tests && ctest --test-dir build/tests --output-on-failure`: 175 of 175 passed.
5. **Stricter warnings (syntax only, stand-in for MSVC /W3).** g++ 13 with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wuseless-cast -Wformat=2`, and clang++ with the same set minus `-Wuseless-cast`: exit 0 for both files, no warnings. MSVC itself was not run.
6. **Scratch harness (not committed).** Source: `/tmp/claude-0/-home-user-auto-clip-sorter/6c995450-a095-5948-9d9e-9af008631cf2/scratchpad/wp23/harness/harness.cpp`. Build script: `/tmp/claude-0/-home-user-auto-clip-sorter/6c995450-a095-5948-9d9e-9af008631cf2/scratchpad/wp23/harness/build-check.sh real|short asan|tsan none|ui`. The harness compiles the real `frontend-events.cpp`, `clip-filer.cpp`, `ui-thread.cpp` and `settings-store.cpp`, plus the core files (`file-mover`, `clip-layout`, `clip-routing`, `remux-rules`, `name-resolver`, `game-state`, `settings-model`). It links the installed libobs 30 for the real `config_*`, `obs_data_*`, `os_*`, `bstrdup` and `bfree`. The OBS frontend API, `obs_log`, the module config path and the UI task queue are stubbed. The harness's main thread is the UI thread, and it runs the queue.

   | Build | Result |
   |---|---|
   | real timeouts, ASan + UBSan + LSan, UI-thread lock check | 140 checks passed, 0 failed, no sanitizer report |
   | short timeouts, ASan + UBSan + LSan, UI-thread lock check | 142 passed, 0 failed, no sanitizer report |
   | real timeouts, TSan, UI-thread lock check | 140 passed, 0 failed, 0 ThreadSanitizer reports |

   Before the last two cases (C17, C18) were added: real ASan 129 of 129, short ASan 131 of 131, real TSan 129 of 129.

   - **Short-timeout build.** The copy of `clip-filer.cpp` has three sed replacements: the plain timeout goes from 30 s to 4 s, and the 10-minute and 60-minute remux timeouts go to 8 s. The script checks that all three applied. Everything else is identical. The real builds compare their copies byte for byte against the sources (`cmp`).
   - **UI-thread lock check.** This build inserts `if (!is_ui_thread()) std::abort();` before the one `self.lock()` in each file's `if_running`. It shows at run time that the weak_ptr is only locked on the UI thread in every case. The shipped code does not contain this check.
   - **What the harness covers.**
     - A replay with a game rule goes to `<clips>/<Game>/<YYYY-MM>/`. Recordings and screenshots use the same folder logic with their own nouns in the log. Unsorted goes to `Unsorted`, and the flat layout works with monthly folders off.
     - A remux pair: the copy grows for about 1.5 s. The original moves only after the copy has been stable, measured as at least 2.5 s from the last write to the first callback. The order is original, then copy, with one `save_id`.
     - Remux never appears (short build only): a WARNING names the copy, and only the original moves.
     - Missing source: the move fails after the timeout, and the WARNING names the path. No clips folder: one WARNING per file, nothing moves. An empty list returns false silently.
     - `stop()` during a plain wait or a remux wait: no callback, no log, nothing moves, the file is untouched, `file()` after `stop()` returns false, and `stop()` returns promptly. Destroying a filer with work pending reports nothing.
     - `save_id`: one per save and shared within it, distinct across saves, and unique across two filers.
     - A name collision gives `Replay (2).mkv`, with the existing file untouched. UTF-8 game and file names work. A listener that throws gives an ERROR log, and the next file is still reported.
     - `copied_source_kept`: an injected `FileOps` makes the first rename fail as cross-device and refuses to remove the source. The result is INFO "replay copied to ...; the original was kept at ...".
     - `cancelled`: the plugin's mover is shut down first, so the move is cancelled on the spot and logged as a WARNING.
     - Through `FrontendEvents`: the replay and screenshot events file the right paths, and a game rule gives the game folder. NULL and empty paths log the exact WARNING text and file nothing. With `sort_screenshots` or `auto_sort` off, the log says "sorting off ..." and nothing moves. With AutoRemux in simple mode the pair moves in order. With advanced FFmpeg there is no pair. A screenshot is never paired, even with AutoRemux on. `FINISHED_LOADING` is ignored. After `stop()` the callback does nothing, `start()` registers nothing, and no frontend callback is removed. A stale callback after destruction does nothing.
     - `read_remux_config()` for a NULL profile, a missing `Mode`, `Advanced` with `RecType=ffmpeg` (case ignored), `Simple`, `Mode=simple` (not exactly "Simple", so advanced, as in OBS), and `RecType=Standard`. `remux_timeout()` gives 10, 10 and 60 minutes.
   - **Not covered.** The real 10-minute and 60-minute timeouts (only the constants are checked), real OBS, Windows and MSVC, and `copied_source_kept` on a real second volume.
7. **Static checks on the two files.** Neither file references `obs_frontend_remove_event_callback`. `this` appears only in comments: the three `[&]` lambdas run synchronously inside `if_running` and use only their parameters and captures. No `std::filesystem::path` is built from a `std::string`. No TODO.
8. **Parity with M1 (replays).** The same harness, in a `parity` mode, runs replay saves through `FrontendEvents` and prints a transcript of every log line and every `ClipSorted`, without `save_id`. It was built twice: once against M1's `frontend-events.cpp` from `bc31ea2`, and once against the new one. The only edit to M1's copy is two renames, `ClipGame` to `M1ClipGame` and `read_remux_config` to `m1_read_remux_config`, because the current headers declare the same names. There is no logic change.
   - Real timeouts: eleven scenarios (a plain replay in a game folder; NULL and empty paths; sorting off; no clips folder; an AutoRemux pair with the copy already there; an AutoRemux pair with the copy still being written; advanced FFmpeg, no remux; a missing source; a name collision; a save after `stop()`). The two 31-line transcripts are identical.
   - Short timeouts: the same eleven, plus a twelfth where the remux copy never appears. The two transcripts are identical, including the WARNING and the original-only move.
   - The only difference is `save_id`, which goes to stderr. M1 leaves it at 0. The new code gives each save a fresh id, and gives both files of a remux pair the same id (values `1 2 2 3 3 4 5 6 7`).
   - Transcripts: `/tmp/claude-0/-home-user-auto-clip-sorter/6c995450-a095-5948-9d9e-9af008631cf2/scratchpad/wp23/parity-real-m1.txt`, `/tmp/claude-0/-home-user-auto-clip-sorter/6c995450-a095-5948-9d9e-9af008631cf2/scratchpad/wp23/parity-real-new.txt`, `/tmp/claude-0/-home-user-auto-clip-sorter/6c995450-a095-5948-9d9e-9af008631cf2/scratchpad/wp23/parity-short-m1.txt` and `/tmp/claude-0/-home-user-auto-clip-sorter/6c995450-a095-5948-9d9e-9af008631cf2/scratchpad/wp23/parity-short-new.txt`. Build and run scripts: `/tmp/claude-0/-home-user-auto-clip-sorter/6c995450-a095-5948-9d9e-9af008631cf2/scratchpad/wp23/harness/build-parity.sh` and `/tmp/claude-0/-home-user-auto-clip-sorter/6c995450-a095-5948-9d9e-9af008631cf2/scratchpad/wp23/harness/run-parity.sh`. Both binaries were built with ASan and UBSan, and neither printed a sanitizer report.

## Design

- **`ClipFiler::Shared`** is defined in `clip-filer.cpp` as `struct ClipFiler::Shared`. Its helpers (`if_running`, `queue_plain`, `queue_remux`, `queue_move`, `report`) are members, not free functions, because `Shared` is private to `ClipFiler` and a free function in an anonymous namespace cannot name it.
- **Deferred work** captures only a `std::weak_ptr<Shared>` and plain values: a `Save` (kind, `save_id`, destination, `ClipGame`) plus the file paths. The weak_ptr is locked only in `if_running`, which runs only inside `run_on_ui` tasks, so only on the UI thread. The move and wait callbacks never lock it, so the waits mover is never destroyed on its own worker thread.
- **`file()`** (UI thread): if stopped or empty, it returns false silently. With no clips folder it logs one WARNING per file. Otherwise it copies the settings, computes the destination once, takes the next `save_id` from a process-wide `std::atomic<std::uint64_t>` starting at 1, and queues each file: `queue_plain` when `remuxed` is empty, `queue_remux` otherwise.
- **`report()`** builds the log lines with the kind's noun ("replay", "recording", "screenshot"), then calls a copy of the listener inside try/catch.
- **`FrontendEvents::Shared`** holds the `GameState` reference, the `SettingsStore` reference (the sorting check and the game lookup need it), `ClipFiler filer` and `stopped`. `take_obs_path()` takes ownership of OBS's bstrdup'd path, which is freed on every path, and logs the event's warning when the path is empty. `sort_saved_replay()` and `sort_taken_screenshot()` follow the steps in `frontend-events.hpp`. `frontend_handles()`, the `if_running` pattern, `start()` and `stop()` are as in M1.

## Deviations and why

1. **Helper placement.** The helpers are members of `ClipFiler::Shared`, defined in the .cpp, not free functions. The reason is above. The contract does not say where they go, and the interface is unchanged.
2. **`file()` catches all exceptions.** The header says `file()` never throws, so the body is wrapped. If `std::bad_alloc` is thrown partway through the loop, the files queued before it stay queued and are still moved and reported, and `file()` returns false. Callers ignore the result, so only the log is affected. This is noted in the code.
3. **`FrontendEvents::Shared` keeps a `SettingsStore &`.** The contract lists the filer, the `GameState` reference and the flag, but the handlers need the settings for the sorting check and the game lookup.
4. **Two handlers instead of one generic function**, sharing `take_obs_path()`. They differ in the OBS getter, the warning text, the remux argument and the game-independent part, and reading them side by side is clearer.
5. **The screenshot path is read even when sorting is off**, as M1 does for replays: OBS's path is taken first, then the setting is checked. This is a read and a free, so it is harmless.

## Known gaps and risks

1. **Shared waits.** Replays and screenshots share one `ClipFiler` and one FIFO waits mover, as the contract says. A replay's remux wait (up to 10 minutes if the copy never appears) therefore delays any screenshot queued behind it. This is the same FIFO risk already noted for M1 (WP1.8, gap 4); screenshots now share it. Normal remux delays are a few seconds.
2. **Real timeouts not run.** The 10-minute and 60-minute timeouts are checked as constants only. The short-timeout build exercises the branch.
3. **Not run in OBS, Windows or MSVC.** MSVC /W3 is reasoned, not run: there is no implicit narrowing (explicit `static_cast`s), no unused parameter, `u8string()` is only copied byte by byte, the `std::chrono` conversions are exact, and `std::atomic<std::uint64_t>` is lock-free on x64.
4. **`copied_source_kept`** is tested through an injected `FileOps`, not a real second volume.
5. **The `bad_alloc` case** above leaves earlier files queued while `file()` reports false.
6. **The threading rule is by design, not by assertion.** The UI-thread check exists only in the harness build. The shipped code relies on the lock living only in `if_running`, which runs only in `run_on_ui` tasks and the frontend callback.
7. **Log lines are English and do not go through `obs_module_text`,** as in M1. They are logs, not UI strings.
8. **Parity covers replays only.** Recordings and screenshots have no M1 baseline. Their behaviour rests on the same code path and on the harness's own checks.

## Questions for the orchestrator

1. Replays and screenshots share one `ClipFiler`, as the contract and the header say. Should screenshots get their own filer, so a stuck replay remux cannot delay them? `FrontendEvents` could own two filers without a header change. If so, the sentence in `frontend-events.hpp` ("Replays and screenshots share one ClipFiler, so their waits run in order") would need to change, so I have left it alone.
2. Is `file()` returning false (with earlier files still queued) after a `bad_alloc` acceptable, or should the contract say something else for that case?
3. Please confirm the helper placement in deviation 1.
4. Someone with a Windows/MSVC setup should confirm the CI build. I could only check with GCC and Clang.

## Orchestrator integration notes

- Read `clip-filer.cpp` and `frontend-events.cpp` in full; changes stay within WP2.3's ownership. Gates in the main checkout (with WP2.4 and WP2.5 already integrated): Ubuntu CI-preset build OK with no undefined `clip_atlas` symbols, 202 of 202 unit tests, `build-aux/check-format` clean.
- With `save_id` now set, WP2.5's one-sound-per-save rule is effective for remuxed replays.
- Answers: (1) replays and screenshots keep one shared filer for now; a stuck replay remux (up to 10 min) delaying a screenshot is rare and leaves nothing lost, and it is the same FIFO gap already on the M1 list; (2) accepted; (3) accepted; (4) Windows CI checks the MSVC build on push.
