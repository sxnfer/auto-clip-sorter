# WP5.0 hand-over: M5 interface headers (orchestrator)

M4 is deferred, so M5 follows M3. Headers, all new:

| File | Implemented by |
|---|---|
| `src/core/version.hpp` | WP5.1 |
| `src/core/update-policy.hpp` | WP5.1 |
| `src/platform/http.hpp` | WP5.2 |
| `src/obs/hotkeys.hpp` | WP5.4 |
| `src/obs/update-checker.hpp` | WP5.5 |
| `src/ui/settings-dialog.hpp` (doc comment: the "Updates" group) | WP5.5 |

Also: 12 M5 locale keys in `data/locale/en-US.ini`, and `winhttp` linked on WIN32.

## Verified

- GitHub: the repository is public (`"visibility": "public"`), so the API needs no sign-in.
  `releases/latest` answers 404 today (no release yet), which `evaluate_update` reports as
  `no_release`. The template's release job (`.github/workflows/push.yaml`) only accepts
  bare tags `X.Y.Z` or `X.Y.Z-rcN`/`-betaN`, and creates **draft** releases, which the
  `latest` endpoint never returns until the user publishes them.
- `windows-2022` runner image 20261012 lists Inno Setup 6.7.1 (actions/runner-images,
  `images/windows/Windows2022-Readme.md`).
- OBS 31 hotkeys (`libobs/obs-hotkey.h:143-207`): `obs_hotkey_register_frontend`,
  `obs_hotkey_unregister`, `obs_hotkey_load` and `obs_hotkey_load_bindings` both remove the
  old bindings first (`obs-hotkey.c:503-533`), `OBS_INVALID_HOTKEY_ID`. Callbacks are
  routed to the UI thread (`window-basic-main.cpp:2629-2640`). OBS's Settings dialog saves
  frontend hotkeys, plugins' included, to the profile's `[Hotkeys]` section as
  `{"bindings": [...]}` (`window-basic-settings.cpp:3581-3603`; `:2841` marks frontend
  hotkeys), and loads only its own (`window-basic-main.cpp:2642-2660`).
- OBS's locale parser turns `\n` in a value into a newline (`text-lookup.c:149`).
- Every header compiles alone with the plugin's flags.
