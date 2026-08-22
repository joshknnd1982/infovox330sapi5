; Infovox 330 SAPI 5 - Windows installer
;
; Built by tools\build_all.ps1, which passes the paths in as /D defines. To compile it by
; hand:
;
;   ISCC.exe /DStageDir=..\output /DVersion=1.0.0 installer\infovox330_sapi5.iss
;
; Accessibility notes, since this installer is meant to be usable by the people most likely
; to want these voices:
;   * Every page is a standard Inno Setup page built from real Win32 controls, which screen
;     readers read natively. Nothing is owner-drawn and there is no splash screen.
;   * Custom controls carry captions and explicit TabOrder, and each input is preceded by a
;     label that describes it.
;   * Nothing steals focus, and no page auto-advances.
;   * Every outcome that matters - registration succeeded, registration failed, how many
;     voices were installed - is stated in text on the final page and written to the log,
;     not signalled by a colour or an icon.
;   * SetupLogging is on, so a failed install always leaves a full log behind.

#ifndef StageDir
  #define StageDir "..\output"
#endif
#ifndef Version
  #define Version "1.0.0"
#endif

#define AppName        "Infovox 330 SAPI 5"
#define AppPublisher   "Infovox 330 SAPI 5 project"
#define EngineDllName  "Infovox330SAPI5.dll"
#define ServerExeName  "Infovox330Server.exe"

[Setup]
#ifdef Probe
; A separate identity so an accessibility probe run can never be mistaken for, or clash
; with, a real installation.
AppId={{2F4A8E17-6C55-4E90-9A02-6B1D77C3E840}
#else
AppId={{7C0C1A93-4B31-4A0D-9B75-2E8D4A31F5C2}
#endif
AppName={#AppName}
AppVersion={#Version}
AppVerName={#AppName} {#Version}
AppPublisher={#AppPublisher}
AppComments=Sixteen Infovox 330 voices in twelve languages, available to any SAPI 5 application.
UninstallDisplayName={#AppName}
DefaultDirName={autopf}\Infovox330SAPI5
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
OutputDir={#StageDir}\..\dist
#ifdef Probe
OutputBaseFilename=Infovox330SAPI5_AccessibilityProbe
#else
OutputBaseFilename=Infovox330SAPI5_Setup_{#Version}
#endif
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

; Both the CLSID registrations and the speech TokenEnums key live under HKLM.
; Compiling with /DProbe builds the same wizard without elevation and without the 145 MB of
; payload, which is how the pages are checked against a screen reader.
#ifdef Probe
PrivilegesRequired=lowest
#else
PrivilegesRequired=admin
#endif

; Install into the 64-bit Program Files on a 64-bit Windows so that {sys} means the 64-bit
; System32 and {syswow64} means the 32-bit one, which is what the registration steps below
; depend on. On 32-bit Windows only the 32-bit half is installed.
ArchitecturesInstallIn64BitMode=x64compatible

; A full log is written to the temp folder and copied beside the program at the end.
SetupLogging=yes

AlwaysShowComponentsList=yes
ShowComponentSizes=yes
InfoBeforeFile={#StageDir}\..\installer\before_install.txt

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full";   Description: "Everything - all sixteen voices in all twelve languages"
Name: "custom"; Description: "Choose which languages to install"; Flags: iscustom

[Components]
Name: "engine";     Description: "Speech engine and SAPI 5 interfaces (required)"; Types: full custom; Flags: fixed
Name: "voices";     Description: "Voices"; Types: full custom; Flags: fixed
Name: "voices\am";  Description: "American English - Larry, Lucy";  Types: full
Name: "voices\br";  Description: "British English - Roger";         Types: full
Name: "voices\da";  Description: "Danish - Poul";                   Types: full
Name: "voices\du";  Description: "Dutch - Rik";                     Types: full
Name: "voices\fi";  Description: "Finnish - Matti";                 Types: full
Name: "voices\fr";  Description: "French - Pierre";                 Types: full
Name: "voices\ge";  Description: "German - Gerhard, Helga";         Types: full
Name: "voices\ic";  Description: "Icelandic - Snorri";              Types: full
Name: "voices\it";  Description: "Italian - Roberto";               Types: full
Name: "voices\no";  Description: "Norwegian - Trygve, Vegard";      Types: full
Name: "voices\sp";  Description: "Spanish - Juan";                  Types: full
Name: "voices\sw";  Description: "Swedish - AnnMarie, Ingmar";      Types: full
Name: "tools";      Description: "Diagnostic tools (sample renderer and test harness)"; Types: full

[Files]
#ifdef Probe
Source: "{#StageDir}\..\installer\before_install.txt"; DestDir: "{app}"; Components: engine
#else
; ---- the SAPI 5 interfaces -----------------------------------------------------------
; The 32-bit DLL serves 32-bit applications; the 64-bit DLL serves 64-bit ones and reaches
; the engine through the 32-bit helper, because the engine has no 64-bit build.
Source: "{#StageDir}\{#EngineDllName}";      DestDir: "{app}";     Components: engine; Flags: ignoreversion
Source: "{#StageDir}\{#ServerExeName}";      DestDir: "{app}";     Components: engine; Flags: ignoreversion
Source: "{#StageDir}\infovox_host.dll";      DestDir: "{app}";     Components: engine; Flags: ignoreversion
Source: "{#StageDir}\ivx_speak.exe";         DestDir: "{app}";     Components: engine; Flags: ignoreversion
Source: "{#StageDir}\..\installer\open_logs.cmd"; DestDir: "{app}"; Components: engine; Flags: ignoreversion
Source: "{#StageDir}\x64\{#EngineDllName}";  DestDir: "{app}\x64"; Components: engine; Flags: ignoreversion; Check: Is64BitInstallMode
Source: "{#StageDir}\x64\ivx_speak.exe";     DestDir: "{app}\x64"; Components: engine; Flags: ignoreversion; Check: Is64BitInstallMode

; ---- the engine itself ---------------------------------------------------------------
Source: "{#StageDir}\Ivx330\Ivx330nt.dll";   DestDir: "{app}\Ivx330"; Components: engine; Flags: ignoreversion
Source: "{#StageDir}\Ivx330\Sx32w.dll";      DestDir: "{app}\Ivx330"; Components: engine; Flags: ignoreversion
Source: "{#StageDir}\Ivx330\cryput.dll";     DestDir: "{app}\Ivx330"; Components: engine; Flags: ignoreversion

; ---- voice data, one component per language ------------------------------------------
; VoiceDescriptions.txt lists every voice; the engine skips any whose data is absent, so a
; part-installed set stays consistent.
Source: "{#StageDir}\Voices Ivx330\VoiceDescriptions.txt"; DestDir: "{app}\Voices Ivx330"; Components: voices; Flags: ignoreversion

Source: "{#StageDir}\Voices Ivx330\am0*.*";     DestDir: "{app}\Voices Ivx330"; Components: voices\am; Flags: ignoreversion
Source: "{#StageDir}\Voices Ivx330\Amrules.ivx";DestDir: "{app}\Voices Ivx330"; Components: voices\am; Flags: ignoreversion
Source: "{#StageDir}\Voices Ivx330\br0*.*";     DestDir: "{app}\Voices Ivx330"; Components: voices\br; Flags: ignoreversion
Source: "{#StageDir}\Voices Ivx330\BLRULES.IVX";DestDir: "{app}\Voices Ivx330"; Components: voices\br; Flags: ignoreversion
Source: "{#StageDir}\Voices Ivx330\da0*.*";     DestDir: "{app}\Voices Ivx330"; Components: voices\da; Flags: ignoreversion
Source: "{#StageDir}\Voices Ivx330\DARULES.IVX";DestDir: "{app}\Voices Ivx330"; Components: voices\da; Flags: ignoreversion
Source: "{#StageDir}\Voices Ivx330\darules.dll";DestDir: "{app}\Voices Ivx330"; Components: voices\da; Flags: ignoreversion
Source: "{#StageDir}\Voices Ivx330\du0*.*";     DestDir: "{app}\Voices Ivx330"; Components: voices\du; Flags: ignoreversion
Source: "{#StageDir}\Voices Ivx330\Durules.IVX";DestDir: "{app}\Voices Ivx330"; Components: voices\du; Flags: ignoreversion
Source: "{#StageDir}\Voices Ivx330\fi0*.*";     DestDir: "{app}\Voices Ivx330"; Components: voices\fi; Flags: ignoreversion
Source: "{#StageDir}\Voices Ivx330\FIRULES.IVX";DestDir: "{app}\Voices Ivx330"; Components: voices\fi; Flags: ignoreversion
Source: "{#StageDir}\Voices Ivx330\fr0*.*";     DestDir: "{app}\Voices Ivx330"; Components: voices\fr; Flags: ignoreversion
Source: "{#StageDir}\Voices Ivx330\Frrules.IVX";DestDir: "{app}\Voices Ivx330"; Components: voices\fr; Flags: ignoreversion
Source: "{#StageDir}\Voices Ivx330\ge0*.*";     DestDir: "{app}\Voices Ivx330"; Components: voices\ge; Flags: ignoreversion
Source: "{#StageDir}\Voices Ivx330\Gerules.ivx";DestDir: "{app}\Voices Ivx330"; Components: voices\ge; Flags: ignoreversion
Source: "{#StageDir}\Voices Ivx330\ic0*.*";     DestDir: "{app}\Voices Ivx330"; Components: voices\ic; Flags: ignoreversion
Source: "{#StageDir}\Voices Ivx330\ICRULES.IVX";DestDir: "{app}\Voices Ivx330"; Components: voices\ic; Flags: ignoreversion
Source: "{#StageDir}\Voices Ivx330\it0*.*";     DestDir: "{app}\Voices Ivx330"; Components: voices\it; Flags: ignoreversion
Source: "{#StageDir}\Voices Ivx330\ITRULES.IVX";DestDir: "{app}\Voices Ivx330"; Components: voices\it; Flags: ignoreversion
Source: "{#StageDir}\Voices Ivx330\no0*.*";     DestDir: "{app}\Voices Ivx330"; Components: voices\no; Flags: ignoreversion
Source: "{#StageDir}\Voices Ivx330\NORULES.IVX";DestDir: "{app}\Voices Ivx330"; Components: voices\no; Flags: ignoreversion
Source: "{#StageDir}\Voices Ivx330\sp0*.*";     DestDir: "{app}\Voices Ivx330"; Components: voices\sp; Flags: ignoreversion
Source: "{#StageDir}\Voices Ivx330\SPRULES.IVX";DestDir: "{app}\Voices Ivx330"; Components: voices\sp; Flags: ignoreversion
Source: "{#StageDir}\Voices Ivx330\sw0*.*";     DestDir: "{app}\Voices Ivx330"; Components: voices\sw; Flags: ignoreversion
Source: "{#StageDir}\Voices Ivx330\Swrules.ivx";DestDir: "{app}\Voices Ivx330"; Components: voices\sw; Flags: ignoreversion

; ---- diagnostics ----------------------------------------------------------------------
Source: "{#StageDir}\ivx_render.exe";        DestDir: "{app}";     Components: tools; Flags: ignoreversion
Source: "{#StageDir}\ivx_sapitest.exe";      DestDir: "{app}";     Components: tools; Flags: ignoreversion
Source: "{#StageDir}\x64\ivx_sapitest.exe";  DestDir: "{app}\x64"; Components: tools; Flags: ignoreversion; Check: Is64BitInstallMode
#endif

[Icons]
Name: "{group}\Speak a test sentence"; Filename: "{app}\ivx_speak.exe"; Comment: "Speaks one sentence using an Infovox 330 voice"
Name: "{group}\List installed voices"; Filename: "{app}\ivx_speak.exe"; Parameters: "--list"; Comment: "Lists every SAPI 5 voice Windows can see"
Name: "{group}\Open the log folder";   Filename: "{app}\open_logs.cmd"; Comment: "Opens the folder the speech engine writes its logs to"

[Run]
Filename: "{app}\ivx_speak.exe"; Description: "Speak a test sentence now"; Flags: postinstall nowait skipifsilent unchecked

[UninstallDelete]
Type: filesandordirs; Name: "{app}\Ivx330"
Type: filesandordirs; Name: "{app}\Voices Ivx330"
Type: files;          Name: "{app}\install.log"
Type: files;          Name: "{app}\open_logs.cmd"

[Code]
{ Used to give the components tree an accessible name. Inno exposes no property for it, and
  without one a screen reader announces the control as nothing but its type - on the page
  where the user chooses which languages to install, which is the page that matters most. }
procedure SetWindowTextW(Wnd: HWND; Text: String);
  external 'SetWindowTextW@user32.dll stdcall';

var
  RegisteredX86: Boolean;
  RegisteredX64: Boolean;
  RegistrationNotes: String;

procedure Note(const S: String);
begin
  Log('[infovox] ' + S);
  if RegistrationNotes <> '' then
    RegistrationNotes := RegistrationNotes + #13#10;
  RegistrationNotes := RegistrationNotes + S;
end;

{ The helper keeps the engine loaded, so it has to go before files are replaced or removed. }
procedure StopHelper;
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/IM {#ServerExeName} /F', '',
       SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

{ regsvr32 is bitness-specific: the copy in System32 registers 64-bit DLLs, the copy in
  SysWOW64 registers 32-bit ones. Getting these the wrong way round is the classic way to
  end up with a voice that is registered but never enumerated. }
function RunRegsvr(const Regsvr32, DllPath, Args: String): Boolean;
var
  ResultCode: Integer;
begin
  Result := Exec(Regsvr32, Args + ' "' + DllPath + '"', '', SW_HIDE,
                 ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
  Log(Format('[infovox] %s %s "%s" -> %d', [Regsvr32, Args, DllPath, ResultCode]));
end;

function VoicesRegistered: Boolean;
begin
  Result := RegKeyExists(HKEY_LOCAL_MACHINE,
    'SOFTWARE\Microsoft\Speech\Voices\TokenEnums\Infovox330');
  if not Result then
    Result := RegKeyExists(HKEY_LOCAL_MACHINE,
      'SOFTWARE\WOW6432Node\Microsoft\Speech\Voices\TokenEnums\Infovox330');
end;

procedure InitializeWizard;
begin
  { Verified with tools\check_installer_a11y.ps1, which reads every control through MSAA
    the way NVDA does. Before this line the components tree reported no name at all. }
  SetWindowTextW(WizardForm.ComponentsList.Handle, 'Components to install');
  SetWindowTextW(WizardForm.TypesCombo.Handle, 'Installation type');
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  LogPath: String;
begin
  if CurStep = ssInstall then
  begin
    StopHelper;
  end
  else if CurStep = ssPostInstall then
  begin
    RegistrationNotes := '';

    if Is64BitInstallMode then
    begin
      RegisteredX64 := RunRegsvr(ExpandConstant('{sys}\regsvr32.exe'),
                                 ExpandConstant('{app}\x64\{#EngineDllName}'), '/s');
      RegisteredX86 := RunRegsvr(ExpandConstant('{syswow64}\regsvr32.exe'),
                                 ExpandConstant('{app}\{#EngineDllName}'), '/s');
      if RegisteredX64 then
        Note('The 64-bit SAPI 5 interface was registered.')
      else
        Note('The 64-bit SAPI 5 interface could NOT be registered.');
      if RegisteredX86 then
        Note('The 32-bit SAPI 5 interface was registered.')
      else
        Note('The 32-bit SAPI 5 interface could NOT be registered.');
    end
    else
    begin
      RegisteredX64 := True;  { nothing to do on 32-bit Windows }
      RegisteredX86 := RunRegsvr(ExpandConstant('{sys}\regsvr32.exe'),
                                 ExpandConstant('{app}\{#EngineDllName}'), '/s');
      if RegisteredX86 then
        Note('The 32-bit SAPI 5 interface was registered.')
      else
        Note('The 32-bit SAPI 5 interface could NOT be registered.');
    end;

    if VoicesRegistered then
      Note('Windows speech settings can now see the Infovox 330 voices.')
    else
      Note('Warning: the speech voice list was not updated. See the log named below.');

    { Keep the installer's own log with the program, where a bug report can find it. }
    LogPath := ExpandConstant('{log}');
    if LogPath <> '' then
    begin
      CopyFile(LogPath, ExpandConstant('{app}\install.log'), False);
      Note('A full installation log was saved as ' + ExpandConstant('{app}\install.log') + '.');
    end;
    { Written as an unexpanded environment variable on purpose: under an administrative
      install the localappdata constant resolves to the administrator's folder, not the
      folder the person who actually uses the voices will find their logs in. }
    Note('The speech engine writes its own logs to '
         + '%LOCALAPPDATA%\Infovox330 SAPI5\Logs'
         + ' - there is a shortcut to it in the Start menu.');
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    StopHelper;
    if IsWin64 then
    begin
      RunRegsvr(ExpandConstant('{sys}\regsvr32.exe'),
                ExpandConstant('{app}\x64\{#EngineDllName}'), '/s /u');
      RunRegsvr(ExpandConstant('{syswow64}\regsvr32.exe'),
                ExpandConstant('{app}\{#EngineDllName}'), '/s /u');
    end
    else
      RunRegsvr(ExpandConstant('{sys}\regsvr32.exe'),
                ExpandConstant('{app}\{#EngineDllName}'), '/s /u');
    { Give the loader a moment to let go of the DLLs before the files are deleted. }
    Sleep(1500);
  end;
end;

{ The finish page normally shows one fixed line. Replacing it with the notes above means a
  screen reader reads the actual outcome - which interfaces registered, where the logs are -
  instead of a generic success message. }
procedure CurPageChanged(CurPageID: Integer);
var
  Blank: String;
  Summary: String;
begin
  if (CurPageID = wpFinished) and (RegistrationNotes <> '') then
  begin
    Blank := #13#10 + #13#10;
    Summary := '{#AppName} has been installed.' + Blank + RegistrationNotes + Blank +
      'Choose a voice in your screen reader or in Windows speech settings, or tick the box ' +
      'below to hear one now.';
    WizardForm.FinishedLabel.AutoSize := False;
    WizardForm.FinishedLabel.Height := WizardForm.FinishedLabel.Parent.ClientHeight - 8;
    WizardForm.FinishedLabel.Caption := Summary;
  end;
end;
