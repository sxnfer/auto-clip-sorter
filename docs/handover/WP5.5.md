# WP5.5 hand-over: obs/update-checker + Updates setting + Tools menu item + update banner + hotkey wiring (worker)

## Summary

- `src/obs/update-checker.cpp` (new) implements `UpdateChecker`, declared in `src/obs/update-checker.hpp` (unchanged):
  - An automatic check runs 60 s after start, then on hourly ticks. It runs only while `check_for_updates` is on and the
    last check that got an answer is at least 24 h old (`update_check_due()`, `update-check.json`).
  - A manual check (Tools menu) runs at once, whatever the setting.
  - Answers come back on the HTTP thread and hop to the UI thread with `run_on_ui()`, through a weak reference.
  - `last_check` is saved only for answers that are not failures. Nothing is downloaded or installed. The only URL opened
    is `UpdateResult::download_url`.
- `src/plugin-main.cpp` wires it in:
  - `UpdateChecker` and `Hotkeys` are created after `recordings`, each in its own try.
  - `on_update_available` shows the dock's dismissible banner. Its "Download…" button opens the release page.
  - "Check for Clip Atlas Updates…" is added to the Tools menu, right after the settings item. Its answer goes to
    `show_update_result()`: an information, warning or Yes/No box, all plain text, never shown after stop. "Yes" opens
    the page and shows the banner.
  - Both objects start at FINISHED_LOADING, the hotkey reloads its binding at PROFILE_CHANGED, and both stop in
    `stop_all()`. `http_client().shutdown()` is called there too.
- `src/ui/settings-dialog.cpp/.hpp`: an "Updates" group after "Save sound", with the "Check for updates once a day" checkbox
  (loaded and saved) and a word-wrapped plain-text help line.

## Files changed

| File | Change |
|---|---|
| `src/obs/update-checker.cpp` | new, 418 lines |
| `src/plugin-main.cpp` | +170 / -2: includes, two members in `PluginContext`, update and menu handlers, start, stop and profile wiring |
| `src/ui/settings-dialog.cpp` | +17: the Updates group, `load()`, `accept()` |
| `src/ui/settings-dialog.hpp` | +1: private `QCheckBox *check_for_updates_` |
| `docs/handover/WP5.5.md` | new (this note) |

Staged with `git add -A`, not committed. Nothing else changed: the headers, core, platform, `clip-dock.*`, `banner-area.*`,
`CMakeLists.txt` and `en-US.ini` are untouched. Build output went to the ignored `build_x86_64/` and `build/tests/`. The
scratch harness is outside the repo, in the session scratchpad (`wp55/harness/harness.cpp`).

## How it was tested

Repo checks, run from the worktree:

| Check | Command | Result |
|---|---|---|
| Baseline, before any change | `cmake --preset ubuntu-ci-x86_64` and `cmake --build` in a scratch copy of HEAD `4687bb0` | configure OK, build exit 0 |
| Linux plugin build, warnings as errors | `cmake --preset ubuntu-ci-x86_64`, then `cmake --build build_x86_64` | build exit 0, no `warning:` lines, `clip-atlas.so` linked |
| No undefined clip_atlas symbols | `nm -C build_x86_64/clip-atlas.so \| grep " U " \| grep clip_atlas` | prints nothing |
| Unit tests | `cmake -S tests -B build/tests && cmake --build build/tests && ctest --test-dir build/tests --output-on-failure` | 343 of 343 passed |
| Format, the four changed C++ files | `clang-format-19 --style=file --dry-run --Werror` | clean |
| Format, repo | `git add -A && build-aux/check-format` | "all 87 C/C++ and 16 CMake files are formatted", exit 0 |
| Locale keys | every `obs_module_text()` key in the changed files checked against `en-US.ini` | all present |
| Line width | code points, tabs as 8 | no added line over 120. One existing 121-column line (plugin-main.cpp, HEAD line 139) is untouched |

Scratch harness (outside the repo, in the session scratchpad at `wp55/harness/harness.cpp`):

- It compiles the real `update-checker.cpp`, with the real `update-policy.cpp` and `version.cpp`. It links the real libobs
  30.0.2 for `obs_data_*`, `os_mkdirs`, `os_file_exists`, `bstrdup` and `bfree`, and Qt 6.4.2 Core.
- Stand-ins, all defined in the harness: the config folder (a temp dir), `obs_log` (recorded), `PLUGIN_VERSION` (0.2.0),
  and an HTTP client whose answers the test scripts. The answer is given inside `get()`, or held and given later on the
  same thread or another. `run_on_ui()` runs inline on the UI thread or queues to the event loop, as the header allows.
  `SettingsStore::get/set` are in memory, and `time()` is a fixed clock the tests move. The private members are reached by
  compiling the .cpp with `private` defined as `public` after the class declaration is parsed.
- Build: `g++ -std=c++17 -O1 -fPIC -fsanitize=address,undefined`, with `-DSCRATCH_DIR`, linked with `-lobs -lQt6Core -lpthread`.
- Final result, with the final code: `checks: 1044, failures: 0`, `PASSED: 0 failure(s)`. AddressSanitizer,
  UndefinedBehaviorSanitizer and LeakSanitizer were all on, and reported nothing. The run takes about 70 s, because it
  includes the real timer test.
- What it covers:
  - **First check after 60 s, on the real Qt timer.** No request at 58.5 s, one at 61.5 s, to the
    `releases/latest` URL with user agent `clip-atlas/0.2.0`. The first timer is single-shot, and the hourly tick is 1 h.
  - **Every answer outcome**, 20 cases through 5 delivery modes. The cases: newer, same, older, release candidates, an
    untrusted URL (the releases page is offered), 404, HTTP 500 and 403, transport error, invalid JSON, empty body,
    top-level array, draft, pre-release, bad tag, missing, numeric and empty tag, and an error with status 200. The modes:
    run_on_ui inline or queued, with the answer inline, held and given on another thread, or held and given on the UI
    thread. In each case the callback ran once on the UI thread with the expected outcome, latest version, download URL
    and error text. `last_check` was saved only for non-failures. One log line each, at INFO or WARNING.
  - **Due rule and setting.** No file is due. Exactly 24 h is due, 23 h is not. 25 h is due. A clock set back 2 h is due. A
    time 30 min in the future is not. A corrupt file, or one with no `last_check`, counts as never checked (the corrupt one
    logs a WARNING). With the setting off, no automatic check runs but a manual one does. While a request is in flight a
    tick starts nothing. After an answer the next tick is not due.
  - **Callbacks.** An automatic update_available reaches the update callback once. An automatic up_to_date does not. A
    manual update_available reaches only its caller. A manual check during an automatic one makes no second request, and
    both get the same result, with the update callback called once. During a failing automatic check, the manual caller
    gets the failure and the update callback is not called. Two manual callers share one request.
  - **Saving.** A failed check leaves the saved time as it was. A blocked config folder logs an ERROR, and the in-memory
    time still spaces the next check.
  - **Retry.** At a 20 ms test interval, a failed check was retried on every tick, and nothing was saved.
  - **Stop.** An answer after `stop()` is dropped, with no callback and no save, and an automatic one likewise. `stop()`
    is idempotent. `start()` after `stop()` arms nothing, and `check_now()` after it sends nothing. The checker destroyed
    while a request is held, or while a queued hop is pending, gives no callback and no crash. `stop()` from a waiting
    caller ends the list. `stop()` from the update callback stops the manual callers. A caller that destroys the checker
    during the answer causes no crash, and the next caller is not called.
  - **Failures inside.** A request that cannot be sent (an exception from `get()`) reports a failure to its callers, and
    the next check works. An empty callback is ignored.
- Probes of the real libobs 30.0.2:
  - `obs_data_get_string()` returns `""`, not NULL, for a missing key and for numbers, bools and objects.
  - `obs_data_create_from_json("[1,2]")` returns an object, with no tag, so the answer is "could not be read".
  - `obs_data_create_from_json("")` and `"{not json"` return NULL.
  - `os_mkdirs()` returns 0 when it creates the folder, 1 when it exists, and -1 under a regular file. `MKDIR_ERROR` is -1.
- Environment note: in this container, `QCoreApplication` gets no instance in a non-PIC executable at -O1 or under
  sanitizers, so the harness is built with `-fPIC`. This is not a plugin issue.

Not tested: Windows (the WinHTTP code in `http-win.cpp` and any Win32 build), the OBS 31 headers (only the libobs 30.0.2
headers are installed here), and the OBS UI (menu, banner, dialogs, the EXIT order). Those need the M5 manual checklist.

## Deviations, and why

1. **`PLUGIN_VERSION` is a variable here, not a macro.** `src/plugin-support.h` declares `extern const char *PLUGIN_VERSION`,
   and `plugin-support.c.in` defines it. So the literal form `"clip-atlas/" PLUGIN_VERSION` cannot compile. The code uses
   `std::string("clip-atlas/") + PLUGIN_VERSION`, as the brief's code says, and `QString::fromUtf8(PLUGIN_VERSION)` for Qt.
2. **`obs_module_config_path()` is a macro here**, over the function `obs_module_get_config_path()` declared in `obs.h`. The
   call sites are the same.
3. **A `last_check_loaded` flag beside the `std::optional`.** The brief said `last_check` is loaded lazily. Without a flag,
   "not read yet" and "never checked" cannot be told apart.
4. **`Impl` derives from `std::enable_shared_from_this`** to get its `weak_ptr` for the timer slots and the HTTP answers.
   The header allows either.
5. **Log line for `no_release`:** "update check: no release published yet (this X)". The brief's "(latest <v>, this <v>)"
   form has no latest version to print here.
6. **`checking` is cleared at the start of `complete()`**, before logging, not after the save. The observable behaviour is
   the same. A callback that starts the next check, for example through a dialog's event loop, sees consistent state.
7. **Each of `UpdateChecker` and `Hotkeys` is created in its own try** in `start_plugin()`. A failure to create one does not
   stop the plugin loading. The brief did not say this. It follows the policy for the clip browser, which is optional.
8. **Dialog types.** `show_update_result()` uses the static `QMessageBox::information` for up-to-date and no-release, whose
   text comes from the locale or from `PLUGIN_VERSION`. Failed and available use an explicit `QMessageBox` with
   `Qt::PlainText`, because their text can carry network data. That is what the brief asked for.
9. **The settings help label** is set to `Qt::PlainText` explicitly.
10. **Comment widths.** The repo's `.clang-format` sets `ReflowComments: false`, so clang-format does not wrap comments. I
    re-wrapped my own comment paragraphs to 120 columns by hand. No existing code was touched.

## Known gaps and risks

1. **EXIT can wait for the network.** `stop_all()` calls `http_client().shutdown()` on the UI thread. That waits for a request
   in flight, up to the WinHTTP timeouts of WP5.2 (10 s per phase). A stalled network could delay OBS's exit by tens of
   seconds on Windows. See question 2.
2. **An answer can be lost.** If queuing an answer with `run_on_ui()` throws (bad_alloc), or the UI tasks are already shut
   down, `checking` stays true for the session. Automatic checks then stop until OBS restarts, and a manual check waits
   for an answer that never comes. Only allocation failure or EXIT causes this. It is not handled.
3. **Repeated clicks.** Clicking "Check for Clip Atlas Updates…" again while a check is in flight gets a second dialog after
   the first. Each caller is answered, as the contract says. See question 1.
4. **Log noise offline.** With no network, each hourly tick logs one WARNING, because a failed check is retried at each
   tick, as the brief asked.
5. **Missing checker.** If the update checker could not be created, the Tools item still appears and does nothing.
6. **Missing dock.** If the dock did not register, an automatic update cannot show the banner. The Tools path still works.
7. **libobs's own log line.** A reply body that is not JSON also makes libobs log "Failed reading json string". That is one
   extra OBS log line, in that rare case.
8. **Existing long line.** `plugin-main.cpp` line 147 (HEAD line 139) is 121 columns. It predates this WP and is untouched.
9. **Windows is not built or run here.** The WinHTTP path (`http-win.cpp`) and the Win32 build are unverified. The M5 manual
   checklist should cover: the Tools item opens the dialog; a newer tag shows the banner and the Yes/No box; Download opens
   the release page; the setting off stops the checks; the first check comes about a minute after start; a failed check does
   not change `update-check.json`.
10. **libobs 30.0.2 headers only.** Every OBS function used exists in those headers with the signatures the brief gives.
    The OBS 31 headers were not available here.
11. **QTimer in `src/obs/`.** `update-checker.cpp` uses `QTimer`, which the layer rule does not name. `src/obs/replay-controller.cpp`
    already uses `QTimer` and `QObject`, so this follows precedent.

## Questions for the orchestrator

1. A second click while a manual check is in flight gets a second dialog, per the contract. Should such clicks be ignored
   while a manual check is pending instead?
2. Is it acceptable that `stop_all()` can wait on Windows for an in-flight update request (`http_client().shutdown()`), or
   should the update check skip that wait at EXIT?
3. Are the two small deviations acceptable: the `no_release` log wording (deviation 5) and the `last_check_loaded` flag
   (deviation 3)?
4. Should the Tools item be hidden or disabled when the update checker did not load?

## Orchestrator integration notes

- Read the whole diff; only the WP's files changed. Gates: plugin build with warnings as errors, no undefined
  `clip_atlas` symbols, 343/343 unit tests, check-format clean.
- Answers: (1) a second click gets its own answer, as the contract says. (2) Accepted: EXIT waits only if a check
  is in flight at that moment, bounded by WP5.2's 30 s deadline (plus send/receive timeouts); listed on the M5
  checklist's known limits. (3) Both deviations accepted. (4) The Tools item stays; it logs nothing and does nothing
  only in the near-impossible case that the checker failed to construct.
- `PLUGIN_VERSION` is an `extern const char *` here (plugin-support.h), not a macro; the header comment in
  `update-checker.hpp` says `"clip-atlas/" PLUGIN_VERSION` loosely; the code concatenates at run time, which is right.
