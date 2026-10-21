# WP5.6 hand-over: docs (README, install guide, CHANGELOG) (worker)

## Summary

- `README.md` (edited in place): status now reads "first release, in testing", with M4 named as
  later. The old feature list is replaced by what the release does, and the M4 items are listed as
  planned. Short Install, Update, Uninstall and Troubleshooting sections point to `docs/install.md`.
  The title, tagline, folder-layout section and development section are unchanged.
- `docs/install.md` (new): the installer (including the SmartScreen steps), the zip, first start,
  updating, uninstalling, moving over from the Python script, and troubleshooting. It uses the
  on-screen names from `data/locale/en-US.ini`.
- `CHANGELOG.md` (new): Keep a Changelog format. `## [Unreleased]` has Added items grouped by M1,
  M2, M3 and M5, then Deprecated and Known limitations. No version number or date.
- The docs describe the first release as it will be once every work package has landed. Several of
  those features are not on this branch yet. See question 1.

## Files changed

| File | Change |
|---|---|
| `README.md` | modified |
| `CHANGELOG.md` | new |
| `docs/install.md` | new |
| `docs/handover/WP5.6.md` | new (this note) |

The three docs are staged with `git add -A`. Nothing is committed or pushed. `legacy/`, `plan.md`,
`docs/PLAN.md`, the source code, the workflows and the installer are unchanged.

## Checks run

| Check | Result |
|---|---|
| `git merge --ff-only claude/pensive-hamilton-cbg1l4`, then `git log -1 --oneline` | `af7b327 WP5.4: Hotkey to toggle auto-sort` |
| `git add -A && build-aux/check-format` | `all 76 C/C++ and 16 CMake files are formatted.`, exit 0. The script covers C/C++ and CMake only, so the markdown is not checked by it. |
| Relative links resolve (`ls` and a link script) | Every relative target exists in the repo. Every anchor exists in its target file. The only absolute URL is the Releases page. |
| Link script tested | Run once against a deliberately broken sample, where it flagged the missing file, the bad anchor, the disallowed URL and the skipped heading level. The script is in the session scratchpad, not the repo. |
| Heading structure | No skipped heading levels in the three files. |
| Markdown parse (pandoc, GitHub-flavoured) | All three render with no warnings. The HTML has no leftover `**`, backticks or link syntax. |
| Format | No emoji. Prose in the three user-facing docs is at most 100 columns, as in the original README. |

## How the facts were checked

Each claim in the docs, with the source it came from:

| Claim in the docs | Source |
|---|---|
| Tools → Clip Atlas Settings…, Docks → Clip Atlas, Tools → Check for Clip Atlas Updates…, Toggle Clip Atlas auto-sort | `data/locale/en-US.ini` (`SettingsMenuItem`, `Dock.Title`, `CheckForUpdatesMenuItem`, `Hotkey.ToggleAutoSort`); `src/plugin-main.cpp:329` (Tools item); `docs/handover/M3-dock-spec.md` (the dock lists under Docks); `src/obs/hotkeys.hpp` (listed in Settings → Hotkeys) |
| Setting names, group titles and defaults (Clips, What to sort, Replay Buffer, Save sound; monthly folders on; grace 10 s; volume 80; save sound off; updates check on; "Unsorted") | `en-US.ini` (`Settings.*`); `src/core/settings-model.hpp`; `src/ui/settings-dialog.cpp` (group boxes) |
| Dock header, banners, filter, footer, right-click items, Recycle Bin question defaults to No | `en-US.ini` (`Dock.*`); `docs/handover/M3-dock-spec.md` (safety rules); `src/ui/clip-dock.hpp` (header comment) |
| "Not sorted" in the game filter means clips directly in the clips folder | `src/core/clip-index.hpp` (file header and `has_unfiled()`) |
| The dock lists at most 5,000 of the newest clips | `src/core/clip-index.hpp` (`max_clips = 5000`); `docs/handover/M3-dock-spec.md` |
| Folder names come from the window title, cleaned up; no built-in list until M4 | `src/core/name-resolver.hpp` and `.cpp` (`clean_window_title`, resolution order); `src/core/clip-routing.cpp` (`clip_game_for` passes no product name and an empty bundled map); `plan.md` WP4.1 and WP4.2 (deferred) |
| Game Capture and Window Capture are both watched | `plan.md` WP1.7; `docs/handover/WP1.7.md` (window_capture emits hooked and unhooked in OBS 31.0.0) |
| A recording's game is decided when it starts | `plan.md` WP2.1 |
| Replay Buffer starts and stops on its own, after a 10 s grace period, and only stops a buffer it started; a user stop is not undone | `plan.md` (Decisions; WP1.3; decision log 2026-10-20); `docs/testing/M1.md` steps 4, 8, 11, 12 and 19 |
| Clips are copied and checked before the original is removed across drives; locked files retried for about 30 s | `src/core/file-mover.hpp`; `plan.md` WP1.5 |
| A name clash becomes `Replay (2).mkv` | `plan.md` WP1.5; the existing folder-layout section of the README |
| A replay remuxed to `.mp4` moves with its `.mkv` | `plan.md` WP1.8 |
| Save sound: off by default; Windows default playback device; Desktop Audio captures it; plays for replays only; no sound file bundled | `src/platform/sound-player.hpp` (header); `src/core/settings-model.hpp` (`save_sound_path`: "no default ships yet"); `plan.md` WP2.5 and Decisions; `docs/testing/M2.md` (Known limits) |
| Auto-sort switch and its tooltip | `en-US.ini` (`Dock.AutoSort`, `Dock.AutoSort.Tooltip`) |
| Update check: once a day, about a minute after start, published full releases only, nothing downloaded, banner with Download… and Dismiss, manual check offers to open the download page | `src/core/update-policy.hpp`; `src/obs/update-checker.hpp`; `en-US.ini` (`Update.*`, `Settings.CheckForUpdates*`, `Dock.Banner.Dismiss`); `plan.md` M5 |
| Installer: needs admin; installs to `C:\ProgramData\obs-studio\plugins\clip-atlas`; OBS-running message with Retry and Cancel; no folder page; no start after install; uninstall name "Clip Atlas (OBS plugin)"; the uninstaller asks you to close OBS; settings kept | `installer/clip-atlas.iss` (`PrivilegesRequired`, `DefaultDirName`, `DisableDirPage`, `UninstallDisplayName`, `ObsIsRunning`, `ConfirmObsClosed`, `InitializeUninstall`; no `[Run]` section, `RestartApplications=no`); `docs/handover/WP5.3.md` |
| Warning about an older copy in `%APPDATA%\obs-studio\plugins\clip-atlas`; setup never deletes it | `installer/clip-atlas.iss` (`CurStepChanged`, message text) |
| File names `clip-atlas-<version>-windows-x64.zip` and `…-windows-x64-installer.exe`; the zip holds a `clip-atlas` folder; the DLL is `bin\64bit\clip-atlas.dll` | `.github/scripts/Package-Windows.ps1` (`$OutputName`, `$InstallerName`, `$PluginDll`, `Compress-Archive` of `release/<cfg>`) |
| Releases page files; draft releases; `-beta` and `-rc` tags are pre-releases; checksums in the release notes; Linux packages are attached too | `.github/workflows/push.yaml` (`create-release` job) |
| Zip steps: delete the old folder, copy the new one in, admin prompt | `docs/testing/M2.md` "Download and install" steps 6 and 7 |
| First-start log line `[clip-atlas] loaded` | `src/plugin-main.cpp:343`; `docs/testing/M1.md` step 1 |
| Help → Log Files → View Current Log | `docs/testing/M1.md` step 1 (passed in the 2026-10-20 run) |
| Settings → Output → Replay Buffer → Enable Replay Buffer | `docs/testing/M1.md` ("You need") |
| The Python script: Tools → Scripts; remove it; it logs a deprecation warning; running both makes them fight over a clip | `legacy/README.md`; `legacy/rb_event_driven.py` line 326 |
| Legacy banner is not in this release | `docs/handover/M3-dock-spec.md` ("The legacy-script banner waits for M4") |
| Thumbnails come from Windows' own thumbnail cache; the file type's icon is shown when Windows has none | `src/platform/thumbnails.hpp` (header) |
| Clip Atlas needs OBS 31 or newer, Windows only | `plan.md` (Decisions); `buildspec.json` (obs-studio 31.0.0); `docs/testing/M1.md` ("You need") |
| Closing OBS within a few seconds may leave a file unsorted; a Scene Collection switch can make Clip Atlas miss a stop; auto-remux adds a few seconds | `docs/testing/M2.md` (Known limits) |
| Planned, not in this release: built-in games list, rules editor, full settings pages, legacy-script import | `plan.md` M4 (deferred); the user's deferral note (2026-10-21) |

Facts from outside the repo, or not confirmed:

- `Help → Log Files → Upload Current Log File`: from the brief. Not in the repo.
- `Start Replay Buffer` as the button label in OBS: general OBS knowledge. Not in the repo.
- The SmartScreen wording ("Windows protected your PC", "More info", "Run anyway", "Unknown
  publisher"): from the brief, and standard Windows text. Not in the repo.
- The Inno Setup labels "I accept the agreement", "Next", "Install" and "Finish": Inno defaults.
  "Retry" and "Cancel" come from the script itself.
- The `.mkv` thumbnail statement (a codec pack that gives Windows a thumbnail handler): from the
  brief. `thumbnails.hpp` does not say it.
- `%APPDATA%\obs-studio\plugin_config\clip-atlas\` as the settings folder: from the brief, and the
  standard OBS module config location. `obs_module_config_path` is used in
  `src/obs/settings-store.hpp`, but the OBS source was not checked here.
- "Apps & features" as the Windows 10 name for Settings → Apps → Installed apps: general Windows
  knowledge.
- `certutil -hashfile <file> SHA256`: a standard Windows command. The checksum tip is my addition,
  marked optional in `install.md`.

## Left out and why

- **M4 features beyond a "planned" line**: the built-in games list, the rules editor, the full
  settings pages, the legacy-script import and the legacy-script banner. The user deferred M4 on
  2026-10-21, so they are named as planned only.
- **Dock keyboard shortcuts** (double-click or Enter to open, Delete to recycle, F2 to rename) from
  the `clip-dock.hpp` header comment. They are not in the brief, and WP3.6 has not landed on this
  branch. Add them once the dock is in.
- **Per-profile hotkey bindings** (`WP5.4`: each profile keeps its own binding). Left out to keep the
  guide short. It could be one sentence.
- **Settings save failure message** (`Settings.SaveFailed`) and the `settings.json.bad` recovery
  (`SettingsStore::load`). Not in the brief. A user could see either one, so say if you want them.
- **Silent installs.** WP5.3 notes that the installer's message box blocks `/VERYSILENT`. That is not
  a supported path, so it is not documented.
- **Build, CI and packaging detail** beyond the file names. These are for developers, not users.
- **Version numbers and dates.** Left out as instructed. The CHANGELOG has `## [Unreleased]` only.

## Questions for the orchestrator

1. **Features not on this branch yet.** The docs describe the release once every WP has landed.
   On this branch the dock is not registered (WP3.1 to WP3.4 and WP3.6 are not here; there is no
   `clip-dock.cpp`, `clip-index.cpp`, `clip-list-model.cpp`, `thumbnails-*.cpp` or `shell-ops-*.cpp`).
   The update checker, the Tools menu item, the Updates setting and the update banner are not wired
   (WP5.2 and WP5.5). The hotkey is not wired into `plugin-main.cpp` (WP5.5). Please confirm these
   land before the docs go out. If they don't, I will cut the matching sentences.
2. **Where the update setting lives.** The docs say **Tools → Clip Atlas Settings… → Updates → Check
   for updates once a day**. Confirm that WP5.5 puts it there.
3. **Sound file.** The docs say no sound file ships in this release. Confirm that, or give me the
   file name and its licence status so I can say so.
4. **OBS facts not checked here.** Please confirm against OBS 31 (no OBS source is available in this
   environment): `Help → Log Files → Upload Current Log File` and the `Start Replay Buffer` label.
5. **Thumbnail statement.** The `.mkv` codec-pack line comes from the brief. Confirm it, or I will
   soften it.
6. **Settings path.** Confirm `%APPDATA%\obs-studio\plugin_config\clip-atlas\` against the OBS source.
7. **`legacy/README.md` is out of date.** It says "Advanced → Import settings from the Python script
   (coming in v1)". The Advanced page and the import are both M4. `legacy/` is outside my scope, so I
   did not touch it. Please decide whether to update it.
8. **Linux files on the Releases page.** `push.yaml` attaches the `.deb`, `.ddeb`, `.tar.xz` and source
   files to every release. `install.md` tells Windows users to ignore them. Confirm that is
   acceptable, or leave them off the release.
9. **First tag.** The update check only offers published, non-pre-release releases. A `-beta` or
   `-rc` tag will not trigger the banner. The docs say so. Confirm that the first public tag is a bare
   `X.Y.Z`.
10. **Licence page.** WP5.3 notes that `LICENSE` uses LF-only line endings, which may show as one long
    line on Inno's licence page. `install.md` step 5 assumes it is readable. Check it in the first
    Windows run. The file is not mine to change.
11. **Window Capture.** The README says Window Capture sources are watched too. The basis is WP1.7.
    The user's M1 run did not record step 17 (Window Capture). Confirm it, or I will limit the line
    to Game Capture.
12. **Checksum tip.** The optional `certutil` line in `install.md` is my addition. Keep it or drop it.
13. **plan.md status.** WP5.3 is still marked not started, but the installer and its hand-over note
    are on this branch. Not my file, so I am only noting it.

## Orchestrator integration notes

- Answers: (1) the dock (WP3.2, WP3.4, WP3.6) and the update checker and hotkey wiring (WP5.5) land in the same pull
  request before release; the docs are re-read at the M5 gate. (2) Yes, General page → Updates. (3) No sound file
  ships. (4) Confirmed in OBS 31.0.0: `Basic.MainMenu.Help.Logs.UploadCurrentLog` = "Upload Current Log File",
  `Basic.Main.StartReplayBuffer` = "Start Replay Buffer". (6) Confirmed: OBS's plugin config root is
  `obs-studio/plugin_config` under the app config path (`UI/obs-app.cpp:554`). (7) `legacy/` must not be modified
  (CLAUDE.md), so its README stays. (8) Accepted. (9) Yes. (10) and (5) are on the M5 checklist. (11) Window Capture
  emitting hook signals was confirmed in OBS 31 source for WP1.7. (12) Kept.
