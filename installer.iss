; ZettaHUD v3.3 - Inno Setup Installer Script

#define MyAppName "ZettaHUD"
#define MyAppVersion "3.3"
#define MyAppPublisher "Raf446"
#define MyAppURL "https://github.com/Raf446/ZettaHUD"
#define MyAppExeName "zetta_studio.exe"
#define MyOverlayExeName "zetta_hud_live.exe"

[Setup]
AppId={{8F6D3E1A-4C92-4B7E-9A15-7D2C8E4F3019}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} v{#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile=LICENSE
OutputDir=dist
OutputBaseFilename=ZettaHUD_v3.3_Setup_x64
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
UninstallDisplayIcon={app}\{#MyAppExeName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "autostart"; Description: "Run ZettaHUD automatically when Windows starts"; GroupDescription: "Startup Options:"; Flags: unchecked

[Dirs]
Name: "{app}\resources\presets"; Permissions: users-modify

[Files]
Source: "build\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\{#MyOverlayExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\{#MyOverlayExeName}"; DestDir: "{app}\build"; Flags: ignoreversion
Source: "resources\*"; DestDir: "{app}\resources"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName} Studio"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Comment: "Launch ZettaHUD v3.3 Studio & Telemetry Control Deck"
Name: "{group}\{#MyAppName} Live Overlay"; Filename: "{app}\{#MyOverlayExeName}"; WorkingDir: "{app}"; Comment: "Launch ZettaHUD Direct2D Click-Through Overlay"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "ZettaHUD"; ValueData: """{app}\{#MyAppExeName}"" --background"; Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
