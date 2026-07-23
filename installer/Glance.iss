#define PayloadDir GetEnv("GLANCE_SOURCE_DIR")
#define OutputDir GetEnv("GLANCE_OUTPUT_DIR")
#define RepoRoot GetEnv("GLANCE_REPO_ROOT")
#define AppVersion GetEnv("GLANCE_VERSION")
#define AppFileVersion GetEnv("GLANCE_FILE_VERSION")

[Setup]
AppId={{F4A2E1FC-BA77-4A24-83BF-A1D5B90A3E13}
AppName=Glance
AppVersion={#AppVersion}
AppVerName=Glance {#AppVersion}
AppPublisher=ElluIFX
AppPublisherURL=https://github.com/ElluIFX/Glance
AppSupportURL=https://github.com/ElluIFX/Glance/issues
AppUpdatesURL=https://github.com/ElluIFX/Glance/releases
DefaultDirName={autopf}\Glance
DefaultGroupName=Glance
DisableProgramGroupPage=yes
LicenseFile={#RepoRoot}\LICENSE
OutputDir={#OutputDir}
OutputBaseFilename=Glance-Setup-{#AppVersion}-x64
SetupIconFile={#RepoRoot}\src\Glance.App\Assets\AppIcon.ico
UninstallDisplayIcon={app}\Glance.exe
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
MinVersion=10.0.22000
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=force
RestartApplications=no
VersionInfoVersion={#AppFileVersion}
VersionInfoCompany=ElluIFX
VersionInfoDescription=Glance Setup
VersionInfoProductName=Glance
VersionInfoProductVersion={#AppVersion}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimplified"; MessagesFile: "{#RepoRoot}\installer\Languages\ChineseSimplified.isl"

[CustomMessages]
english.AdditionalTasks=Additional options:
english.CreateStartMenuShortcut=Create a Start Menu shortcut
english.CreateDesktopShortcut=Create a desktop shortcut
english.StartAtSignIn=Start Glance when signing in to Windows
english.DeleteUserData=Also delete Glance settings, logs, crash dumps, and cached previews?
chinesesimplified.AdditionalTasks=其他选项：
chinesesimplified.CreateStartMenuShortcut=创建开始菜单快捷方式
chinesesimplified.CreateDesktopShortcut=创建桌面快捷方式
chinesesimplified.StartAtSignIn=登录 Windows 时启动 Glance
chinesesimplified.DeleteUserData=同时删除 Glance 设置、日志、崩溃转储和预览缓存吗？

[Tasks]
Name: "startmenuicon"; Description: "{cm:CreateStartMenuShortcut}"; GroupDescription: "{cm:AdditionalTasks}"
Name: "desktopicon"; Description: "{cm:CreateDesktopShortcut}"; GroupDescription: "{cm:AdditionalTasks}"; Flags: unchecked
Name: "startup"; Description: "{cm:StartAtSignIn}"; GroupDescription: "{cm:AdditionalTasks}"

[Files]
Source: "{#PayloadDir}\*"; DestDir: "{app}"; Excludes: "*.exp,*.ilk,*.lib,*.pdb,Glance.Tests.exe"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#RepoRoot}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Glance"; Filename: "{app}\Glance.exe"; Tasks: startmenuicon
Name: "{autodesktop}\Glance"; Filename: "{app}\Glance.exe"; Tasks: desktopicon

[InstallDelete]
Type: files; Name: "{group}\Glance.lnk"; Tasks: not startmenuicon
Type: files; Name: "{autodesktop}\Glance.lnk"; Tasks: not desktopicon

[Run]
Filename: "{app}\Glance.exe"; Parameters: "--set-startup=enabled"; Flags: runasoriginaluser runhidden; Tasks: startup
Filename: "{app}\Glance.exe"; Parameters: "--set-startup=disabled"; Flags: runasoriginaluser runhidden; Tasks: not startup
Filename: "{app}\Glance.exe"; Description: "{cm:LaunchProgram,Glance}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{app}\Glance.exe"; Parameters: "--cleanup-startup"; RunOnceId: "CleanupGlanceStartup"; Flags: runhidden skipifdoesntexist

[Code]
const
  GlanceMutexName = 'Local\Glance.App';
  ShutdownWaitAttempts = 100;
  ShutdownWaitInterval = 100;

var
  DeleteUserData: Boolean;
  UserDataRestartRequired: Boolean;

function HasCommandLineParameter(const Value: String): Boolean;
var
  Index: Integer;
begin
  Result := False;
  for Index := 1 to ParamCount do
  begin
    if CompareText(ParamStr(Index), Value) = 0 then
    begin
      Result := True;
      Exit;
    end;
  end;
end;

function WaitForGlanceExit: Boolean;
var
  Attempt: Integer;
begin
  for Attempt := 1 to ShutdownWaitAttempts do
  begin
    if not CheckForMutexes(GlanceMutexName) then
    begin
      Result := True;
      Exit;
    end;
    Sleep(ShutdownWaitInterval);
  end;
  Result := not CheckForMutexes(GlanceMutexName);
end;

function RequestGlanceShutdown: Boolean;
var
  ExecutablePath: String;
  ResultCode: Integer;
begin
  if not CheckForMutexes(GlanceMutexName) then
  begin
    Result := True;
    Exit;
  end;

  ExecutablePath := ExpandConstant('{app}\Glance.exe');
  if FileExists(ExecutablePath) then
  begin
    Exec(
      ExecutablePath,
      '--shutdown',
      ExpandConstant('{app}'),
      SW_HIDE,
      ewNoWait,
      ResultCode);
  end;
  Result := WaitForGlanceExit;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  RequestGlanceShutdown;
  Result := '';
end;

procedure ScheduleTreeDeletion(const Path: String);
var
  FindRec: TFindRec;
  ChildPath: String;
begin
  if FindFirst(AddBackslash(Path) + '*', FindRec) then
  begin
    try
      repeat
        if (FindRec.Name <> '.') and (FindRec.Name <> '..') then
        begin
          ChildPath := AddBackslash(Path) + FindRec.Name;
          if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
            ScheduleTreeDeletion(ChildPath)
          else
          begin
            try
              RestartReplace(ChildPath, '');
              UserDataRestartRequired := True;
            except
              Log('Could not schedule user data file deletion: ' + ChildPath);
            end;
          end;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;

  try
    RestartReplace(Path, '');
    UserDataRestartRequired := True;
  except
    Log('Could not schedule user data directory deletion: ' + Path);
  end;
end;

procedure DeleteUserDataTree(const Path: String);
begin
  DelTree(Path, True, True, True);
  if DirExists(Path) then
    ScheduleTreeDeletion(Path);
end;

procedure DeleteGlanceUserData;
begin
  RegDeleteKeyIncludingSubkeys(HKCU, 'Software\Glance');
  DeleteUserDataTree(ExpandConstant('{localappdata}\Glance'));
  DeleteUserDataTree(ExpandConstant('{localappdata}\Temp\Glance'));
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    RequestGlanceShutdown;
    DeleteUserData := HasCommandLineParameter('/PURGEUSERDATA');
    if not DeleteUserData and not UninstallSilent then
    begin
      DeleteUserData :=
        SuppressibleMsgBox(
          CustomMessage('DeleteUserData'),
          mbConfirmation,
          MB_YESNO or MB_DEFBUTTON2,
          IDNO) = IDYES;
    end;
    if DeleteUserData then
      DeleteGlanceUserData;
  end;
end;

function UninstallNeedRestart: Boolean;
begin
  Result := UserDataRestartRequired;
end;
