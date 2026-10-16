; Atlas Windows installer (Inno Setup 6.3 or newer). It installs the whole
; Atlas.vst3 folder of a finished Release build into the system VST3 folder,
; C:\Program Files\Common Files\VST3 by default; the user can pick another
; folder, since some hosts scan one of their own. .github/workflows/release.yml
; builds it on every push to dev; by hand, from the repository root:
;
;   ISCC.exe /DAppVersion=2.0.0 /DOutputName=Atlas-2.0.0-beta.1-Windows installer\windows\Atlas.iss
;
; AppVersion is the version in CMakeLists.txt (project(Atlas VERSION ...)).
; SourceDir (the folder that holds Atlas.vst3), OutputDir and OutputName have
; defaults for a build in build/. The uninstaller removes the bundle again.

#ifndef AppVersion
  #error Pass /DAppVersion=<version>, the version in CMakeLists.txt
#endif
#ifndef SourceDir
  #define SourceDir "..\..\build\Atlas_artefacts\Release\VST3"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\dist"
#endif
#ifndef OutputName
  #define OutputName "Atlas-" + AppVersion + "-Windows"
#endif

[Setup]
; Fixed for the life of the product so that a newer installer upgrades the
; older one in place instead of adding a second entry in Apps & features.
AppId={{E0C2F1A4-6B3D-4F57-9A1E-2C8B5D7F3A10}
AppName=Atlas
AppVersion={#AppVersion}
AppVerName=Atlas {#AppVersion}
AppPublisher=Blind Producers
AppPublisherURL=https://github.com/blindproducers/atlas
AppSupportURL=https://github.com/blindproducers/atlas/issues
AppUpdatesURL=https://github.com/blindproducers/atlas/releases
; The plug-in is 64-bit only.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
; Common Files needs administrator rights.
PrivilegesRequired=admin
; The chosen folder is the VST3 folder itself; Atlas.vst3 goes inside it.
DefaultDirName={commoncf64}\VST3
AppendDefaultDirName=no
DirExistsWarning=no
DisableDirPage=no
UsePreviousAppDir=yes
DisableProgramGroupPage=yes
; The uninstaller must not sit among the plug-ins.
UninstallFilesDir={commonpf}\Blind Producers\Atlas
UninstallDisplayName=Atlas {#AppVersion}
LicenseFile=..\..\LICENSE
OutputDir={#OutputDir}
OutputBaseFilename={#OutputName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; Standard Windows controls throughout, so screen readers read every page.
ShowLanguageDialog=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[InstallDelete]
; A previous bundle may hold files this version no longer ships.
Type: filesandordirs; Name: "{app}\Atlas.vst3"

[Files]
Source: "{#SourceDir}\Atlas.vst3\*"; DestDir: "{app}\Atlas.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs

[UninstallDelete]
Type: filesandordirs; Name: "{app}\Atlas.vst3"

[Messages]
; The folder page asks for the VST3 folder, not an application folder.
SelectDirDesc=Where should Atlas.vst3 be installed?
SelectDirLabel3=Setup will put the Atlas.vst3 folder into the VST3 folder below. Keep the default unless your host scans a different folder.
