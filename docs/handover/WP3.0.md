# WP3.0 hand-over: M3 interface headers and dock spec (orchestrator)

## Summary

| File | Kind | Implemented by |
|---|---|---|
| `docs/handover/M3-dock-spec.md` | layout spec: wireframe, widget tree, threads, lifecycle, safety rules | — |
| `src/core/task-queue.hpp/.cpp`, `tests/test-task-queue.cpp` | new, implemented | WP3.0 |
| `src/core/lru-cache.hpp`, `tests/test-lru-cache.cpp` | new, header-only, implemented | WP3.0 |
| `src/core/clip-index.hpp` | new header | WP3.1 |
| `src/platform/thumbnails.hpp` | new header | WP3.2 |
| `src/platform/shell-ops.hpp` | new header | WP3.3 ★ |
| `src/ui/clip-list-model.hpp` | new header | WP3.4 |
| `src/ui/status-header.hpp`, `src/ui/banner-area.hpp` | new headers | WP3.5 |
| `src/ui/clip-dock.hpp` | new header | WP3.6 |
| `data/locale/en-US.ini` | every M3 string (`Dock.*`, 46 keys) | WP3.0 |

No `.cpp` includes the new headers yet, so the commit links on MSVC.

## Verified against OBS 31.0.0 source (and 32.2.2)

- `bool obs_frontend_add_dock_by_id(const char *id, const char *title, void *widget)`,
  `void obs_frontend_remove_dock(const char *id)` (`UI/obs-frontend-api/obs-frontend-api.h:141-143`).
  The implementation (`UI/api-interface.cpp:322-339`) refuses a used id with a warning and
  returns false (the widget is then still ours); otherwise it wraps the widget in an
  `OBSDock`, adds it floating and hidden, and adds a Docks menu entry. 32.2.2:
  `frontend/OBSStudioAPI.cpp:336`.
- Order at start: modules load in `OBSInit()` (`window-basic-main.cpp:2114`), the saved
  dock state is restored later (`:2302`), FINISHED_LOADING last (`:2443`).
- Order at exit: `closeEvent` sends EXIT (`:4887`), then the main window is deleted
  (`WA_DeleteOnClose`, `obs-app.cpp:1297`), which deletes `extraDocks`
  (`std::shared_ptr<QDockWidget>`, `:9627`); `obs_shutdown()` unloads modules afterwards
  (`obs-app.cpp:1013`). So the dock widget dies after EXIT and before `obs_module_unload`.
- `config_get_string(cfg, "SimpleOutput", "RecRBPrefix")` is the replay prefix in both
  output modes (Simple: `window-basic-main-outputs.cpp:1278`; Advanced: `:2294`; default
  "Replay", `window-basic-main.cpp:1741`; 32.2.2: `frontend/utility/AdvancedOutput.cpp:905`).
- OBS's locale parser (`libobs/util/text-lookup.c:149-152`) turns `\"`, `\n`, `\t`, `\r`
  into their characters and leaves a single `\` alone, which `Dock.Error.BadName` relies on.
- `obs_frontend_get_main_window_handle()` exists (`obs-frontend-api.h:101`); M3 doesn't
  need it (Shell dialogs run unowned on the Shell thread).

## Checks

- Every new header compiles alone with the plugin's flags (Ubuntu, Qt 6.4, libobs 30.0.2).
- Unit tests: 246 of 246 pass; the task-queue and LRU tests also pass under ThreadSanitizer
  (3 runs, no reports).
- `build-aux/check-format` clean.

## Decisions

In the plan.md decision log: rename via `MoveFileExW` without replacing, Recycle Bin only
with `FOF_WANTNUKEWARNING` and a confirmation, replay type from `RecRBPrefix`, the shared
`task-queue` and `lru-cache`, and M3 + M5 on the session branch.
