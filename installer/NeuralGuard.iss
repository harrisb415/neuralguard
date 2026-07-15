; NeuralGuard installer (Inno Setup 6).
;
; Built by scripts\package.ps1, which passes /DAppVersion and /DSourceDir:
;   ISCC.exe /DAppVersion=1.0.0 /DSourceDir=..\dist\stage installer\NeuralGuard.iss
;
; NeuralGuard is a single-user, per-machine tool (see README "What it is
; not") - one policy database, no enterprise multi-user story - but it can
; still be INSTALLED either per-user or per-machine, and Inno's own
; admin-mode dialog (PrivilegesRequiredOverridesAllowed=dialog) offers that
; choice. DefaultDirName/the startup shortcut use the "auto" constants
; ({autopf}, {autostartup}) specifically so that choice is honoured - a
; literal path like {%USERPROFILE}\NeuralGuard would ignore it and install
; per-user regardless of what was picked, which is what used to happen here.
;
; The dashboard's manifest requires Administrator (it commands the service
; over a pipe that only admits Administrators - see app.manifest), so it
; elevates once at launch rather than per-action.

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
DefaultDirName={autopf}\NeuralGuard
DefaultGroupName=NeuralGuard
DisableDirPage=no
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile=..\LICENSE
SetupIconFile=..\assets\icons\neuralguard.ico
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
UninstallDisplayIcon={app}\dashboard\NeuralGuard.exe
UninstallDisplayName=NeuralGuard {#AppVersion}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
; Checked by default. The tray is the ONLY way to see or control an installed
; service - and it's what answers ngd's "allow this connection?" prompts. Leaving
; it opt-in meant you could install a background firewall and get no icon, no
; prompts, and no way to stop it without a command line. That is what happened.
Name: "startup"; Description: "Start NeuralGuard when I log in (tray icon)"

[Files]
; ngtray.exe is gone - the dashboard owns the tray icon now (see gui/.../Tray.cpp).
Source: "{#SourceDir}\ngmon.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\ngd.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\ngctl.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\onnxruntime.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\dashboard\*"; DestDir: "{app}\dashboard"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
; Upgrading from <=1.4.0: remove the retired tray binary and its shortcuts, or a
; stale ngtray.exe keeps starting at login and fights the dashboard for the icon
; and the prompt pipe.
Type: files; Name: "{app}\ngtray.exe"
Type: files; Name: "{userstartup}\NeuralGuard.lnk"

[Icons]
; One entry, one frontend. --tray starts it as just an icon, no window.
Name: "{group}\NeuralGuard"; Filename: "{app}\dashboard\NeuralGuard.exe"
Name: "{group}\Uninstall NeuralGuard"; Filename: "{uninstallexe}"
Name: "{autostartup}\NeuralGuard"; Filename: "{app}\dashboard\NeuralGuard.exe"; Parameters: "--tray"; Tasks: startup

[Run]
; The dashboard's manifest requires Administrator (it commands the service over a
; pipe that only admits Administrators). [Run] launches via CreateProcess by
; default, which can't elevate a manifested-admin target ("CreateProcess failed;
; code 740" = ERROR_ELEVATION_REQUIRED). shellexec routes it through ShellExecute,
; which Windows elevates normally - the same path Start Menu shortcuts already use.
Filename: "{app}\dashboard\NeuralGuard.exe"; Description: "Launch NeuralGuard now"; Flags: nowait postinstall skipifsilent shellexec

[Code]
// Stop a previous install before touching its files. The background service must
// go through the SCM (`ngd stop`), NOT taskkill: killing ngd.exe doesn't read as
// a stop, it reads as a crash, and the service is deliberately configured with
// restart-on-failure - so it comes back ~5s later, still holding ngd.exe and the
// database locked. An upgrade then silently fails to replace the engine, and an
// uninstall leaves a running, unmanageable service behind still filtering traffic
// (observed for real). The GUI processes have no SCM lifecycle, so taskkill is
// right for those. Returns False if the service may have been left running - e.g.
// an Administrator prompt was declined - so callers can warn instead of guessing.
function StopNeuralGuard: Boolean;
var
  ResultCode: Integer;
begin
  Result := True;
  if FileExists(ExpandConstant('{app}\ngd.exe')) then
    Result := ShellExec('runas', ExpandConstant('{app}\ngd.exe'),
                ExpandConstant('stop "{app}\ngpolicy.db"'), '',
                SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM ngtray.exe /IM NeuralGuard.exe',
    '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
    StopNeuralGuard;   // best-effort; CloseApplications/restart-manager covers the rest
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
  okStop, okSvc, okPanic: Boolean;
begin
  if CurUninstallStep = usUninstall then
  begin
    // MsgBox is a blocking modal dialog - /VERYSILENT only suppresses the
    // wizard's own pages, not explicit MsgBox calls, so a silent/unattended
    // uninstall (no one present to click OK) would hang here forever unless
    // these are skipped. Warn BEFORE the elevation prompts, not after.
    if not UninstallSilent then
      MsgBox('NeuralGuard is stopping its background service and removing any ' +
        'active firewall rules. You may see a few Administrator prompts - please ' +
        'approve them. If you decline, a hidden service can be left running and ' +
        'still filtering your traffic, with the tools to stop it already deleted.',
        mbInformation, MB_OK);

    okStop := StopNeuralGuard;   // SCM stop, never a kill - see StopNeuralGuard
    okSvc := ShellExec('runas', ExpandConstant('{app}\ngd.exe'), 'uninstall',
      '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    okPanic := ShellExec('runas', ExpandConstant('{app}\ngctl.exe'), 'panic',
      '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

    if ((not okStop) or (not okSvc) or (not okPanic)) and (not UninstallSilent) then
      MsgBox('One of the cleanup steps did not complete (an Administrator prompt ' +
        'may have been declined). Check "sc query NeuralGuard" from an elevated ' +
        'prompt: if the service still exists, run "sc stop NeuralGuard" - it is ' +
        'marked for deletion and disappears once it actually stops. Then run ' +
        '"ngctl panic" to clear any filters it left active.', mbError, MB_OK);
  end;
end;
