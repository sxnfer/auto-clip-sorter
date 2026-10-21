# WP5.2 hand-over: platform/http (WinHTTP)

## Summary

Implements `HttpClient` and `http_client()`, whose contract is `src/platform/http.hpp` (unchanged).

- `src/platform/http-win.cpp` (Windows, WinHTTP): one HTTPS GET at a time on the client's own TaskQueue thread, in FIFO order. `shutdown()` answers the queued requests with "Clip Atlas is shutting down" and waits for the running one.
- `src/platform/http-stub.cpp` (other platforms): every `get()` answers at once on the calling thread with "not supported on this platform".

Nothing here has run against WinHTTP. What was checked is listed under Testing; the Windows-only behaviour that is unverified is listed under Known gaps.

## Files changed

| File | Status | Lines |
|---|---|---|
| `src/platform/http-win.cpp` | new | 385 |
| `src/platform/http-stub.cpp` | new | 62 |
| `docs/handover/WP5.2.md` | new (this note) | |

Nothing else is changed. `git add -A` was run (staged, not committed). Build directories are ignored by `.gitignore`.

## Design

- `get()` copies the strings and moves `done` into a request. Under the client's mutex it either refuses (after `shutdown()`), or appends the request to `pending` and posts one task to the TaskQueue. The post happens under the same mutex.
- Each task (`run_next()`) takes the oldest pending request under the mutex, then runs `fetch()` on the client's thread. All checks live in `fetch()`, so a bad address is answered on the client's thread, never inside `get()`, as `http.hpp` requires.
- `shutdown()` sets `stopped` and takes every pending request out under the mutex. It answers them on the calling thread outside the lock, then calls `TaskQueue::shutdown()`, which waits for the running request and joins the thread.
- `fetch()` order: "https://" prefix check, NUL check, User-Agent check, UTF-8 conversion (`MB_ERR_INVALID_CHARS`), `WinHttpCrackUrl`, scheme and host check, `WinHttpOpen`, `WinHttpSetTimeouts`, `WinHttpConnect`, `WinHttpOpenRequest`, `WinHttpAddRequestHeaders`, `WinHttpSendRequest`, `WinHttpReceiveResponse`, `WinHttpQueryHeaders`, then the read loop (`WinHttpQueryDataAvailable` and `WinHttpReadData`, with the 1 MiB cap checked before each read), then close (RAII).
- Total deadline: `kTotalDeadlineMs` (30 s) is measured with `GetTickCount64()` from just before `WinHttpOpen`, and checked before each body query and read (the end-of-body query included), so a body whose last query comes at or after 30 s is refused. When it has passed, the request fails with "the answer took too long" (status 0, empty body), and the handles close as usual.

## WinHTTP and Win32 signatures checked

Every call and constant was checked against the MinGW-w64 headers installed here. Line numbers refer to `/usr/share/mingw-w64/include/winhttp.h` unless noted.

| Call or constant | Line | Signature or value |
|---|---|---|
| `WinHttpOpen` | 842 | `(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD)` |
| `WinHttpSetTimeouts` | 858 | `(HINTERNET, int, int, int, int)` |
| `WinHttpConnect` | 826 | `(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD)` |
| `WinHttpOpenRequest` | 843 | `(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR*, DWORD)` |
| `WinHttpAddRequestHeaders` | 822 | `(HINTERNET, LPCWSTR, DWORD, DWORD)` |
| `WinHttpSendRequest` | 853 | `(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR)` |
| `WinHttpReceiveResponse` | 851 | `(HINTERNET, LPVOID)` |
| `WinHttpQueryHeaders` | 847 | `(HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD)` |
| `WinHttpQueryDataAvailable` | 846 | `(HINTERNET, LPDWORD)` |
| `WinHttpReadData` | 849 | `(HINTERNET, LPVOID, DWORD, LPDWORD)` |
| `WinHttpCloseHandle` | 825 | `(HINTERNET)` |
| `WinHttpCrackUrl` | 827 | `(LPCWSTR, DWORD, DWORD, LPURL_COMPONENTS)` |
| `URL_COMPONENTS` | 534-551 | fields used: `lpszHostName`, `nPort`, `lpszUrlPath`, `lpszExtraInfo`, the `dw...Length` fields, `nScheme` |
| `INTERNET_SCHEME_HTTPS` | 48 | 2 |
| `WINHTTP_FLAG_SECURE` | 67 | 0x00800000 |
| `WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY` | 72 | 4 |
| `WINHTTP_QUERY_STATUS_CODE` | 378 | 19 |
| `WINHTTP_QUERY_FLAG_NUMBER` | 438 | 0x20000000 |
| `WINHTTP_ADDREQ_FLAG_ADD` | 92 | 0x20000000 |
| `WINHTTP_NO_PROXY_NAME`, `..._BYPASS`, `WINHTTP_NO_REFERER`, `WINHTTP_DEFAULT_ACCEPT_TYPES`, `WINHTTP_NO_ADDITIONAL_HEADERS`, `WINHTTP_NO_REQUEST_DATA`, `WINHTTP_HEADER_NAME_BY_INDEX`, `WINHTTP_NO_HEADER_INDEX` | 74-87 | all `NULL` |
| `WINHTTP_OPTION_REDIRECT_POLICY_DEFAULT` | 206 | `DISALLOW_HTTPS_TO_HTTP` (not changed) |

Win32 (other MinGW headers in `/usr/share/mingw-w64/include/`): `FormatMessageW` (winbase.h:1441); `FORMAT_MESSAGE_IGNORE_INSERTS`, `FORMAT_MESSAGE_FROM_HMODULE` and `FORMAT_MESSAGE_FROM_SYSTEM` (winbase.h:1446-1449); `GetModuleHandleW` (libloaderapi.h:140); `MultiByteToWideChar` and `WideCharToMultiByte` (stringapiset.h:41-42); `MB_ERR_INVALID_CHARS` (winnls.h:41); `CP_UTF8` (winnls.h:184); `MAKELANGID` (winnt.h:1353); `LANG_ENGLISH` (winnt.h:959); `SUBLANG_ENGLISH_US` (winnt.h:1128); `GetTickCount64` (sysinfoapi.h:42, declared under `_WIN32_WINNT >= 0x0600`; MinGW's default `_WIN32_WINNT` is 0xa00, `_mingw.h:232`).

`CMakeLists.txt` already links `winhttp` on WIN32. It was not changed.

## Testing

Run from the worktree. Results are exact.

1. Fast-forward: `git merge --ff-only claude/pensive-hamilton-cbg1l4`, then `git log -1 --oneline`, which shows `217b811 WP5.0: M5 interface headers and locale keys`.
2. Linux CI preset: `cmake --preset ubuntu-ci-x86_64` exits 0 (it prints one CMake warning that `ENABLE_CCACHE` is unused; that comes from the preset). `cmake --build build_x86_64` exits 0 with no warnings in the log. `http-stub.cpp` is compiled; `http-win.cpp` is not part of the Linux build. Re-run after fix round 1 with `--clean-first`: exit 0, no warnings.
3. MinGW syntax check, exactly as the brief gives it (`x86_64-w64-mingw32-g++-posix -std=c++17 -fsyntax-only -Wall -Wextra -Werror -DUNICODE -D_UNICODE -I src -I src/core -I src/platform -I src/obs -I /usr/include/obs -I build_x86_64 src/platform/http-win.cpp`): exits 0, before and after the final comment edit, and again after fix round 1.
4. MinGW link proof (scratch program outside the repo, not run): `main.cpp` calls `http_client().get()` and `shutdown()`. It is linked with `http-win.cpp`, `task-queue.cpp` and `-lwinhttp` (`-Wall -Wextra -Werror`, `-O1`), and exits 0. It was re-linked after fix round 1, when it also imports `GetTickCount64`. The result, `link-test.exe`, is a PE32+ file that imports all twelve WinHTTP functions above from `WINHTTP.dll`. It cannot be run here, because there is no Wine. Path: `/tmp/claude-0/-home-user-auto-clip-sorter/6c995450-a095-5948-9d9e-9af008631cf2/scratchpad/wp52-link/link-test.exe`.
5. Runtime stress harness on Linux under ThreadSanitizer (scratch, outside the repo). `harness.cpp` compiles the real `http-win.cpp` and `task-queue.cpp` against stand-in Win32 headers (`fakewin/`), and the WinHTTP calls are simulated. The run covers 8 caller threads with 120 requests each, over 25 request variants (among them three for the deadline: a trickling body, a slow body that finishes in time, and a body that completes at the deadline; bad scheme, empty host, failed connect, slow connect, 404, empty body, chunked body, read failure, send failure, status failure, a 2 MiB body, exactly 1 MiB, 1 MiB + 1, invalid UTF-8, empty User-Agent, embedded NUL and others). It also covers a shutdown race (4 threads keep calling `get()` across `shutdown()`), a shutdown from inside a callback, a throwing callback, a re-entrant `get()`, a shutdown that waits for a running request, and refusals after shutdown. Result after fix round 1: PASS, 0 failed checks. Handles: 1591 opened and 1591 closed, 0 double closes, 0 insecure requests, 1 logged callback failure, no ThreadSanitizer output. Before fix round 1 the same harness passed three runs in a row (1418 opened and closed). The first run failed only because the stand-in `DWORD` was 64 bits on Linux. I fixed the stand-in; the code under test did not change. Path: `/tmp/claude-0/-home-user-auto-clip-sorter/6c995450-a095-5948-9d9e-9af008631cf2/scratchpad/wp52-harness/`.
6. Mutation check of the harness: a copy of `http-win.cpp` that answers shutdown refusals twice makes the harness fail with `shutdown race: request 3 was answered 2 times`. A second mutant, with the deadline check disabled, fails 73 checks, all on the trickling and edge variants; the slow body that finishes in time still passes.
7. Unit tests: `cmake -S tests -B build/tests` (exit 0), `cmake --build build/tests` (exit 0), `ctest --test-dir build/tests --output-on-failure`: 100% tests passed, 0 tests failed out of 246.
8. Formatting: `clang-format-19 --dry-run --Werror src/platform/http-win.cpp src/platform/http-stub.cpp` exits 0, after one `clang-format-19 -i` pass on `http-win.cpp` that only aligned continuation lines. Re-run after fix round 1: exits 0 with no further changes. `git add -A && build-aux/check-format` reports `check-format: all 71 C/C++ and 16 CMake files are formatted.`
9. TLS grep: `grep -n "SetOption\|SECURITY_FLAGS\|REDIRECT\|CLIENT_CERT" src/platform/http-win.cpp` finds nothing.

## Correctness

### Every done is called exactly once

Each `get()` ends in one of three ways:

- Refused, because `stopped` is set. `get()` answers on the calling thread after the mutex is released. The request was never queued.
- Queued, then answered by a task. `run_next()` pops it under the mutex and calls `done` on the client's thread.
- Queued, then answered by shutdown. `shutdown()` takes it out under the mutex and calls `done` on the calling thread.

Why this holds: a request is added only under the mutex and only while `stopped` is false, together with one posted task. It leaves `pending` only under the mutex, in one of four places:

1. `run_next()` pops it, and that task answers it.
2. `shutdown()` swaps the whole queue out, and shutdown answers it.
3. `get()` takes it back after a refused post, and `get()` answers it.
4. `get()` pops it back in the exception path. There is no answer in this case, because `get()` throws (see Deviations, item 7).

Once `shutdown()` has set `stopped` and swapped, nothing can be added, so `pending` stays empty. A task that runs after that finds nothing and returns without calling `done`. Every `done` call happens outside the mutex and inside a try block, so an exception from it cannot reach the thread or the queue. A failure inside `fetch()` is caught in `run_next()` and becomes an error answer.

Shutdown races covered by the argument and by the harness: `get()` racing `shutdown()` in both orders; two `shutdown()` calls at once (the second drains nothing and waits on the TaskQueue's join mutex); `shutdown()` from inside a callback (the queue returns without joining its own thread and stops after the callback); `get()` from inside a callback during the drain (refused, answered inline, no lock held); and a `shutdown()` that waits for a running request, where the harness checks that the running request is answered before `shutdown()` returns. One shutdown race can deadlock; it is listed under Known gaps.

### Every handle is closed on every path

`WinHttpHandle` wraps each handle at the moment it comes back from `WinHttpOpen`, `WinHttpConnect` and `WinHttpOpenRequest`, and its destructor closes it. The three wrappers are declared in open order, so they close in reverse: request, connection, session. Every return after the session is open leaves through their destructors. Returns before `WinHttpOpen` succeeds hold no handle.

The harness counted 1591 opens and 1591 closes, with 0 double closes. It covered a refused open, a failed connect, a failed send, a failed status query, a failed read, a body over the cap (2 MiB and 1 MiB + 1), a body exactly at the cap (accepted), 404, an empty body, chunked reads and the deadline failure on trickling bodies. The harness does not simulate failures of `WinHttpSetTimeouts`, `WinHttpAddRequestHeaders` or `WinHttpReceiveResponse`. Those returns are covered by the same unwinding, by inspection.

### No path weakens TLS

- Every `WinHttpOpenRequest` passes `WINHTTP_FLAG_SECURE`. The harness checks this for each request.
- The address must start with exactly `https://`, and `WinHttpCrackUrl` must report `INTERNET_SCHEME_HTTPS`, so the code cannot build an HTTP request.
- The code calls no `WinHttpSetOption`. Certificate checks stay at WinHTTP's default, and the redirect policy stays at its default, `DISALLOW_HTTPS_TO_HTTP` (MinGW header line 206). No client certificate and no protocol flags are set.
- The harness shows that the code asks for these settings. It cannot show Windows' actual TLS behaviour.

## Deviations from the brief or contract

1. Extra English error texts, because the contract lists only some failures: "the address is not valid UTF-8", "the User-Agent is not valid UTF-8", "the address is not valid", "the address could not be read", "the address has no host name", "could not open a WinHTTP session", "could not set the timeouts", "could not connect to the server", "could not open the request", "could not add the Accept header", "could not send the request", "could not receive the response", "could not read the status code", "could not read the answer", and "the request failed" (from the exception guard). "Windows gave no description" is used when FormatMessageW has no text. Where a Windows code exists, every failure has the contract's form, "<what>: <Windows text> (error <code>)".
2. Extra NUL check: an address with an embedded NUL is refused. The C API calls would stop at the NUL, while the earlier checks saw the whole string.
3. A `WinHttpSetTimeouts` failure aborts the request. The contract says nothing about it.
4. `call_done` catches `const std::exception &` first, so the log shows the message, and then `...`, as sound-player does. Both are inside `try`, so the brief's `catch (...)` requirement is met.
5. The read loop stops if `WinHttpReadData` returns TRUE with 0 bytes, even if WinHTTP reported more. This avoids a spin.
6. Stub: `get()` answers "not supported on this platform" every time, including after `shutdown()`. The brief accepted the inline answer. The header's after-shutdown wording is not followed by the stub.
7. `get()` may throw `std::bad_alloc` before it queues a request. Then the request is not queued and `done` is not called. This is the only path where `done` is not called, and the caller sees the exception. Catching it would need a second ownership scheme for `done`, which I judged not worth the complexity for an out-of-memory condition.
8. A 30 s total deadline (`kTotalDeadlineMs`) was added in fix round 1, at the coordinator's request. It is checked only before each body query and read, where the coordinator asked for it. The phase before the body is not covered (see Known gaps 1).

## Known gaps and risks

1. Timeouts are per WinHTTP operation (10 s each for resolve, connect, send and receive). The 30 s total deadline bounds the body phase, but it is checked only before each body query and read, as specified in fix round 1. The phase before the body (`WinHttpSendRequest`, which covers resolve, connect and send, and `WinHttpReceiveResponse`) is bounded only by the per-operation timeouts, so the worst case before the first body check is about 40 s, and `shutdown()` at EXIT can wait that long. See question 1.
2. Latent deadlock in `src/core/task-queue.cpp`, which is not my file. `TaskQueue::shutdown()` takes `join_mutex_` before it checks whether it is running on its own thread. If a callback on the queue thread calls `shutdown()` while another thread is inside `shutdown()`, the callback waits for `join_mutex_`, and the other thread waits for the join. The plugin does not do this: `run_on_ui` is asynchronous off the UI thread (`src/obs/ui-thread.hpp`), so callbacks do not block. The orchestrator is fixing the ordering in `task-queue.cpp`, as stated in fix round 1.
3. Not run on Windows. WinHTTP error texts, FormatMessageW output, TLS and certificate errors, and WPAD and proxy behaviour are all unverified. MSVC is the real compile check for `http-win.cpp` (CI, /W3 with /WX). The MinGW syntax check and link are local stand-ins. I checked the MSVC warning settings by reading the code (explicit size casts, no signed/unsigned comparisons), which is not a compile. The harness stand-in converts ASCII only, so valid non-ASCII UTF-8 is not exercised, and its FormatMessageW returns fixed text.
4. I have not verified how `WinHttpCrackUrl` splits a `#fragment`. If it leaves the fragment in `lpszExtraInfo`, the request line would carry it. The one URL in use has no fragment.
5. Each request opens and closes its own WinHTTP session, so there is no connection reuse. That is fine for one check a day.
6. `http_client()` creates the TaskQueue thread on first use (inside the HttpClient constructor). If Windows cannot create the thread, `std::system_error` escapes from `http_client()`. This is not handled.
7. Destroying the client from inside its own callback is unsupported, as for TaskQueue. `http_client()` never destroys it.
8. The `Accept: application/vnd.github+json` header is GitHub-specific, but the contract places it in this layer.

## Questions for the orchestrator

1. Resolved in fix round 1 by a 30 s total deadline, checked before each body query and read. Should the deadline also be checked before `WinHttpSendRequest` and `WinHttpReceiveResponse`, so that the 30 s bound is strict? That is two more checks. The alternative is to accept the about 40 s worst case and say so in the header.
2. Answered in fix round 1: the orchestrator will fix the self-shutdown ordering in `src/core/task-queue.cpp`. I have not touched it.
3. Accepted in fix round 1, as they are.
4. Accepted in fix round 1: `get()` may throw `std::bad_alloc` before queueing, as now.
5. Accepted in fix round 1: the stub stays as it is.

## Orchestrator integration notes

- Read the whole diff; only the WP's files changed. Gates: MinGW syntax check (warnings as errors), plugin build
  (stub), check-format clean.
- Fix round 1 added the 30 s total deadline. Its two edges are accepted as they are: send/receive are bounded by
  the 10 s per-operation timeouts (about 40 s before the first body check, worst case), and a body whose last
  query comes after 30 s is refused (the next daily check retries).
- Question 2 (the TaskQueue self-shutdown ordering) was fixed by the orchestrator in commit 15422e5, with a test.
