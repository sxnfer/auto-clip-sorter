# M3 dock layout spec (WP3.0, orchestrator)

The clip browser dock: what it looks like, which widget does what, and how it behaves.
Every M3 worker reads this. The headers named below are the contracts; where this file and
a header disagree, the header wins.

## Wireframe

```
┌─ Clip Atlas ─────────────────────────────────────┐
│ Counter-Strike 2                                  │  StatusHeader (ui/status-header)
│ ● Replay Buffer on (started by Clip Atlas) [✓] Auto-sort │
├───────────────────────────────────────────────────┤
│ ⚠ The clips folder D:\Clips doesn't exist. Is …   │  BannerArea (ui/banner-area),
│                                         [Settings…] [✕]   │  hidden when empty
├───────────────────────────────────────────────────┤
│ [All games          ▾] [🔍 Search clips       ✕]  │  filter row (ui/clip-dock)
├───────────────────────────────────────────────────┤
│ ┌────────┐ Replay 2026-10-21 18-04-12.mkv  Replay │  QListView with
│ │ thumb  │ Counter-Strike 2 · 2 min ago · 84 MB    │  ClipListModel + ClipListDelegate
│ └────────┘                                        │  (ui/clip-list-model)
│ ┌────────┐ 2026-10-21 17-50-03.mp4      Recording │
│ │ thumb  │ Counter-Strike 2 · 16 min ago · 1.2 GB  │
│ └────────┘                                        │
├───────────────────────────────────────────────────┤
│ [Open clips folder]                        ⟳   ⚙ │  footer (ui/clip-dock)
└───────────────────────────────────────────────────┘
```

When there is nothing to list, the list is replaced by a centred, word-wrapped message:
"No clips yet. …" with no clips at all, or "No clips match." when the filter hides them.

## Widget tree

```
ClipDock : QWidget                         ui/clip-dock.hpp      WP3.6
└─ QVBoxLayout (margins 0, spacing 4)
   ├─ StatusHeader                         ui/status-header.hpp  WP3.5
   ├─ BannerArea                           ui/banner-area.hpp    WP3.5
   ├─ QHBoxLayout: QComboBox (games) + QLineEdit (search, clear button)
   ├─ QStackedWidget
   │  ├─ QListView (model ClipListModel, delegate ClipListDelegate)   ui/clip-list-model.hpp WP3.4
   │  └─ QLabel (empty message)
   └─ QHBoxLayout: QPushButton "Open clips folder", stretch, QToolButton ⟳, QToolButton ⚙
```

The game combo lists "All games", then each `ClipIndex::games()` entry, then "Not sorted"
when `ClipIndex::has_unfiled()`. Its item data is the `ClipQuery::game_folder` value
(nullopt for "All games" is stored as an invalid QVariant). Rebuilding the combo after a
scan keeps the current choice when it still exists.

The search box filters as the user types (ClipQuery::search), with no delay: the index
holds at most 5000 clips, and the query is a linear scan.

## Theme rules (all UI WPs)

- No hard-coded colours, fonts or style sheets. Use `palette()` roles (Text, Highlight,
  PlaceholderText or the Disabled group's Text) and standard icons
  (`style()->standardIcon(...)`). OBS's themes then style every part.
- Every string comes from `obs_module_text("Key")`. All M3 keys are already in
  `data/locale/en-US.ini` (the `Dock.*` keys); workers never edit that file. If a key
  is missing, say so in the hand-over note and use the closest existing key.
- No `Q_OBJECT` unless a class declares its own signals or slots; use `std::function`
  callbacks and lambdas (as `settings-dialog` does). A new `Q_OBJECT` header needs a clean
  build tree (plan.md decision log).
- The private members of a UI header may be changed by the WP that owns its `.cpp`. The
  public part is fixed.

## Threads

| Work | Thread |
|---|---|
| Widgets, models, ClipIndex, SettingsStore, `obs_frontend_*` | UI |
| `scan_clips_folder`, `describe_clip` | the dock's own `TaskQueue` |
| Thumbnails (`IShellItemImageFactory`) | `thumbnail_provider()`'s thread (COM STA) |
| Open, reveal, rename, recycle | `shell_ops()`'s thread (COM STA) |
| Move to game | the plugin's `FileMover` |

Every result comes back to the UI thread through `run_on_ui()`, guarded by a `QPointer`
(widgets and models) or a `std::weak_ptr` locked on the UI thread (the dock's shared
state), so a result that arrives after the dock is gone or stopped does nothing.

## Lifecycle (OBS 31.0, re-checked against 32.2.2)

1. `obs_module_load`: `register_clip_dock()` creates the widget and calls
   `obs_frontend_add_dock_by_id("clip-atlas-dock", title, widget)`
   (`UI/obs-frontend-api/obs-frontend-api.h:141`). OBS wraps it in an `OBSDock`, adds it
   floating and hidden, and lists it under the **Docks** menu
   (`UI/api-interface.cpp:322-339`). Modules load in `OBSBasic::OBSInit()` before the
   saved dock layout is restored (`window-basic-main.cpp:2114` then `:2302`), so a dock
   registered at load gets the user's saved position back.
2. `OBS_FRONTEND_EVENT_FINISHED_LOADING` (`window-basic-main.cpp:2443`):
   `ClipDock::start()` reads the clips folder (OBS's recording folder exists now) and
   starts the first scan.
3. `OBS_FRONTEND_EVENT_EXIT` (`closeEvent`, `:4887`): `stop_all()` calls
   `ClipDock::stop()`, `thumbnail_provider().shutdown()` and `shell_ops().shutdown()`.
4. The main window is deleted on close (`obs-app.cpp:1297`, `WA_DeleteOnClose`). Its
   `extraDocks` list (`std::shared_ptr<QDockWidget>`, `window-basic-main.cpp:9627`)
   deletes the dock, and with it our widget, while the plugin is still loaded:
   `obs_shutdown()` unloads modules later, from `~OBSApp` (`obs-app.cpp:1013`).
5. `obs_module_unload`: the plugin context is destroyed. The dock is already gone, so it
   never refers to a destroyed `GameState`, `SettingsStore` or `FileMover`.

The plugin keeps a `QPointer<ClipDock>`, so it never calls a deleted dock.

## Safety rules for the clip actions

- **Delete is Recycle Bin only**, after a Yes/No question whose default is No. The Shell
  call uses `FOF_WANTNUKEWARNING`, so Windows asks before it would delete a file for good
  (a drive without a Recycle Bin). Clip Atlas never deletes a clip permanently itself.
- **Rename** never replaces a file (`MoveFileExW` without `MOVEFILE_REPLACE_EXISTING`) and
  never changes the extension.
- **Move to game** goes through `FileMover`, the same verified-copy code that sorts clips.
- Every failure leaves the clip where it was, logs a WARNING and shows a message box.
