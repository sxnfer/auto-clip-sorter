# WP1.10 Minimal settings dialog (General page): hand-over

## Summary

Implemented the General page of the settings dialog in `src/ui/settings-dialog.cpp`, against the unchanged contract in
`settings-dialog.hpp`. The dialog is non-modal and has two group boxes, "Clips" (clips folder with Browse…, monthly
folders, unsorted folder name) and "Replay Buffer" (start and stop toggles, grace period 0 to 300 s), then OK and
Cancel. `open_settings_dialog()` raises the open window instead of opening a second one, and the window deletes itself
on close. The locale file gained the new keys, and the unused `Settings.Seconds` key is gone.

The CI-preset plugin build passes with warnings as errors. The unit suite passes 131 of 131. A scratch harness runs the
real dialog, store and settings model offscreen and checks the behaviour: **94 checks, 0 failed**, also under
AddressSanitizer, UndefinedBehaviorSanitizer and LeakSanitizer (see How it was tested). Menu wiring is not in this WP:
nothing calls `open_settings_dialog()` yet.

## Files changed

- `src/ui/settings-dialog.cpp` (new)
- `data/locale/en-US.ini` (added `Settings.Clips`, `ClipsFolder.BrowseTitle`, `Browse`, `ReplayBuffer`,
  `SecondsSuffix`, `SaveFailed`, `OK`, `Cancel`; removed `Settings.Seconds`, which nothing referenced)
- `docs/handover/WP1.10.md` (this note)

Not touched: the header, the other `src/` files, CMake, `plugin-main.cpp`, `legacy/`. Nothing is committed.

## How it was tested

Commands run from the worktree root, `/home/user/auto-clip-sorter/.claude/worktrees/agent-abef1e2645d70add1`.
Logs are in `/tmp/claude-0/-home-user-auto-clip-sorter/7156fddb-48fe-5a2e-b1b6-f895365608ba/scratchpad/wp110/`.

1. **STEP 0.** `git merge --ff-only claude/m1-feature-parity` fast-forwarded `77f7bcd..e754231`. `git log --oneline -1`
   shows `e754231 WP1.6: obs/ui-thread + obs/settings-store`, and both required files exist.
2. **(a) CI preset build.** `cmake --preset ubuntu-ci-x86_64 && cmake --build --preset ubuntu-ci-x86_64` gives exit 0 for
   both steps. The preset sets `CMAKE_COMPILE_WARNING_AS_ERROR=ON`, and the compile line for `settings-dialog.cpp`
   carries `-Werror`. Every build log, from the first build to the last, has zero warnings. `nm -D --defined-only` on
   the built `clip-atlas.so` shows the `obs_module_*` entry points as its only global functions (the rest are weak STL
   instantiations), so `open_settings_dialog` is hidden, as the template's `-fvisibility=hidden` intends. The wiring WP
   calls it from the same DLL.
3. **(a) Format.** `clang-format-19 --dry-run --Werror src/ui/settings-dialog.cpp` gives exit 0. The first run flagged
   one wrapped line. I applied `clang-format-19 -i` to my file only. `ReflowComments` is off in this repo, so I
   rewrapped three comments over 120 columns by hand (tab width 8). Re-measured: no line is wider than 120 columns.
4. **(a) Unit suite.** `cmake -S tests -B build/tests && cmake --build build/tests -j4 && ctest --test-dir build/tests
   --output-on-failure` gives **131 of 131 passed**.
5. **(b) Behaviour harness.** Source: `scratchpad/wp110/harness/harness.cpp`, not committed. Build script:
   `scratchpad/wp110/harness/build.sh`. It compiles the real `src/ui/settings-dialog.cpp`, `src/obs/settings-store.cpp`,
   `src/core/settings-model.cpp` and `src/core/clip-layout.cpp`, and links libobs 30.0.2 and Qt 6.4.2 (run with
   `QT_QPA_PLATFORM=offscreen`). The locale is read through OBS's own `text_lookup_*` from the real `en-US.ini`.
   The stubs are the functions OBS defines in the plugin itself (`obs_module_text`, `obs_current_module`), plus
   `obs_module_get_config_path` (a scratch folder), `obs_frontend_get_current_record_output_path` (settable) and
   `obs_log` (captured). Result: **94 checks, 0 failed**. Covered:
   - Defaults and layout: non-modal, WA_DeleteOnClose, parent, title, every label and button from the locale file,
     the placeholder naming the OBS recording folder, spin range 0 to 300, suffix " s".
   - OK: the clips folder is trimmed and stored as UTF-8 (`Clips Ü日本`), the other toggles and the grace period are
     written, the fields the page doesn't show keep their values (`auto_sort`, `check_for_updates`,
     `use_product_name`, chosen sources, game rules), and `settings.json` reloads to the same settings.
   - Cancel, Esc and the close box: the store and `settings.json` stay byte-identical, and the window is deleted.
   - `open_settings_dialog()` called three times leaves one window, and the same one. After close, the next call opens
     a new window.
   - Placeholder: a stored folder cleared by the user shows the words "Use OBS recording folder". No recording folder,
     or an empty one, also shows the words.
   - Browse: an empty field opens the picker at the OBS recording folder. A typed folder is the starting point. The
     chosen folder is written to the field. Cancel in the picker leaves the field as it was.
   - Spin clamp to 300, and an empty unsorted name is stored as "Unsorted".
   - Save failure: a regular file where the settings folder should be makes `set()` return false. The warning is shown
     once, titled with `Settings.Title`, with the text of `Settings.SaveFailed`. The window still closes, the change
     applies for the session, and the failure is logged.
   - Every `obs_module_text()` key the dialog uses exists in `en-US.ini`.

   The same harness, built with `SANITIZE="-fsanitize=address,undefined"`, gives 94 of 94 with no AddressSanitizer,
   UndefinedBehaviorSanitizer or LeakSanitizer reports. Both runs used the final source.

   Screenshots (offscreen, Qt's Fusion style, not OBS's theme; I could not look at them in OBS):
   - `scratchpad/wp110/out/01-defaults.png`: fresh settings
   - `scratchpad/wp110/out/02-edited.png`: values edited before OK
   - `scratchpad/wp110/out/03-cleared-stored-folder.png`: stored folder cleared, placeholder in words
6. **(c) Warnings and narrowing.** MSVC is not available here. Instead, `g++ 13.3` builds the four project translation
   units with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wcast-align
   -Wnon-virtual-dtor -Woverloaded-virtual -Wformat-security -Werror` and passes. Qt and OBS headers are `-isystem`, so
   their warnings don't count. By inspection: no `QString::size()` (a `qsizetype`) is narrowed to `int`, the only
   `int` values are the spin-box value and its range, and no parameter is unused.

## Deviations from the contract, and why

1. **Placeholder in words when a folder is stored.** The contract says to show `store.effective_clips_folder()` as the
   placeholder. That returns the stored folder when one is set, so after the user clears the field it would show the
   stored path, which is not where clips go. With no stored folder, the placeholder is the effective folder (the
   recording path), as specified. With a stored folder, it is `Settings.ClipsFolder.Default` ("Use OBS recording
   folder"). Question 1.
2. **OK and Cancel labels come from the locale file** (`Settings.OK`, `Settings.Cancel`). The standard button roles are
   kept, so Enter, Esc and the platform button order work. CLAUDE.md says every user-facing string goes through
   `obs_module_text`, and Qt's labels would bypass that. Question 2.
3. **Exception paths.** `accept()` catches `std::exception` and logs it, but the dialog stays open, since nothing was
   applied and what the user typed is still there. The contract didn't say whether to close. Question 4.
   `open_settings_dialog()` also catches around creation, so no exception reaches the Qt event loop. The contract asked
   only for `accept()` and the browse handler.
4. **Unsorted name not trimmed in the dialog.** `validated()` runs `sanitize_folder_name()`, which trims it and turns an
   empty name into "Unsorted". The clips folder is trimmed, as specified.
5. **Button autoDefault off for Browse (addition).** Browse doesn't take the default-button role, so Enter goes to the
   dialog's default button, OK. The harness does not press Enter, so this is by design, not tested.
6. **Group boxes.** The contract allowed "Clips" and "Replay Buffer" group boxes. I used them, with `Settings.Clips` and
   `Settings.ReplayBuffer` as the titles.
7. **Minimum width on the clips field (addition).** It is 40 characters in the widget's own font, so a typical path
   isn't clipped. The first screenshot showed the default path cut off without it. This is a minimum, not a fixed size.
8. **A closed dialog does not count as open (interpretation).** The contract says "if it is alive, show it". A dialog
   that has closed but is not yet deleted is hidden, so `open_settings_dialog()` creates a new one. Re-showing it would
   bring back a window that is about to be deleted. The harness covers the normal close-then-reopen path, not this gap.

## Known gaps and risks

1. **Placeholder can go stale.** It is set when the dialog is created. If OBS's recording folder changes while the
   dialog stays open, it shows the old path until the dialog is reopened.
2. **Unload with the dialog open.** Nothing in the contract closes the dialog from `obs_module_unload()`. The dialog is
   a child of OBS's main window, so OBS deleting that window should delete the dialog before the DLL unloads. I did not
   verify that in OBS. Question 3.
3. **The folder picker reads directories on the UI thread.** That is Qt's standard picker. It runs only after the user
   clicks Browse, and the plugin does no file I/O of its own in the dialog. CLAUDE.md's rule is about plugin I/O, but
   I'm noting it.
4. **Not tested in OBS, on Windows, or with OBS's theme.** The harness runs offscreen with Qt's Fusion style. MSVC is not
   available here, so the `/W3` check is by compiler approximation and by reading the code. The native Windows picker
   and `QDir::toNativeSeparators` are not exercised.
5. **Save failure is a modal warning.** It blocks the window until dismissed, which is intended, but it reads as an
   error. The text says that the change applies for the session.
6. **`Settings.General` is unused.** It is the page name for the M4 sidebar, so I left it in the locale file.

## Notes for other work packages

- **Wiring (not in this WP).** `obs_module_load()` should call `settings_store().load()`, and the Tools menu item should
  call `open_settings_dialog(settings_store(), <OBS main window>)` on the UI thread. Both belong to the WP that edits
  `plugin-main.cpp`.
- **Harness gotcha (affects other harnesses).** A scratch executable that links Qt and calls `qobject_cast<T>` or
  `findChildren<T>()` on Qt classes matches nothing. The executable gets its own copy of `T::staticMetaObject` through a
  copy relocation, and the library's objects point at the library's copy. The harness uses `dynamic_cast` and direct
  children instead. The plugin is a shared module, so it has no copy relocations, and the dialog doesn't rely on this.

## Questions for the orchestrator

1. Accept deviation 1 (the placeholder shows "Use OBS recording folder" when a folder is stored)?
2. Accept deviation 2 (OK and Cancel labels from `Settings.OK` and `Settings.Cancel`)? If not, I'll switch to Qt's
   standard labels and drop those two keys.
3. Who closes the dialog during unload? Options: (a) `plugin-main` deletes it before `obs_module_unload` returns, which
   needs a small close function in the header, a header change that is not mine; (b) rely on OBS deleting the main
   window first. I recommend (a).
4. Keep the dialog open after an unexpected exception in `accept()` (current), or close it?
5. Keep `Settings.General` in the locale file for M4, or remove it now?

## Orchestrator integration notes

- Reviewed the code and the offscreen screenshots. Gates re-run in the main checkout: check-format clean, Ubuntu CI-preset plugin build OK.
- Answers: (1) accepted. (2) Accepted: `Settings.OK` and `Settings.Cancel` follow the CLAUDE.md string rule. (3) Option (a): the orchestrator added `close_settings_dialog()` to `settings-dialog.hpp`/`.cpp`, which deletes the dialog immediately, and WP1.8 calls it from `obs_module_unload()`. (4) Keep the dialog open. (5) Keep `Settings.General` for M4.
