# WP3.3 hand-over: platform/shell-ops (worker, ★)

## Summary

`ShellOps` for Windows: open a clip, open a folder, reveal a clip in Explorer, rename it within its folder, and move it to the Recycle Bin. Each call is queued on one thread that starts COM as a single-threaded apartment, and runs in order. A call's `done` runs once: on that thread, or on the caller's thread when the call comes after `shutdown()` has begun.

Checked on Linux only: the stub, the Linux plugin build, the 246 unit tests, the formatter, the MinGW syntax check, and a scratch stress harness of the queue and shutdown protocol. Nothing has run on Windows. CI (MSVC) and the manual checks under "Known gaps and risks" are still needed. The top open risk is Known gaps item 1 (the permanent-delete warning).

## Files changed

| File | Change |
|---|---|
| `src/platform/shell-ops-win.cpp` | new, 569 lines: the Windows implementation |
| `src/platform/shell-ops-stub.cpp` | new: answers every call with "not supported on this platform" |
| `docs/handover/WP3.3.md` | new: this note |

`shell-ops.hpp`, `src/core/*`, `tests/*` and every other file are unchanged. The three files are staged with `git add -A`, which the acceptance step needs so that `check-format` sees the new files. Nothing is committed or pushed.

## How it was tested

| Check | Command | Result |
|---|---|---|
| Base | `git merge --ff-only claude/pensive-hamilton-cbg1l4`; `git log -1 --oneline` | fast-forwarded 417ab86..11b9311; HEAD is `11b9311 WP3.0: format CMakeLists.txt` |
| MinGW syntax, warnings as errors | the contract's command on `shell-ops-win.cpp` | exit 0, no warnings (run before and after formatting) |
| Format | `/usr/bin/clang-format-19 --style=file --fallback-style=none --dry-run --Werror` on both files (19.1.1) | exit 0. The Windows file was first reformatted with `-i`; the change was layout only |
| Linux plugin build | `cmake --preset ubuntu-ci-x86_64`; `cmake --build build_x86_64` | configure OK (a notice only: the project does not use `ENABLE_CCACHE`). Build OK, no warnings, `shell-ops-stub.cpp.o` built |
| Unit tests | `cmake -S tests -B build/tests && cmake --build build/tests && ctest --test-dir build/tests --output-on-failure` | 246 of 246 passed (Catch2 v3.7.1 fetched) |
| Repo format | `git add -A && build-aux/check-format` | "all 66 C/C++ and 16 CMake files are formatted", exit 0 |
| Queue and shutdown protocol | scratch harness outside the repo. It mirrors `submit`, `run_next` and `shutdown` and uses the real `src/core/task-queue.cpp`. 300 rounds of 4 producers x 60 submissions racing `shutdown()`, plus two targeted cases | ThreadSanitizer: PASS, 0 reports. ASan + UBSan: PASS, no errors, no leaks. Both runs had 72,000 callbacks, each called exactly once (TSan run: 67,155 answered during shutdown, 4,845 completed; ASan run: 66,168 and 5,832) |

Not tested here: any Win32 call at runtime. The MinGW check proves that the declarations and types compile. It proves nothing about behaviour.

### Windows declarations checked

Checked against `/usr/share/mingw-w64/include`. Line numbers are where each name is declared.

- `objbase.h`: `COINIT_APARTMENTTHREADED` (17), `COINIT_DISABLE_OLE1DDE` (19).
- `combaseapi.h`: `CoInitializeEx` (176), `CoUninitialize` (175), `CoCreateInstance` (258), `CoTaskMemFree` (365), `IID_PPV_ARGS` (84), `CLSCTX_ALL` (135).
- `shobjidl.h`: `CLSID_FileOperation` (32469), `SHCreateItemFromParsingName` (9150), `IShellItem` (9094), `IFileOperation::SetOperationFlags` (18889), `DeleteItem` (18939), `PerformOperations` (18953), `GetAnyOperationsAborted` (18956), `FOFX_RECYCLEONDELETE` = 0x00080000 (18857), `FOFX_EARLYFAILURE` = 0x00100000 (18858).
- `shlobj.h`: `SHParseDisplayName` (2681), `SHOpenFolderAndSelectItems` (631). `shtypes.h`: `PIDLIST_ABSOLUTE` (95).
- `shellapi.h`: `ShellExecuteW` (76), `FOF_SILENT` (165), `FOF_NOCONFIRMATION` (167), `FOF_ALLOWUNDO` (169), `FOF_NOERRORUI` (173), `FOF_WANTNUKEWARNING` (177). `winuser.h`: `SW_SHOWNORMAL` (201).
- `winbase.h`: `MoveFileExW` (2417), `FormatMessageW` (1441), `FORMAT_MESSAGE_ALLOCATE_BUFFER` (1469), `FORMAT_MESSAGE_FROM_SYSTEM` (1449), `FORMAT_MESSAGE_IGNORE_INSERTS` (1446), `LocalFree` (1127). `errhandlingapi.h`: `GetLastError`, `SetLastError` (48-49). `fileapi.h`: `GetFileAttributesW` (31), `INVALID_FILE_ATTRIBUTES` (26). `winnt.h`: `FILE_ATTRIBUTE_DIRECTORY` (5956), `MAKELANGID` (1353), `LANG_ENGLISH` (959), `SUBLANG_ENGLISH_US` (1128). `stringapiset.h`: `WideCharToMultiByte` (42). `winnls.h`: `CP_UTF8` (184). `basetsd.h`: `INT_PTR` (46).
- `winerror.h`: `FACILITY_WIN32` (24), `HRESULT_FACILITY` (3089), `HRESULT_CODE` (3087), `HRESULT_FROM_WIN32` (3096; inline form 3103). Error codes used: `ERROR_SUCCESS` (43), `ERROR_FILE_NOT_FOUND` (47), `ERROR_PATH_NOT_FOUND` (48), `ERROR_ACCESS_DENIED` (50), `ERROR_FILE_EXISTS` (106), `ERROR_SHARING_VIOLATION` (77), `ERROR_ALREADY_EXISTS` (184).
- Not in this MinGW install: `shobjidl_core.h` and `shlobj_core.h`, as the contract warned. Nothing is included from either.

## Walk-through

### (a) Every `done` is called exactly once

- **Refused after shutdown:** `submit()` reports "Clip Atlas is shutting down" on the caller's thread. Nothing is queued.
- **Queued (the normal case):** the entry waits in `pending`. Exactly one of two things takes it. `run_next()` on the queue thread pops it and reports the result, or `shutdown()` swaps it out and reports the shutting-down error. The shutdown flag is set and the list is taken under the same mutex, so no entry can be queued after the list has been taken.
- **`post()` refused:** not reachable while the lock is held, but guarded. The entry is popped back under the lock and reported once on the caller's thread. No task exists for it.
- **A task that finds `pending` empty** does nothing. `shutdown()` has already taken and reported that entry.
- **A call running at shutdown:** it reports itself once on the queue thread. `queue.shutdown()` waits for it.
- **Work that throws:** turned into an error result and reported once. **A `done` that throws:** caught and logged in `report()`, never re-reported.
- **Empty `done`:** not called. `as_finish()` checks it, and an empty `RenameDone` is not called.
- **Stub:** every call reports once, synchronously, on the calling thread. The contract allows this for the stub.

### (b) No path permanently deletes or overwrites a file

- A `grep` of `shell-ops-win.cpp` finds none of `DeleteFile`, `SHFileOperation`, `std::filesystem::remove`, `unlink`, `RemoveDirectory`, `CopyFile` or `CreateFile`. The `MOVEFILE_*` names appear only in a comment. `MoveFileExW` gets flags `0`.
- **Recycle:** the only deletion is `IFileOperation::DeleteItem` with `FOFX_RECYCLEONDELETE`. Success needs `PerformOperations()` to succeed, `GetAnyOperationsAborted` to report FALSE, and the file to be gone. A permanent delete can only come through Windows' own warning (`FOF_WANTNUKEWARNING`). Known gaps item 1 covers what is unverified.
- **Rename:** no replace flag, so an existing target fails with `ERROR_ALREADY_EXISTS`. No copy fallback. Refused before `MoveFileExW` is called: names containing `\ / : * ? " < > |`, control characters, a trailing dot or space, and `.` or `..`. Folders are detected with one read-only `GetFileAttributesW` call and refused the same way.
- **Open, open folder, reveal:** no writes.

### (c) COM objects, PIDLs and handles

- **COM:** `CoInitializeEx` runs in the queue thread's start hook. `CoUninitialize` runs in the exit hook, and only if the start succeeded (`com_ready`).
- **COM interfaces:** `IFileOperation` and `IShellItem`, each held in a `ComPtr` that releases in its destructor on every return path of `shell_recycle_clip`.
- **PIDL:** from `SHParseDisplayName`. Freed with `CoTaskMemFree` right after the two Shell calls and before any `return`, whatever their result. `CoTaskMemFree(nullptr)` is a no-op.
- **`FormatMessageW` buffer:** held in a `unique_ptr` with `LocalFree`, which also releases it on the `continue` path.
- **Handles:** none opened. The `HINSTANCE` from `ShellExecuteW` is a return code, not a handle this code owns.
- **Strings:** `std::string` and `std::wstring` only.

## Deviations from the contract

1. **Extra refusals, all before any Windows call:**
   - "that path can't be used": a path with an embedded NUL. Windows reads only up to the NUL, so it would act on a different file.
   - "that name can't be used": also control characters (NUL included), `"<>|?*`, and a trailing `.` or space. Windows silently removes a trailing dot or space, so the file would not get the name that was reported. Windows refuses the others anyway, with less clear text.
   - "no file was given": a rename whose last path part is empty, `.` or `..`. These name a folder.
   - "that is a folder, not a clip": rename and recycle refuse folders. For rename this costs one read-only `GetFileAttributesW` call. For recycle, recycling a folder would send everything inside it to the Recycle Bin.
   - "the clip no longer exists": recycle's `GetFileAttributesW` finds nothing. This is checked before the Shell is asked. The contract's not-found mapping on `SHCreateItemFromParsingName` is kept as well.
2. **`open`:** `SetLastError(ERROR_SUCCESS)` before `ShellExecuteW`. If the last error is still 0 on failure, the message says "ShellExecute returned N" instead of "error 0".
3. **COM gating is central in `run_next()`:** `open`, `open_folder`, `reveal` and `recycle` are refused when COM did not start. `rename` does not use COM, so it runs regardless. The COM-start message carries Windows' text too, following "use it for every failure".
4. **Error text:** an HRESULT prints as "(error 0x80070005)" and a Win32 code as "(error 5)". An HRESULT with facility `FACILITY_WIN32` is looked up by its Win32 code, because FormatMessage has no text for the wrapped form.
5. **Work that throws:** reported as "Clip Atlas could not finish the request: <what>". The contract was silent on this.
6. **Include:** `<objbase.h>` after `<windows.h>`, for `CoInitializeEx`, `CoCreateInstance`, `CoTaskMemFree`, `IID_PPV_ARGS` and `COINIT_*`. It is in the Windows SDK and in MinGW.
7. **Reveal fallback keeps the drive root:** the parent of `C:\clip.mkv` is `C:\`, not `C:`, which means the drive's current folder. A clip at a drive root falls back to `\`.
8. **Singleton variable:** named `ops`, not `p`. No behavioural difference.
9. **Stub includes:** `obs-module.h` and `plugin-support.h`, for `obs_log` in its exception guard, as `sound-player-stub.cpp` does. No Windows headers.

## Known gaps and risks

1. **Permanent-delete warning under `FOF_NOCONFIRMATION` (top risk).** The flags are exactly the contract's. I could not confirm from Microsoft's documentation that `FOF_WANTNUKEWARNING` still raises the permanent-delete question when `FOF_NOCONFIRMATION` is also set. WebFetch could not resolve the docs host, and `curl` through the proxy got a 403. A read of the proxy's README and status page was denied by the sandbox's auto-mode policy, and I did not try other routes. The consequence: the success check ("the file is gone") cannot tell a recycled file from one deleted permanently with no prompt. If Windows answers that prompt on its own, a clip would be destroyed and reported as moved. Before release: test on a volume with the Recycle Bin off, or on a file larger than the bin allows. The file must still be there after the prompt is declined. See Question 1.
2. **Nothing has run on Windows.** Manual checklist:
   - open a `.mkv`, and a file with no associated program; open a missing file
   - reveal a file (it should be selected) and a missing file (the folder should open); reveal a path with spaces and non-ASCII characters
   - rename normally; to an existing name (refused); to a name with a trailing dot; with `/`; case-only (`clip.mkv` to `Clip.mkv`); a file OBS is writing (in use)
   - recycle a clip and restore it from the Recycle Bin; decline Windows' warning (cancelled, file kept); recycle a missing file
   - press EXIT while a Shell dialog is open (EXIT waits for the user, as the header says)
3. **Case-only rename** relies on `MoveFileExW` treating source and target as the same file when no replace flag is given. If Windows reports `ERROR_ALREADY_EXISTS`, the user sees "a file with that name already exists". No data is lost either way.
4. **`IID_PPV_ARGS(operation.put())` on MSVC:** the macro names its argument twice. I could not check MSVC here. `put()` is harmless even if the operand were evaluated twice, because it only releases a null pointer before the creation call. CI will confirm.
5. **The extension rule is not enforced here.** The spec says rename "never changes the extension". The dock builds the name with `renamed_file_name()` (`src/core/clip-index.hpp:184`, WP3.1, not yet on this branch). `ShellOps::rename` accepts any valid bare name.
6. Reserved device names (`CON`, `NUL`, `COM1` and so on) are not refused by rename. Windows decides. No overwrite is possible.
7. Paths over `MAX_PATH` depend on the process's long-path setting. The error text comes from Windows.
8. If memory runs out while an error text is being built, the call's `done` is not called, because the queue drops the failing task. Out-of-memory only.
9. `shell_ops()` starts the queue thread on first use. If the thread cannot start, `std::system_error` escapes from the constructor. It is not caught here, as with `TaskQueue`.
10. **A `done` must not block on the UI thread.** `shutdown()` joins the queue thread from the caller's (UI) thread, so a callback that waits for the UI thread would deadlock at EXIT. Use `run_on_ui()`, which posts and returns.
11. Explorer or the launched program may open behind the OBS window (Windows' foreground rules). This is UX only.
12. `recycle()` trusts the caller's file, apart from refusing folders. The dock passes clips only.
13. The protocol harness is a scratch file outside the repo. It mirrors `Impl` because the Win32 code cannot link on Linux. Moving the submit/shutdown protocol into `core/` would let Catch2 test it. That is outside WP3.3's files.

## Questions for the orchestrator

1. **Known gaps item 1:** keep the contract's flags and verify on Windows; drop `FOF_NOCONFIRMATION` (one more Shell prompt per recycle, although the dock already asked); or change the recycle flags after reading the documentation? I would not ship before one of these is decided.
2. `open_folder("")` reports "no file was given", the contract's text. Should it say "no folder was given"? The dock never sends an empty folder, because it disables the button when the base is empty.
3. Keep the extra refusals in deviation 1 (NUL, trailing dot or space, `"<>|?*`, the folder refusal)? I recommend keeping them.
4. Who enforces the extension rule: WP3.1's `renamed_file_name()` alone, or `rename()` as well?
5. Logging: the dock logs Shell errors at WARNING (`src/ui/clip-dock.hpp`, lines 77-79), so `shell-ops` logs only callback exceptions and the COM-start failure. Please confirm this split is intended.
6. Should the submit/shutdown protocol move into `core/`, so it can get a Catch2 test on Linux? I did not do it, because it would touch files outside WP3.3.

## Fix round 1 (Reviewer)

Three changes, all in `src/platform/shell-ops-win.cpp`. The flags, the success test and the earlier extra refusals are unchanged.

1. **Last component and wildcards.** `path_problem()` is the check that open, open folder, reveal, rename and recycle all run first. It now also refuses, with "that path can't be used", a path whose last component (after the final `\` or `/`) ends in `.` or a space, or contains `*` or `?`. Win32 strips trailing dots and spaces, so such a path would act on a different file. The empty-path, NUL and "no file was given" checks are unchanged and still run first.
2. **Absolute paths only.** The same `path_problem()` refuses any path for which `path_from_utf8(p).is_absolute()` is false, with "that path can't be used". A relative path would resolve against OBS's working directory. Open folder and reveal's fallback go through the same check, so they are covered too.
   - Knock-on change in `shell_reveal()`: its fallback folder used to lose its separator. That made `C:` and a bare UNC share root (`\\server\share`) non-absolute, so they would have been refused. The fallback now passes `path_utf8.substr(0, separator + 1)`, which keeps the separator. The special cases for an empty folder and for a trailing `:` are gone. This is the only change beyond the three listed, and it exists only so that reveal keeps working under change 2.
3. **Aborts.** When `GetAnyOperationsAborted` reports TRUE, the error is now "cancelled, or Windows could not move the clip (it may be in use)". I took the Reviewer's reading of the SHFILEOPSTRUCT docs as given: aborts can be silent under FOF_NOERRORUI and FOF_NOCONFIRMATION. I could not open those docs from this sandbox (Known gaps item 1), so this is not independently checked.

Effects on earlier text:

- Deviation 1 gains the refusals from changes 1 and 2. It lists only the round-0 refusals.
- The manual-test expectation for a declined warning (Known gaps item 2) is now "cancelled, or Windows could not move the clip (it may be in use)", not "cancelled".
- The "the clip's folder is unknown" branch in `shell_rename_clip` can no longer be reached, because a path without a separator is relative and is refused first. It stays as a defensive check.
- Intermediate folder components are not checked for a trailing dot or space, or for `*` and `?`, because the Reviewer's wording limits those checks to the last component. I have not verified whether Win32 also trims trailing dots and spaces from folder components. The dock's folders come from `sanitize_folder_name()` (`src/core/clip-layout.hpp`), which trims trailing dots and spaces. Windows does not allow `*` or `?` in any name, so such a path fails rather than acting elsewhere.

Checks for this round:

| Check | Command | Result |
|---|---|---|
| MinGW syntax | the contract's command on `shell-ops-win.cpp` | exit 0, no warnings |
| Linux CI preset | `cmake --preset ubuntu-ci-x86_64`; `cmake --build build_x86_64` | configure exit 0; build exit 0. The build had nothing to do: the Windows file is not part of the Linux build, and the stub is unchanged |
| Format | `clang-format-19 --style=file --fallback-style=none --dry-run --Werror` on both files | exit 0; no reformatting needed |
| Rule check | scratch driver outside the repo, compiling the `path_problem()` text extracted from the edited file | 11 of 11 cases as expected. The cases use POSIX absolute paths, so Windows `is_absolute()` for drive and UNC paths is not exercised |
| Repo format | `git add -A && build-aux/check-format` | "all 66 C/C++ and 16 CMake files are formatted", exit 0 |

Diff summary: `shell-ops-win.cpp` has 17 insertions and 11 deletions in this round, all within `path_problem()`, `shell_reveal()` (the fallback) and the aborted branch of `shell_recycle_clip()`.

## Orchestrator integration notes

- Read the whole diff; only the WP's files changed. Gates: MinGW syntax check with warnings as errors, plugin
  build (stub), check-format clean.
- Reviewer (★): no path overwrites a file or deletes one without the user's consent. Its "blocker" (the success
  test can't tell a recycled file from one deleted for good) is not one here: a permanent delete happens only
  after the user answers Yes to Windows' own warning (`FOF_WANTNUKEWARNING` "partially overrides
  FOF_NOCONFIRMATION", Microsoft's SetOperationFlags page), and the file is gone either way, so removing the row
  is right. Fixed in round 1: trailing dot/space and wildcards in the source path, absolute paths only, the abort
  wording. The TaskQueue self-shutdown deadlock it also found was fixed in `core/task-queue` (commit 15422e5).
  Not taken: device names in `rename()` (the dock's `renamed_file_name()` refuses them), English reasons after a
  localised prefix in the dock's message boxes (the same pattern as the save-sound Test error).
- Answers: the extra refusals are kept; the extension rule is the dock's (`renamed_file_name()`); the log split is
  as intended.
- On the M3 checklist: recycle and restore; decline Windows' warning on a drive without a Recycle Bin; rename to
  an existing name; reveal.
