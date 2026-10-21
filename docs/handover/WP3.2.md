# WP3.2 hand-over: platform/thumbnails (Windows Shell thumbnails)

## Summary

- `src/platform/thumbnails-win.cpp` implements `ThumbnailProvider` and `thumbnail_provider()` for Windows. Requests are
  answered on the provider's own TaskQueue thread. That thread starts COM as a single-threaded apartment (CoInitializeEx
  in the queue's start hook, CoUninitialize in its exit hook). The image comes from
  `IShellItemImageFactory::GetImage`, is copied out as a top-down 32 bpp premultiplied BGRA `Thumbnail`, and a bitmap
  with no alpha gets opaque alpha bytes.
- `src/platform/thumbnails-stub.cpp` accepts requests and never calls `done`. It includes no Windows or OBS headers.
- The queue follows `thumbnails.hpp`: newest first, a waiting request for the same path is replaced, at most 256 wait
  (the oldest is dropped beyond that), `cancel_pending()` answers every waiting request with `dropped`, and
  `shutdown()` drops what is queued and waits for the image in progress.
- Two bounds differ from the brief's recommended threading design: serve tasks are posted only when they are needed,
  and dropped answers are batched. Both are explained under Deviations. The guarantees are the same.
- Not verified on Windows: this container has no MSVC and no Wine. What was verified is a MinGW syntax check, the Linux
  build, and a Linux harness that runs the real queue code against fake Win32 headers, including under ThreadSanitizer
  and AddressSanitizer. CI and the manual checklist are still needed.

## Files changed

| File | Change |
|---|---|
| `src/platform/thumbnails-win.cpp` | new |
| `src/platform/thumbnails-stub.cpp` | new |
| `docs/handover/WP3.2.md` | new (this note) |

Nothing else was changed. The files are staged with `git add -A` and not committed.

## How it was tested

Commands were run from the worktree.

1. STEP 0: `git merge --ff-only claude/pensive-hamilton-cbg1l4` fast-forwarded `417ab86..11b9311`. `git log -1 --oneline`
   shows `11b9311 WP3.0: format CMakeLists.txt`.
2. Linux plugin build: `cmake --preset ubuntu-ci-x86_64` configures, with a pre-existing warning that `ENABLE_CCACHE` is
   unused. `cmake --build build_x86_64` exits 0. The build ran before the stub existed (26 steps, no warnings), with the
   stub, and on the final tree. The CI preset makes warnings errors, so exit 0 means no warnings.
3. MinGW syntax check, exactly as the brief gives it:
   `x86_64-w64-mingw32-g++-posix -std=c++17 -fsyntax-only -Wall -Wextra -Werror -DUNICODE -D_UNICODE -I src -I src/core -I src/platform -I src/obs -I /usr/include/obs -I build_x86_64 src/platform/thumbnails-win.cpp`
   exits 0 on the final tree. A stricter pass with `-Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast`
   reports nothing for `thumbnails-win.cpp`. Its four warnings come from OBS headers.
4. Unit tests: `cmake -S tests -B build/tests && cmake --build build/tests && ctest --test-dir build/tests --output-on-failure`
   passes 246 of 246. This WP does not touch `src/core`.
5. Formatting: `clang-format-19 --dry-run --Werror src/platform/thumbnails-win.cpp src/platform/thumbnails-stub.cpp` is
   clean. Version 19 is the one `build-aux/check-format` pins. The container's default `clang-format` (18, the brief's
   literal command) is also clean. The full repo check is in the Format check section below.
6. Harness. It is scratch code outside the repo, in
   `/tmp/claude-0/-home-user-auto-clip-sorter/6c995450-a095-5948-9d9e-9af008631cf2/scratchpad/wp32/harness/`. It builds
   the real `thumbnails-win.cpp`, `task-queue.cpp` and `clip-layout.cpp` on Linux, against small fake headers in
   `fake/` (windows.h, objbase.h, shobjidl.h, obs-module.h). Build line:
   `g++ -std=c++17 -g -O1 -Wall -Wextra -pthread -I<harness>/fake -I<repo>/src -I<repo>/src/core -I<repo>/src/platform harness.cpp <repo>/src/core/task-queue.cpp <repo>/src/core/clip-layout.cpp`,
   with `-fsanitize=thread` or `-fsanitize=address,undefined` for the sanitizer runs.

   The fake counts each COM Release, each DeleteObject, each GetDC/ReleaseDC pair and each CoInitializeEx and
   CoUninitialize call. It also flags a Win32 call whose arguments the real API would reject. Tests use real threads.
   `#define private public` is used around the include of `thumbnails.hpp` only, so the tests can read queue state. The
   harness does not exercise Windows behaviour (Shell, GDI or COM semantics). It exercises the queue, the threading,
   the RAII releases and the pixel conversion.

   - Full suite: 19 test groups, all checks pass: basic, newest_first, replace, overflow, cancel, shutdown_waits,
     shutdown_discards_dropped, reentrancy, done_throws, size_clamp, alpha, size_guards, failure_paths, nul_and_empty,
     com_pairing, singleton, destructor, storm, storm_shutdown. The storm groups were run five times each.
   - Storm: 6000 requests from 4 threads over 40 paths, with random sizes, `cancel_pending()` running in parallel and
     slow fake images. After the queue drains, every request has exactly one callback. The shutdown variant shuts down
     during the storm: no request is answered twice, and no callback runs after `shutdown()` returns. In both variants
     every `done` runs on the provider's thread.
   - ThreadSanitizer, final tree: no reports, all checks pass. An earlier report was in the harness itself (a plain
     fake global read at provider start). It was fixed in the harness, and none involved `thumbnails-win.cpp`.
   - AddressSanitizer and UBSan, final tree: no errors, no leaks, all checks pass.
   - Mutation check: 15 deliberately broken copies of `thumbnails-win.cpp`, generated by `make-mutants.py` and run
     through the full harness by `run-mutants.sh`. A watchdog turns a deadlock into a failed run. Results are in the
     table at the end of this note.

### Invariants: argued and checked

- Each request is in exactly one container at a time: `waiting`, or `dropped`, or taken by a serve or flush task. So
  its `done` runs at most once.
- A replaced request moves from `waiting` to `dropped` under the lock. The next serve or flush task answers it once,
  with `dropped`.
- Overflow moves the oldest entry to `dropped`, inside the same loop that enforces the 256 limit. That is once per entry.
- `cancel_pending()` moves every waiting entry to `dropped` under the lock. Each is answered once.
- `shutdown()` swaps both containers out under the lock and destroys them outside it, without calling `done`.
  `TaskQueue::shutdown()` then drops the queued tasks and waits for the image in progress, whose `done` still runs.
- No `done` runs with the mutex held. The calls are in `serve_one()` and `flush_dropped()`, after their lock blocks.
  `shutdown()` calls none.
- No `done` runs inside `request()`. It only pushes and posts. Dropped answers are delivered by a task on the
  provider's thread.
- Serve tasks are bounded. `serves_pending` counts posted serve tasks that have not started. `request()` posts one only
  when `serves_pending < waiting.size()`. After every locked section `serves_pending >= waiting.size()`, so each waiting
  request has a task that can take it, and `serves_pending <= 256`. The storm test checks both under the lock.

## Deviations from the brief and the contract

1. **Serve tasks are posted only when needed.** The brief said to post one serve task per request. The dock calls
   `request()` on every paint, and with slow thumbnails one task per request would grow the TaskQueue without limit.
   Under the rule above every waiting request still has a task that can take it, so every request is served or dropped.
   The invariant replaces the brief's argument that each request posts one task.
2. **Dropped answers are batched.** The brief said to post one task per dropped request. Instead at most one flush task
   is pending, and each serve task takes and delivers the owed answers before it makes its image. The reason is the same
   unbounded growth. A dropped answer now waits for at most the image in progress, not for the whole serve backlog.
3. **`Impl` owns the request type.** `Impl` nests `Request` and `RequestPtr` (`std::unique_ptr<Request>`), as
   `SoundPlayer::Impl` does. The queue holds unique_ptrs, so a moved-from slot is null: nothing is destroyed under the
   lock, and no caller's captures run there. `answer_dropped` is a static member, because `Impl` is private to
   `ThumbnailProvider` and a free function cannot name it. (The MinGW check caught the first version.)
4. **`<objbase.h>` is included** between `<windows.h>` and `<shobjidl.h>`. It declares `CoInitializeEx`,
   `CoUninitialize` and the `COINIT_*` values (checked in the MinGW headers). The other include-order rules are the
   brief's.
5. **Empty paths and paths with an embedded NUL fail** without calling the Shell. The Shell reads a path only up to its
   first NUL, so an embedded NUL could open a different file. The contract does not mention this.
6. **Image size limits.** Both dimensions must be 1 to 1024. The check uses 64-bit arithmetic, so the sign of a negative
   height cannot overflow. Anything else fails. `request()` clamps `size_px` to 16 to 1024, as specified.
7. **Logging.** Per-file failures are not logged, because a folder with many unthumbnailed files would flood the log.
   WARNINGs are logged for a failed COM start, for an exception from `GetImage` or the pixel copy, and for an exception
   from a `done` callback.

## Known gaps and risks

- **Unverified on Windows.** The Win32 calls, the COM apartment behaviour, the Shell's output (premultiplied alpha, the
  zero-alpha rule taken from the contract, the row order from `GetDIBits`) and the MSVC build are all unverified until CI
  and the manual checklist.
- **No message pump.** The provider thread is a single-threaded apartment that never pumps window messages. If a Shell
  thumbnail handler needs a message delivered to this thread, `GetImage` can block. This is untested.
- **One thread, no timeout.** A slow or hung `GetImage` (a network drive, a broken codec) stops every later thumbnail
  until it returns. The dock shows placeholders in the meantime.
- **Exceptions.** `request()` can throw `std::bad_alloc` if the queue cannot grow, and that is not caught. If the thread
  cannot start, `thumbnail_provider()` throws `std::system_error` on first use, because the TaskQueue starts its thread
  in its constructor. That is not caught either.
- **`done` must not block on the UI thread.** `shutdown()` runs on the UI thread at EXIT and waits for the image in
  progress. If that image's `done` waits for the UI thread, the two deadlock. The dock must hop with `run_on_ui`, and
  asynchronously.
- **Re-asking.** A `done` that calls `request()` for a dropped path straight away can cycle while more than 256 requests
  are waiting. The dock should ask again on its next paint.
- **Duplicate work.** A path that is already being made is not replaced. A new request for it is queued, and the image
  is made again. The dock should not ask for a path that is already pending.
- **Answers after shutdown.** Dropped answers still owed when `shutdown()` runs are never delivered. This follows the
  contract, which says shutdown drops queued requests without calling `done`.
- **Pixel behaviour** was checked only against the fake's bitmaps.
- **Mutation results** cover the queue, the threading, the RAII releases and the pixel conversion. They say nothing about
  Win32 behaviour.

## Questions for the orchestrator

1. Accept the two bounds in deviations 1 and 2? The brief's exact posting rule is still correct, but it grows the
   TaskQueue on every paint. If you want it back, say so, and I will change it and re-run the harness.
2. The dock (WP3.4 and WP3.6) should ask again for a dropped path on its next paint, not inside `done`, and should not
   ask for a path it already has pending. Please pass that on to the UI WPs.
3. Manual checks for the user's list (WP3.2): a real `.mkv`, `.mp4` and `.png` each show their image. A non-media file
   shows its type icon. A clip whose file was deleted shows the placeholder. A path with non-ASCII characters (for
   example `Clips/Crème`) works. A clips folder on a network drive stays responsive. Closing OBS while thumbnails are
   loading does not hang.
4. Confirm the extra `<objbase.h>` include (deviation 4).

## TODOs

None in the code.

## Mutation check results

Each row is one deliberately broken copy of `thumbnails-win.cpp`, run through the whole harness. "Caught" means the run
exits non-zero: failed checks, or the watchdog ending a deadlock (exit 3).

| Mutant | What it breaks | Result |
|---|---|---|
| bitmap_leak | the GetImage bitmap is never deleted | caught: exit 1, 2 failed checks |
| cancel_silent | `cancel_pending()` discards requests without answering them | caught: exit 1, 28 failed checks |
| com_uninit_unconditional | CoUninitialize runs even when CoInitializeEx did not succeed | caught: exit 1, 1 failed check |
| done_under_lock | `done` runs with the mutex held, so a callback that calls back deadlocks | caught: exit 3 (watchdog), 3 failed checks |
| never_serve | no serve task is ever posted, so no request is made | caught: exit 1, 595 failed checks |
| no_alpha_fix | zero alpha bytes are left at zero | caught: exit 1, 1 failed check |
| no_clamp | `size_px` is not clamped to 16 to 1024 | caught: exit 1, 2 failed checks |
| no_serves_decrement | `serves_pending` never decreases, so the queue never looks drained | caught: exit 1, 48 failed checks |
| oldest_first | the oldest waiting request is made first, not the newest | caught: exit 1, 4 failed checks |
| overflow_disabled | nothing is dropped when the queue overflows | caught: exit 1, 49 failed checks |
| overflow_limit_255 | the queue holds 255 requests instead of 256 | caught: exit 1, 5 failed checks |
| post_flush_inline | the flush runs on the calling thread, inside `request()` | caught: exit 1, 13200 failed checks (the thread check fails on every callback) |
| replace_silent | a replaced request disappears with no dropped answer | caught: exit 1, 10 failed checks |
| shutdown_answers_queued | `shutdown()` calls `done` for the requests it discards | caught: exit 1, 26 failed checks |
| unbounded_serves | every request posts a serve task | caught: exit 1, 3 failed checks |

All 15 mutants are caught. `done_under_lock` also ends in the watchdog: after three checks have failed, the reentrancy
test deadlocks on its own mutex, and the watchdog ends the run after 150 seconds.

## Format check

`git add -A && build-aux/check-format` (clang-format 19, gersemi 0.21.0) prints
`check-format: all 66 C/C++ and 16 CMake files are formatted.`

## Orchestrator integration notes

- Read the whole diff; only the WP's files changed. Gates: MinGW syntax check (warnings as errors), plugin build
  (stub), check-format clean.
- Deviations 1-7 accepted: posting serve tasks only when needed and batching dropped answers keep the TaskQueue
  bounded while the dock asks on every paint, with the same guarantees. `<objbase.h>` is fine (it is in the SDK).
- Question 2: WP3.4's brief already asks for a thumbnail only when a path is neither pending nor failed, and asks
  again for a dropped one on the next paint, never inside `done`.
- No message pump: COM's STA modal loop pumps during outgoing calls to out-of-process thumbnail handlers, so a
  handler in another process can still answer. A hung in-process handler stalls later thumbnails only (placeholders
  stay). On the M3 checklist with the worker's manual checks.
