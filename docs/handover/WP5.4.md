# WP5.4 hand-over: obs/hotkeys (toggle auto-sort) (worker)

## Summary

`src/obs/hotkeys.cpp` implements the `Hotkeys` class declared in `src/obs/hotkeys.hpp`, which is unchanged:

- `start()` registers `clip_atlas.toggle_auto_sort` as a frontend hotkey, described by `obs_module_text("Hotkey.ToggleAutoSort")`, and loads its binding from the profile's `[Hotkeys]` section. It runs once.
- `on_profile_changed()` reloads the binding for the new profile.
- A press (not the release) flips `Settings::auto_sort` through `SettingsStore::set()`. The dock follows through the settings listener, and the new state is logged at INFO.
- `stop()` makes the callback inert and unregisters the hotkey. It is idempotent, and the destructor calls it.

Not wired into `plugin-main.cpp`. That is WP5.5's job, as the plan says.

## Files changed

| File | Change |
|---|---|
| `src/obs/hotkeys.cpp` | new, 186 lines |
| `docs/handover/WP5.4.md` | new (this note) |

Both are staged with `git add -A` and not committed. Nothing else in the repo was touched. `hotkeys.hpp`, `plugin-main.cpp`, `CMakeLists.txt` and the locale are unchanged. Build output went to the gitignored `build_x86_64/` and `build/`. The throwaway harness described below is in the session scratchpad, not the repo.

## How it was tested

Repo checks, run from the worktree:

| Check | Command | Result |
|---|---|---|
| Linux plugin build, warnings as errors | `cmake --preset ubuntu-ci-x86_64`, then `cmake --build build_x86_64` | Configure OK. Build 27/27, exit 0. `hotkeys.cpp` compiled with `-Werror` (from the CI preset), and `clip-atlas.so` linked. The configure warning "ENABLE_CCACHE not used" comes from the preset. |
| Unit tests | `cmake -S tests -B build/tests && cmake --build build/tests && ctest --test-dir build/tests --output-on-failure` | 246 of 246 passed, exit 0 |
| Format, this file | `clang-format-19 --style=file --fallback-style=none --dry-run --Werror src/obs/hotkeys.cpp` (the version `check-format` pins). Also the default `clang-format` 18.1.3 on PATH. | Clean with both |
| Format, repo | `git add -A && build-aux/check-format` | "all 70 C/C++ and 16 CMake files are formatted", exit 0 |

Also: no line is over 120 columns (tabs counted as 8), and the file has no TODO or FIXME.

Declarations: each OBS and config declaration the brief lists was checked against `/usr/include/obs` (libobs 30.0.2). All match, with no differences. That covers `obs_hotkey_register_frontend`, `obs_hotkey_unregister`, `obs_hotkey_load`, `obs_hotkey_load_bindings`, `obs_hotkey_func`, `OBS_INVALID_HOTKEY_ID`, `obs_frontend_get_profile_config`, `config_get_string`, `obs_data_create_from_json`, `obs_data_get_array`, `obs_data_release`, `obs_data_array_release` and `obs_module_text`.

Behaviour against real libobs (optional, outside the repo):

- Harness: `/tmp/claude-0/-home-user-auto-clip-sorter/6c995450-a095-5948-9d9e-9af008631cf2/scratchpad/hotkey-harness/harness.cpp`. It compiles `src/obs/hotkeys.cpp` unchanged with `-fsanitize=address,undefined` and links it against `libobs.so.30`, which is the Ubuntu 30.0.2 library, not OBS 31. `obs_startup()` runs headless under `xvfb-run`. Log of the passing run: `.../hotkey-harness/run3.log`.
- Stubbed: the frontend profile config (a real `config_t` owned by the harness, standing in for the frontend's), `SettingsStore::get/set` (in memory), and `obs_module_text` (returns the key). The two registration calls are wrapped to record the callback and then forward to libobs. Presses go through `obs_hotkey_inject_event`, so libobs itself calls the callback.
- Result: `PASSED: 0 failure(s)`, with no AddressSanitizer or UBSan reports. Registration attempts 3, unregister calls 2 and settings writes 3, as the scenarios predict. LeakSanitizer was off, because libobs leaks at shutdown, so leak checking of this file was not done.
- Covered: the saved binding loads (checked through `obs_hotkey_save`). A press toggles and a release does not. `start()` registers once. Profile changes reload the binding: an empty list, another key, no value, invalid JSON (with the warning), `{}`, and no profile open. `stop()` unregisters once and is idempotent. A callback that arrives after `stop()` does nothing. `start()` after `stop()` does nothing. A failed registration is logged and not retried. The destructor unregisters, and a callback that runs after the object is gone still finds a live handle.
- Caveats: this is libobs 30.0.2 under Xvfb, not an OBS UI. The callback ran on the main thread, because no frontend routing function was set. The Settings dialog and the real EXIT sequence were not exercised.
- Finding, from a bare probe with no plugin code on the same libobs: injected key events are handled asynchronously. A second press sent right after the first was not delivered, and counters read right after injection looked like missing callbacks. A 200 ms pause after each event fixed it, so the harness uses one. Each injected press made libobs call the callback with `pressed=true` and then with `pressed=false`. The plugin acts on `pressed=true` only, so one press toggles once. Whether the real keyboard path behaves the same was not checked.

## Deviations, and why

1. **Handle lifetime.** The brief asks for `Impl` to own a `std::shared_ptr<Handle>` and for the Handle never to be freed while OBS may call it. Both hold only if the static container keeps a second strong reference. So `hotkey_handles()` is a `std::deque<std::shared_ptr<Handle>>`. The brief's suggested `unique_ptr` cannot coexist with `Impl` owning a `shared_ptr` to the same Handle. There is one entry per `Hotkeys` object, which means one per plugin load.
2. **Unbind path.** When there is no saved value, or it cannot be parsed, the code calls `obs_hotkey_load_bindings(id, nullptr, 0)`. The brief used `obs_hotkey_load(id, NULL)`. Both unbind. The header does not say that `obs_hotkey_load` accepts NULL, so the code avoids it. `obs_hotkey_load` is called only with a real array.
3. **Warning on invalid JSON.** A saved value that does not parse logs a WARNING. Without it the hotkey would be silently unbound. This is log text only, not user-facing.
4. **`run_guarded` on the public methods** (`start`, `on_profile_changed`, `stop`), as in `replay-controller.cpp`. No exception leaves the object.
5. **Callback linkage.** The callback is a file-local function with `obs_hotkey_func`'s signature. It is not declared `extern "C"`. `frontend-events.cpp` does the same, and GCC 13 with `-Werror` accepts it.

## Known gaps and risks

- **OBS version.** The compile and the harness use the 30.0.2 headers and library. The brief says its declarations were verified against OBS 31.0.0. The functions used are the ones the brief lists, and the 30.0.2 headers declare them the same way. The OBS source itself was not re-checked here, including the claims in `hotkeys.hpp` about UI-thread routing and the `[Hotkeys]` section format.
- **EXIT unregistration.** `stop()` calls `obs_hotkey_unregister`, and the header says to call it at EXIT. `plan.md`'s decision log says frontend event callbacks must never be removed during EXIT, because OBS dispatches them by index. That rule is about frontend callbacks, not hotkeys, so I followed the header. It is not verified against OBS 31.
- **UI thread.** The brief says OBS routes hotkey callbacks to the UI thread. The harness did not test that routing.
- **Queued press after `stop()`.** The design covers this, since the Handle is never freed and `active` is false. The harness checks a direct call after the object is gone, not a real queued event.
- **Store lifetime.** `Hotkeys` holds the `SettingsStore` by reference, and its Handle keeps a raw pointer to it. The owner must destroy `Hotkeys`, or call `stop()`, before the store is destroyed. That owner is WP5.5.
- **Save failures.** `SettingsStore::set()` logs a failed save, and the new value still applies for the session. Nothing further is done here.
- **Profile timing.** The reload runs in `on_profile_changed()`. It assumes the frontend's profile config already points at the new profile when `OBS_FRONTEND_EVENT_PROFILE_CHANGED` is sent. The harness simulates that and does not test OBS's event order.
- **Windows.** The file has no Win32 code. It was not built with MSVC or MinGW, which the brief marks as not applicable.

## Questions for the orchestrator

1. Handle ownership: is a `shared_ptr` in `Impl` plus a second strong reference in the static deque what you want? The alternatives are leaking one small handle per load, or having `Impl` borrow a `unique_ptr`.
2. Is unbinding with `obs_hotkey_load_bindings(id, nullptr, 0)`, rather than a NULL array to `obs_hotkey_load`, acceptable?
3. WP5.5 should call `stop()` from the EXIT handler, as the header says, and destroy the object at unload. Please confirm this, given the decision-log rule for frontend callbacks.
4. Would a manual check in OBS 31 on Windows be worth doing before release? Rebind the key in Settings → Hotkeys, change profile, press the key, and confirm the dock switch follows. That needs a real OBS, which this environment does not have.

## Orchestrator integration notes

- Read the whole diff; only the WP's files changed. Gates: plugin build with warnings as errors, unit tests,
  check-format clean.
- Answers: the `shared_ptr` in `Impl` plus the process-lifetime deque is the intended ownership; unbinding with
  `obs_hotkey_load_bindings(id, nullptr, 0)` is fine (both calls remove the old bindings first, `obs-hotkey.c:503-533`
  in OBS 31.0.0). The OBS 31 facts in the header were checked by the orchestrator in WP5.0 (routing to the UI thread,
  `window-basic-main.cpp:2629-2640`; the `[Hotkeys]` format, `window-basic-settings.cpp:3581-3603`). WP5.5 calls
  `stop()` at EXIT (the "never remove" rule is about frontend event callbacks, not hotkeys) and destroys the object at
  unload. A Windows check (bind the key, switch profile, press it) is on the M5 checklist.
