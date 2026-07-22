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
AppMutex=Local\Glance.App
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
chinesesimplified.AdditionalTasks=其他选项：
chinesesimplified.CreateStartMenuShortcut=创建开始菜单快捷方式
chinesesimplified.CreateDesktopShortcut=创建桌面快捷方式
chinesesimplified.StartAtSignIn=登录 Windows 时启动 Glance

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
