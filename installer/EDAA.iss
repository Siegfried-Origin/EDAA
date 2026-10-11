; EDAA installer (Inno Setup 6).
;
; - Finds Elite Dangerous (Steam libraries, Epic manifests, Frontier
;   launcher); the user picks one or browses.
; - Checks ReShade in the game folder (dxgi.dll): present, version, and
;   whether it is the build with full add-on support. If needed, offers to
;   download the official ReShade setup from reshade.me and run it silently
;   for the game (API DXGI), or to open reshade.me.
; - Copies EDAA.addon64 next to dxgi.dll. The uninstaller and the README go
;   to Program Files\EDAA (the game folder already has Frontier's unins000.exe).
;
; Build: ISCC /DAppVersion=1.0.0 installer\EDAA.iss  (expects build\Release\EDAA.addon64)

#ifndef AppVersion
  #define AppVersion "dev"
#endif
#ifndef AddonFile
  #define AddonFile "..\build\Release\EDAA.addon64"
#endif
#define AppName "EDAA"
#define AppPublisher "Siegfried Origin"
#define AppURL "https://github.com/Siegfried-Origin/EDAA"
; Official ReShade build with full add-on support, tested with EDAA.
#define ReShadeVersion "6.8.0"
#define ReShadeSetup "ReShade_Setup_" + ReShadeVersion + "_Addon.exe"
#define ReShadeUrl "https://reshade.me/downloads/" + ReShadeSetup
; SHA-256 of that file (lower case hex), checked after the download.
; Update it with ReShadeVersion: (Get-FileHash <file> -Algorithm SHA256).Hash.ToLower()
#ifndef ReShadeSha256
  #define ReShadeSha256 "afe4c8f13048306307983b8b3d41d5bf00a86820440b0e57dea10950e1176445"
#endif

[Setup]
AppId={{6C1B8A52-3E1D-4F7B-9B0E-2A9F5D8E7C41}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}/issues
DefaultDirName={code:GetGameDir}
DisableDirPage=yes
DisableProgramGroupPage=yes
DirExistsWarning=no
UsePreviousAppDir=no
PrivilegesRequired=admin
UninstallFilesDir={autopf}\EDAA
UninstallDisplayName={#AppName} {#AppVersion} - TAA for Elite Dangerous
OutputDir=Output
OutputBaseFilename=EDAA-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern

[Files]
Source: "{#AddonFile}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{autopf}\EDAA"; Flags: ignoreversion

[Code]
const
  GameExe = 'EliteDangerous64.exe';
  ProductSubdir = '\Products\elite-dangerous-odyssey-64';
  SteamAppId = '359320';
  UninstallKey = 'SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall';

var
  GamePage: TWizardPage;
  GameLabel: TNewStaticText;
  GameCombo: TNewComboBox;
  ReShadePage: TInputOptionWizardPage;
  DownloadPage: TDownloadWizardPage;
  Candidates: TArrayOfString;
  GamePath: String;
  ReShadeState: String; // 'ok', 'missing', 'limited', 'old', 'other'
  ReShadeDownloaded: Boolean;

{ ---------- Game detection ---------- }

function IsGameFolder(Path: String): Boolean;
begin
  Result := (Path <> '') and FileExists(AddBackslash(Path) + GameExe);
end;

procedure AddCandidate(Path: String);
var
  I: Integer;
begin
  Path := RemoveBackslashUnlessRoot(Trim(Path));
  if not IsGameFolder(Path) then
    Exit;
  for I := 0 to GetArrayLength(Candidates) - 1 do
    if CompareText(Candidates[I], Path) = 0 then
      Exit;
  SetArrayLength(Candidates, GetArrayLength(Candidates) + 1);
  Candidates[GetArrayLength(Candidates) - 1] := Path;
end;

{ Value of "Key" "value" (VDF) or "Key": "value" (JSON) on one line, or '' }
function QuotedValue(Line, Key: String): String;
var
  P: Integer;
begin
  Result := '';
  P := Pos('"' + Key + '"', Line);
  if P = 0 then
    Exit;
  Line := Copy(Line, P + Length(Key) + 2, MaxInt);
  P := Pos('"', Line);
  if P = 0 then
    Exit;
  Line := Copy(Line, P + 1, MaxInt);
  P := Pos('"', Line);
  if P = 0 then
    Exit;
  Result := Copy(Line, 1, P - 1);
  StringChangeEx(Result, '\\', '\', True);
  StringChangeEx(Result, '/', '\', True);
end;

procedure FindSteam();
var
  Steam, Lib, InstallDir: String;
  Lines, Manifest: TArrayOfString;
  I, J: Integer;
begin
  if not RegQueryStringValue(HKCU, 'Software\Valve\Steam', 'SteamPath', Steam) then
    if not RegQueryStringValue(HKLM, 'SOFTWARE\WOW6432Node\Valve\Steam', 'InstallPath', Steam) then
      Exit;
  StringChangeEx(Steam, '/', '\', True);
  { libraryfolders.vdf lists every Steam library; add the main one in case it is missing }
  if not LoadStringsFromFile(Steam + '\steamapps\libraryfolders.vdf', Lines) then
    SetArrayLength(Lines, 0);
  SetArrayLength(Lines, GetArrayLength(Lines) + 1);
  Lines[GetArrayLength(Lines) - 1] := '"path" "' + Steam + '"';
  for I := 0 to GetArrayLength(Lines) - 1 do
  begin
    Lib := QuotedValue(Lines[I], 'path');
    if Lib = '' then
      Continue;
    InstallDir := 'Elite Dangerous';
    if LoadStringsFromFile(Lib + '\steamapps\appmanifest_' + SteamAppId + '.acf', Manifest) then
      for J := 0 to GetArrayLength(Manifest) - 1 do
        if QuotedValue(Manifest[J], 'installdir') <> '' then
          InstallDir := QuotedValue(Manifest[J], 'installdir');
    AddCandidate(Lib + '\steamapps\common\' + InstallDir + ProductSubdir);
  end;
end;

procedure FindEpic();
var
  Dir, Location: String;
  Rec: TFindRec;
  Lines: TArrayOfString;
  I: Integer;
  IsElite: Boolean;
begin
  Dir := ExpandConstant('{commonappdata}\Epic\EpicGamesLauncher\Data\Manifests');
  if not FindFirst(Dir + '\*.item', Rec) then
    Exit;
  try
    repeat
      if LoadStringsFromFile(Dir + '\' + Rec.Name, Lines) then
      begin
        IsElite := False;
        Location := '';
        for I := 0 to GetArrayLength(Lines) - 1 do
        begin
          if Pos('Elite Dangerous', Lines[I]) > 0 then
            IsElite := True;
          if QuotedValue(Lines[I], 'InstallLocation') <> '' then
            Location := QuotedValue(Lines[I], 'InstallLocation');
        end;
        if IsElite and (Location <> '') then
        begin
          AddCandidate(Location + ProductSubdir);
          AddCandidate(Location);
        end;
      end;
    until not FindNext(Rec);
  finally
    FindClose(Rec);
  end;
end;

procedure FindFrontier();
var
  Keys: TArrayOfString;
  I: Integer;
  Name, Location: String;
begin
  AddCandidate(ExpandConstant('{commonpf32}\Frontier\EDLaunch') + ProductSubdir);
  AddCandidate(ExpandConstant('{commonpf32}\Frontier') + ProductSubdir);
  AddCandidate(ExpandConstant('{commonpf64}\Frontier\EDLaunch') + ProductSubdir);
  if RegGetSubkeyNames(HKLM, UninstallKey, Keys) then
    for I := 0 to GetArrayLength(Keys) - 1 do
      if RegQueryStringValue(HKLM, UninstallKey + '\' + Keys[I], 'DisplayName', Name) and
        (Pos('Elite Dangerous', Name) > 0) and
        RegQueryStringValue(HKLM, UninstallKey + '\' + Keys[I], 'InstallLocation', Location) then
      begin
        Location := RemoveBackslashUnlessRoot(Location);
        AddCandidate(Location + ProductSubdir);
        AddCandidate(Location + '\EDLaunch' + ProductSubdir);
      end;
end;

function GetGameDir(Param: String): String;
begin
  Result := GamePath;
  if Result = '' then
    Result := ExpandConstant('{autopf}\EDAA');
end;

{ ---------- ReShade detection ---------- }

function ReShadeDll(): String;
begin
  Result := AddBackslash(GamePath) + 'dxgi.dll';
end;

{ The ReShade build with limited add-on support contains this log message;
  the build with full add-on support does not. }
function DetectReShade(): String;
var
  Data: AnsiString;
  Version: String;
  Major, Minor, Rev, Build: Word;
begin
  if not FileExists(ReShadeDll()) then
    Result := 'missing'
  else if not LoadStringFromFile(ReShadeDll(), Data) or (Pos('ReShade', Data) = 0) then
    Result := 'other'
  else if Pos('limited add-on functionality', Data) > 0 then
    Result := 'limited'
  else
  begin
    Result := 'ok';
    if GetVersionComponents(ReShadeDll(), Major, Minor, Rev, Build) and
      ((Major < 6) or ((Major = 6) and (Minor < 8))) then
      Result := 'old';
  end;
  if not GetVersionNumbersString(ReShadeDll(), Version) then
    Version := '-';
  Log('dxgi.dll ' + Version + ': ' + Result);
end;

function ReShadeStatusText(): String;
begin
  if ReShadeState = 'ok' then
    Result := 'ReShade with full add-on support is installed for this game.'
  else if ReShadeState = 'limited' then
    Result := 'ReShade is installed, but the normal build, which does not load add-ons. EDAA needs the build "with full add-on support".'
  else if ReShadeState = 'old' then
    Result := 'ReShade with add-on support is installed, but older than {#ReShadeVersion}. EDAA needs {#ReShadeVersion} or newer.'
  else if ReShadeState = 'other' then
    Result := 'The game folder has a dxgi.dll that is not ReShade (another mod?). Remove it or install ReShade yourself; the automatic install is disabled to avoid replacing it.'
  else
    Result := 'ReShade is not installed for this game. EDAA is a ReShade add-on and needs ReShade with full add-on support.';
end;

{ ---------- Wizard ---------- }

procedure BrowseClick(Sender: TObject);
var
  Dir: String;
begin
  Dir := GameCombo.Text;
  if BrowseForFolder('Select the folder that contains ' + GameExe + ' (...\Products\elite-dangerous-odyssey-64):', Dir, False) then
    GameCombo.Text := Dir;
end;

function OnDownloadProgress(const Url, FileName: String; const Progress, ProgressMax: Int64): Boolean;
begin
  Result := True;
end;

procedure InitializeWizard();
var
  BrowseButton: TNewButton;
  I: Integer;
begin
  FindSteam();
  FindEpic();
  FindFrontier();

  GamePage := CreateCustomPage(wpWelcome, 'Elite Dangerous', 'Where is the game installed?');
  GameLabel := TNewStaticText.Create(GamePage);
  GameLabel.Parent := GamePage.Surface;
  GameLabel.WordWrap := True;
  GameLabel.Width := GamePage.SurfaceWidth;
  GameCombo := TNewComboBox.Create(GamePage);
  GameCombo.Parent := GamePage.Surface;
  GameCombo.Top := ScaleY(48);
  GameCombo.Width := GamePage.SurfaceWidth - ScaleX(90);
  GameCombo.Style := csDropDown;
  BrowseButton := TNewButton.Create(GamePage);
  BrowseButton.Parent := GamePage.Surface;
  BrowseButton.Top := GameCombo.Top - ScaleY(1);
  BrowseButton.Left := GamePage.SurfaceWidth - ScaleX(80);
  BrowseButton.Width := ScaleX(80);
  BrowseButton.Height := ScaleY(23);
  BrowseButton.Caption := 'Browse...';
  BrowseButton.OnClick := @BrowseClick;
  for I := 0 to GetArrayLength(Candidates) - 1 do
    GameCombo.Items.Add(Candidates[I]);
  if GetArrayLength(Candidates) > 0 then
  begin
    GameCombo.ItemIndex := 0;
    GameLabel.Caption := 'Select the game folder (it contains ' + GameExe + '), or browse to it.';
  end
  else
    GameLabel.Caption := 'Elite Dangerous was not found automatically. Browse to the folder that contains ' + GameExe +
      ' (Steam: ...\steamapps\common\Elite Dangerous\Products\elite-dangerous-odyssey-64).';

  ReShadePage := CreateInputOptionPage(GamePage.ID, 'ReShade', 'EDAA runs inside ReShade (build with full add-on support).', '', True, False);
  ReShadePage.Add('Download ReShade {#ReShadeVersion} with full add-on support from reshade.me and install it for Elite Dangerous (recommended)');
  ReShadePage.Add('Open reshade.me, I will install ReShade myself ("with full add-on support", then the game and "DirectX 10/11/12")');
  ReShadePage.Add('Continue without ReShade (EDAA will not run until ReShade with add-on support is installed)');

  DownloadPage := CreateDownloadPage(SetupMessage(msgWizardPreparing), 'Downloading ReShade from reshade.me...', @OnDownloadProgress);
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PageID = ReShadePage.ID) and (ReShadeState = 'ok');
end;

function DownloadReShade(): Boolean;
begin
  DownloadPage.Clear;
  DownloadPage.Add('{#ReShadeUrl}', '{#ReShadeSetup}', '{#ReShadeSha256}');
  DownloadPage.Show;
  try
    try
      DownloadPage.Download;
      Result := True;
    except
      Result := False;
      if not DownloadPage.AbortedByUser then
        MsgBox('Downloading ReShade failed:'#13#10 + GetExceptionMessage, mbError, MB_OK);
    end;
  finally
    DownloadPage.Hide;
  end;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = GamePage.ID then
  begin
    GamePath := RemoveBackslashUnlessRoot(Trim(GameCombo.Text));
    if not IsGameFolder(GamePath) then
    begin
      MsgBox('This folder does not contain ' + GameExe + ':'#13#10 + GamePath, mbError, MB_OK);
      Result := False;
      Exit;
    end;
    WizardForm.DirEdit.Text := GamePath;
    ReShadeState := DetectReShade();
    ReShadePage.Description := ReShadeStatusText() + #13#10#13#10 +
      'Note from ReShade: the build with add-on support is meant for single-player games and may be flagged by anti-cheat in multiplayer games. ' +
      'Use it at your own discretion.';
    ReShadePage.CheckListBox.ItemEnabled[0] := ReShadeState <> 'other';
    if ReShadeState = 'other' then
      ReShadePage.Values[1] := True
    else
      ReShadePage.Values[0] := True;
  end
  else if (CurPageID = wpReady) and (ReShadeState <> 'ok') and ReShadePage.Values[0] and not ReShadeDownloaded then
  begin
    { Download before installing, so a failure can still be cancelled }
    ReShadeDownloaded := DownloadReShade();
    if not ReShadeDownloaded then
      Result := MsgBox('ReShade was not downloaded. Install EDAA anyway? You can install ReShade later from https://reshade.me.',
        mbConfirmation, MB_YESNO) = IDYES;
  end;
end;

procedure InstallReShade();
var
  Code: Integer;
begin
  { Silent install for the game, API DXGI (works next to EDHM's d3d11.dll) }
  WizardForm.StatusLabel.Caption := 'Installing ReShade {#ReShadeVersion} for Elite Dangerous...';
  if not Exec(ExpandConstant('{tmp}\{#ReShadeSetup}'), '"' + AddBackslash(GamePath) + GameExe + '" --api dxgi --headless',
    '', SW_SHOW, ewWaitUntilTerminated, Code) then
    Code := -1;
  ReShadeState := DetectReShade();
  if ReShadeState <> 'ok' then
    MsgBox('The ReShade setup did not finish correctly (code ' + IntToStr(Code) + '). ' +
      'Install it yourself from https://reshade.me ("with full add-on support").', mbError, MB_OK);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
begin
  if (CurStep <> ssPostInstall) or (ReShadeState = 'ok') then
    Exit;
  if ReShadePage.Values[0] and ReShadeDownloaded then
    InstallReShade()
  else if ReShadePage.Values[1] then
    ShellExec('open', 'https://reshade.me', '', '', SW_SHOWNORMAL, ewNoWait, Code);
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := 'Game folder:' + NewLine + Space + GamePath + NewLine + NewLine +
    'Files:' + NewLine + Space + 'EDAA.addon64 (next to dxgi.dll)' + NewLine + NewLine +
    'ReShade:' + NewLine + Space + ReShadeStatusText();
  if ReShadeState <> 'ok' then
  begin
    if ReShadePage.Values[0] then
      Result := Result + NewLine + Space + '-> download and install ReShade {#ReShadeVersion} with full add-on support'
    else if ReShadePage.Values[1] then
      Result := Result + NewLine + Space + '-> open reshade.me after the install'
    else
      Result := Result + NewLine + Space + '-> not installed now';
  end;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID <> wpFinished then
    Exit;
  if ReShadeState = 'ok' then
    WizardForm.FinishedLabel.Caption := 'EDAA is installed. In the game''s graphics options, set anti-aliasing to off. ' +
      'TAA is on by default; Ctrl+Shift+T toggles it.' + #13#10#13#10 +
      'Press Home in game for the ReShade overlay: the "EDAA" tab shows the status and the settings.'
  else
    WizardForm.FinishedLabel.Caption := 'EDAA is installed, but ReShade with full add-on support is not installed for the game yet. ' +
      'Get it from https://reshade.me: choose the build "with full add-on support", select ' + GameExe +
      ' and "DirectX 10/11/12".';
end;
