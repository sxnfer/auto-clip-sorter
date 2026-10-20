# WP0.1 hand-over: Bring in obs-plugintemplate

**Status:** done locally. Two checks can only run in CI or need the OBS source download, and both are listed under known gaps.
**Template:** obsproject/obs-plugintemplate at 3e7d7ac (HEAD). **OBS pin:** 31.0.0, values identical to template commit 5da535c.

## Summary

- Imported the template's build system, CI, CMake modules and plugin source into the repo root. The module is `clip-atlas`, version `0.1.0`, display name "Clip Atlas".
- `buildspec.json` pins OBS Studio 31.0.0, obs-deps 2024-09-12 and Qt6 2024-09-12, with the contract's hashes.
- Windows is the release target. Ubuntu is kept for compile checks only. macOS is removed from presets, CI jobs, scripts and the codesigning action.
- `cmake/common/buildspec_common.cmake` passes `-DENABLE_UI:BOOL=OFF`, the OBS 31.0 name for the UI switch.
- The hidden `template` preset sets `ENABLE_FRONTEND_API` and `ENABLE_QT` to ON.
- `push.yaml` runs on `claude/**` branches.
- **Constraint 3:** the container can configure and build the Ubuntu preset with `ENABLE_FRONTEND_API` and `ENABLE_QT` ON, and it produces `build_x86_64/clip-atlas.so`. It uses Ubuntu's own OBS 30.0.2 packages, not 31.0.0, because the OBS PPA is blocked by the proxy.

## Files changed

Paths are relative to the repo root. `git status` shows only these paths, plus ignored build directories.

### Added (copied from template HEAD 3e7d7ac, byte-identical unless listed under Modified)

- Top level: `.clang-format`, `.gersemirc`, `CMakeLists.txt`
- `.github/actions/check-changes/action.yaml`
- `.github/scripts/`: `.Aptfile`, `Build-Windows.ps1`, `Package-Windows.ps1`, `build-ubuntu`, `package-ubuntu`
- `.github/scripts/utils.pwsh/`: `Ensure-Location.ps1`, `Invoke-External.ps1`, `Logger.ps1`
- `.github/scripts/utils.zsh/`: `check_ubuntu`, `log_debug`, `log_error`, `log_group`, `log_info`, `log_output`, `log_status`, `log_warning`, `mkcd`, `set_loglevel`, `setup_ubuntu`
- `.github/workflows/`: `check-format.yaml`, `dispatch.yaml`, `pr-pull.yaml`
- `build-aux/`: `.run-format.zsh`, `run-swift-format`, and `run-clang-format` and `run-gersemi` (symlinks to `.run-format.zsh`)
- `build-aux/.functions/`: `log_debug`, `log_error`, `log_group`, `log_info`, `log_output`, `log_status`, `log_warning`, `set_loglevel`
- `cmake/common/`: `bootstrap.cmake`, `buildnumber.cmake`, `ccache.cmake`, `compiler_common.cmake`, `helpers_common.cmake`, `osconfig.cmake`
- `cmake/linux/`: `compilerconfig.cmake`, `defaults.cmake`, `helpers.cmake`
- `cmake/windows/`: `buildspec.cmake`, `compilerconfig.cmake`, `defaults.cmake`, `helpers.cmake`, `resources/resource.rc.in`
- `data/locale/en-US.ini` (empty, as in the template)

Not copied: `cmake/macos/`, the template's `README.md`, `LICENSE`, `.gitignore` and `.git`.

### Modified

- `.gitignore`: kept the four existing lines and appended `build_*/`, `release/`, `.ccache/`, `*.lock.json`, `.DS_Store`, `/cmake/.CMakeBuildNumber`, `/.deps/`.
- `buildspec.json`: dependency pins from 5da535c. Set `name`, `displayName`, `version`, `author`, `website`, `email`, and `platformConfig.macos.bundleId` to `com.sxnfer.clip-atlas`.
- `CMakePresets.json`: removed the `macos` and `macos-ci` configure and build presets. The hidden `template` preset sets `ENABLE_FRONTEND_API` and `ENABLE_QT` to true.
- `cmake/common/buildspec_common.cmake`: `-DENABLE_FRONTEND:BOOL=OFF` became `-DENABLE_UI:BOOL=OFF`, with a one-line comment above the call.
- `src/plugin-main.c`, `src/plugin-support.h`, `src/plugin-support.c.in`: license header only. `Plugin Name` became `Clip Atlas`, and the copyright line became `Copyright (C) 2026 sxnfer`.
- `.github/workflows/build-project.yaml`: deleted the `macos-build` job. Removed the `codesign` and `notarize` outputs and config items from `check-event` (see Deviations).
- `.github/workflows/push.yaml`: added `claude/**` to `on.push.branches`. Removed the `macos-universal` rename variant, and `*.pkg` from the checksum glob and the release files.
- `.github/actions/build-plugin/action.yaml`: removed the macOS build step and the `codesign`, `codesignIdent` and `codesignTeam` inputs. The summary step runs on Linux only.
- `.github/actions/package-plugin/action.yaml`: removed the macOS packaging step and the seven macOS signing and notarization inputs. The `package` input now says "Windows or Ubuntu".
- `.github/actions/run-clang-format/action.yaml`, `.github/actions/run-gersemi/action.yaml`: notice text only. "macOS-based or Linux-based runner" became "Linux-based runner".

### Deleted (template files not kept)

- `.github/actions/setup-macos-codesigning/` (the directory and its `action.yaml`)
- `.github/scripts/build-macos`
- `.github/scripts/package-macos`
- `.github/scripts/utils.zsh/check_macos`
- `.github/scripts/.Brewfile`

Each was deleted only after `grep -rn` showed that its only remaining references were inside files also being deleted (the scripts that call each other, and the action's own warning text). A repo-wide grep after the deletions found none.

### Added by this WP (repo-owned)

- `docs/handover/WP0.1.md` (this note)

### Created by builds (gitignored, not part of the change)

- `build/tests/`, `build_x86_64/`, `cmake/.CMakeBuildNumber`

## How it was tested

| Check | Command | Result |
|---|---|---|
| Unit tests | `cmake -S tests -B build/tests && cmake --build build/tests -j4 && ctest --test-dir build/tests --output-on-failure` | Configured and built. 11/11 passed. Catch2 was fetched from GitHub through the proxy. |
| JSON | `python3 -m json.tool buildspec.json` and `CMakePresets.json` | Valid. |
| YAML | `yaml.safe_load` on every `.github/**/*.yaml` | 10 files parse (5 workflows, 5 actions). |
| Workflow structure | Custom checker: `needs:` targets, reusable-workflow paths, `with:` keys against action inputs, `needs.*.outputs.*` references, `inputs.*` used in actions, and no leftover macOS references | Passes on the repo. It catches two planted errors in a mutated copy. |
| `check-event` logic | Ran the extracted "Check Event Data" script with six simulated events: push to a branch, push of tag `1.2.3`, PR with and without the "Seeking Testers" label, `workflow_dispatch`, `schedule` | Outputs are as intended (`package`, `config`, `commitHash`, `pluginName`). No `codesign` or `notarize`. |
| clang-format | `clang-format-19 --dry-run --Werror src/plugin-main.c src/plugin-support.h` (19.1.1) | Exit 0. |
| Template fidelity | Compared each of the 65 kept files with `git show HEAD:<path>` in the template. Checked symlink targets and executable bits. | Identical except the listed modifications. Executable bits and symlinks preserved. |
| Pins | Compared `dependencies` with 5da535c. Ran the `string(JSON …)` reads from `bootstrap.cmake` with `cmake -P`. | Identical. The reads succeed (name `clip-atlas`, version `0.1.0`, bundleId, OBS `31.0.0`, Qt `2024-09-12`). |
| Ownership | `git status --short`; `git check-ignore` on `docs/`, `legacy/`, `tests/`, `plan.md`, `CLAUDE.md`, `README.md`, `src/core/` | Only owned paths changed. None of the protected paths is ignored. |
| Ubuntu preset | See Constraint 3. | Configure and build succeed. |

## Constraint 3: can the container build the Ubuntu preset?

**Yes, using Ubuntu's own packages. The OBS PPA is blocked.**

- **Option A (OBS PPA): blocked by the proxy.** `curl -v https://ppa.launchpadcontent.net/obsproject/obs-studio/ubuntu/dists/noble/Release` gives `CONNECT tunnel failed, response 403`. The proxy status records `connect_rejected` for `ppa.launchpadcontent.net:443`. This is a policy denial, so I did not route around it. `launchpad.net` and `keyserver.ubuntu.com` are reachable.
- **Option B (Ubuntu archive): works.** Environment: Ubuntu 24.04.5, x86_64.
  - Installed with apt: `obs-studio 30.0.2+dfsg-3build1`, `libobs-dev 30.0.2+dfsg-3build1`, `libobs0t64 30.0.2+dfsg-3build1` (libobs `PACKAGE_VERSION` 30.0.2.1), `qt6-base-dev 6.4.2+dfsg-21.1build5`, `qt6-base-private-dev 6.4.2+dfsg-21.1build5`, `libsimde-dev 0.7.2-6`, `libgles2-mesa-dev 25.2.8-0ubuntu0.24.04.4`, `build-essential`.
  - Already present: CMake 3.28.3, Ninja 1.11.1. CMake reports GNU 13.3.0.
  - `libqt6svg6-dev` from the contract's list does not exist on noble. It isn't needed for this build.
  - `libobs-dev` provides `/usr/include/obs/obs-frontend-api.h`, `/usr/lib/x86_64-linux-gnu/cmake/libobs/libobsConfig.cmake` and `/usr/lib/x86_64-linux-gnu/cmake/obs-frontend-api/obs-frontend-apiConfig.cmake`.
- **Commands and results:**
  - `cmake --preset ubuntu-x86_64`: configured. The cache has `ENABLE_FRONTEND_API=TRUE` and `ENABLE_QT=TRUE`. `libobs_DIR`, `obs-frontend-api_DIR` and `Qt6_DIR` resolve to `/usr/lib/x86_64-linux-gnu/cmake/...`.
  - `cmake --build --preset ubuntu-x86_64`: succeeded with no warnings. Output: `build_x86_64/clip-atlas.so` (23,528 bytes, RelWithDebInfo). It exports `obs_module_load`, `obs_module_unload`, `obs_module_ver`, `obs_module_set_locale`, `obs_module_free_locale` and `obs_module_set_pointer`, and it links `libobs.so.0`.
  - `cmake --preset ubuntu-ci-x86_64` and build (`CMAKE_COMPILE_WARNING_AS_ERROR=ON`): succeeded. `-Werror` appears in the compile commands.
- **Caveat:** this compile check uses OBS 30.0.2 headers and libraries, not 31.0.0. It is a valid check for APIs that did not change between 30.0 and 31.0. It will not catch problems with APIs that exist only in 31.0, so the 31.0.0 pin is exercised only in CI.
- **Left in place:** `build_x86_64/` (gitignored) and the packages above, installed on this container. Nothing was committed.

## Deviations from the contract

1. **`check-event` lost its `codesign` and `notarize` outputs and config items.** Only the deleted macOS job used them. Removing them shortened `config_data`, so the `case` indices changed. I ran each event type through the script to confirm the outputs. The job itself and its other outputs are kept.
2. **Dropped the macOS signing inputs** from `build-plugin` and `package-plugin`. The contract allowed keeping them only if a Windows or Ubuntu step used them. None does. I also removed the `*.pkg` globs from `push.yaml`, because only macOS produces `.pkg` files.
3. **Notice text** in `run-clang-format` and `run-gersemi` now says "Linux-based runner". Both run on Linux only.
4. **`build-aux/run-swift-format` is kept.** Nothing references it, but it is a symlink to `.run-format.zsh`, and swift-format is not macOS-only. Per the contract's "when in doubt, keep it".
5. **Left in place on purpose:** the Apple branches in `cmake/common/` (per the contract), the `AppleClang` flag in `CMakeLists.txt`, and the dead `--codesign` and `--notarize` flags in `build-ubuntu` and `package-ubuntu`. All are harmless.

## Known gaps and risks

- **Windows build not verified locally.** The `ENABLE_UI` change is verified only by reading the OBS 31.0.0 source. Root `CMakeLists.txt` line 18 defines `ENABLE_UI`. `add_subdirectory(UI)` is unconditional, and `UI/CMakeLists.txt` adds `obs-frontend-api` before its `ENABLE_UI` check. `libobs` requires Qt only under `if(ENABLE_UI)`. CI is the real check. In the OBS configure step, look for the warning "Manually-specified variables were not used by the project" mentioning `ENABLE_UI`. It should not appear.
- **Pinned SHA-256 hashes not verified.** `GET https://github.com/obsproject/obs-studio/archive/refs/tags/31.0.0.zip` returns HTTP 403 through the proxy, although the CONNECT to github.com succeeds. I could not hash the OBS or obs-deps archives. CMake's `file(DOWNLOAD ... EXPECTED_HASH)` will check them in CI.
- **"31.1+ renamed it ENABLE_FRONTEND" is unverified.** The comment in `buildspec_common.cmake` comes from the contract and the template's 31.1.1 pin. The 31.1 source is not available here.
- **The Ubuntu CI job uses the OBS PPA** (`setup_ubuntu`), so it builds against whatever OBS version the PPA serves, not the `buildspec.json` pin. Linux never reads the `dependencies` block. Not verified.
- **MSVC warnings:** `windows-ci-x64` sets `CMAKE_COMPILE_WARNING_AS_ERROR`. Any MSVC warning in the template C code would fail CI. Not checked locally.
- **Template bugs left as-is** (outside WP0.1):
  - `package-plugin/action.yaml`, Ubuntu packaging step: appends `--debug` to `build_args` instead of `package_args`.
  - `check-changes/action.yaml`: `echo "changedFiles=[]" >> GITHUB_OUTPUT` is missing `$`, so the output is never set. This affects WP0.3, which wires the format check.
  - `build-ubuntu` autoloads `setup_ccache`, which does not exist. Harmless, because autoload is lazy.
- **CI cost:** `push.yaml` now runs the full build, including packaging, on every push to `claude/**`. The contract asks for this, but it uses more CI time.

## Questions for the orchestrator

1. **Email.** The contract sets `buildspec.json` `email` to `sxnfernandes@outlook.com`. That differs from the account email on file. This value is published in the package metadata, so please confirm it.
2. **Packaging on branch pushes.** Should `claude/**` pushes package (`package: true`, as the template does for `push`) or only build? I kept the template behaviour.
3. **Ubuntu OBS version.** Should the Ubuntu CI pin OBS 31.0.x explicitly, or accept whatever the PPA serves?
4. **Container packages.** The OBS 30.0.2 and Qt 6.4.2 dev packages are installed here for compile checks. Keep them for later WPs, or remove them?
5. **Template bugs.** Should WP0.3 fix the `check-changes` `$` bug and the `package-plugin` `--debug` slip? WP0.3 wires the format check, so it touches the same area.

## Orchestrator integration notes

- Reviewed the full diff against template HEAD 3e7d7ac; every difference matches the brief.
- Gates re-run by the orchestrator: unit tests 11/11, `clang-format-19` clean, `gersemi 0.21.0 --check CMakeLists.txt cmake/` clean, `cmake --build --preset ubuntu-x86_64` OK.
- Answers to the questions above:
  1. Email: kept `sxnfernandes@outlook.com`, the address already public in this repo's commit history.
  2. Packaging on `claude/**` pushes: keep the template behaviour.
  3. Ubuntu CI OBS version: accept the PPA's; Ubuntu is a compile check only and Windows CI is authoritative for 31.0.
  4. Container packages: keep them for later compile checks.
  5. Template bugs: WP0.3 replaces the changed-files clang-format check, so the `check-changes` bug no longer affects it (gersemi still uses it; WP0.3 fixes the missing `$`). The `--debug` slip only matters with `RUNNER_DEBUG`; left as is.
