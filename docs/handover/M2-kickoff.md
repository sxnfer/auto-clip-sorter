# M2 kickoff: state and verified facts (written by the M0–M1 orchestrator)

## Where things stand (2026-10-20)

| Branch | State | PR |
|---|---|---|
| `claude/m0-scaffold` | M0 done, CI green | #1 → main (open, not merged) |
| `claude/m1-feature-parity` | M1 done; the user's Windows test **passed** (`docs/testing/M1.md`); CI green on `baf96b3` | #2 → main (open, stacked on #1) |
| `claude/m2-recordings-screenshots` | created from `baf96b3`, pushed, no M2 work yet | none yet |

Neither PR is merged. M2 builds on the M1 branch. Its PR goes against `main` and says it is stacked on #2.

The user tested M1 on **OBS 32.2.2**, Windows 11 25H2, with a UK-English locale. Their settings are clips folder `D:/Clips`, grace 5 s, monthly folders off. They use Counter-Strike 2 with Game Capture.

## Product decisions made with the user since docs/PLAN.md

- The auto Replay Buffer reacts only to real game hooks, not show/activate. The user confirmed "the real game hook for now is ok".
- **WP2.5, save sound.** It is optional and off by default. It plays through Windows, with no OBS source and no device routing. One line of help text warns that Desktop Audio captures it (stream and later clips). The user rejected anything more complex. The bundled ElevenLabs "Replay saved" WAV ships **only once the user confirms a licence that allows redistribution**. Until then the feature uses a user-chosen `.wav`. See `plan.md` WP2.5 and `docs/PLAN.md`.

## Facts verified against OBS 31.0.0 source for M2

Source: tag `31.0.0`, the files named below.

**Recordings**
- The recording output declares `void file_changed(string next_file)`: `plugins/obs-ffmpeg/obs-ffmpeg-mux.c:104,748` and `plugins/obs-outputs/mp4-output.c:197,404`.
  - It fires when a split file starts.
  - `next_file` is the **new** file. The file that just finished is the previous path.
  - It is emitted on the output's thread, not the UI thread.
- OBS's own handler is `UI/window-basic-main-outputs.cpp:104-114` (`OBSRecordFileChanged`). It queues `RecordingFileChanged(old_path)` to the UI thread, which calls `AutoRemux(old_path, true)` (`UI/window-basic-main.cpp:7160-7166`). Then it sets `lastRecordingPath = next_file`. So **each finished split file is auto-remuxed too**, not only the last one.
- `obs_frontend_get_recording_output()` returns `obs_output_get_ref(fileOutput)`. Release it with `obs_output_release`.
  - Outputs are recreated by `ResetOutputs()` on profile and settings changes, so reconnect `file_changed` on `OBS_FRONTEND_EVENT_RECORDING_STARTING` or `RECORDING_STARTED`, and hold no output reference across recordings.
  - `obs_output_get_signal_handler(output)` gives the handler.
- `OBSBasic::RecordingStop` (`window-basic-main.cpp:7097-7158`):
  - It shows its error dialogs, then emits `OBS_FRONTEND_EVENT_RECORDING_STOPPED` (:7150), then calls `AutoRemux(lastRecordingPath)` (:7155). This is the same order as replays: remux starts right after the event.
  - A non-success stop code still emits STOPPED. Check that the file exists and is stable before moving it.
- `obs_frontend_get_last_recording()` returns `bstrdup(lastRecordingPath)` (`UI/api-interface.cpp:577`), valid after STOPPED. Free it with `bfree`.
- With auto-remux on, OBS records `.mp4` as `.mkv` (`BasicOutputHandler::SetupAutoRemux`, `window-basic-main-outputs.cpp:2372-2377`), so the remuxed `.mp4` is expected. `core/remux-rules` (`expected_remux_path`) already models this.

**Screenshots**
- `UI/window-basic-main-screenshot.cpp:40-60`: the writer thread is joined, then `main->lastScreenshot = path` is set, then `OBS_FRONTEND_EVENT_SCREENSHOT_TAKEN` is emitted. The file is complete when the event arrives.
- `obs_frontend_get_last_screenshot()` returns `bstrdup(main->lastScreenshot)` (`api-interface.cpp:582`).

**Already established in M0/M1** (see `plan.md` §7 and `docs/handover/WP0.4.md`, `WP1.*.md`):
- Modules load before `ResetOutputs()`: never touch outputs in `obs_module_load`; use `FINISHED_LOADING`.
- Frontend callbacks are never removed, only made inert. Objects are destroyed only in `obs_module_unload`, after EXIT.
- `run_on_ui()` runs inline when already on the UI thread.
- `get_hooked` may only be read on the graphics thread.

## Process lessons from M0–M1 (apply them)

1. **Worktrees start from the wrong base.** `isolation: "worktree"` created worktrees from `main`. Every brief needs a STEP 0: `git merge --ff-only <milestone branch>`, then confirm the expected `git log -1`.
2. **`build-aux/check-format` only sees git-tracked files.** `git add` new files before running it at integration.
3. **Don't push glue whose dependencies aren't on the branch yet.** MSVC fails to link unresolved symbols, but Linux shared modules link anyway. Commit locally and push when the dependency lands.
4. **The ★ Reviewer agents earned their cost.** They found real bugs in file-mover (it could delete the only copy) and source-tracker (a UI-thread use-after-free). Keep them for ★ WPs, and have them check claims against the OBS source at `scratchpad/ext/obs-studio-31.0.0`. Re-clone it if the scratchpad is gone: `git clone --depth 1 --branch 31.0.0 https://github.com/obsproject/obs-studio.git`.
5. **Workers sometimes misread OBS source.** One claimed SCENE_COLLECTION_CHANGING is never emitted, but it is, in `window-basic-main-scene-collections.cpp`. Verify surprising claims yourself before acting on them.
6. **Session usage limits can stop agents mid-task.** Their work is saved. Resume each one with SendMessage rather than starting a new agent.
7. **Local tools:**
   - clang-format-19 (19.1.1) and gersemi 0.21.0 are installed.
   - Ubuntu's libobs-dev 30.0.2 and Qt 6.4.2 are installed for the `ubuntu-ci-x86_64` compile check.
   - The OBS PPA and GitHub artifact downloads are blocked by the container proxy.
   - If the container is fresh, reinstall: `apt-get install -y clang-format-19 libobs-dev obs-studio qt6-base-dev qt6-base-private-dev libsimde-dev` and `pip install --break-system-packages gersemi==0.21.0`.
8. **The user is not a developer.** Give simple, numbered Windows steps, and link the exact Actions run whose **Artifacts** section holds `clip-atlas-…-windows-x64-<sha>`. Downloading requires being signed in to GitHub.

## Open items carried into M2

- WP2.5's default WAV is blocked on the user's licence confirmation.
- Known gaps from M1 to keep in mind:
  - the no-replace publish primitive on Windows (deferred)
  - the FIFO wait queue in frontend-events
  - Preview-only window capture can count as current
  - a profile switch that stops the buffer suppresses restarts
- `user_rules()` is duplicated in `plugin-main.cpp` and `frontend-events.cpp`. Move it into core in M4.
