; Inno Setup script for sighmake (Windows x64).
; Invoke from the repo root, e.g.:
;   iscc /DAppVersion=0.1.0 installer\windows\sighmake.iss
; The compiled installer is written to <repo>\artifacts\sighmake-windows-x64-setup.exe.

#define AppName "sighmake"
#define AppPublisher "CitroenGames"
#define AppURL "https://github.com/CitroenGames/sighmake"
#define AppExeName "sighmake.exe"

#ifndef AppVersion
  #define AppVersion "0.0.0-dev"
#endif

[Setup]
AppId={{8B0E5B8C-7A6C-4B5D-9E8F-7D0E5C3F2A1D}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
AppUpdatesURL={#AppURL}/releases
DefaultDirName={localappdata}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
OutputBaseFilename=sighmake-windows-x64-setup
OutputDir=..\..\artifacts
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ChangesEnvironment=yes
UninstallDisplayName={#AppName} {#AppVersion}
UninstallDisplayIcon={app}\{#AppExeName}

[Files]
Source: "..\..\dist\{#AppExeName}"; DestDir: "{app}"; Flags: ignoreversion

[Tasks]
Name: "addtopath"; Description: "Add {#AppName} to user PATH"; GroupDescription: "Additional options:"

[Code]
const
  EnvKey = 'Environment';

function NeedsAddPath(NewPath: string): Boolean;
var
  OrigPath: string;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, EnvKey, 'Path', OrigPath) then
  begin
    Result := True;
    exit;
  end;
  Result := Pos(';' + Uppercase(NewPath) + ';', ';' + Uppercase(OrigPath) + ';') = 0;
end;

procedure EnvAddPath(NewPath: string);
var
  Paths: string;
begin
  if not NeedsAddPath(NewPath) then
    exit;
  if not RegQueryStringValue(HKEY_CURRENT_USER, EnvKey, 'Path', Paths) then
    Paths := '';
  if (Paths <> '') and (Paths[Length(Paths)] <> ';') then
    Paths := Paths + ';';
  Paths := Paths + NewPath;
  RegWriteStringValue(HKEY_CURRENT_USER, EnvKey, 'Path', Paths);
end;

procedure EnvRemovePath(OldPath: string);
var
  Paths: string;
  P: Integer;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, EnvKey, 'Path', Paths) then
    exit;
  P := Pos(';' + Uppercase(OldPath) + ';', ';' + Uppercase(Paths) + ';');
  if P = 0 then
    exit;
  Delete(Paths, P, Length(OldPath) + 1);
  RegWriteStringValue(HKEY_CURRENT_USER, EnvKey, 'Path', Paths);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('addtopath') then
    EnvAddPath(ExpandConstant('{app}'));
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    EnvRemovePath(ExpandConstant('{app}'));
end;
