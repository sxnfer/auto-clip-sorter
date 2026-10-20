# WP1.6 obs/ui-thread + obs/settings-store: hand-over

## Summary

Implemented both contracts in `src/obs/`: `run_on_ui()`, `is_ui_thread()` and `ui_tasks_shutdown()` in
`ui-thread.cpp`, and `SettingsStore`, the obs_data reader and writer, and `settings_store()` in
`settings-store.cpp`. The `ubuntu-ci-x86_64` build compiles both with warnings as errors. A scratch harness that
runs the real code against the installed libobs passes 101 of 101 checks under ASan and UBSan, with no leaks.

**One contract statement does not hold on OBS's UI thread.** `ui-thread.hpp` says `run_on_ui()` never runs `fn`
synchronously, even on the UI thread. OBS 31's own UI task handler runs the task before `obs_queue_task()` returns
when it is called on the UI thread, so there `run_on_ui()` is synchronous. I implemented the specified mechanism
(`obs_queue_task`, `wait = false`) and documented the behavior in `ui-thread.cpp`. No approved API can defer on the
UI thread. This needs your decision before WP1.8 (Question 1).

**Three OBS calls are outside the approved list.** Reading a record generically requires listing each object's
fields, so I use `obs_data_first()`, `obs_data_item_next()` and `obs_data_item_get_name()`. They exist in the
installed 30.0.2 headers and in the OBS 31.0.0 source. I flag them under Deviations and ask for approval (Question 2).

## Files changed

- `src/obs/ui-thread.cpp` (new)
- `src/obs/settings-store.cpp` (new)
- `docs/handover/WP1.6.md` (new, this note)

Nothing else was touched. No header, CMake file or `legacy/` file changed. `git status` shows only the three paths
above. Nothing is committed.

## How it was tested

Commands run from the worktree root, `/home/user/auto-clip-sorter/.claude/worktrees/agent-a48662fad69582bef`.

1. **STEP 0.** `git merge --ff-only claude/m1-feature-parity` fast-forwarded `77f7bcd..076a89f`.
   `git log --oneline -1` shows `076a89f Fix sanitize_folder_name for device names with an extension`. The three
   files exist.
2. **(a) CI preset build.** `cmake --preset ubuntu-ci-x86_64 && cmake --build --preset ubuntu-ci-x86_64` gives exit 0,
   with `CMAKE_COMPILE_WARNING_AS_ERROR=ON`, and no compiler warnings for either file. A baseline build before the
   new files was also exit 0. Configure prints the same "ENABLE_CCACHE not used" note as WP1.4. Symbol check
   `nm -C build_x86_64/clip-atlas.so | grep -E "run_on_ui|SettingsStore"` shows `clip_atlas::run_on_ui(std::function<void ()>)`
   and the `SettingsStore` members. They are local symbols (`t`). Only the `obs_module_*` entry points are
   exported (`T`).
3. **(b) Scratch round-trip harness.** Source is `/tmp/claude-0/-home-user-auto-clip-sorter/7156fddb-48fe-5a2e-b1b6-f895365608ba/scratchpad/wp16/harness/harness.cpp` (not committed). It compiles the
   real `src/obs/settings-store.cpp`, `src/obs/ui-thread.cpp`, `src/core/settings-model.cpp` and
   `src/core/clip-layout.cpp` with `g++ 13.3 -std=c++17 -fsanitize=address,undefined -Wall -Wextra -Wpedantic
   -Wshadow -Wconversion -Wsign-conversion -Werror` and links `-lobs` (libobs 30.0.2). The real `obs_data_*` and
   `os_*` functions run. Stubbed: `obs_module_get_config_path` (points at a scratch folder),
   `obs_frontend_get_current_record_output_path`, `obs_log` (captured) and `obs_queue_task` /
   `obs_in_task_thread` (a queue the harness drains on the test thread). Result: **101 checks, 0 failed**, exit 0,
   no sanitizer or LeakSanitizer reports. Covered:
   - Round trip with every field customised: UTF-8 path, quotes and backslashes in a display name, padded and
     duplicate UUIDs, an uppercase executable. Result: `load(save(s)) == validated(s)`.
   - Wrong-typed keys keep defaults: `"monthly_folders": "yes"`, `"grace_period_seconds": 4.5`,
     `"replay_auto_start": 1`, an array where a string belongs, a number inside a string list, a boolean `ignore`.
     Other keys in the same file still load.
   - Corrupt file: renamed to `settings.json.bad`, defaults used, warning names both files. An older `.bad` is
     replaced. An empty file is treated as corrupt.
   - Missing file: defaults, nothing created. No config path: warning, defaults, and `set()` returns false but applies.
   - Save into a blocked folder: returns false, settings still applied, listeners still called.
   - Second save keeps the previous file as `settings.json.bak`, with no `.tmp` left behind.
   - Listeners: ascending id order. A thrown `std::exception` and a thrown `int` are caught and logged, and the rest still
     run. Unsubscribe during notify skips the removed listener. Subscribe during notify waits for the next change.
     A listener whose destructor unsubscribes another listener is safe. A nested `set()` from a listener leaves every
     listener on the newest value.
   - `effective_clips_folder()`: own setting, then recording path, then `""`.
   - UI thread: empty function ignored; FIFO order; `wait` is false; a throwing task is logged and later tasks still run;
     after `ui_tasks_shutdown()` nothing new is queued and queued tasks are dropped.
4. **(c) Format.** `clang-format-19 --dry-run --Werror src/obs/ui-thread.cpp src/obs/settings-store.cpp` gives exit 0
   (clang-format 19.1.1). The first run flagged three wrapped lines in `load()`. I applied `clang-format-19 -i` to
   `settings-store.cpp` (my file only) and re-checked.
5. **(d) Unit tests.** `cmake -S tests -B build/tests && cmake --build build/tests -j4 && ctest --test-dir build/tests
   --output-on-failure` gives **99 of 99 passed**. This WP does not touch core.
6. **(e) Warnings.** MSVC is not available here. `clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Wpedantic -Wshadow
   -Wconversion -Wsign-conversion -Wold-style-cast -Wcast-align -Wnon-virtual-dtor -Woverloaded-virtual -Werror`
   over both files gives exit 0. For /W3 I reasoned about the code: explicit `size_t` indices, no unused parameters, and
   every `obs_log` format is a literal (`-Wformat-security`).
7. **Evidence for the UI-thread finding (Question 1).** `/tmp/claude-0/-home-user-auto-clip-sorter/7156fddb-48fe-5a2e-b1b6-f895365608ba/scratchpad/wp16/qt-sync/main.cpp` uses Qt 6.4.2 and calls
   `QMetaObject::invokeMethod(obj, fn, Qt::AutoConnection)`, the same rule OBS uses. From the UI thread the task ran
   before the call returned (yes). From a worker thread it did not run before the call returned (no), and it ran after
   `processEvents()` (yes). OBS 31.0.0 source: `libobs/obs.c` `obs_queue_task` (~line 3128) and
   `obs_set_ui_task_handler` (~line 3200, which marks the UI thread by running `set_ui_thread` through the handler);
   `UI/obs-app.cpp` lines 1224-1231 (`ui_task_handler` uses `wait ? WaitConnection() : Qt::AutoConnection`).

The JSON the store wrote in the round trip, verbatim (one line, the file as saved):

```json
{"schema_version":1,"clips_folder":"D:\\Clips Ü日本","monthly_folders":false,"unsorted_folder":"Ungrouped","auto_sort":false,"sort_replays":false,"sort_recordings":false,"sort_screenshots":false,"replay_auto_start":false,"replay_auto_stop":false,"grace_period_seconds":45,"source_selection":"chosen","chosen_sources":[{"value":"0f7c2a9e-1b2c"},{"value":"aabbccdd-0000"}],"use_product_name":false,"recording_dominant_game":true,"game_rules":[{"display_name":"Elden \"Ring\" \\ edition","executable":"eldenring.exe","ignore":"false"},{"display_name":"","executable":"launcher.exe","ignore":"true"}],"check_for_updates":false}
```

Key order is OBS's, not the order of `save_settings()`. Everything else is as the writer produced it.

## Deviations from the contract, and why

1. **`run_on_ui()` is synchronous on OBS's UI thread.** The header says "never synchronously, even when called on the
   UI thread". On the UI thread, OBS 31 calls the task directly (see How it was tested, item 7). The only
   deferral available through the approved API is for calls from other threads. I kept the specified mechanism and
   documented the behavior in `ui-thread.cpp`. A `run_on_ui()` call made on the UI thread runs `fn` inside that call. The
   exception catch still applies, so nothing propagates to the caller.
2. **Three OBS functions outside the approved list.** `obs_data_first()` (returns a new reference),
   `obs_data_item_next()` (releases the old reference and returns the next, also a new reference) and
   `obs_data_item_get_name()` (borrowed name). All three are in the installed 30.0.2 `obs-data.h` and in OBS 31.0.0
   `libobs/obs-data.c`. Needed because the reader must read every string field of a record generically, as the
   contract requires. Approval requested (Question 2).
3. **Listener behavior, slightly beyond "iterate over a copy".** `notify_listeners()` iterates a snapshot of the map as
   specified. It also skips a listener unsubscribed earlier in the same pass, so unsubscribing means no further calls. Each
   call gets its own copy of the settings, not a reference to the live `settings_`, so a nested `set()` cannot
   change the argument a listener is reading. `unsubscribe()` moves the listener out before the erase, so a listener
   whose destructor calls back into the store cannot change the map mid-erase.
4. **`load()` does not notify listeners.** The header is silent on this. Subscribers should read `get()` after load.
   See Question 6.
5. **Exceptions.** `load()` never throws: `std::exception` is caught, logged, and defaults are kept. The listener loop
   and the write path catch their own exceptions. `set()` can still throw `std::bad_alloc` from `validated()`, and
   `subscribe()` and `effective_clips_folder()` can throw it from allocation. I did not catch those, since
   allocation failure is not recoverable in these paths.

## Known gaps and risks

1. **UI-thread payload leak.** A task still queued at process exit never runs, so its small heap payload is not freed.
   If OBS has no UI task handler, it logs an error and drops the task, which leaks the payload too. OBS Studio's UI always
   sets one.
2. **POSIX save window.** `obs_data_save_json_safe()` renames the old `settings.json` to `settings.json.bak`, then
   renames the temp file into place (`libobs/util/platform-nix.c`, `os_safe_replace`). A crash between the two leaves no
   `settings.json`. `load()` then uses defaults and does not read `.bak`. Windows uses `ReplaceFileW` and has no such
   window, so this matters only for Linux builds. See Question 5.
3. **No fsync.** OBS's save helper does not flush before the rename. After a power cut right after a save, the new
   file could be empty on some filesystems. `load()` would then move it to `.bad` and use defaults. Not reproduced here.
4. **Invalid UTF-8 is dropped silently.** Measured in the harness: after saving `clips_folder` containing the bytes
   `0xFF 0xFE`, the key is missing from the file, and it reloads as `""`. The rest of the save succeeds and `set()`
   reports success. The store does not validate UTF-8. See Question 4.
5. **Non-object JSON.** A valid file whose root is not an object (measured with `[1, 2, 3]`) is read as defaults and is
   not moved to `.bad`. OBS reads it without error, so there is no `.bad` file and no log line. The next save replaces
   it, and the old file survives as `settings.json.bak`.
6. **Empty file.** Moved to `.bad`, because OBS cannot parse empty input. This matches the contract and is covered by the
   harness.
7. **`.bad` overwrite.** An older `settings.json.bad` is replaced, as the contract says, so the previous corrupt copy is
   lost. See Question 3.
8. **Hand-edited values.** A string list written as plain strings (`["a"]`) reads as empty, because OBS drops non-object
   array elements when it imports JSON. A boolean `ignore` in a game rule reads as false, because only string fields are
   read. The store always writes strings, so this affects only hand-edited files.
9. **Windows only verified from source.** `os_rename` uses `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING`, so `.bad`
   replacement works. `os_safe_replace` uses `ReplaceFileW` and falls back to `MoveFileExW` when the target is missing,
   so the first save works. Nothing was run on Windows and MSVC was not run.
10. **Stubbed in the harness.** `obs_module_config_path()` and the recording path were stubbed, so real OBS paths were not
    exercised. The real location is the one `obs_module_get_config_path()` builds from OBS's plugin config folder plus
    `clip-atlas/settings.json` (read from the 31.0.0 source, not run in OBS).

## Questions for the orchestrator

1. **(Blocking before WP1.8 and WP1.9.)** `run_on_ui()` called on the UI thread runs synchronously, which contradicts
   the header. Options: (a) accept it, correct the comment in `ui-thread.hpp` in a follow-up, and check that UI-thread
   callers (WP1.8 frontend events, WP1.9 replay controller) do not assume deferral; (b) allow Qt inside `ui-thread.cpp`
   (`QMetaObject::invokeMethod` with `Qt::QueuedConnection`), which breaks the `src/obs/` layering rule. I recommend
   (a).
2. Approve `obs_data_first`, `obs_data_item_next` and `obs_data_item_get_name` for the approved list? They are needed
   to read records without changing the header.
3. Replacing an older `settings.json.bad` loses the previous corrupt copy. Keep that, or number the copies?
4. Should UTF-8 be checked in `validated()` (core, `settings-model`), so a bad string is rejected or repaired instead of
   dropped on save?
5. Should `load()` fall back to `settings.json.bak` when `settings.json` is missing? That would close the POSIX window in
   gap 2.
6. Should `load()` notify listeners? I did not, because the header is silent.
7. Please confirm deviation 3 (listeners skipped once unsubscribed, and per-listener copies of the settings).

## Notes for other work packages

- Subscribers must call `unsubscribe()` on unload. `obs_module_unload()` should call `ui_tasks_shutdown()` first, as the
  header says. Neither is wired here, since `plugin-main.cpp` is read-only for this WP.
- `obs_module_load()` should call `settings_store().load()` (also not wired here).
- After `ui_tasks_shutdown()`, `run_on_ui()` does nothing.

## Orchestrator integration notes

- Reviewed both files in full. Gates re-run in the main checkout: check-format clean, full suite green, Ubuntu CI-preset plugin build OK.
- Answers:
  1. Option (a). The orchestrator verified `ui_task_handler` in `UI/obs-app.cpp` (31.0.0) and corrected `ui-thread.hpp`: synchronous on the UI thread, queued from other threads. The WP1.8 and WP1.9 briefs say so.
  2. Approved: `obs_data_first`, `obs_data_item_next` and `obs_data_item_get_name`, all checked in 31.0.0 `obs-data.h`.
  3. Keep replacing `.bad`.
  4. No UTF-8 validation. Strings come from Qt and OBS, which are valid UTF-8.
  5. No `.bak` fallback. Windows (`ReplaceFileW`) has no window, and Linux is a compile check only.
  6. `load()` doesn't notify: it runs in `obs_module_load` before anyone subscribes. Subscribers read `get()`.
  7. Deviation 3 accepted.
