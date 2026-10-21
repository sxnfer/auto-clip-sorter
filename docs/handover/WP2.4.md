# WP2.4 General page: "What to sort" and "Save sound" groups: hand-over

## Summary

The General page of the settings dialog gained two groups, in this order: Clips, What to sort, Replay Buffer, Save sound,
then OK and Cancel. All the work is in `src/ui/settings-dialog.cpp`. The public API and the doc comments of
`settings-dialog.hpp` are unchanged; the header gained only private members and helpers.

- **What to sort:** Replays, Recordings and Screenshots check boxes (`sort_replays`, `sort_recordings`,
  `sort_screenshots`), then "File each recording under the game played longest" (`recording_dominant_game`). The last
  one is enabled only while Recordings is checked. While disabled it keeps its checked value and saves it.
- **Save sound:** "Play a sound when a replay is saved" (`play_save_sound`); a "Sound file (.wav)" row with a text field
  and Browse…; a Volume slider (0 to 100) with a "NN %" label whose minimum width is measured for "100 %"; a Test button;
  and one word-wrapped help line. The file row, volume row and Test button, labels included, are enabled only while the
  checkbox is on. The help line stays enabled.
- **Test** plays the file and volume shown in the dialog, before OK, through `sound_player()`. An empty file field shows
  an information box. A failure the player reports is shown as a warning, with the reason on the line after the message.
  A report from the player's thread is handed to the UI thread with `run_on_ui`, and it is dropped if the dialog has
  closed.
- **Strings:** 16 keys added to `data/locale/en-US.ini`, with the contract's exact text. No key was changed or removed.
  Browse… reuses the existing `Settings.Browse`.

**Build status:** the CI-preset build passes with warnings as errors and zero warnings. Until WP2.5 lands, the module has
two undefined symbols, `clip_atlas::sound_player()` and `clip_atlas::SoundPlayer::play`. See the first risk below.

## Files changed

- `src/ui/settings-dialog.cpp`: the two new groups, `browse_save_sound()`, `test_save_sound()`, and the new fields in
  `load()` and `accept()`. M1 code is unchanged apart from the comment rewrap listed under deviations.
- `src/ui/settings-dialog.hpp`: private section only. Adds `class QSlider;`, the helpers `browse_save_sound()` and
  `test_save_sound()`, and the members `sort_replays_`, `sort_recordings_`, `sort_screenshots_`,
  `recording_dominant_game_`, `play_save_sound_`, `save_sound_path_` and `save_sound_volume_`. The diff is additions only.
- `data/locale/en-US.ini`: 16 keys added (5 under What to sort, 11 under Save sound).
- `docs/handover/WP2.4.md`: this note.

Not touched: core, platform, obs, `plugin-main.cpp`, `CMakeLists.txt`, `en-GB.ini`. Nothing is committed. The scratch
harness lives outside the repository (see How it was tested).

## How it was tested

Commands run from the worktree root, `/home/user/auto-clip-sorter/.claude/worktrees/agent-adc85dbd7238cea79`.

1. **STEP 0.** `git merge --ff-only claude/m2-recordings-screenshots` succeeded. `git log -1 --oneline` shows
   `bc31ea2 WP2.0: tracker uses OBS's last recording path only as a fallback`.
2. **CI-preset build.** `cmake --preset ubuntu-ci-x86_64 && cmake --build --preset ubuntu-ci-x86_64`: exit 0. Compiled
   with `-Werror -Wextra -Wunused-parameter`, and the log has no warnings. Log:
   `/tmp/claude-0/-home-user-auto-clip-sorter/6c995450-a095-5948-9d9e-9af008631cf2/scratchpad/wp24/build-final.log`
   (the full first build is in the same folder as `build.log`).
3. **Undefined symbols.** `nm -C build_x86_64/clip-atlas.so | grep " U " | grep clip_atlas` prints exactly:
   - `clip_atlas::SoundPlayer::play(std::string const&, int, std::function<void (std::string const&)>)`
   - `clip_atlas::sound_player()`

   Both belong to WP2.5's `platform/sound-player-{win,stub}.cpp`, which this branch does not have yet.
4. **Strict warnings (MSVC substitute).** `g++ -std=c++17 -fsyntax-only` on `src/ui/settings-dialog.cpp` with the CI
   preset's include paths, plus `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast
   -Wcast-align -Wnon-virtual-dtor -Woverloaded-virtual -Wformat-security -Wunused -Wunreachable-code`: 0 warnings. MSVC
   is not available here, so this is an approximation.
5. **Unit tests.** `cmake -S tests -B build/tests && cmake --build build/tests && ctest --test-dir build/tests
   --output-on-failure`: 100 % passed, 175 of 175. There are no core changes in this WP.
6. **Format.** `clang-format-19 --dry-run --Werror src/ui/settings-dialog.cpp src/ui/settings-dialog.hpp`: clean. The
   file was formatted with `clang-format-19 -i`, which only rewrapped lines I had added. No line is wider than 120
   columns at tab width 8, the C++ setting in `.clang-format`.
7. **Locale.** Every key the dialog passes to `obs_module_text` (32 keys) is defined in `en-US.ini`. The 16 new lines were
   compared with the contract text in `git diff`.
8. **Offscreen behaviour harness** (scratch, not committed):
   `/tmp/claude-0/-home-user-auto-clip-sorter/6c995450-a095-5948-9d9e-9af008631cf2/scratchpad/wp24/harness/harness.cpp`.
   It builds the real `settings-dialog.cpp`, `settings-store.cpp`, `settings-model.cpp` and `clip-layout.cpp` against
   libobs 30.0.2 and Qt 6.4.2, and runs with `QT_QPA_PLATFORM=offscreen`. Stubs: `obs_module_text` (read through OBS's
   own `text_lookup` from the worktree's `en-US.ini`), `obs_log`, `obs_module_get_config_path` (a scratch folder),
   `obs_frontend_get_current_record_output_path`, `run_on_ui` (queues to the Qt thread from other threads) and
   `sound_player()` (records each call, and runs the `done` callback as each scenario scripts it). A watchdog timer
   answers every message box and file dialog.
   - **Result:** 71 checks, 0 failed. The same run under `-fsanitize=address,undefined` with LeakSanitizer: 71 of 71, no
     sanitizer reports.
   - **Covered:** group order; the contract texts; defaults (dominant enabled while Recordings is on; play-sound off
     disables the file, volume and Test rows and their labels, and the help line stays enabled); the What-to-sort
     toggles and the kept dominant value; the volume label at 0, 35 and 100 %; the label's minimum width; Test with a
     blank file (information box text); Test passing the trimmed path and the slider value (35), not the stored value;
     a synchronous failure (the Linux stub's behaviour) shows the warning with the reason on the next line; an
     asynchronous failure from another thread shows exactly one warning on the UI thread; a failure after the dialog
     closed shows nothing; Browse's title, filter and start folder (the typed file's folder), choosing sets the field,
     cancelling keeps it, and an empty field still picks; OK saves the new fields, keeps the fields the page does not
     show (`auto_sort`, `check_for_updates`, `use_product_name`, chosen sources, game rules), keeps the disabled
     dominant value, and `settings.json` reloads to the same settings; Cancel changes nothing; reopening shows the saved
     values; Browse and Test are not default buttons.
   - **Renders** (offscreen, Qt's default style, not OBS's theme; the dialog is 676 × 617 px at its default size):
     - `/tmp/claude-0/-home-user-auto-clip-sorter/6c995450-a095-5948-9d9e-9af008631cf2/scratchpad/wp24/out/01-defaults.png`
     - `/tmp/claude-0/-home-user-auto-clip-sorter/6c995450-a095-5948-9d9e-9af008631cf2/scratchpad/wp24/out/02-sound-on-recordings-off.png`
     - `/tmp/claude-0/-home-user-auto-clip-sorter/6c995450-a095-5948-9d9e-9af008631cf2/scratchpad/wp24/out/03-recordings-on.png`

     The defaults render shows the four groups in order and then OK and Cancel. Replays, Recordings and Screenshots are
     checked, and the dominant box is unchecked and enabled. Save sound is off, so its file, volume and Test rows and
     their labels are greyed, and the help line is in full colour. The sound-on render, with Recordings off, shows the
     dominant box greyed but still checked. The file field, the 42 % slider and Test are active. The help line fits on
     one line at this width. The wrap only shows in a narrower dialog.

## Deviations from the contract, and why

1. **Labels follow their rows.** The file, volume and Test rows sit in a nested `QFormLayout` inside a container
   (`sound_details`), so one `setEnabled()` greys their labels as well as the fields. The contract only named the rows.
2. **Test placement.** Test sits in the field column with an empty row label, at its natural width, so the button does
   not stretch. The contract did not specify where it goes.
3. **Error text.** The player's reason is converted with `to_qstring(error)`, the existing helper, not
   `QString::fromUtf8(error.c_str())`. The result is identical, and the rule is that strings cross only through the
   helpers. The message joins the two parts with `QLatin1Char('\n')` rather than a `"\n"` literal, which avoids an
   implicit `const char*` to `QString` conversion.
4. **Creation order.** The new groups are created between the existing ones, in visual order, so Tab order follows the
   page. The M1 statements keep their order.
5. **M1 comment rewrapped (comment only).** The comment "A closed dialog waits to be deleted..." in
   `open_settings_dialog()` was already 124 columns wide at tab width 8 in HEAD. I rewrapped it to 120 columns. No code
   changed.
6. **Volume label width.** The minimum width is measured with the same `percent_text()` helper that builds the live label,
   so the measured text is the locale's own suffix.

## Known gaps and risks

1. **Windows link waits for WP2.5.** `sound_player()` and `SoundPlayer::play` are undefined until
   `platform/sound-player-{win,stub}.cpp` exist. A Windows build of this branch alone will fail at link time with
   unresolved externals. Linux builds the module with the two symbols undefined. I did not check whether OBS can load it
   in that state. WP2.0's note that its commit is "safe to push (MSVC links)" no longer holds once this WP is in. plan.md's WP2.4 row
   already says it links with 2.5, so integrate 2.5 before the first Windows CI run of this branch.
2. **Not verified in OBS.** Not run on Windows or with MSVC, and not seen with OBS's own theme. The renders use Qt's
   default style offscreen. Native file-picker behaviour and `QDir::toNativeSeparators` are not exercised.
3. **Linux Test always fails.** The Linux stub reports "not supported on this platform", so Test shows a warning there.
   That is the player's contract, not a bug.
4. **Cross-thread copy of the `QPointer`.** The brief's design copies the `QPointer` on the player's thread, when the
   callback captures it and passes it to `run_on_ui`. Qt's weak-reference counts are atomic, so I expect this to be
   safe. The async check passes, but I did not run ThreadSanitizer on it. Question 1 asks whether to move it instead.
5. **Test while a sound plays.** Test is not disabled while a sound is playing. A second click restarts the sound, since
   the player replaces a playing sound.
6. **Browse from a typed folder.** A typed path that is a folder, not a file, makes the picker start in that folder's
   parent (`QFileInfo::absolutePath()`).
7. **Help line wrap.** Word wrap is on, but the dialog's size hint is its text's full width, so the line wraps only when
   the dialog is narrower than that. Not checked in a narrow window.
8. **Size.** The General page is 676 × 617 px at Qt's default font. That fits a 768-px-high screen. M4 splits the page.
9. **No placeholder** on the sound file field. The contract did not ask for one.
10. **`Settings.General`** stays unused, as in M1.

## Notes for other work packages

- **WP2.5:** define `sound_player()` in both `sound-player-win.cpp` and `sound-player-stub.cpp`. The header says the stub
  answers synchronously. The dialog's synchronous path is covered by a harness check that uses a synthetic synchronous
  error.
- **Harness gotcha (for offscreen harnesses).** Compile the harness with `-fPIC`. Without it the executable gets copy
  relocations of the Qt typeinfo and `staticMetaObject` symbols it references, and `QApplication` crashes during
  startup in `QGuiApplication::screenAdded`. The plugin is a shared module and has no copy relocations.

## Questions for the orchestrator

1. Accept the `QPointer` copy on the player thread, as briefed? Or should the callback move it, which needs a `mutable`
   callback because `done` runs once?
2. Will WP2.5 be integrated before any Windows CI run of this branch? If not, this WP fails to link there. I did not add
   a temporary stub, since `platform/` is outside my scope.
3. Accept the comment-only rewrap of the M1 line in `open_settings_dialog()` (124 to 120 columns)?
4. Is a 676 × 617 px General page acceptable until M4?

## Orchestrator integration notes

- Read the full diff; it stays within WP2.4's ownership. Gates in the main checkout: Ubuntu CI-preset build OK (only `sound_player()` and `SoundPlayer::play` undefined, from WP2.5), `build-aux/check-format` clean. Reviewed render `02-sound-on-recordings-off.png`.
- Answers: (1) the `QPointer` copy on the player thread is accepted (Qt's weak reference counts are atomic); (2) yes, committed locally and pushed only together with WP2.5; (3) the comment rewrap is accepted; (4) the page size is accepted until M4 splits it.
