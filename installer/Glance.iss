#define PayloadDir GetEnv("GLANCE_SOURCE_DIR")
#define ComponentsDir GetEnv("GLANCE_COMPONENTS_DIR")
#define SourcesDir GetEnv("GLANCE_SOURCES_DIR")
#define ComponentInnoDir GetEnv("GLANCE_COMPONENT_INNO_DIR")
#define OutputDir GetEnv("GLANCE_OUTPUT_DIR")
#define RepoRoot GetEnv("GLANCE_REPO_ROOT")
#define AppVersion GetEnv("GLANCE_VERSION")
#define AppFileVersion GetEnv("GLANCE_FILE_VERSION")
#include ComponentInnoDir + "\component-catalog.iss"

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
MinVersion=10.0.17763
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=force
RestartApplications=no
VersionInfoVersion={#AppFileVersion}
VersionInfoCompany=ElluIFX
VersionInfoDescription=Glance Setup
VersionInfoProductName=Glance
VersionInfoProductVersion={#AppVersion}
UsePreviousSetupType=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimplified"; MessagesFile: "{#RepoRoot}\installer\Languages\ChineseSimplified.isl"

[CustomMessages]
english.AdditionalTasks=Additional options:
english.CreateStartMenuShortcut=Create a Start Menu shortcut
english.CreateDesktopShortcut=Create a desktop shortcut
english.StartAtSignIn=Start Glance when signing in to Windows
english.DeleteUserData=Also delete Glance settings, logs, crash dumps, and cached previews?
english.FullInstallation=Full installation
english.CoreInstallation=Core only
english.CustomInstallation=Custom installation
english.ComponentsGroup=Add-on components
english.SourcesGroup=Add-on sources
chinesesimplified.AdditionalTasks=其他选项：
chinesesimplified.CreateStartMenuShortcut=创建开始菜单快捷方式
chinesesimplified.CreateDesktopShortcut=创建桌面快捷方式
chinesesimplified.StartAtSignIn=登录 Windows 时启动 Glance
chinesesimplified.DeleteUserData=同时删除 Glance 设置、日志、崩溃转储和预览缓存吗？
chinesesimplified.FullInstallation=完整安装
chinesesimplified.CoreInstallation=仅核心程序
chinesesimplified.CustomInstallation=自定义安装
chinesesimplified.ComponentsGroup=附加组件
chinesesimplified.SourcesGroup=附加来源
#include ComponentInnoDir + "\component-messages.iss"

[Types]
Name: "full"; Description: "{cm:FullInstallation}"
Name: "core"; Description: "{cm:CoreInstallation}"
Name: "custom"; Description: "{cm:CustomInstallation}"; Flags: iscustom

[Components]
#include ComponentInnoDir + "\component-definitions.iss"

[Tasks]
Name: "startmenuicon"; Description: "{cm:CreateStartMenuShortcut}"; GroupDescription: "{cm:AdditionalTasks}"
Name: "desktopicon"; Description: "{cm:CreateDesktopShortcut}"; GroupDescription: "{cm:AdditionalTasks}"; Flags: unchecked
Name: "startup"; Description: "{cm:StartAtSignIn}"; GroupDescription: "{cm:AdditionalTasks}"

[Files]
Source: "{#PayloadDir}\*"; DestDir: "{app}"; Excludes: "*.exp,*.ilk,*.lib,*.pdb,Glance.Tests.exe,components\*,sources\*"; Flags: ignoreversion recursesubdirs createallsubdirs
#include ComponentInnoDir + "\component-files.iss"
Source: "{#RepoRoot}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Dirs]
Name: "{app}\components"
Name: "{app}\sources"

[Icons]
Name: "{group}\Glance"; Filename: "{app}\Glance.exe"; Tasks: startmenuicon
Name: "{autodesktop}\Glance"; Filename: "{app}\Glance.exe"; Tasks: desktopicon

[InstallDelete]
Type: filesandordirs; Name: "{app}\plugins"
#include ComponentInnoDir + "\component-delete.iss"
Type: files; Name: "{group}\Glance.lnk"; Tasks: not startmenuicon
Type: files; Name: "{autodesktop}\Glance.lnk"; Tasks: not desktopicon

[Run]
Filename: "{app}\Glance.exe"; Parameters: "--set-startup=enabled"; Flags: runasoriginaluser runhidden; Tasks: startup
Filename: "{app}\Glance.exe"; Parameters: "--set-startup=disabled"; Flags: runasoriginaluser runhidden; Tasks: not startup
Filename: "{app}\Glance.exe"; Description: "{cm:LaunchProgram,Glance}"; Flags: nowait postinstall skipifsilent
Filename: "{app}\Glance.exe"; Flags: runasoriginaluser runhidden nowait; Check: IsAutomaticUpdate

[UninstallRun]
Filename: "{app}\Glance.exe"; Parameters: "--cleanup-startup"; RunOnceId: "CleanupGlanceStartup"; Flags: runhidden skipifdoesntexist

[Code]
const
  GlanceMutexName = 'Local\Glance.App';
  LegacyComponentCatalog = 'adobe,model3d,office';
  ShutdownWaitAttempts = 100;
  ShutdownWaitInterval = 100;

var
  DeleteUserData: Boolean;
  UserDataRestartRequired: Boolean;
  UpdatingComponentSelection: Boolean;
  ComponentSelectionSnapshot: String;
  ComponentsListClickCheckPrevious: TNotifyEvent;

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

function IsAutomaticUpdate: Boolean;
begin
  Result := HasCommandLineParameter('/GLANCEUPDATE');
end;

function ComponentCatalogContains(
  const Catalog: String;
  const ComponentName: String): Boolean;
begin
  Result := Pos(
    ',' + Lowercase(ComponentName) + ',',
    ',' + Lowercase(Catalog) + ',') > 0;
end;

function AddComponentToSelection(
  const Selection: String;
  const ComponentName: String): String;
begin
  if Selection = '' then
    Result := ComponentName
  else if ComponentCatalogContains(Selection, ComponentName) then
    Result := Selection
  else
    Result := Selection + ',' + ComponentName;
end;

function RemoveComponentFromSelection(
  const Selection: String;
  const ComponentName: String): String;
var
  Working: String;
begin
  Working := ',' + Selection + ',';
  StringChangeEx(
    Working,
    ',' + ComponentName + ',',
    ',',
    True);
  if (Length(Working) > 0) and (Working[1] = ',') then
    Delete(Working, 1, 1);
  if (Length(Working) > 0) and (Working[Length(Working)] = ',') then
    Delete(Working, Length(Working), 1);
  Result := Working;
end;

procedure ResolveComponentDependency(
  const Dependent: String;
  const Dependency: String;
  const PreviousSelection: String);
var
  Selection: String;
begin
  Selection := WizardSelectedComponents(False);
  if ComponentCatalogContains(Selection, Dependent) and
     not ComponentCatalogContains(Selection, Dependency) then
  begin
    if ComponentCatalogContains(PreviousSelection, Dependent) and
       ComponentCatalogContains(PreviousSelection, Dependency) then
      Selection := RemoveComponentFromSelection(Selection, Dependent)
    else
      Selection := AddComponentToSelection(Selection, Dependency);
    WizardSelectComponents(Selection);
  end;
end;

procedure NormalizeComponentDependencies(const PreviousSelection: String);
var
  SelectionBefore: String;
begin
  repeat
    SelectionBefore := WizardSelectedComponents(False);
#include ComponentInnoDir + "\component-dependencies.iss"
  until SelectionBefore = WizardSelectedComponents(False);
end;

procedure SelectComponentIfNew(
  const ComponentId: String;
  const InstallerName: String;
  const PreviousComponentCatalog: String);
begin
  if not ComponentCatalogContains(PreviousComponentCatalog, ComponentId) then
    WizardSelectComponents(InstallerName);
end;

procedure ComponentsListClickCheck(Sender: TObject);
var
  PreviousSelection: String;
begin
  if UpdatingComponentSelection then
    Exit;
  UpdatingComponentSelection := True;
  PreviousSelection := ComponentSelectionSnapshot;
  NormalizeComponentDependencies(PreviousSelection);
  ComponentSelectionSnapshot := WizardSelectedComponents(False);
  UpdatingComponentSelection := False;
  if ComponentsListClickCheckPrevious <> nil then
    ComponentsListClickCheckPrevious(Sender);
end;

procedure InitializeWizard;
var
  PreviousComponentCatalog: String;
  PreviousSourceCatalog: String;
begin
  ComponentsListClickCheckPrevious := WizardForm.ComponentsList.OnClickCheck;
  PreviousComponentCatalog := GetPreviousData(
    'KnownComponents', LegacyComponentCatalog);
  PreviousSourceCatalog := GetPreviousData('KnownSources', '');
#include ComponentInnoDir + "\component-select-new.iss"
  NormalizeComponentDependencies('');
  ComponentSelectionSnapshot := WizardSelectedComponents(False);
  WizardForm.ComponentsList.OnClickCheck := @ComponentsListClickCheck;
  if ComponentsListClickCheckPrevious <> nil then
    ComponentsListClickCheckPrevious(WizardForm.ComponentsList);
end;

procedure RegisterPreviousData(PreviousDataKey: Integer);
begin
  SetPreviousData(
    PreviousDataKey, 'KnownComponents', '{#CurrentComponentCatalog}');
  SetPreviousData(
    PreviousDataKey, 'KnownSources', '{#CurrentSourceCatalog}');
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
  NormalizeComponentDependencies('');
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
