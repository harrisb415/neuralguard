; NeuralGuard installer (Inno Setup 6).
;
; Built by scripts\package.ps1, which passes /DAppVersion and /DSourceDir:
;   ISCC.exe /DAppVersion=1.0.0 /DSourceDir=..\dist\stage installer\NeuralGuard.iss
;
; NeuralGuard is a single-user, per-machine tool (see README "What it is
; not") - one policy database, no enterprise multi-user story - but it can
; still be INSTALLED either per-user or per-machine, and Inno's own
; admin-mode dialog (PrivilegesRequiredOverridesAllowed=dialog) offers that
; choice. DefaultDirName uses the "auto" constant ({autopf}) specifically so
; that choice is honoured - a literal path like {%USERPROFILE}\NeuralGuard
; would ignore it and install per-user regardless of what was picked, which
; is what used to happen here.
;
; The dashboard's manifest requires Administrator (it commands the service
; over a pipe that only admits Administrators - see app.manifest), so it
; elevates once at launch rather than per-action. That's also why "start at
; login" is a SCHEDULED TASK (/rl highest), not a Startup-folder shortcut:
; Windows does not reliably auto-elevate a requireAdministrator exe launched
; from the Startup folder at logon (observed for real - the tray never came
; up), where a task registered with "run with highest privileges" launches
; elevated with no interactive consent prompt, because that consent was
; already given once, at install time, when the task was registered.

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
; Upgrading from <=1.4.0: remove the retired tray binary and its shortcut, or a
; stale ngtray.exe keeps starting at login and fights the dashboard for the icon
; and the prompt pipe.
Type: files; Name: "{app}\ngtray.exe"
Type: files; Name: "{userstartup}\NeuralGuard.lnk"
; Upgrading from 1.5.0/1.5.1: those shipped "start at login" as an {autostartup}
; shortcut - the one this version replaces with a scheduled task (see [Code]),
; because the shortcut doesn't reliably auto-elevate. Remove it so it can't sit
; alongside the new task and either double-launch or (mostly) just silently fail
; forever, unnoticed, exactly as it did before this fix.
Type: files; Name: "{autostartup}\NeuralGuard.lnk"

[Icons]
Name: "{group}\NeuralGuard"; Filename: "{app}\dashboard\NeuralGuard.exe"
Name: "{group}\Uninstall NeuralGuard"; Filename: "{uninstallexe}"

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

// "Start at login" as a SCHEDULED TASK, not a Startup-folder shortcut. The
// dashboard's manifest requires Administrator, and a plain shortcut to an
// elevated exe in the Startup folder does not reliably auto-elevate at logon -
// this was shipped in 1.5.0/1.5.1 and the tray simply never appeared, silently,
// with nothing logged (the process never got far enough to log anything). A
// task registered with "run with highest privileges" (/rl highest) launches
// elevated with NO interactive consent prompt at logon: the consent is
// effectively given once, right now, by the person running this (elevated, for
// a per-machine install) or already-elevated-equivalent Setup - not on every
// login. /F overwrites a same-named task, so re-running install is idempotent.
procedure InstallStartupTask;
var
  ResultCode: Integer;
  exePath, taskParams: String;
begin
  exePath := ExpandConstant('{app}\dashboard\NeuralGuard.exe');
  // schtasks' /tr wants ONE token; since exePath itself contains spaces and an
  // argument, it's wrapped in its own (escaped) quotes - the standard schtasks
  // pattern for a quoted inner command.
  taskParams := '/create /tn "NeuralGuard" /tr "\"' + exePath + '\" --tray"' +
    ' /sc onlogon /rl highest /f';
  // 'runas': registering an /rl highest task needs an elevated caller, and
  // PrivilegesRequired=lowest means Setup itself might not be one (the "just me"
  // choice). Matches how StopNeuralGuard elevates individually rather than
  // requiring the whole installer to run as admin. If Setup is ALREADY elevated
  // (the "for all users" choice), this doesn't add a second prompt.
  ShellExec('runas', ExpandConstant('{sys}\schtasks.exe'), taskParams, '',
    SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
    StopNeuralGuard;   // best-effort; CloseApplications/restart-manager covers the rest
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('startup') then
    InstallStartupTask;
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

    // Not gated on success/failure - a missing task just makes schtasks return
    // a nonzero code, which nothing here depends on.
    ShellExec('runas', ExpandConstant('{sys}\schtasks.exe'), '/delete /tn "NeuralGuard" /f',
      '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

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
