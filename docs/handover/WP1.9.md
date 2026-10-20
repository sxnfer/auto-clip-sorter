# WP1.9 obs/replay-controller: hand-over

## Summary

Implemented the contract in `src/obs/replay-controller.cpp`. The header is unchanged. After every game change, settings change and relevant OBS event, the controller asks `replay-policy` what to do, and it makes the `obs_frontend_replay_buffer_start/stop` calls. It records that a buffer is ours only when OBS's STARTED answers a start we requested. It never stops a buffer the user started, and `stop()` never calls `obs_frontend_replay_buffer_stop`.

The `ubuntu-ci-x86_64` build compiles the file with `-Werror`. A scratch harness runs the real controller against a fake OBS frontend: all 19 scenarios (124 checks) pass under AddressSanitizer and UBSan, and two deliberately broken copies of the controller are caught.

Two decisions are needed from you. Question 1 is where WP1.8 first calls `start()`. Question 2 is whether you accept one refinement of the STOPPED rule (Deviation 1).

## Files changed

- `src/obs/replay-controller.cpp` (new, 447 lines).
- `docs/handover/WP1.9.md` (new, this note).

Nothing else is tracked. `git status` shows only `?? src/obs/replay-controller.cpp`, plus this note. My builds created `build/`, `build_x86_64/` and `cmake/.CMakeBuildNumber`, all of which `.gitignore` covers. I did not touch any header, CMake file, `plan.md` or test. The harness and mutants are scratch files under `/tmp/claude-0/-home-user-auto-clip-sorter/7156fddb-48fe-5a2e-b1b6-f895365608ba/scratchpad/wp19/`. Nothing is committed or pushed.

## How it was tested

Commands were run from `/home/user/auto-clip-sorter/.claude/worktrees/agent-a72861ca8b90a2eef`.

1. **STEP 0.** `git merge --ff-only claude/m1-feature-parity` fast-forwarded `77f7bcd..9a1136a`. `git log --oneline -1` shows `9a1136a WP1.5: core/file-mover`. Both `src/obs/replay-controller.hpp` and `src/core/replay-policy.cpp` exist.

2. **(a) CI preset build.** `cmake --preset ubuntu-ci-x86_64 && cmake --build --preset ubuntu-ci-x86_64` exits 0, both before and after the final edits. `ninja -t commands` shows `-Werror` on the controller's compile line. The only CMake note is the pre-existing "ENABLE_CCACHE not used", which WP1.6 also reported. `nm -C` on `clip-atlas.so` shows the controller's symbols as local (`t`). Only the `obs_module_*` entry points are exported.

3. **(a) Format.** `clang-format-19 --dry-run --Werror src/obs/replay-controller.cpp` exits 0 (clang-format 19.1.1). I applied `clang-format-19 -i` to this file only, before that check. No line is wider than 120 columns, with tabs counted as 8.

4. **(a) Unit suite.** `cmake -S tests -B build/tests && cmake --build build/tests -j4 && ctest --test-dir build/tests --output-on-failure` gives 170 of 170 passed. A later `ctest` run after the final edits also gives 170 of 170.

5. **Strict syntax (extra).** g++ 13.3.0 and clang++ 18.1.3, both with `-std=c++17 -fsyntax-only -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wformat-security -Werror` (clang also `-Wshorten-64-to-32`), exit 0. OBS and Qt headers are passed as `-isystem`, so their own pedantic warnings do not count.

6. **OBS API check.** Every call and event named in the brief matches the installed headers. `/usr/include/obs/obs-frontend-api.h` has the event callback typedef, the add and remove callback functions, the replay start, stop and active functions, `obs_frontend_get_replay_buffer_output`, and the enum values. `obs.h:2039` has `obs_output_release`. These are the libobs 30.0.2 headers in this container. The OBS 31.0.0 source is not here, so the 31.0.0 match rests on the brief.

7. **(b) Scenario harness.** Source: `scratchpad/wp19/harness/harness.cpp`, scratch only. It is built with g++ 13.3 `-std=c++17 -g -O1 -fsanitize=address,undefined -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion`. It links the real `replay-controller.cpp`, `game-state.cpp` and `replay-policy.cpp` against a fake OBS frontend. The fake implements the `obs_frontend_*` functions, `obs_output_release` and `obs_log`. It delivers OBS's answers 20 ms later on the Qt event loop, or inside the call in synchronous mode. It also counts acquired and released output references for each scenario.

   Result: **19 scenarios, 124 checks, 0 failures**, exit 0. The run had no AddressSanitizer, UBSan or LeakSanitizer output. The full run takes about 90 seconds, because S6, S13 and S16 wait out the real 10 s timeout.

   | | Scenario | Result |
   |---|---|---|
   | S1 | Game hooked: one start requested. STARTED makes the buffer ours (`active_by_us`) | ok |
   | S2 | Game closes: the stop is sent only after the grace period (1000 to 1050 ms after the close, grace 1 s) | ok |
   | S3 | Game returns within the grace period: no stop, no second start | ok |
   | S4 | User-started buffer: never stopped, never started by us | ok |
   | S5 | User stops while the game runs: not restarted; restarted when the game changes (another window) | ok |
   | S6 | OBS never answers a start: one request, the real 10 s timeout, no retry; a new game gets one request | ok |
   | S7 | Replay Buffer off in Output settings: no start, status `disabled_in_obs`; PROFILE_CHANGED after enabling starts it | ok |
   | S8 | `stop()`: buffer keeps running, callback removed, replayed events and game changes do nothing, `status()` makes no OBS call, nothing logged after stop | ok |
   | S9 | OBS answers synchronously, inside the call: no duplicate requests | ok |
   | S10 | `OBS_FRONTEND_EVENT_EXIT`: no start or stop afterwards | ok |
   | S11 | `apply_settings()`: auto start and auto stop toggles apply at once | ok |
   | S12 | Start answered after the game already closed: the grace period still applies before the stop | ok |
   | S13 | Stop answered after our timeout, with the game back: the late answer does not suppress the restart | ok |
   | S14 | Grace of 0: immediate stop | ok |
   | S15 | A status callback that throws is contained and logged; the controller keeps working | ok |
   | S16 | A stop OBS drops is sent again after the timeout, and that answer ends it | ok |
   | S17 | Game flapping during a pending start: one request | ok |
   | S18 | OBS stops the buffer before our start is answered, with the game running: warning, no retry | ok |
   | S19 | Nothing happens before `start()`; teardown removes the callback once and releases every reference | ok |

   A verbose run of S2 (`HARNESS_VERBOSE=1`) gives this log, which is what to expect on Windows:

   ```
   Replay Buffer control on: auto start on, auto stop on, grace 1 s
   Current game is now eldenring.exe
   Replay Buffer start requested: eldenring.exe is running and the buffer is off
   Replay Buffer started by Clip Atlas, 20 ms after the request
   Replay Buffer is running, started by Clip Atlas
   Current game is now no game
   Replay Buffer stop waits for the grace period (1000 ms left)
   Replay Buffer stop requested: no game for 1050 ms, past the 1 s grace period
   Replay Buffer stopped by Clip Atlas
   Replay Buffer is off
   Replay Buffer control off; the Replay Buffer keeps running as it is
   ```

8. **Mutants (checking that the harness catches the rules).** Two scratch copies of the controller were made from the final file, each with one line changed. Both were rerun after the final edits.
   - **M1**, the literal STOPPED rule (`pending != stop`, without `stop_unanswered`). S13 fails with 3 failed checks, because the buffer is not restarted. This is the reason for Deviation 1.
   - **M2**, no grace period for the stop. S2 fails with 2 failed checks: the stop is sent 0 ms after the game closes.

9. **(c) MSVC /W3 /WX.** MSVC is not available in this container, so this is reasoned from the code, not run. Every integer narrowing is an explicit `static_cast` (`int` for the timer, `long long` for the log values). `(std::numeric_limits<int>::max)()` is parenthesised so that a `max` macro cannot expand. Every parameter is used. Every `obs_log` format is a literal, and `%lld` always gets a `long long`. I expect no C4244, C4267 or C4100 warnings, but please confirm on CI.

## Deviations from the contract, and why

1. **STOPPED rule refined with `stop_unanswered`.** The brief says that a STOPPED with `pending != stop` and a game running sets `suppressed`. That is wrong for a stop OBS answers after our 10 s timeout. The timeout clears `pending`, so the late answer would be taken as the user's stop, and the game would be left without a buffer. `stop_unanswered` is set when we send a stop and cleared when the answer arrives. A STOPPED counts as the user's only when that flag is clear and a game is running. The two rules differ only for a stop answered after our timeout. Mutant M1 and S13 show the difference (Question 2).
2. **`status()` after `stop()`.** It returns the last reported status and makes no OBS call. The brief says it computes live. After `stop()`, OBS may be tearing down, so an OBS call there is unsafe. While running, `status()` is live as the brief says.
3. **Single use.** `start()` after `stop()` or after `OBS_FRONTEND_EVENT_EXIT` logs a warning and does nothing. The brief does not say. WP1.8 creates the controller once, so it should not need a restart.
4. **Cancellable timers.** The grace re-check and the pending timeout are `QTimer` members set to single-shot, not the static `QTimer::singleShot`. A pending re-check has to be cancellable on `stop()`, when the game returns, and on a settings change. The brief's implementation list asks for a `QTimer`, but the header comment says `QTimer::singleShot` (Question 5).
5. **`on_game_changed()` after `stop()` or EXIT** returns at once, with no log and no state change. The brief says nothing runs afterwards.
6. **Logs name the program, not the window title,** because window titles can carry private text. The executable from `CurrentGame` is logged, or "a program with no name" if it is empty.

Not deviations, but worth knowing: `apply_settings()` before `start()` only stores the settings. The re-check wait is rounded up with `std::chrono::ceil`, so the wake-up never comes before the deadline, and it is capped to the `int` range that `QTimer::start` takes.

## Known gaps and risks

1. **OBS itself has not been run.** Nothing here runs OBS. The fake's timing (20 ms, queued or synchronous) is my assumption from the brief. So is what OBS does when a start is silent, such as a modal error dialog. The first Windows test should check the log lines for start, stop, timeout and suppression.
2. **Status callback lag.** `update_status()` runs only inside `evaluate()`. `evaluate()` runs on STARTED, STOPPED, PROFILE_CHANGED, FINISHED_LOADING, game changes, settings changes, and our own re-check or timeout. Toggling "Enable Replay Buffer" in Output settings without a profile change is not seen until one of those happens, so the M3 banner can lag. `status()` itself is live while the controller runs.
3. **Stop retry.** A stop that OBS never answers is sent again every 10 s, while the buffer runs and the game is gone. Only buffers we started are stopped. Each retry is logged, and the loop ends when OBS reports STOPPED.
4. **A late start answer counts as the user's.** A STARTED that arrives after our start timed out sets `we_started` to false, so that buffer is never stopped by us. This is the safe way to fail.
5. **A silent start means no buffer for this game.** After a timeout, the game gets no more requests until the game changes. Fixing the output path while the same game keeps running does not restart the buffer. The user can start it in OBS, and that buffer is theirs, so we will not stop it.
6. **Profile changes.** If OBS stops the buffer on a profile change while a game runs, that stop counts as the user's, and the buffer is not restarted for that game (Question 3).
7. **`stop()` must not run inside a frontend callback.** `obs_frontend_remove_event_callback` runs during OBS's dispatch, and it could change the list OBS is walking. That includes a call to `stop()` from the status callback. The caller must follow this rule.
8. **Callback removal during unload.** `stop()` removes the frontend callback. If WP1.8 calls it after OBS's frontend has gone, that call could misbehave. The header asks for `stop()` in `obs_module_unload()`, so this is only a wiring risk.
9. **Fixed 10 s timeout.** It is a constant, not a setting. The harness runs the real value.
10. **No `run_on_ui()`.** The controller is UI-thread only, as the contract says. Callers hop before calling it.

## Questions for the orchestrator

1. **Where does WP1.8 call `start()`?** The controller calls OBS frontend functions as soon as `start()` runs, because `evaluate()` calls `obs_frontend_get_replay_buffer_output()`. If `start()` runs from `obs_module_load()`, OBS's main window may not exist yet. `obs-module.h` documents `obs_module_post_load()` as "Called when all modules have finished loading", so I suggest calling `start()` there. I could not confirm OBS 31's order from source in this container. Please decide, and tell WP1.8.
2. **Accept the `stop_unanswered` refinement?** This is Deviation 1. I recommend accepting it. S13 with M1 shows that the literal rule would leave a game without a buffer after a late stop answer.
3. **Profile change and suppression.** Should PROFILE_CHANGED clear `suppressed`? Today a stop that OBS makes on a profile change counts as the user's. I have not verified OBS's behaviour here.
4. **Status lag.** Is there an OBS event for Output settings being applied that I should evaluate on? If there is none, do you accept the lag in Risk 2?
5. **Header wording.** Should `replay-controller.hpp` describe the re-check as a cancellable single-shot `QTimer` instead of `QTimer::singleShot`? The header is yours, so I did not change it.
6. **Single use.** Do you accept that the controller cannot be restarted after `stop()`?
7. **Suppression after a silent start.** Accept Risk 5, or should the next settings change also clear `suppressed`?

## Notes for other work packages

- **WP1.8 (plugin-main wiring).** Construct `ReplayController(game_state)` once. Call `start(settings)` once, at the time decided in Question 1. Call `apply_settings()` from the settings listener, `on_game_changed()` after each source-tracker change that moves `GameState::current()`, and `stop()` in `obs_module_unload()`, before anything else tears down. Do not call `stop()` from inside any frontend or status callback.
- **M3 (dock banners).** `status()` is live while the controller runs, and it returns the last value after `stop()`. Use the callback for changes, but expect the lag in Risk 2.
- **Log lines to look for on Windows:** "Replay Buffer start requested", "Replay Buffer started by Clip Atlas, N ms after the request", "Replay Buffer stop requested", "did not start the Replay Buffer within 10 s", and "stopped while a game runs; not restarting until the game changes".
- **Plan status.** I did not touch the status table in `plan.md`. WP1.9 is ready for your review.

## Orchestrator integration notes

- Reviewed the event handling and the `stop_unanswered` rule. Gates re-run in the main checkout: check-format clean, Ubuntu CI-preset plugin build OK.
- Answers:
  1. **Neither `obs_module_load` nor `obs_module_post_load`.** The orchestrator checked `OBSBasic::OBSInit` in OBS 31.0.0: modules load (~2114) and post-load (~2118) **before** `ResetOutputs()` (~2141), so the replay-output call would dereference a null `outputHandler`. WP1.8 calls `start()` on `OBS_FRONTEND_EVENT_FINISHED_LOADING` (~2443). The header now says so.
  2. Deviation 1 (`stop_unanswered`) is accepted.
  3. PROFILE_CHANGED doesn't clear suppression. This goes on the M1 test checklist to watch.
  4. The status lag is accepted for now and revisited with the M3 banner.
  5. The header wording is updated.
  6. Single use is accepted.
  7. Risk 5 is accepted: the user can start the buffer by hand.
