; SynthMatch installer for Windows (Inno Setup 6.3 or newer).
;
; Build the plugin first (Release), then either run scripts\package-windows.ps1
; or:  ISCC.exe /DAppVersion=0.2.0 packaging\windows\SynthMatch.iss
; The installer is written to dist\.
;
; It installs the VST3 plugin and the standalone app. The optional "AI
; assistant" component downloads Ollama from ollama.com (if it isn't already
; installed), installs it for the current user, and starts downloading the
; assistant's language model in the background. Nothing is bundled: Ollama and
; the model come from their official sources at install time.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#define Root "..\.."
#define Built Root + "\build\plugin\SynthMatch_artefacts\Release"
#define Model "qwen3:1.7b"

[Setup]
AppId={{E2A180CA-70CB-4755-BEE3-1439884598A5}
AppName=SynthMatch
AppVersion={#AppVersion}
AppVerName=SynthMatch {#AppVersion}
AppPublisher=Bedrot Audio
DefaultDirName={autopf}\Bedrot Audio\SynthMatch
DisableDirPage=yes
DisableProgramGroupPage=yes
OutputDir={#Root}\dist
OutputBaseFilename=SynthMatch-{#AppVersion}-Windows
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; The VST3 folder under Common Files needs administrator rights.
PrivilegesRequired=admin
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
UninstallDisplayName=SynthMatch
UninstallDisplayIcon={app}\SynthMatch.exe
MinVersion=10.0

[Types]
Name: "full"; Description: "Plugin, app and AI assistant"
Name: "compact"; Description: "Plugin and app only"
Name: "custom"; Description: "Choose"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3 plugin"; Types: full compact custom; Flags: fixed
Name: "standalone"; Description: "Standalone app"; Types: full compact
Name: "assistant"; Description: "AI assistant: Ollama and the {#Model} language model (downloads about 2.5 GB)"; Types: full

[Files]
Source: "{#Built}\VST3\SynthMatch.vst3\*"; DestDir: "{commoncf64}\VST3\SynthMatch.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Built}\Standalone\SynthMatch.exe"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "fetch-model.ps1"; DestDir: "{app}"; Components: assistant; Flags: ignoreversion
; SynthMatch is free software (AGPLv3); these always ship with it.
Source: "{#Root}\LICENSE.md"; DestDir: "{app}"; DestName: "License.txt"; Components: vst3; Flags: ignoreversion
Source: "{#Root}\THIRD_PARTY_NOTICES.txt"; DestDir: "{app}"; Components: vst3; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\SynthMatch"; Filename: "{app}\SynthMatch.exe"; Components: standalone

[Run]
Filename: "{app}\SynthMatch.exe"; Description: "Open SynthMatch"; Flags: nowait postinstall skipifsilent runasoriginaluser; Components: standalone

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\SynthMatch.vst3"

[Code]
var
  DownloadPage: TDownloadWizardPage;

// Ollama installs per user, under Local AppData.
function OllamaExe: String;
begin
  Result := ExpandConstant('{localappdata}\Programs\Ollama\ollama.exe');
end;

function OllamaInstalled: Boolean;
begin
  Result := FileExists(OllamaExe);
end;

procedure InitializeWizard;
begin
  DownloadPage := CreateDownloadPage('Downloading Ollama',
    'Ollama runs the assistant''s language model on this computer.', nil);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpReady) and WizardIsComponentSelected('assistant') and not OllamaInstalled then
  begin
    DownloadPage.Clear;
    DownloadPage.Add('https://ollama.com/download/OllamaSetup.exe', 'OllamaSetup.exe', '');
    DownloadPage.Show;
    try
      try
        DownloadPage.Download;
      except
        // The plugin works without the assistant's model, so carry on.
        if not DownloadPage.AbortedByUser then
          SuppressibleMsgBox('Ollama could not be downloaded: ' + AddPeriod(GetExceptionMessage) + #13#10#13#10 +
            'SynthMatch will still be installed. You can install Ollama later from ollama.com.',
            mbInformation, MB_OK, IDOK);
      end;
    finally
      DownloadPage.Hide;
    end;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
  OllamaSetup: String;
begin
  if (CurStep <> ssPostInstall) or not WizardIsComponentSelected('assistant') then
    Exit;

  // Install Ollama for the person running the installer, not the administrator account.
  OllamaSetup := ExpandConstant('{tmp}\OllamaSetup.exe');
  if (not OllamaInstalled) and FileExists(OllamaSetup) then
  begin
    WizardForm.StatusLabel.Caption := 'Installing Ollama...';
    if not ExecAsOriginalUser(OllamaSetup, '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-', '', SW_SHOW,
                              ewWaitUntilTerminated, ResultCode) then
      Log('Ollama installer could not be started');
  end;

  // Start the model download in the background; the plugin shows its progress
  // with /download if it isn't finished when you first open it.
  if OllamaInstalled then
  begin
    WizardForm.StatusLabel.Caption := 'Starting the language model download...';
    ExecAsOriginalUser('powershell.exe',
      '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + ExpandConstant('{app}\fetch-model.ps1') + '" -Model "{#Model}"',
      '', SW_HIDE, ewNoWait, ResultCode);
  end;
end;
