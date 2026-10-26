; Clip Atlas Windows installer (WP5.3). Built by .github/scripts/Package-Windows.ps1 with
; ISCC.exe (Inno Setup 6.7.1), which passes MyAppVersion, MySourceDir, MyOutputDir and
; MyOutputName. The defaults below only matter for a hand build from this folder.

#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif
#ifndef MySourceDir
  #define MySourceDir "..\release\RelWithDebInfo\clip-atlas"
#endif
#ifndef MyOutputDir
  #define MyOutputDir "..\release"
#endif
#ifndef MyOutputName
  #define MyOutputName "clip-atlas-0.0.0-windows-x64-installer"
#endif

[Setup]
; The opening brace is doubled because Inno reads a single brace as the start of a constant.
AppId={{0DE82F85-6F78-4982-9DC4-7A32F58D2C36}
AppName=Clip Atlas
AppVersion={#MyAppVersion}
AppPublisher=sxnfer
AppPublisherURL=https://github.com/sxnfer/auto-clip-sorter
AppSupportURL=https://github.com/sxnfer/auto-clip-sorter
AppUpdatesURL=https://github.com/sxnfer/auto-clip-sorter
; OBS 31 loads plugins from this folder. Writing to ProgramData needs admin rights.
DefaultDirName={commonappdata}\obs-studio\plugins\clip-atlas
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=no
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; SourceDir is left at its default, the folder of this script, so this is the repository's LICENSE.
LicenseFile=..\LICENSE
OutputDir={#MyOutputDir}
OutputBaseFilename={#MyOutputName}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=Clip Atlas (OBS plugin)
CloseApplications=yes
RestartApplications=no
SetupLogging=yes

[Files]
Source: "{#MySourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
; Remove the previous version's program files first, so nothing from an older build lingers.
; These are the plugin's own files, never user data.
Type: filesandordirs; Name: "{app}\bin"
Type: filesandordirs; Name: "{app}\data"

[Code]
const
  // Used in the WMI query and in the log line.
  ObsProcessName = 'obs64.exe';
  // How long /WAITOBS waits for OBS Studio to finish closing, and how often it looks.
  ObsExitWaitMs = 120000;
  ObsExitPollMs = 500;

// True when setup was started with /WAITOBS. The plugin passes it, with /SILENT, when it runs this installer
// as OBS Studio closes, so setup starts while OBS is still shutting down.
function WaitForObsRequested(): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if CompareText(ParamStr(I), '/WAITOBS') = 0 then
    begin
      Result := True;
      Exit;
    end;
end;

// True while OBS Studio is running. A WMI failure counts as not running: Restart Manager
// (CloseApplications=yes) is the second line of defence, so broken WMI must not block setup.
function ObsIsRunning(): Boolean;
var
  Locator: Variant;
  Service: Variant;
  Processes: Variant;
begin
  Result := False;
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Service := Locator.ConnectServer('', 'root\CIMV2', '', '');
    Processes := Service.ExecQuery('SELECT ProcessId FROM Win32_Process WHERE Name = ''' + ObsProcessName + '''');
    Result := Processes.Count > 0;
  except
    Log('clip-atlas: WMI check for ' + ObsProcessName + ' failed; continuing without it.');
    Result := False;
  end;
end;

// Waits, with Retry and Cancel, while OBS Studio is running. False means the user cancelled.
function ConfirmObsClosed(const Verb: String): Boolean;
begin
  Result := True;
  while ObsIsRunning() do
  begin
    if MsgBox('OBS Studio is running. Close it, then click Retry to continue ' + Verb + ' Clip Atlas. Click Cancel to stop.', mbError, MB_RETRYCANCEL) <> IDRETRY then
    begin
      Result := False;
      Exit;
    end;
  end;
end;

// Gives OBS Studio up to ObsExitWaitMs to finish closing. If it is still running after that, ConfirmObsClosed
// asks the user as usual.
procedure WaitForObsExit();
var
  Waited: Integer;
begin
  Waited := 0;
  while ObsIsRunning() and (Waited < ObsExitWaitMs) do
  begin
    Sleep(ObsExitPollMs);
    Waited := Waited + ObsExitPollMs;
  end;
  Log('clip-atlas: waited ' + IntToStr(Waited) + ' ms for ' + ObsProcessName + ' to close.');
end;

function InitializeSetup(): Boolean;
begin
  if WaitForObsRequested() then
    WaitForObsExit();
  Result := ConfirmObsClosed('installing');
end;

function InitializeUninstall(): Boolean;
begin
  Result := ConfirmObsClosed('removing');
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  UserCopyDir: String;
begin
  if CurStep = ssPostInstall then
  begin
    // Setup never deletes this folder. It is the user's own copy, and OBS would load it next to
    // the copy that was just installed. In an elevated install this is the elevated account's profile.
    UserCopyDir := ExpandConstant('{userappdata}\obs-studio\plugins\clip-atlas');
    if DirExists(UserCopyDir) then
      MsgBox('An older copy of Clip Atlas was found in your user profile: ' + UserCopyDir + #13#10 + #13#10 + 'OBS Studio would load both copies. Delete that folder yourself, then restart OBS Studio. Setup does not delete it.', mbInformation, MB_OK);
  end;
  // A silent update shows only a progress window, so this says that it worked and when it takes effect.
  if (CurStep = ssDone) and WizardSilent() and WaitForObsRequested() then
    MsgBox('Clip Atlas {#MyAppVersion} is installed. It loads the next time you start OBS Studio.', mbInformation, MB_OK);
end;
