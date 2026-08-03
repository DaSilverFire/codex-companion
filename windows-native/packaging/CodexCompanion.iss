[Setup]
AppId={{9B3C42CB-4B7F-4A08-B675-071708948C88}
AppName=Codex Companion
AppPublisher=DaSilverFire
AppVersion={#Version}
DefaultDirName={localappdata}\Programs\Codex Companion
DefaultGroupName=Codex Companion
DisableProgramGroupPage=yes
AllowNoIcons=no
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.22000
Compression=lzma2
SolidCompression=yes
CloseApplications=force
RestartApplications=no
RestartIfNeededByRun=no
OutputDir={#OutputDir}
OutputBaseFilename=Codex-Companion-{#Version}-{#Build}-windows-x64
SetupIconFile={#IconPath}
VersionInfoVersion={#VersionMajor}.{#VersionMinor}.{#VersionPatch}.{#Build}
VersionInfoProductName=Codex Companion
VersionInfoOriginalFileName=Codex-Companion-{#Version}-{#Build}-windows-x64.exe
VersionInfoProductTextVersion=cc-update/1|{#Version}|{#Build}|w|x64|10.0.22000
CreateUninstallRegKey=yes
Uninstallable=yes
UninstallDisplayName=Codex Companion
UninstallDisplayIcon={app}\bin\CodexCompanion.exe
#ifdef EnableSigning
SignedUninstaller=yes
SignTool=companion
#endif

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}\resources\skills\companion-pet\*"; DestDir: "{userprofile}\.codex\skills\companion-pet"; Flags: ignoreversion recursesubdirs createallsubdirs uninsneveruninstall

[Icons]
Name: "{group}\Codex Companion"; Filename: "{app}\bin\CodexCompanion.exe"; WorkingDir: "{app}\bin"; IconFilename: "{app}\bin\CodexCompanion.exe"

[Run]
Filename: "{app}\bin\CodexCompanion.exe"; WorkingDir: "{app}\bin"; Description: "Launch Codex Companion"; Flags: postinstall nowait skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}"
