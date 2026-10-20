# WP1.7 hand-over: obs/source-tracker (★)

**Status:** implemented and verified on Linux. Fix round 1 is applied; its section at the end has the current results.
On the final source the CI-preset build, the format check, the strict GCC and Clang checks, the unit suite (131 of 131)
and the OBS-linked harness pass. One TSan report remains in the stop-during-stress scenario. It is analysed in Fix round 1
and is probably a false positive, but it is not proven. Not committed. Windows and MSVC have not been run. The ★ review
should focus on the threading assumptions and the open items at the end.

## Summary

- `src/obs/source-tracker.cpp` implements the `SourceTracker` contract in `src/obs/source-tracker.hpp`. The header is
  unchanged. Its threading paragraph needs the wording proposed in Fix round 1. The orchestrator owns that file.
- Discovery is event-driven. The global `source_create`, `source_destroy` and `source_rename` signals drive it, together
  with the frontend events `SCENE_COLLECTION_CHANGING`, `SCENE_COLLECTION_CHANGED` and `FINISHED_LOADING`. OBS 31.0.0 never
  emits `SCENE_COLLECTION_CHANGING` (see Known gaps). `obs_enum_sources()` finds sources that already exist. There is no
  polling.
- Every `game_capture` and `window_capture` source is connected, whatever the selection. Its `hooked`, `unhooked`,
  `activate` and `deactivate` signals are wired on its own handler. The selection is applied on the UI thread.
- Callbacks copy their data and post to the UI thread with `run_on_ui()`. Only the UI thread touches `GameState`, the UI
  map and the settings. The registry holds only `obs_weak_source_t` references.
- `get_hooked` runs only on the graphics thread. When the UI thread needs a source's state, it queues an
  `OBS_TASK_GRAPHICS` task. That task reads the state and hands it back, with a strong reference, through `run_on_ui()`.
  The UI thread releases the reference.
- `stop()` clears `running`, waits for OBS's destroy queue, disconnects the three global signals, then disconnects every
  per-source signal. The frontend callback is never removed. It stays registered and does nothing once the tracker has
  stopped. A task still queued after `stop()` does nothing.
- Answer to the plan's open question: `window_capture` emits `hooked` and `unhooked` in OBS 31.0.0, with the same
  signatures as `game_capture` and the same `get_hooked` proc. Checked in `plugins/win-capture/window-capture.c`
  (declarations at lines 334-335, emission at 584, 605, 668, 723 and 761). `game-capture.c` declares them at 612-613.

## Files changed

- Added `src/obs/source-tracker.cpp`.
- Added `docs/handover/WP1.7.md` (this note).

Nothing else changed. For the commit, stage those two files. `build/` and `build_x86_64/` are gitignored. The harness is
scratch only and is not committed. It lives in
`/tmp/claude-0/-home-user-auto-clip-sorter/7156fddb-48fe-5a2e-b1b6-f895365608ba/scratchpad/wp17/`: `harness/harness.cpp`,
the build and run scripts (`build-asan.sh`, `run-fix-asan.sh`, `run-fix-tsan.sh`, `run-fix-repeat.sh`,
`run-stop-repeat.sh`, `run-final-sweep.sh`), and the checks (`strict-check.sh`, `final-checks.sh`).

## How it was tested

The tables in this section are from the first report, before fix round 1. The results on the final source are in Fix
round 1. The commands are unchanged.

Commands run from the worktree `/home/user/auto-clip-sorter/.claude/worktrees/agent-a6154bc27bd17c913`.

| Check | Command | Result |
|---|---|---|
| STEP 0 | `git merge --ff-only claude/m1-feature-parity` | Fast-forwarded `77f7bcd..e754231`. `git log --oneline -1` shows `e754231 WP1.6: obs/ui-thread + obs/settings-store`. The three named files exist. |
| CI preset build (a) | `cmake --preset ubuntu-ci-x86_64 && cmake --build --preset ubuntu-ci-x86_64` | Exit 0. `CMAKE_COMPILE_WARNING_AS_ERROR=ON`, and the compile of `source-tracker.cpp` runs with `-Werror -Wextra -Wunused-parameter`. The build log has 0 warnings. Local libobs 30.0.2.1. |
| Format (a) | `clang-format-19 --dry-run --Werror src/obs/source-tracker.cpp` | Exit 0. |
| Strict GCC (a) | `g++ -std=c++17 -fsyntax-only -isystem /usr/include/obs -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wcast-qual -Wunused -Wuseless-cast -Wnull-dereference -Wlogical-op -Werror` (GCC 13.3) | Exit 0. OBS headers are passed with `-isystem`, so only this file is checked. |
| Strict Clang (a) | `clang++ -std=c++17 -fsyntax-only -isystem /usr/include/obs -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wcast-qual -Wunused -Werror`, with `-Wno-` for the C++98, padding, missing-prototype, exit-time-destructor and global-constructor checks | Exit 0. |
| Unit suite (a) | `cmake -S tests -B build/tests -G Ninja && cmake --build build/tests -j4 && ctest --test-dir build/tests --output-on-failure` | `100% tests passed, 0 tests failed out of 131`. This WP does not touch `core/` and adds no test. |

MSVC is not installed here. The `/W3` requirement is covered by the strict GCC and Clang runs and by reading the code.
C4457 (a lambda capture hiding a parameter) was avoided by renaming the parameter in `Impl::post`.

### Behavioural harness (b), first report

A scratch program links the real `source-tracker.cpp`, `ui-thread.cpp` and `game-state.cpp` against the installed libobs
30.0.2 and runs under `xvfb-run -a`, since `obs_startup()` needs a display. The sources are fake `game_capture` and
`window_capture` inputs. They declare `hooked`, `unhooked` and `get_hooked` the way win-capture does. OBS's
`obs_queue_task(OBS_TASK_UI)` is routed to a test queue. The main thread drains it, as Qt does in OBS. Logging and the
frontend callback list are stubbed. The first report had 65 check statements across four scenarios:

| Scenario | What it exercises | ASan + UBSan | TSan |
|---|---|---|---|
| `basic` | A source present before `start()` and hooked and active there. A source created after `start()`. Hooks from a worker thread, and a repeat hook. Liveness, with a live source beating a non-live one. An ignored executable. Rename. Destroying the current source. Unhooking, then `game_for_save()` inside the grace period. Selection changes. `SCENE_COLLECTION_CHANGING` and `CHANGED`. `stop()`, after which nothing reaches `GameState`. | 0 failures, no sanitizer report (4 runs) | 0 races, 0 failures (4 runs) |
| `collision` | A source is released while its `destroy()` is blocked. A new source is then loaded with the same UUID. The old source's removal must not touch the new one. Later events for the new source still apply. | 0 failures (4 runs) | 0 races, 0 failures (4 runs) |
| `leak` | `stop()` runs while a source is at zero references and its `source_destroy` has not run. The record is kept on purpose. | 0 failures. LeakSanitizer reports only the intended record. | 0 races, 0 failures (1 run) |
| `stress` | Four threads, each running 150 cycles of create, hook, liveness, unhook, destroy, with the UI thread draining tasks. | 0 failures, no leak report (1 run) | 0 races, 0 failures (3 runs) |

Notes on the first-report harness, so the numbers are read correctly:

- Two harness faults were found and fixed. libobs 30 needs `get_width` and `get_height` on video inputs. Without them the
  fake sources were never registered. `obs_wait_for_destroy_queue()` returns at once when video and audio were never
  initialised. The harness therefore counts its own destroys. Neither fault is in the tracker.
- The `activate` and `deactivate` signals are emitted from the video tick (assumption F), which does not run here. The
  harness calls `obs_source_inc_active()` and `obs_source_dec_active()`, which change `obs_source_active()`, and emits the
  signal by hand.
- `obs_load_source()` is used to reuse a UUID. It is test-only and is not in the plugin.

## Deviations from the contract

1. **Contexts are keyed by source pointer, not UUID.** A UUID can be reused while the old source is still waiting for its
   `source_destroy`. A scene collection switch can do this. The old source's UUID is removed from OBS's registry when it is
   released, but its `source_destroy` runs later on the destruction thread. A new source can take the UUID in between. With
   a UUID key the new source would not be connected, and the old source's removal would erase the new source's entry. The
   registry is keyed by the source pointer instead. That pointer is unique among live contexts, because a context is
   removed before OBS frees the source. The `collision` scenario covers this path.
2. **The UI keys entries by UUID and records the owning context's serial.** Serials are never reused. A task from an older
   context, such as its hook, liveness or removal event, is ignored when the entry belongs to a newer serial. When a new
   context claims a UUID, the old owner's state is removed from `GameState` first, because `GameState` is keyed by UUID too.
3. **Connecting happens after `obs_enum_sources()` returns, not inside its callback.** The callback keeps a weak reference
   to each capture source. The brief says to connect inside `enum_proc`. Connecting there would hold OBS's `sources_mutex`
   while taking a per-signal mutex. Another listener on that signal that takes `sources_mutex` would then deadlock against
   the enumeration. The weak references are converted and connected after the callback returns.
4. **No `std::shared_ptr<Impl>`.** The header fixes `std::unique_ptr<Impl>`. Each queued task captures a `weak_ptr` to an
   `Anchor` that `Impl` owns. `~Impl` clears the anchor's pointer on the UI thread, where tasks run, so a task that runs
   after the tracker is destroyed finds nothing to touch. The guarantee is the same. Contexts hold only the `weak_ptr` to the
   anchor, not a back-pointer to `Impl`.
5. **`start()` clears `GameState` first.** `stop()` leaves `GameState` alone, and nothing would remove an entry left from an
   earlier run. This is not in the brief.
6. **A second `start()` applies the new settings and logs a warning.** The contract says to call `start()` once.
7. **`chosen_sources` is compared ASCII-case-insensitively.** OBS UUIDs are lowercase. Hand-edited settings may not be.
8. **`source_rename` looks the source up in the registry first.** Only capture sources post a task. The brief does not say
   how to filter renames.
9. **Readouts are asynchronous.** `apply_settings()` and the frontend events run on the UI thread, as before. A readout
   does not run inline. `get_hooked` may run only on the graphics thread (fix round 1, B1), so a readout is queued, and
   its result is applied when it hands back. Until then GameState holds the previous state. `apply_settings()` therefore
   returns before GameState reflects a new selection. The contract does not say when the state must change, so this is
   recorded here.
10. **Logging.** Errors and warnings only. There are no info-level hook logs.

## Threading assumptions (each checked against the OBS 31.0.0 source)

Source: the OBS 31.0.0 tag, fetched from `raw.githubusercontent.com/obsproject/obs-studio/31.0.0/...` (the `gh` API was not
authorised for this repository). Files: `libobs/callback/signal.c`, `libobs/obs.c`, `libobs/obs-source.c`,
`libobs/obs-internal.h`, `libobs/obs-video.c`, `libobs/obs-weak.c`, `libobs/obs-ref.c`, `frontend/api-interface.cpp`,
`frontend/window-basic-main.cpp`, `frontend/obs-frontend-api.h`, `plugins/win-capture/game-capture.c` and `window-capture.c`.
The local libobs is 30.0.2, and the harness ran against it. Every API and behaviour below was checked in the 31.0.0 source.
The 30.0.2 headers have the same signatures. Line numbers are those of the 31.0.0 copies.

- **A. A disconnect waits for in-flight callbacks.** `signal_handler_signal()` holds the per-signal recursive mutex for
  the whole callback loop (`signal.c` 298-321). `signal_handler_disconnect()` takes the same mutex (251-277). So once a
  disconnect returns, no callback for that signal is running. A callback that disconnects its own signal only marks the
  slot for removal. We do not do that.
- **B. `source_destroy` runs on the destruction thread, before the source's own destroy.**
  `obs_source_destroy_defer()` (`obs-source.c` 625) waits for the source's context, emits `source_destroy` (633), and only
  then calls `info->destroy` (636). It frees the signal handler in `obs_context_data_free()` (695). So the source and its
  handler are valid in our callback. For game capture, `game_capture_destroy()` (387-390) emits `unhooked` through
  `stop_capture()` (362). That happens after our per-source disconnect, so it never reaches us. Window capture's destroy
  path emits nothing (`wc_destroy()`, 373-383).
- **C. A last release is synchronous, except for the destroy signal.** `obs_source_release()` (`obs-source.c` 713-727)
  calls `obs_source_destroy()` on the releasing thread (575-623). That removes the UUID from the registry under
  `sources_mutex`. `obs_context_data_remove_uuid()` locks `context->mutex`, which `obs_context_data_insert_uuid()` set to
  `&obs->data.sources_mutex` (`obs-source.c` 238). The destroy is then queued on the destruction thread. So no release may
  run under our mutex. This is the reason for the rule in the file header.
- **D. `obs_weak_source_get_source()` takes a strong reference only while the count is nonzero.** It is a compare-and-swap
  loop (`obs-source.c` 766-772, `obs_weak_ref_get_ref()` in `obs-internal.h` 617). Calling it under our mutex cannot
  deadlock with an OBS lock. `disconnect_all()` uses it to get the strong reference it holds across a disconnect (fix
  round 1).
- **E. `obs_enum_sources()` holds `sources_mutex` while it calls us.** It passes a borrowed pointer, with an extra reference
  held only for the call (`obs.c` 1766-1790). Inside the callback we only take a weak reference. Connecting happens after
  the call returns (deviation 3).
- **F. `activate` and `deactivate` come from the video tick, and only on transitions.** They are emitted from
  `obs_source_video_tick()` (`obs-source.c` 1213-1277; `activate_source()` at 1263). Emission compares the activation count
  with a cached flag (`source->active`). `obs_source_inc_active()` does not emit them, and `obs_source_active()` reads the
  count directly. This is the basis for the startup read, and it has a known gap (below).
- **G. `source_create` is emitted on the creating thread, after the source is set up.** It is emitted in
  `obs_source_create_internal()` (`obs-source.c` 321, emission at 382). Private sources do not emit it. So the signals and
  the proc handler exist when we connect.
- **H. The `source_rename` calldata keys are `"source"`, `"new_name"` and `"prev_name"`.** `obs_source_set_name()`
  (`obs-source.c` 4105) emits it on the renaming thread (4125).
- **I. `get_hooked` returns `hooked` and strings.** The strings are empty when nothing is hooked (`game-capture.c`
  514-531, `window-capture.c` 235-260).
- **J. Graphics thread for `hooked` and `unhooked`.** Every emission is in a tick path, or in a call that the tick makes.
  - `hooked`: `game_capture_tick()` (`game-capture.c` 1776; the tick is registered at 2335), and `wc_tick()`
    (`window-capture.c` 723 and 761).
  - `unhooked`: `stop_capture()` (`game-capture.c` 362), reached from `game_capture_tick()`, from `try_hook()` (1188), and
    from `game_capture_update()` (570). `wc_tick()` emits it at 605 and 668. `wc_hide()` emits it at 584, and the tick
    reaches it through `hide_source()` (`obs-source.c` 1048, called at 1242).
  - `force_reset()` (`window-capture.c` 385-393) emits nothing.
  - `game_capture_update()` and `wc_update()` run on the graphics thread, because `obs_source_update()` only defers video
    sources to the tick (assumption M).
  - The one emission outside the tick is `game_capture_destroy()` (B), which we never see.
- **K. Our side.** Every callback copies what it needs and posts with `run_on_ui()`. A post from a non-UI thread is queued.
  A post from the UI thread runs inline (WP1.6, `ui-thread.hpp`). The only callbacks that can run on the UI thread are
  `source_create` and `source_rename`, when the UI thread creates or renames a source. Their post then runs inline, and
  that only queues a graphics readout or a rename. So `on_changed` does not run inside an OBS signal. It runs from queued
  UI tasks and from `start()`. Frontend events do not notify in this design, because the only notifying path there is
  `SCENE_COLLECTION_CHANGING`, which OBS 31.0.0 never emits. `on_changed` must still not disconnect this tracker's signals,
  and it must not destroy the sources the tracker is working on.
- **L. Ordering.** `on_source_destroy()` disconnects the per-source signals first. That waits for in-flight callbacks, so
  their events are already posted, and only then does it post the removal. Tasks from one thread keep their order. A
  readout that hands back after the removal finds its serial gone from the registry and is dropped (`apply_readout()`).
- **M. Settings updates reach the tick.** `obs_source_update()` (`obs-source.c` 922-936) increments `defer_update_count`
  for video sources (932). `obs_source_video_tick()` then calls `obs_source_deferred_update()` (1229), which calls
  `info.update` (912-920). So `game_capture_update()` and `wc_update()` run on the graphics thread (J).
- **N. `get_hooked` only on the graphics thread (our rule).** The proc reads buffers that win-capture reallocates on the
  graphics thread. `read_hook_state()` is called only from `graphics_readout()` (fix round 1, B1).
- **O. Frontend dispatch.** `obs_frontend_remove_event_callback()` erases by index (`api-interface.cpp` 372-379).
  `on_event()` dispatches by index, in reverse, and drops every event except CLEANUP and EXIT while `disableSaving` is
  nonzero (618-628). OBS releases its frontend API at EXIT (`window-basic-main.cpp` 4887-4891).
- **P. Task queue.** `obs_queue_task(OBS_TASK_GRAPHICS, …, false)` pushes under `video->task_mutex` (`obs.c`, near 3156).
  The graphics thread runs its tasks under the same mutex (`obs-video.c` 963-970), so a queue call can wait for a running
  graphics task. If the caller is already on the graphics thread, the task runs inline (`obs_queue_task()`, `obs.c`).
- **Q. `obs_wait_for_destroy_queue()`.** It returns false at once unless the video and audio threads are initialised
  (`obs.c` 3175-3180). Otherwise it waits for the graphics and audio queues, and then for the destruction queue
  (3181-3192).

## Known gaps and risks

1. **The startup liveness read can be wrong for about one frame.** The graphics readout reads `obs_source_active()`, which
   is the activation count. `activate` is emitted from the next video tick (assumption F). If the count changes twice
   within one frame, the tick sees no transition and sends no signal. The tracker would then report the source live until
   its next transition. This needs two changes inside one frame, so it is unlikely. Not reproduced, because the harness has
   no video tick.
2. **A context can be kept at unload.** A source that is at zero references when `stop()` runs keeps its tracking record.
   Its `source_destroy` is disconnected, and its handler may already be freed, so the context cannot be freed safely. The
   tracker logs a warning. The `leak` scenario reproduces this, and LeakSanitizer reports exactly that record. `stop()` now
   waits for OBS's destroy queue first (fix round 1, S1). So the record is kept only for a source released during `stop()`,
   or when OBS's video and audio threads are not running and the wait returns at once. The harness cannot exercise the
   wait itself, because its video and audio threads are not initialised.
3. **Two live sources with the same UUID.** Deviation 2 keeps only the most recently synced one. This needs OBS to hand out
   a duplicate UUID, which it should not do. Not tested.
4. **The frontend callback stays registered for the life of the process** (fix round 1, S2). It does nothing once the
   tracker has stopped. Its function lives in the plugin's module. OBS releases its frontend callback list at EXIT
   (`window-basic-main.cpp` 4887-4891). I did not verify that OBS unloads plugin modules only after EXIT. The ★ review
   should check that order.
5. **A hooked `window_capture` that is not live loses to a live game** (S5, known gap). The tracker reports a
   `window_capture` as hooked, whether or not it is live. `GameState` ranks a live source above a hooked source that is not
   live (the `basic` scenario checks this). So while a `game_capture` is live, a `window_capture` that is hooked but not in
   the Program output does not become the current game, even if it was hooked more recently. This is `GameState`'s rule,
   which the tracker feeds. It is recorded here for the ★ review. Not changed.
6. **OBS 31.0.0 never emits `SCENE_COLLECTION_CHANGING`** (S4 finding). I found no emission in `window-basic-main.cpp` or
   `api-interface.cpp`. The only other reference in the frontend headers is the enum declaration (`obs-frontend-api.h`
   58). So the tracker's clear on that event never runs. Source removal still cleans up through `source_destroy`.
   `SCENE_COLLECTION_CLEANUP` is emitted by `ClearSceneData()` (`window-basic-main.cpp` 4717). It passes the `disableSaving`
   filter, so it is a reliable alternative. Not changed in fix round 1. The orchestrator decides.
7. **Frontend events are suppressed while `disableSaving` is set** (S4). `on_event()` drops every event except CLEANUP and
   EXIT while the counter is nonzero (`api-interface.cpp` 618-628). The counter is raised by `CreateDefaultScene()` (984),
   `Load()` (1127), `ClearSceneData()` (4675) and exit (4881). `SCENE_COLLECTION_CHANGED` is emitted at 2201, just after the
   counter is lowered at 2198. So it reaches the tracker if the counter is at its baseline on entry. I did not trace that
   baseline. `FINISHED_LOADING` is emitted from `OnFirstLoad()` (2443). Correctness does not rely on either event, because
   `source_create` and `source_destroy` do the real work. The two events are a backstop for sources that exist before
   `start()`.
8. **Residual risks of the readout hand-back** (fix round 1, B1). (a) After `ui_tasks_shutdown()`, `run_on_ui()` drops a
   hand-back task, and its strong reference is never released. This happens only during unload. (b) A `std::bad_alloc` on
   the graphics thread leaks that reference, rather than releasing it on the graphics thread. (c) A readout task that OBS
   never runs leaks its small request and its weak reference at exit.
9. **A TSan report in the stop-during-stress scenario** is analysed in Fix round 1. It is probably a false positive, but it
   is not proven.
10. **Not verified on Windows.** win-capture is not available on Linux, so the harness uses a fake source with the same
    signals and proc. No MSVC build was run. The real `window_capture` and `game_capture` behaviour has only been read from
    the source.
11. **Exceptions.** `std::bad_alloc` can escape `register_source` and `claim`. Callbacks and tasks catch it. The
    `SourceTracker` public functions that allocate (`tracked_sources`) let it escape to the caller, as the other WPs do.

## Questions for the orchestrator

1. Deviations 1 to 10: accepted by the coordinator in the fix round 1 instructions.
2. Gap 2, the context kept at unload: answered by the coordinator. `stop()` now waits for the destroy queue (S1), and the
   record is kept only as a fallback.
3. Should `plugin-main.cpp` call `stop()` before the frontend is torn down? The frontend callback is never removed, so
   `stop()` needs no frontend call (S2). Calling `stop()` early is still safe. It is idempotent, and the callback goes inert.
4. Approved list: the coordinator approved `obs_wait_for_destroy_queue()` in `stop()` and
   `obs_queue_task(OBS_TASK_GRAPHICS, …, false)` for readouts. `obs_load_source()` is test code only.
5. New: should the scene-collection clear use `SCENE_COLLECTION_CLEANUP`, in place of `SCENE_COLLECTION_CHANGING` (gap 6)?
6. New: can someone confirm that plugin modules unload only after the frontend EXIT (gap 4)?

## Fix round 1

Requested by the coordinator: B1, S1, S2, S3, S4 and S5, the nits J, B and K, then the test runs. Everything below was done
in the same worktree, and nothing is committed.

### Code changes (`src/obs/source-tracker.cpp`)

- **B1. `get_hooked` only on the graphics thread.** `request_readout()` runs on the UI thread. It takes a request that holds
  its own weak reference, and queues `graphics_readout()` with `obs_queue_task(OBS_TASK_GRAPHICS, …, false)`. That task
  calls `read_hook_state()` and `obs_source_active()`. It then hands the plain values and a strong reference to the UI
  thread with `run_on_ui()`. `hand_back_readout()` applies the result on the UI thread and releases the strong reference
  there. The graphics thread releases only the request's weak reference, which does not take OBS's sources mutex. No
  UI-thread path calls `get_hooked` any more.
- **S1. Wait before disconnecting.** `stop()` sets `running = false`, then calls `obs_wait_for_destroy_queue()`, then
  disconnects the three global signals. The fallback that keeps a record whose source is at zero references is unchanged.
- **S2. The frontend callback is never removed.** `obs_frontend_remove_event_callback()` is never called. The callback is
  registered once. Its private data is a `FrontendGate` that is never freed, and it holds only a `weak_ptr` to the anchor.
  The callback does nothing unless the anchor is alive and the tracker is running. That is the documented inert state.
- **S3. Ordering in `start()` and `stop()`.** `on_changed` is called last in `start()`, after everything is connected.
  `stop()` has no early return, so it drains whatever is still connected. A separate `connected` flag records the connected
  signals. `running` gates the queued tasks, the frontend callback, `register_existing()` and `resync_contexts()`. A
  re-entrant `stop()` from `on_changed` is tested (`reentrant_stop`).
- **Comments.** The comments on the last-release wording, the frontend dispatch, the disconnect path and the stop path now
  match the verified OBS behaviour. Sixteen comment lines that were over 120 columns were rewrapped. No code changed apart
  from B1, S1, S2 and S3.

### Documentation changes

- S4: frontend events suppressed under `disableSaving` (Known gap 7), the `SCENE_COLLECTION_CHANGING` finding (gap 6), and
  the header wording (below).
- S5: the hooked-but-not-live `window_capture` (Known gap 5).
- Nit J: the window-capture emission sites are now given exactly (assumption J).
- Nit B: the destroy-path `unhooked` claim now applies only to game capture. Window capture's destroy emits nothing
  (assumption B).
- Nit K: `activate` and `deactivate` come only from `obs_source_video_tick()`, so they never run on the UI thread
  (assumptions F and K).
- Also corrected: the summary bullet that said `stop()` removes the frontend callback, deviation 9 (readouts are
  asynchronous), the grounds for assumption C, and Questions 2 and 3, which are now answered.

### Checks on the final source

| Check | Command | Result |
|---|---|---|
| Format | `clang-format-19 --dry-run --Werror src/obs/source-tracker.cpp` | Exit 0 |
| Strict GCC | `g++ -std=c++17 -fsyntax-only` with the flags of the first report, `-Werror` | Exit 0 |
| Strict Clang | `clang++ -std=c++17 -fsyntax-only` with the flags of the first report, `-Werror` | Exit 0 |
| CI preset build | `cmake --build --preset ubuntu-ci-x86_64` | Exit 0. 0 lines containing "warning" in the log. |
| Unit suite | `cmake --build build/tests -j4 && ctest --test-dir build/tests --output-on-failure` | 100% passed, 131 of 131 |
| Column limit | awk, tab = 8 columns | 0 lines over 120 columns |

### Sanitizer and harness results (final source)

The harness is built from the final `source-tracker.cpp`, `ui-thread.cpp` and `game-state.cpp` (`build-asan.sh`). Each
ASan and UBSan run is one process. TSan runs are counted per process. `failures` is the harness's own check count.

| Scenario | ASan + UBSan (final source) | TSan (final source) |
|---|---|---|
| `basic` | 0 failures, no report (1 run) | 0 reports (4 runs) |
| `collision` | 0 failures, no report (1 run) | 0 reports (4 runs) |
| `leak` | 0 failures. LeakSanitizer with `detect_leaks=1` reports only the intended record: a 64-byte direct allocation (the `SourceContext`), and two indirect allocations, 56 bytes (the libobs weak object from `obs_context_init_control`) and 37 bytes (the UUID string). 157 bytes in 3 allocations. The first report had 181 bytes in 4. The never-freed `FrontendGate` (S2) now keeps the anchor's 24-byte control block reachable, so LeakSanitizer no longer lists it. | 0 reports (1 run) |
| `stress` | 0 failures, 306 `get_hooked` calls, no report (1 run) | 0 reports (4 runs) |
| `stress_stop` | 0 failures. 0 ASan and 0 UBSan reports in 7 runs (1 sweep, 6 repeats). | 4 of 10 runs report the race (see below). Every run has `failures=0`. |
| `reentrant_stop` (S3) | 0 failures, no report (1 run) | 0 reports (4 runs) |
| `frontend_after_destroy` | 0 failures, no report (1 run) | 0 reports (1 run) |
| `graphics_buffer` | 0 failures. 76 `get_hooked` calls, 0 off the graphics thread. | 0 reports (4 runs), 0 off the graphics thread |
| `graphics_buffer_negative` (control) | 0 failures. 301 calls, 300 off the graphics thread. | 5 reports, as expected from the control |

The `leak` scenario exits with code 1 under LeakSanitizer, because it keeps one record on purpose. `stress_stop` keeps 0 to
1 record per run, and that count does not correlate with the TSan report.

Scenario notes:

- `stress_stop` runs four worker threads, each creating, hooking, changing liveness, unhooking and destroying sources,
  while the main thread calls `stop()` partway through. `reentrant_stop` calls `stop()` from inside `on_changed`, which
  runs inside `start()`. `frontend_after_destroy` destroys the tracker, then sends frontend events, which the callback
  must ignore without touching freed memory. `graphics_buffer` has a fake graphics thread reallocate the fake title
  string while the UI thread queues readouts. Every `get_hooked` call must happen on the graphics thread.
- The harness emulates OBS's graphics thread. It interposes `obs_queue_task()`. `OBS_TASK_GRAPHICS` tasks run on an
  emulated graphics thread (`graphics_loop`), and `OBS_TASK_UI` tasks run on the main thread's drain. OBS's real video
  thread, its tick and its task mutex do not run. So the TSan buffer scenario tests the discipline of our code, that every
  `get_hooked` call happens on the emulated graphics thread (`off_graphics=0`, 76 calls). It does not test OBS's real
  thread. The negative control calls the fake `get_hooked` from a worker thread: 300 of 301 calls are off the graphics
  thread, and TSan reports 5 races. So the detector does see such a read.
- `obs_wait_for_destroy_queue()` returns at once in the harness, because its video and audio threads are not initialised.
  The wait is therefore not exercised. The S1 wait is verified only by reading the OBS source (assumption Q).

### TSan report in `stress_stop`

The report is a write by OBS's destruction thread (`pthread_mutex_destroy`, inside libobs's signal-handler teardown). It
conflicts with an earlier lock by the UI thread, in `signal_handler_disconnect()`, called from `SourceTracker::stop()` at
`source-tracker.cpp` 906 (`impl.disconnect_all()`). All reported runs have this shape. `stress`, which never stops during
destruction, reported nothing in its 4 runs.

Counts on the final source: 4 of 10 `stress_stop` TSan runs reported it. The three sweep rounds gave 0 of 3, the six
repeats gave 3 of 6, and the final TSan pass gave 1 of 1. Earlier runs of the same code (before the comment-only edits)
gave 2 of 4. The ASan runs of the same scenario, 7 in total, had no error.

Why it is most likely a false positive:

- `disconnect_all()` holds a strong reference to each source across its disconnect. It gets the reference with
  `obs_weak_source_get_source()`, and releases it only after `signal_handler_disconnect()` has returned.
- The handler is freed only on the destruction thread, in `obs_source_destroy_defer()` (`obs-source.c` 625-695). That
  runs only after the last strong reference is released (`obs_source_release()`, 713-727, which queues the destroy). So
  the free cannot happen while `disconnect_all()` holds its reference.
- That ordering goes through libobs's reference-count atomics. libobs is not built with TSan, so TSan cannot see the
  happens-before edge from our release to the destruction thread's free. TSan therefore reports the two accesses as
  unordered.
- The report's order matches this: the lock is recorded first, and the destroy is the later access. If the free had come
  first, the lock would touch freed memory, and ASan would report a heap-use-after-free on the same allocation. None of the
  7 ASan runs did.

This is an inference from the code and the ASan runs. I have not built libobs with TSan, so it is not proven. The
options are a TSan suppression for this libobs teardown path, or a libobs build with TSan. The ★ review should decide
which. The harness cannot measure how often the window occurs in real OBS, because the wait returns at once there.

### Proposed header wording (for the orchestrator)

The header's "Threading" paragraph is correct about which thread emits each signal. It needs the readout hand-back, the
frontend-callback behaviour, and the frontend events. This paragraph is written to replace it:

```
// Threading: hooked, unhooked, activate and deactivate arrive on the graphics thread. win-capture emits hooked and
// unhooked from its video tick, and obs_source_update() defers video sources to that tick, so settings changes reach
// them there too. activate and deactivate come from obs_source_video_tick(). source_destroy arrives on OBS's
// destruction thread, before the source's own destroy. source_create and source_rename arrive on the thread that
// creates or renames the source. Callbacks copy what they need by value and post to the UI thread with run_on_ui().
// GameState is only touched on the UI thread. get_hooked may run only on the graphics thread, so the UI thread queues
// an OBS_TASK_GRAPHICS task to read a source's state. That task hands the values back with a strong reference, which
// the UI thread releases. Otherwise only obs_weak_source_t references are held. stop() disconnects every signal it
// connected. signal_handler_disconnect() waits for an in-flight callback of that signal, so per-source data may be freed
// after it. The frontend callback is never removed, because OBS dispatches callbacks by index: it stays registered and
// does nothing after stop(). OBS does not deliver frontend events while its disableSaving counter is set, so
// SCENE_COLLECTION_CHANGED and FINISHED_LOADING are a backstop; source_create and source_destroy carry the real work.
// OBS 31.0.0 never emits SCENE_COLLECTION_CHANGING.
```

The frontend-events bullet above the paragraph should also say that `SCENE_COLLECTION_CHANGING` is not emitted by OBS
31.0.0. If the orchestrator adopts `SCENE_COLLECTION_CLEANUP` (Question 5), that bullet should name it too.

### Residual risks for the readout hand-back (B1)

- After `ui_tasks_shutdown()`, `run_on_ui()` drops a hand-back task, and its strong reference is never released. This
  happens only during unload.
- A `std::bad_alloc` on the graphics thread leaks the reference. It is not released on the graphics thread.
- A readout task that OBS never runs leaks its small request and its weak reference at exit.

### Findings for the coordinator

- `SCENE_COLLECTION_CHANGING` is declared in `obs-frontend-api.h` (58) but never emitted in the OBS 31.0.0 frontend files
  I checked. The tracker's clear on that event never runs. Source removal still cleans up.
- `SCENE_COLLECTION_CLEANUP` is emitted by `ClearSceneData()` and passes the `disableSaving` filter. It is a reliable
  alternative for the clear.
- The S1 wait cannot be exercised in the harness. It is verified by reading `obs.c` 3175-3192 only.
- The thread facts in the header are correct as written, after the defer clarification above.
- Two things need a decision from the orchestrator or the ★ reviewer: the module unload order against frontend EXIT (gap
  4), and the TSan report (Fix round 1).

Nothing is committed or pushed. Both files are untracked in the worktree: `src/obs/source-tracker.cpp` and
`docs/handover/WP1.7.md`.

## Orchestrator integration notes

- ★ review: the Reviewer verified assumptions A–L against OBS 31.0.0 and accepted deviations 1–10. It found B1 (a use-after-free: `get_hooked` read on the UI thread), S1 (a leak at stop), S2 (callback removal during the EXIT dispatch) and S3 (re-entrant stop during start). Fix round 1 resolved all four. The orchestrator reviewed the graphics-readout code: the readout and the hook signals both originate on the graphics thread, so the UI thread receives them in order, and the strong reference is released on the UI thread.
- **Correction to "Findings 1" above:** OBS 31.0.0 **does** emit `OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING`, in `UI/window-basic-main-scene-collections.cpp` lines 82, 300, 552 and 685 (that file wasn't searched). Handling it is correct; no change.
- Question 6: verified. EXIT is emitted from `OBSBasic::closeEvent` (window-basic-main.cpp ~4887), and modules are unloaded later by `obs_shutdown()` from `~OBSApp` (obs-app.cpp ~1013).
- **TSan stress_stop report:** accepted as a probable false positive and documented. It is in libobs's own signal-handler mutex teardown, ordered through libobs refcount atomics that TSan can't see because libobs isn't built with TSan, and 7 ASan runs found no use-after-free. On the list for the Windows manual test: start and stop OBS repeatedly with capture sources present.
- `source-tracker.hpp`'s threading comment was updated by the orchestrator from the worker's wording.
- Gates in the main checkout: check-format clean, Ubuntu CI-preset build OK, and the plugin now links with no unresolved clip_atlas symbols (checked with nm).
