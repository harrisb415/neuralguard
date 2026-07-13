; NeuralGuard installer (Inno Setup 6).
;
; Built by scripts\package.ps1, which passes /DAppVersion and /DSourceDir:
;   ISCC.exe /DAppVersion=1.0.0 /DSourceDir=..\dist\stage installer\NeuralGuard.iss
;
; NeuralGuard is a single-user, per-machine tool (see README "What it is
; not"): there is no multi-user/shared install story, and the dashboard
; writes directly to its own policy database for everyday actions (adding a
; rule, changing autonomy) without elevating. So this installs per-user,
; with no admin prompt at install time - only the actions that genuinely
; need it (installing the background service, flipping on enforcement)
; elevate individually, at the moment they're used, exactly as they do when
; run from a build instead of an installer.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\dist\stage"
#endif

[Setup]
AppId={{2A6E9C2E-6C7B-4E27-9E4B-7B7B8E9C0B41}
AppName=NeuralGuard
AppVersion={#AppVersion}
AppVerName=NeuralGuard {#AppVersion}
AppPublisher=Brendan Harris
AppPublisherURL=https://github.com/harrisb415/NeuralGuard
AppSupportURL=https://github.com/harrisb415/NeuralGuard/issues
AppUpdatesURL=https://github.com/harrisb415/NeuralGuard/releases
DefaultDirName={%USERPROFILE}\NeuralGuard
DefaultGroupName=NeuralGuard
DisableDirPage=no
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile=..\LICENSE
OutputDir=..\dist
OutputBaseFilename=NeuralGuard-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; Let the Restart Manager close any app holding a file we're about to replace
; (belt-and-suspenders alongside the taskkill in [Code]); needed so an in-app
; update over a running dashboard can overwrite locked binaries/DLLs.
CloseApplications=yes
RestartApplications=no
UninstallDisplayIcon={app}\ngtray.exe
UninstallDisplayName=NeuralGuard {#AppVersion}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "startup"; Description: "Start the NeuralGuard tray icon when I log in"; Flags: unchecked

[Files]
Source: "{#SourceDir}\ngmon.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\ngd.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\ngctl.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\ngtray.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\onnxruntime.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\dashboard\*"; DestDir: "{app}\dashboard"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\NeuralGuard"; Filename: "{app}\ngtray.exe"
Name: "{group}\Uninstall NeuralGuard"; Filename: "{uninstallexe}"
Name: "{userstartup}\NeuralGuard"; Filename: "{app}\ngtray.exe"; Tasks: startup

[Run]
; ngtray.exe's manifest requires Administrator. [Run] entries launch via
; CreateProcess by default, which can't elevate a manifested-admin target
; (fails with "CreateProcess failed; code 740" - ERROR_ELEVATION_REQUIRED).
; shellexec routes it through ShellExecute instead, which Windows elevates
; with a normal UAC prompt - the same path Start Menu/desktop shortcuts
; already use (shortcut activation is always shell-based, so those work
; without this flag).
Filename: "{app}\ngtray.exe"; Description: "Launch NeuralGuard now"; Flags: nowait postinstall skipifsilent shellexec

[Code]
// An upgrade-over-a-running-install would fail to overwrite locked .exe/.dll
// files, so make sure nothing from a previous install is still running
// before files are copied.
procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if CurStep = ssInstall then
  begin
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM ngtray.exe /IM NeuralGuard.exe /IM ngd.exe',
      '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end;
end;

// Best-effort cleanup on uninstall: remove the background service and clear
// any active enforcement filters before the binaries are deleted, so an
// uninstall can never leave a stray service or a stuck block behind. Each
// step needs Administrator regardless of how this uninstaller itself was
// launched, so elevate them individually (matches how the dashboard/tray
// elevate individual actions rather than requiring the whole app to run
// as admin).
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
  okSvc, okPanic: Boolean;
begin
  if CurUninstallStep = usUninstall then
  begin
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM ngtray.exe /IM NeuralGuard.exe /IM ngd.exe',
      '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

    // MsgBox is a blocking modal dialog - /VERYSILENT only suppresses the
    // wizard's own pages, not explicit MsgBox calls, so a silent/unattended
    // uninstall (no one present to click OK) would hang here forever unless
    // these are skipped.
    if not UninstallSilent then
      MsgBox('NeuralGuard is removing its background service and any active ' +
        'firewall rules. You may see one or two Administrator prompts - please ' +
        'approve them so nothing is left running after uninstall.',
        mbInformation, MB_OK);

    okSvc := ShellExec('runas', ExpandConstant('{app}\ngd.exe'), 'uninstall',
      '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    okPanic := ShellExec('runas', ExpandConstant('{app}\ngctl.exe'), 'panic',
      '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

    if ((not okSvc) or (not okPanic)) and (not UninstallSilent) then
      MsgBox('One of the cleanup steps did not complete (an Administrator ' +
        'prompt may have been declined). If NeuralGuard was enforcing, run ' +
        '"ngctl panic" and check "sc query NeuralGuard" manually to confirm ' +
        'nothing was left behind.', mbError, MB_OK);
  end;
end;
