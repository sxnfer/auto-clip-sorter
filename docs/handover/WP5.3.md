# WP5.3 hand-over: Inno Setup installer and zip

## Summary

- `installer/clip-atlas.iss` (new): Inno Setup 6 script. It installs the staged plugin folder to `{commonappdata}\obs-studio\plugins\clip-atlas` and requires admin. It waits while OBS Studio runs, warns about an older per-user copy, and registers an uninstaller. Nothing under `{userappdata}` is ever deleted.
- `.github/scripts/Package-Windows.ps1` (changed): the zip step is unchanged. Old installers are removed with the old zips. After the zip, a "Building installer..." step finds ISCC.exe, checks that the staged plugin is present, runs ISCC with the four `/D` defines and the script path, and throws if ISCC fails or the .exe is missing. The installer is written to `release/clip-atlas-<version>-windows-x64-installer.exe`, outside `release/<cfg>/`, so it is never inside the zip.
- No workflow change. The Windows artifact glob and push.yaml's `*.exe` collection pick the installer up unchanged.

## Files changed

- `installer/clip-atlas.iss` (new)
- `.github/scripts/Package-Windows.ps1` (modified: cleanup pattern and installer step)
- `docs/handover/WP5.3.md` (new, this note)

Nothing else was touched: no workflows, no CMake, no C++, no `legacy/`. The changes are staged with `git add -A` and not committed.

## How it was checked

- `build-aux/check-format`: "all 69 C/C++ and 16 CMake files are formatted", exit 0. It does not cover `.iss` or `.ps1`.
- PowerShell parse check: **not run.** I unpacked PowerShell 7.4.6 into the scratch directory, but the sandbox guard refused every PowerShell invocation in this worktree-isolated session (three attempts, including one with an explicit `cd` to the worktree). I did not try to get around the guard. The script was reviewed by hand, line by line: braces, quoting, `${env:ProgramFiles(x86)}` interpolation, the array literals, the `Invoke-External` call, and the `trap` path.
- Inno Setup compile: **not run.** ISCC is not installed here, and `jrsoftware.org` was unreachable from the fetch tool (DNS failure) and from curl (connection failure). The first Windows CI run is the first real compile.
- Web checks: WebSearch confirmed that `x64compatible` is the value Inno 6.3 and later recommend for `ArchitecturesAllowed` and `ArchitecturesInstallIn64BitMode`. It also confirmed that Win32 `MB_RETRYCANCEL` is 5 and `IDRETRY` is 4. It could not confirm any Inno Pascal Script identifier. The `LicenseFile` rule is the coordinator's statement.
- Static checks on the `.iss`: no braces inside comments, no tab characters, section names as in the brief, and `#define` defaults guarded by `#ifndef`.

## Directives and functions used

Each line gives the reason for the item and how sure I am of it.

[Setup]
- `AppId={{0DE82F85-...}`: the GUID from the brief, with the doubled opening brace. Per the brief.
- `AppName`, `AppVersion`, `AppPublisher`, `AppPublisherURL`, `AppSupportURL`, `AppUpdatesURL`: metadata for Add/Remove Programs. Standard, from memory.
- `DefaultDirName={commonappdata}\obs-studio\plugins\clip-atlas`: the install folder OBS 31 loads. Per the brief.
- `DisableDirPage`, `DisableProgramGroupPage`, `DisableReadyPage`: hide wizard pages. Per the brief.
- `PrivilegesRequired=admin`: `ProgramData` needs admin rights. Standard.
- `ArchitecturesAllowed`, `ArchitecturesInstallIn64BitMode=x64compatible`: **verified** through the WebSearch excerpt of the Inno 6.3 release notes.
- `LicenseFile=..\LICENSE`: relative to `SourceDir`, which defaults to the script's folder. This is the coordinator's rule. I could not check it against the docs.
- `OutputDir={#MyOutputDir}`, `OutputBaseFilename={#MyOutputName}`: set from the `/D` defines. Standard.
- `Compression=lzma2`, `SolidCompression=yes`, `WizardStyle=modern`: standard values.
- `UninstallDisplayName`, `CloseApplications=yes`, `RestartApplications=no`, `SetupLogging=yes`: per the brief. `SetupLogging` is from memory.

[Files]
- `Source`, `DestDir`, and the flags `ignoreversion`, `recursesubdirs`, `createallsubdirs`: the standard recursive copy. From memory.

[InstallDelete]
- `Type: filesandordirs`, `Name:` for `{app}\bin` and `{app}\data`: removes the old program files before the copy. Per the brief. The `filesandordirs` type is from memory.

Preprocessor
- `#ifndef`, `#define`, `#endif`, and `{#Name}` substitution: standard ISPP.

[Code] (Pascal Script)
- `CreateOleObject('WbemScripting.SWbemLocator')`, `ConnectServer`, `ExecQuery`, `.Count`: the WMI process check. `CreateOleObject` is documented, and this WMI pattern is common in community examples. Neither was checked against the official docs here.
- `try` / `except`, `Exit`, `Variant`, `const`, `while` / `do`: language constructs, from memory.
- `Log(string)`: writes to the setup log. From memory.
- `MsgBox`, `MB_RETRYCANCEL`, `MB_OK`, `IDRETRY`, `mbError`, `mbInformation`: the message box and its constants. The Win32 values match (5 and 4). The Inno spellings are from memory and not confirmed.
- `DirExists`, `ExpandConstant('{userappdata}\...')`: named in the brief. Standard.
- `CurStepChanged(TSetupStep)` with `ssPostInstall`, `InitializeSetup`, `InitializeUninstall`: event functions named in the brief. `ssPostInstall` is from memory.

Not used: `VarIsEmpty` (allowed by the brief, not needed) and `SuppressibleMsgBox` (see the gaps below).

Not certain to exist in Inno Setup 6.7.1 as spelled: `MB_RETRYCANCEL`, `MB_OK`, `IDRETRY`, `mbError`, `mbInformation`, `Log`, `ssPostInstall`, and the `SetupLogging` directive. I believe all of them exist, but I could not check them against the docs. The first CI run will settle it. A wrong name fails the compile loudly, and the step throws with ISCC's output.

## Deviations from the brief, and why

- `ConnectServer` takes four arguments, `('', 'root\CIMV2', '', '')`, not two with `'.'`. The four-argument form is the one used in community scripts, so the call does not depend on optional-argument handling in Pascal Script. An empty server name means the local machine.
- `SourceDir` is not set. `LicenseFile` is relative, as the coordinator directed.
- The script adds two `throw` checks before ISCC runs: the staged folder and `bin\64bit\clip-atlas.dll`. Without them a missing plugin would surface as an ISCC "no files found" error. This is a small addition beyond the brief.
- Any answer other than Retry aborts the wizard: Cancel, Esc, and the close button alike.
- `ISCC.exe` is looked up on PATH first, then at `${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe`. The throw message begins with the brief's wording and adds the paths tried.
- No `[UninstallDelete]` section, because the brief asks for Inno's default only. No `[Languages]` or `[Messages]`, because English only is fine.
- The helpers are named `ObsIsRunning` and `ConfirmObsClosed(verb)`. `ConfirmObsClosed` is shared by `InitializeSetup` and `InitializeUninstall`.

## Known gaps and risks

- **Not compiled.** See "How it was checked". Any Pascal Script or directive error fails the first CI run.
- **Silent installs.** `MsgBox` is not suppressible, so `/VERYSILENT` would block on a message box if OBS is running or an old per-user folder exists. `SuppressibleMsgBox` would avoid that, but it is not in the brief's function list, so I did not use it. Interactive installs are unaffected.
- **Per-user folder check.** `{userappdata}` is the profile of the account that runs the elevated installer. If a standard user elevates with a different admin account, the warning checks the admin's profile and can miss the user's copy. The brief accepts this for a warning.
- **WMI unavailable.** The OBS check is skipped, and Restart Manager (`CloseApplications=yes`) remains the fallback.
- **Built on every Windows run.** `package-plugin`'s Windows step does not check the `package` input, so the zip and now the installer are built and uploaded on pull requests and manual runs too. This adds build time and one more file to the artifact.
- **License page.** `LICENSE` is ASCII text with LF line endings. I did not verify how Inno renders LF-only text. If the page shows one long line, the fix belongs in `LICENSE`, which is outside my ownership.
- **Unsigned.** SmartScreen will warn. WP5.6 documents it.
- **Leftover files.** The uninstaller removes what Setup installed. Anything the plugin writes into `{app}` at runtime would keep the folder. The plugin should not write there.
- **Start Menu.** There is no `[Icons]` section. I did not check what Inno creates by default in the program group.

## Questions for the orchestrator

1. Please run the PowerShell parse check from an unisolated checkout, because this session's guard refused every `pwsh` run:
   `pwsh -NoProfile -Command '$errs = $null; [System.Management.Automation.Language.Parser]::ParseFile(".github/scripts/Package-Windows.ps1", [ref]$null, [ref]$errs) | Out-Null; $errs'`
   It should print nothing.
2. Is it acceptable that the installer is built and uploaded on every Windows run, as the zip already is? A gate would need a change to the `package-plugin` action, which is outside my ownership.
3. Should the installer use `SuppressibleMsgBox` to keep silent installs from blocking? I can switch if you confirm the function exists in 6.7.1.
4. The first CI run is the compile test. If a Pascal Script constant is misspelled, the fix is a one-line change in `installer/clip-atlas.iss`.

## Orchestrator integration notes

- Read the whole diff; only the WP's files changed. `Invoke-External` throws on a non-zero exit code
  (`utils.pwsh/Invoke-External.ps1`), so an ISCC failure fails the job.
- The PowerShell parse check was refused by this environment's permission guard in the worker's session; the
  orchestrator did not re-run it on the worker's behalf. The first Windows CI run is the parse and compile check.
- Building the installer on every Windows run, as the zip already is, is accepted. `MsgBox` (not
  `SuppressibleMsgBox`) is fine: silent installs are not a supported path.
