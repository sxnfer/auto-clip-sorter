# Changelog

All notable changes to Clip Atlas are listed here. The format follows Keep a Changelog.

## [Unreleased]

### Added

- **Updates install themselves when you close OBS.** When a newer version is out, Clip Atlas downloads its installer
  in the background, checks it against the SHA-256 checksum GitHub lists for it, and the dock's banner says it is
  ready. After you close OBS, Windows asks for permission, and the installer updates Clip Atlas with only a progress
  window. It works for copies installed with the installer; a copy from the zip still gets the release page. Turn it
  off with **Download updates and install them when OBS closes** under **Updates** in Settings.

## [1.1.2] - 2026-10-26

### Added

- **Sort clips into a folder for each game** in Settings. It is on by default, as before. Turn it off and every clip
  goes straight into your clips folder, with no game or month folders. **Move to game** in the dock still makes the
  game's folder, since you chose that game.

## [1.1.1] - 2026-10-24

### Fixed

- **Download** on an update banner, and **Yes** in the update check's message, now open the release page in your
  browser. Before, nothing happened (the log said "could not open the download page").

## [1.1.0] - 2026-10-24

### Added

- An activity pill under the dock's header shows what Clip Atlas is doing with your latest save:
  waiting for the file to settle, sorting it, or done, with a timer. Click it (or press **Enter**
  or **Space**) for each file's steps and the recent saves; **Esc** closes them. A save that
  couldn't be sorted keeps its amber warning until the next save, and its details say why.

### Changed

#### M6: The dock, redrawn ("Native Plus")

- The Clip Atlas dock is redrawn to follow your OBS theme, dark or light: rounded rows, painted
  icons, the theme's own green and amber for status, and no Windows-coloured selection.
- It arranges itself for where you put it: a list in a side column, a header rail with a strip of
  large cards in the short bottom row (the mouse wheel scrolls it sideways), or, floated large, a
  list with a preview of the selected clip and its details.
- Clips are grouped by day: Today, Yesterday, Previous 7 days, then by month, each group with its
  count and size. Rows show a short title ("Replay", "Recording", or your own name for a renamed
  clip), the game and size, and the time.
- Game chips with clip counts replace the game list, and a type filter shows only replays,
  recordings or screenshots.
- **Save replay** in the dock saves the Replay Buffer, as OBS's hotkey does.
- After each save, a note says where the clip was filed (**Filed in Elden Ring › 2026-10**), with
  **Show** to open its folder; the new clip is marked **New**. A clip that couldn't be sorted gets
  a note with the reason.
- Hovering a clip shows **Open**, **Show in folder** and **More actions** buttons. **Ctrl+F** goes
  to the search, and **Esc** clears it.
- The footer shows how many clips there are and their total size, next to icon buttons for the
  clips folder, **Refresh** and the settings.
- A clip without a Windows thumbnail shows a tile with its type, instead of the file type's icon.

### Known limitations

- With **Auto-sort** off, a replay saved with **Save replay** appears in the dock after
  **Refresh**.
- Times and month names follow your Windows regional settings, while the dock's words follow OBS's
  language.
- Themes other than OBS's own may not define the green and amber the dock reads; then it uses
  its own.

## [1.0.0] - 2026-10-21

### Added

#### M1: Feature parity with the Python script

- Game detection. Clip Atlas finds every Game Capture and Window Capture source in OBS by itself,
  and files clips under the game that is being captured.
- Replay sorting into `<Clips folder>\<Game>\<YYYY-MM>\`, with monthly folders on by default.
  Clips made while no game is detected go to `Unsorted`.
- Automatic Replay Buffer. It starts when a game is detected and stops after the game closes, with
  a 10-second grace period by default. It only stops a buffer that Clip Atlas started.
- Clip Atlas Settings, under **Tools → Clip Atlas Settings…**, for the clips folder, monthly
  folders, the unsorted folder name, the Replay Buffer options and the grace period.
- A replay that OBS remuxes to `.mp4` is moved together with its `.mkv`.
- A name clash gets a number, such as `Replay (2).mkv`. Nothing is overwritten.
- Clips moved to another drive are copied and checked before the original is removed. A locked file
  is retried for about 30 seconds.

#### M2: Recordings, screenshots and the save sound

- Recordings are sorted when you stop them. Every part of a split recording, and every `.mp4` made
  by auto-remux, is filed with it.
- **File each recording under the game played longest** (off by default).
- Screenshots taken in OBS are sorted into game folders.
- **What to sort** in the settings: Replays, Recordings and Screenshots, each on by default.
- Save sound (off by default). Plays a `.wav` file that you choose, through Windows, once a replay
  has been saved and sorted. It has a **Volume** setting (80 % by default) and a **Test** button. No
  sound file is included in this release.

#### M3: Clip browser dock

- **Docks → Clip Atlas**: a header with the current game, the Replay Buffer state and an
  **Auto-sort** switch; banners for setup problems; a clip list with thumbnails from Windows, a game
  filter and a search box; and a footer with **Open clips folder**, **Refresh** and the settings
  button.
- Right-click actions on a clip: **Open**, **Show in folder**, **Rename…**, **Move to game…**
  and **Move to Recycle Bin…**. Clip Atlas never deletes a clip for good itself.

#### M5: Installer, updates and hotkey

- Windows installer, `clip-atlas-<version>-windows-x64-installer.exe`. It installs to
  `C:\ProgramData\obs-studio\plugins\clip-atlas`, asks you to close OBS first, keeps your settings
  when you uninstall, and warns about an older copy in your user profile.
- Zip download, `clip-atlas-<version>-windows-x64.zip`, for a manual install.
- Update check. Once a day, Clip Atlas checks GitHub for a newer release and shows a banner.
  **Tools → Check for Clip Atlas Updates…** checks at once. Nothing is downloaded or installed
  automatically, and the check can be turned off under **Updates**.
- Hotkey **Toggle Clip Atlas auto-sort**, which you bind in OBS's **Settings → Hotkeys**.
- Install guide in `docs/install.md`, and an updated README.

### Deprecated

- The Python script `legacy/rb_event_driven.py` is deprecated. It logs a warning, and it gets no new
  features or fixes. Remove it from **Tools → Scripts** before you use Clip Atlas.

### Known limitations

- Windows only. Game detection depends on Windows-only capture signals.
- The installer and the zip are not code-signed yet, so Windows shows a warning when you run the
  installer. See `docs/install.md`.
- Thumbnails come from Windows. `.mkv` files usually need a codec pack that adds a thumbnail
  handler. Without one, the dock shows the file type's icon.
- The dock lists at most the 5,000 newest clips.
- If you close OBS within a few seconds of stopping a recording or saving a replay, that file may be
  left unsorted in OBS's folder.
- If you switch Scene Collection at the exact moment a recording stops, Clip Atlas may miss the
  stop. The log says so when you next start recording.
- With auto-remux on, a sorted replay or recording arrives a few seconds after OBS finishes the
  `.mp4`, not instantly.
- The save sound can be heard on stream and in later clips. Windows plays it through the default
  playback device, and Desktop Audio captures that device.
- Folder names come from each game's window title until the built-in games list (M4) arrives.
- If you stop the Replay Buffer yourself while a game runs, or it stops when you switch profiles,
  Clip Atlas does not start it again until the current game changes.
- Not in this release: the built-in games list, the rules editor, the full settings pages, importing
  settings from the Python script, and a warning when the Python script is still loaded.
