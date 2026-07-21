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
AppPublisherURL=https://github.com/ElluIFX/glance
AppSupportURL=https://github.com/ElluIFX/glance/issues
AppUpdatesURL=https://github.com/ElluIFX/glance/releases
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

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#PayloadDir}\*"; DestDir: "{app}"; Excludes: "*.exp,*.ilk,*.lib,*.pdb,Glance.Tests.exe"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#RepoRoot}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Glance"; Filename: "{app}\Glance.exe"
Name: "{autodesktop}\Glance"; Filename: "{app}\Glance.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Glance.exe"; Description: "{cm:LaunchProgram,Glance}"; Flags: nowait postinstall skipifsilent
