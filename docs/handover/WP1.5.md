# WP1.5 hand-over: core/file-mover ★

## Summary

`src/core/file-mover.cpp` implements the contract in `src/core/file-mover.hpp`. The public API is unchanged.

- One worker thread runs jobs FIFO. Callbacks run on that thread, with no lock held.
- `move()`: a rename on the same volume. Across volumes it copies to `<name>.clip-atlas-partial`, checks the size, renames the partial to its final name, and removes the source last. The source is removed only after that verified copy exists.
- Names come from `clip_atlas::unique_destination`. Locked or failing steps retry with doubling backoff until `retry_budget` is spent.
- `wait_stable()`: waits until a file exists with unchanged size and mtime for `stable_for`, or until the timeout.
- `shutdown(true)` drains the queue. `shutdown(false)` interrupts the running job at its next safe point, cancels the queue, and joins. Every job's callback runs exactly once.

## Files changed

| File | Change |
|---|---|
| `src/core/file-mover.cpp` | New. Implementation. |
| `tests/test-file-mover.cpp` | New. 22 Catch2 cases tagged `[file-mover]`. |
| `src/core/file-mover.hpp` | One private member added: `std::mutex join_mutex_;` (see Deviations). Nothing else changed. |
| `docs/handover/WP1.5.md` | This note. |

No other tracked files were touched; the build directories are git-ignored. Nothing was committed or pushed.

## Testing

Commands run from the worktree root.

- `cmake -S tests -B build/tests && cmake --build build/tests -j4 && ctest --test-dir build/tests --output-on-failure`
  - Result: 33 of 33 pass (11 clip-layout, 22 file-mover), about 1.4 s.
- Loop: `for i in $(seq 20); do ./build/tests/clip_atlas_tests "[file-mover]" || break; done`, run as a script.
  - Result: 20 of 20 pass. Repeated after each change; the last run is on the final sources.
- ThreadSanitizer: `cmake -S tests -B build/tsan -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" && cmake --build build/tsan -j4 && ./build/tsan/clip_atlas_tests "[file-mover]"`
  - Result: 41 runs across the work, all with zero TSan reports. The last 8 were on the final sources. TSan reported nothing. The two hazards it would catch (two threads joining the same `std::thread`, and a callback joining its own thread) were handled by design, which is why `join_mutex_` and the self-join guard exist.
- AddressSanitizer and UBSan (extra, built by hand with g++ `-fsanitize=address,undefined`): 22 cases, 249 assertions pass, no reports.
- `cmake --preset ubuntu-ci-x86_64 && cmake --build --preset ubuntu-ci-x86_64`: configures and builds with warnings as errors. `file-mover.cpp` is compiled into `clip-atlas.so`.
- `clang-format-19 --dry-run --Werror` on `src/core/file-mover.cpp`, `tests/test-file-mover.cpp` and `src/core/file-mover.hpp`: clean.
- Syntax checks with `-Wall -Wextra -Wpedantic -Werror` on g++ 13 and clang 18, for both the implementation and the tests: clean. Also clean with `-Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Werror` on g++.
- Durations: every millisecond-to-clock conversion is an explicit `duration_cast<Clock::duration>` applied to a value already clamped to 24 h. Nothing narrows. MSVC itself was not run (see the Windows gap below).
- Threads: `tests/CMakeLists.txt` was not edited. On this container (glibc 2.39) the tests link without `Threads::Threads`. A glibc older than 2.34 would need `find_package(Threads)` and `Threads::Threads` on the test target. Windows needs nothing extra.

What the tests cover:

- Moves: a nested folder is created; name collisions get `name (2).ext` and the existing file is untouched; the callback runs on the worker; the UTF-8 name `ポケモン 2026.mkv` survives.
- Missing source: fails at once, zero rename calls.
- Locked source: succeeds after three refusals; stays locked, so it fails after the budget with the source untouched.
- Cross-volume (injected `cross_device_link`): verified copy, source removed, no partial left. A foreign file already named `<x>.clip-atlas-partial` is kept and the move picks `x (2).mkv`.
- Size mismatch: fails, partial removed, source untouched.
- Source removal failing forever: `copied_source_kept`, both files present.
- Uncreatable destination: fails after the budget, source untouched.
- `wait_stable`: a file that grows then settles is `stable`; a file that never appears is `timed_out`; a file created mid-wait is `stable`; a huge timeout and a huge stable window are clamped (the second must time out, not overflow).
- Shutdown: `shutdown(false)` cancels a running wait (it returns in well under its 60 s timeout) and the queued jobs; `shutdown(false)` ends a retrying move (under a `milliseconds::max()` budget, which must not overflow); `shutdown(true)` runs queued moves in FIFO order; a move after shutdown is cancelled on the calling thread; a repeated `shutdown` is harmless.
- Callbacks: a callback can queue more work; a throwing callback does not stop later jobs.
- `default_file_ops`: refuses to overwrite in `rename` and `copy_file`; missing files give `nullopt`; the default constructor works.

Timing: every test uses a 5–20 ms backoff, a 10 ms poll and a 150–300 ms budget. Assertions use bounds with slack. Tests wait on futures or condition variables, not on fixed sleeps for their outcome. The only sleeps drive a scenario (a writer, or a file that appears late); they do not decide any outcome. Every fstream is closed before a move, so this is Windows-safe in design.

## Deviations from the contract

1. **Private member added: `std::mutex join_mutex_`.** Two threads calling `shutdown()` at once would join the same `std::thread`, which is a data race. `mutex_` cannot be held across `join()` because the worker needs it to finish, so a second mutex is needed. This is the only header change.
2. **Self-join detection uses a `thread_local` pointer, not `worker_.get_id()`.** Comparing with `worker_.get_id()` races with `join()` on another thread. The pointer is set only on the worker.
3. **Copy-path naming (contract 3f).** `unique_destination` is given a predicate that requires both the final name and its `.clip-atlas-partial` name to be free. The rename path uses the exact `exists()` predicate from 3c, as written.
4. **Duration clamps.** `retry_budget`, wait timeouts and `stable_for` are clamped to [0, 24 h]. `initial_backoff`, `max_backoff` and `stable_poll_interval` are clamped to [1 ms, 24 h]. Without this, `clock + duration` and the stable-window comparison can overflow, which is undefined behaviour. The contract sets no limits.
5. **Cleanup of partial files on shutdown.** Partial removal retries on the job's backoff. If `shutdown(false)` interrupts that wait, it makes one final attempt. If removal still fails, the error text says a partial copy was left in the destination folder. This is the best that can be done without ignoring shutdown.
6. **Source removal counts "no such file" as success.** If `remove()` reports `no_such_file_or_directory`, the source is already gone and the job is `moved`.
7. **Extra terminal failure.** A source path with no file name (for example `C:\`) fails with "source has no file name".
8. **Shutdown during source removal.** If `shutdown(false)` interrupts the removal retries, the status is `copied_source_kept` and the error begins "shutting down;".
9. **Exceptions from FileOps.** The default ops never throw. A custom FileOps that throws makes a move `failed` with "unexpected error", and a wait `timed_out`. The callback still runs once.
10. **Callback exceptions are dropped silently.** Core cannot log (no OBS headers), so `deliver()` swallows the exception. The plugin's callbacks must catch and log their own errors.
11. **A partial name that already exists is never removed.** If `copy_file` fails with `file_exists`, the job retries without removing the file, since it is not ours (3f).

## Known gaps and risks

- **Moving a file into the folder it is already in** renames it to `name (2).ext`. The source's own name counts as taken. No data is lost, but the result is surprising. See question 1.
- **Verification is size-only**, as the contract says. A same-size rewrite during a copy goes unnoticed. The caller's `wait_stable` is the protection.
- **`exists()` returns false on any stat error.** A source that cannot be stat-ed is reported as `source missing` at once, with no retry.
- **Crash leftovers.** A crash mid-copy leaves `<name>.clip-atlas-partial` in the destination. Nothing sweeps it up. See question 3.
- **Directories are not rejected.** A same-volume directory source is renamed as a whole. A cross-volume one fails copy, is retried until the budget is spent, and then fails. That is slow but safe.
- **`shutdown(true)` can block** for up to the running job's full budget (30 s by default) or a `wait_stable` timeout. `shutdown(false)` returns promptly.
- **Jobs queued from a callback during `shutdown(true)` are cancelled**, because `move()` and `wait_stable()` reject once shutdown has begun. Queued work already in the queue still runs.
- **Self-shutdown.** Calling `shutdown()` from a callback returns without joining, so callbacks can still run after it returns. Destroying the mover from inside a callback terminates the process, because the joinable thread is destroyed.
- **Construction can throw.** `std::system_error` is thrown if the worker thread cannot start. The caller must catch it.
- **Not run on Windows.** Only Linux was used here. The MSVC build, the Windows test run, the UTF-8 test on Windows, and the `ERROR_NOT_SAME_DEVICE` to `cross_device_link` mapping are all unverified. The locked-file path is tested only with the fake.
- **Timing-dependent tests.** They use generous bounds. Under very heavy CI load a bound could still be hit; the bounds are 5 s or more over the expected time.

## Questions for the orchestrator

1. **Same-folder moves.** Keep the contract behaviour (`name (2).ext`), or treat a source already in its destination folder as `moved` in place? WP1.8 could hit this if OBS saves replays straight into the sorted folder.
2. **Callbacks during a drain.** Should `shutdown(true)` still accept follow-up jobs queued from callbacks? Right now they are cancelled.
3. **Crash leftovers.** Should WP1.8 sweep `*.clip-atlas-partial` on startup, or leave them for the user to see?
4. **Exception split.** Confirm that the plugin's callback wrappers catch and log, since core drops exceptions silently.
5. **Header change.** Is the private `join_mutex_` acceptable? It is the only change to `file-mover.hpp`.

## Fix round 1

Scope: the reviewer's blocking bug B-1, the data-safety items B-2a, S-1 to S-5 and S-7, and the four surviving mutants. Changed files: `src/core/file-mover.cpp`, `tests/test-file-mover.cpp`, this note. `src/core/file-mover.hpp` is unchanged in this round; the private `join_mutex_` from round 0 is the only header change. Nothing was committed.

Where this section and the sections above disagree, this section wins. See "Corrections" below.

### Changes

- **B-1 (blocking): the only copy is never deleted.** A `PartialGuard` owns the partial from before `copy_file` until the partial is published. Its destructor removes the partial on any exit, including an exception, unless it has been disarmed. It is disarmed when the partial is published and when the source is gone. `cleanup()` also refuses to remove a partial while the source does not exist. If the source vanishes during the copy or before publish, the job ends `failed`, with `final_path` set to the partial's path and the error `source disappeared during the copy; partial copy kept at <name>`. If no partial exists, the error says so and `final_path` stays empty.
- **B-2a and S-5: existence and replacement.** `DefaultFileOps::exists()` uses `symlink_status`. A path counts as absent only when the status is not-found with `ENOENT`. Any other error, and any entry of any type (including a dangling symlink), counts as present. `rename()` refuses any entry at the target. A dangling symlink at the target name is never replaced (probe M). A source with a transient stat error is no longer reported as `source missing`; the rename fails and is retried.
- **S-1: the source changed.** The source's size and mtime are recorded before `copy_file`. After the copy, a changed stamp means `source changed during the copy`. The partial is removed (the source is still there) and the job retries. Immediately before the delete, a changed stamp ends the job `copied_source_kept` with `source changed during the move`. Both the source and the published copy stay.
- **S-2: budgets.** `retry_budget` now applies only to lock and transient retries of the main attempt. Partial cleanup and source removal each get their own budget of `min(retry_budget, 5 s)`, counted from when that step starts. A step that shutdown(false) interrupts makes one immediate final attempt with no sleeping. Otherwise it gives up when its budget is spent.
- **S-3: draining abort.** `shutdown(false)` after `shutdown(true)` sets `drain_ = false` under the lock and wakes the worker, so the drain stops.
- **S-4: a published copy survives exceptions.** `perform_move` writes into the caller's `MoveResult`. If an exception escapes after the copy was published, the callback gets `copied_source_kept` with `unexpected error after the copy was published`. Otherwise it gets `failed` with `unexpected error`.
- **Same folder (Q1).** If the source's parent and the destination are the same directory (`fs::equivalent`, no error), nothing happens on disk and the job ends `moved` with `final_path` = source. This check reads the disk directly and bypasses `FileOps`; the code says so in a comment.
- **Non-regular sources (item 8).** A source known to be something other than a regular file (a directory, for example) fails at once with `source is not a file`. The check uses `symlink_status`, so symlinks are refused too. It bypasses `FileOps` (read-only, commented). An error is not treated as proof either way; the rename decides those cases.
- **Shared backoff.** `Backoff` takes an explicit budget, so the main schedule and the step schedules use one implementation.

Behaviour changes to know about: a symlink given as the source is refused with `source is not a file`; a dangling symlink at a target name counts as taken.

### Tests

The file-mover suite grew from 22 to 39 cases. Seventeen cases are new:

- `wait_stable` never reports before the full window has passed since the last change. The early bound is checked with 25 ms of timer allowance. A window cut in half reports about 150 ms after the change and fails.
- A change of mtime alone (size constant) restarts the stability window. This is probe K.
- A zero retry budget gives exactly one attempt. This is probe J.
- A source that vanishes during the copy leaves the partial as the only copy. This is probe A.
- A writer that appends after verification keeps the source and the published copy. The append is made by a FileOps hook at the publish rename, which is after verification and before the pre-delete check. Probe B as written appends inside `remove()`; see gap 1.
- A source that changes while it is copied is copied again.
- M11: a copy that fails after writing part of the partial removes it, and a later attempt succeeds under the plain name.
- M12: a failed publish removes the partial and leaves the source in place.
- Probe F: an exception during publish leaves no partial and the callback runs once.
- Probe C: a slow copy does not use up the time the source removal needs.
- Probe D: a partial refused twice after a failed copy is still cleaned up, with no orphan.
- Probe E: `shutdown(false)` after `shutdown(true)` aborts the drain promptly.
- Probe G: `shutdown()` called from inside a callback returns.
- Probe I: racing producers and shutdown calls report every job exactly once (4 producers, 100 jobs each).
- A directory as the source is refused at once, with no rename attempted and its contents intact.
- A clip already in the destination folder stays where it is.
- Probe M: a dangling symlink at the target name is never replaced. The test skips where symlinks cannot be created.

Changed: `a locked source is retried and moves once it frees up` now uses a 5 s budget. The test is about retries, not timing.

### Results

- `cmake --build build/tests` and `ctest --test-dir build/tests`: 50 of 50 pass (11 clip-layout, 39 file-mover).
- The file-mover tests, 20 runs in a row: 20 of 20 pass.
- ThreadSanitizer (`build/tsan`, the file-mover tests, 8 runs): zero reports.
- AddressSanitizer and UBSan (hand build of the same sources): 39 cases, 370 assertions, zero reports.
- `cmake --preset ubuntu-ci-x86_64 && cmake --build --preset ubuntu-ci-x86_64`: builds with warnings as errors.
- `clang-format-19 --dry-run --Werror` on `file-mover.cpp`, `file-mover.hpp` and `test-file-mover.cpp`: clean.
- Strict warnings: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Werror` (g++) is clean for the core file. `-Wall -Wextra -Wpedantic -Werror` (clang) is clean for the core file and the test file.
- Surviving mutants, re-expressed on the fixed code and built against the same test file. The mutant sources are in the scratchpad; the worktree was not modified.
  - M2 (mtime ignored): 1 failing case, the mtime test.
  - M5 (window halved): 2 failing cases, the window test and the mtime test.
  - M11 (no cleanup on copy failure): 2 failing cases, the copy-failure test and the refused-twice test.
  - M12 (no cleanup on publish failure): 1 failing case, the publish test.
- The reviewer's probes, built against the fixed sources: A, C, D, E, F, G, J, K, M and I pass. Probe H is informational: `shutdown(false)` during a 400 ms copy took 380 ms, and the move then completed as `moved`. Probe B fails as written; see gap 1.

### Known gaps (updated)

1. **Residual race inside the delete (probe B).** Probe B's writer appends inside `remove()`, after the last check. The appended bytes are lost: the source is deleted, and the copy holds only the original bytes. No check made outside the delete call can see such an append, and `std::filesystem` has no atomic check-and-unlink. The fixed code does detect appends that land before the pre-delete check (the new test). The unprotected window is one `remove()` call. Closing it needs OS-level exclusive access, which is not available here.
2. **S-6: `shutdown(false)` waits for an in-flight filesystem call.** A copy is never abandoned midway, so `shutdown(false)` returns only after the current `copy_file` finishes (probe H: 380 ms for a 400 ms copy). For a large cross-volume copy that can be long, and plugin unload can block for that time. This corrects the earlier claim that `shutdown(false)` "returns promptly". The claim holds only when no filesystem call is running.
3. **B-2b (deferred): replacement race on Windows.** The header says `std::filesystem::rename` replaces its target. `exists()` and `rename()` are two separate calls, so a file created between them is replaced. The remedy is a no-replace publish primitive. The orchestrator deferred it, and no platform code was added in this round.
4. **N-2: destroying the mover from inside its own callback** terminates the process, because the worker thread is still joinable. This is not supported. The scratch probe is `dtor_probe.cpp`.
5. **Empty destination folder.** `move(source, "")` is accepted and moves the clip into the current directory, renamed `(2)` if the name is taken. Nothing rejects it. The scratch probe is `empty_dest_probe.cpp`. The reviewer's ID for this case was not in my brief.
6. **N-1, N-3, N-4, N-6, N-7 and the Windows items of S-8 are not documented here.** The brief names them as known gaps, but their wording was not in the brief, so I have not guessed at them. Please send the reviewer's text and I will record them.
7. **Carried over, unchanged:** verification is size-only, as the contract says; crash leftovers are never swept (Q3); the MSVC build and the Windows test run were not done; callback exceptions are dropped by core (Q4, the plugin wrappers catch and log); a constructor can throw `std::system_error`; a callback queued during `shutdown(true)` is cancelled (Q2, accepted); destroying the mover from a callback is unsupported (gap 4); timing-dependent tests use generous bounds.

### Corrections

- "Moving a file into the folder it is already in" (earlier known gaps): resolved. The move is now a no-op with `final_path` = source.
- "`exists()` returns false on any stat error": no longer true. Errors count as present.
- "Directories are not rejected": no longer true. A directory source fails with `source is not a file`.
- "`shutdown(false)` returns promptly": corrected by gap 2.
- Deviation 5 (partial cleanup on shutdown): superseded. Cleanup and source removal now have their own budgets of `min(retry_budget, 5 s)`.
- Questions for the orchestrator: Q1 is answered by the same-folder change. Q2 is accepted as is. Q3: crash leftovers are never swept. Q4: confirmed. Q5: accepted.

### Questions for the orchestrator

1. Gap 1: accept the residual window inside `remove()`, or look for a platform-specific approach?
2. Gap 6: please send the wording of N-1, N-3, N-4, N-6, N-7 and the Windows items of S-8.

## Orchestrator integration notes

- ★ review: the Reviewer reproduced two data-safety bugs. B-1: a vanished source made the mover delete the only copy. B-2: stat errors and dangling symlinks counted as "free". It also raised several should-fixes. Fix round 1 resolved all of them except B-2b (the no-replace publish primitive), which the orchestrator deferred and logged in plan.md. The reviewer's four surviving mutants now fail tests.
- The orchestrator amended the public comments in `file-mover.hpp` to the corrected contract: `exists()` semantics, the same-folder no-op, the non-regular-file rejection, keeping the partial when the source vanishes, the source re-check, step budgets, and shutdown escalation and latency.
- Answers: (1) Accept the residual window inside `remove()`. No portable check-and-unlink exists, and the plugin also waits for the saved file to be stable before moving it. (2) The reviewer's nits, for the record:
  - N-1: a callback that re-queues on `cancelled` recurses on the caller's thread.
  - N-3: permanent errors such as an empty `dest_dir` are retried for the whole budget.
  - N-4: a foreign file appearing at the partial name between the check and a failed copy could be removed (very unlikely).
  - N-6: an exception in `wait_stable` is reported as `timed_out`.
  - N-7: callback captures are destroyed on the worker thread, so plugin captures must be plain data.
  - S-8 Windows items, to cover in manual testing: `ERROR_NOT_SAME_DEVICE` mapping to `cross_device_link`, MAX_PATH with the partial suffix, NTFS size/mtime staleness while a writer has the file open, FAT 2 s mtime granularity, and delete-pending files.
- Gates in the main checkout: check-format clean, 170/170 tests, the file-mover tests 10/10 in a loop, Ubuntu CI-preset plugin build OK.
