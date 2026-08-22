; spfy_setup.iss — Inno Setup script for Speechify (spfy)
;
; Builds a Windows installer that:
;   1. Drops binaries (spfy_sapi.dll, spfy_sapi64.dll, spfy_synth.exe) into
;      Program Files\Speechify
;   2. Drops shared FE data (hpclass + fe_tables + symbol table) into
;      %USERPROFILE%\Documents\Speechify\spfy\ — that's the layout
;      spfy_sapi.dll's path resolution expects (see get_project_root in
;      spfy/src/sapi/spfy_sapi.c).
;   3. Registers the 32-bit SAPI DLL via regsvr32. The DLL's own
;      DllRegisterServer writes both the 32- and 64-bit CLSID hives plus
;      voice tokens for every voice it auto-scans under
;      %USERPROFILE%\Documents\Speechify\en-US\*.
;   4. On uninstall: deregisters cleanly (which removes voice tokens
;      from both registry views) and deletes the bundled binaries.
;
; Voices (VIN/VDB/VCF) are NOT bundled — they're proprietary SpeechWorks
; assets. The user drops them into
; %USERPROFILE%\Documents\Speechify\en-US\<voicename>\ themselves;
; auto-scan picks them up at registration time (re-run regsvr32 after
; adding a voice to refresh the token list).
;
; Tom's PITCH MARKS (tom8.pmindex / tom8.pmdata) ARE bundled — see the
; [Files] note. They are measured metadata, not voice data, and Speechify 4
; mode does not start without them.
;
; Build:  iscc spfy_setup.iss
; Override paths: iscc /DBuildDir=C:\tmp\spfy_build32 /DSourceRoot=..  spfy_setup.iss

; ---------------------------------------------------------------------
; Preprocessor — paths and metadata
; ---------------------------------------------------------------------

#ifndef BuildDir
#define BuildDir "C:\tmp\spfy_build32"
#endif

#ifndef SourceRoot
#define SourceRoot ".."
#endif

; Date-based (calver) versioning: SpfyVersion is the user-facing
; YYYY.MM.DD string used in filenames and AppVersion. SpfyVersionInfo
; is the strict X.X.X.X numeric form required by VersionInfoVersion
; (PE VersionInfo resource); CI passes YYYY.MM.DD.<run_number>.
#ifndef SpfyVersion
#define SpfyVersion "1.0.0"
#endif

#ifndef SpfyVersionInfo
#define SpfyVersionInfo "1.0.0.0"
#endif

; Which Windows this installer targets: "x64" (default) or "x86".
;
; They are SEPARATE INSTALLERS, not one universal build, because the payloads
; genuinely differ — the x86 one has no spfy_sapi64.dll to ship, and its
; regsvr32 lives somewhere else. A single installer allowing both would carry
; a 64-bit DLL it can never install and a Check: on every line that touches it.
;
; Everything that varies between the two is gathered here and at the three
; #if TargetArch sites below ([Setup] architectures + filename, [Files] the
; 64-bit shim, [Run] the regsvr32 path). Nothing else in this script cares.
;
;   iscc /DTargetArch=x86 spfy_setup.iss   ->  spfy-setup-<ver>-x86.exe
;   iscc                  spfy_setup.iss   ->  spfy-setup-<ver>.exe
#ifndef TargetArch
#define TargetArch "x64"
#endif
#if (TargetArch != "x64") && (TargetArch != "x86")
#error TargetArch must be "x64" or "x86"
#endif

; Where the 32-BIT regsvr32 lives on the target. On 64-bit Windows that is
; SysWOW64 (the naming is historical); on 32-bit Windows there is no SysWOW64
; and System32 — {sys} — holds the 32-bit one.
;
; ⚠ THIS IS A #define, NOT AN #if AROUND THE [Run] LINE, and it has to be.
; That entry ends each line with Inno's `\` continuation, which swallows a
; following `#else` as part of the same logical line — the `#if` then never
; closes and the compiler fails at EOF with "'endif' expected", pointing at
; the last line of the file rather than at the actual mistake.
#if TargetArch == "x86"
#define RegSvrDir "{sys}"
#else
#define RegSvrDir "{syswow64}"
#endif

#define MyAppName       "Speechify (spfy)"
#define MyAppShortName  "spfy"
#define MyAppPublisher  "Speechify Open-Source Reimplementation"
#define MyAppURL        "https://github.com/wagwan-piffting-blud/Speechify"
#define MyAppExeName    "spfy_synth.exe"

; ---------------------------------------------------------------------
; [Setup]
; ---------------------------------------------------------------------

[Setup]
; Unique installer identity — NOT the COM CLSID. The COM CLSID is
; {9C3A7D1E-4F5A-4B6C-8EA2-5C71D08F1234}, baked into spfy_sapi.dll.
AppId={{B7EC3D11-1A22-4F2C-9F18-3C7E5E5E3D71}
AppName={#MyAppName}
AppVersion={#SpfyVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases
VersionInfoVersion={#SpfyVersionInfo}
VersionInfoProductName={#MyAppName}
VersionInfoCompany={#MyAppPublisher}
MinVersion=6.1

; Installs to "C:\Program Files\Speechify" on 64-bit Windows. The 32-bit
; SAPI DLL still goes here (not Program Files (x86)) because both DLLs
; need to be side-by-side for the 32-bit DLL's DllRegisterServer to
; find spfy_sapi64.dll for the 64-bit-view InprocServer32 entry.
DefaultDirName={autopf}\Speechify
DefaultGroupName=Speechify
DisableProgramGroupPage=yes
DisableReadyPage=no
DisableDirPage=no

; SAPI registration touches HKLM\SOFTWARE\Microsoft\Speech\Voices\Tokens
; and HKLM\SOFTWARE\Classes\CLSID — admin only.
PrivilegesRequired=admin

; We intentionally write per-user data ({userdocs}\Speechify\) while
; running elevated. This is supported and works as expected: under
; per-user UAC elevation, SHGetFolderPath(CSIDL_PERSONAL) still
; resolves to the invoking user's Documents (not the admin profile).
; spfy_sapi.dll's get_project_root() uses the same call at runtime,
; so the user-time and install-time paths match. Silence Inno's
; preflight warning about this mix.
;
; ⚠ {userdocs}\Speechify MAY BE A SOURCE CHECKOUT. It is on the maintainer's
; machine, where the repository root and the installer's per-user data
; directory are literally the same path. An uninstall has already destroyed
; tracked files there once, because Inno removes everything it installed and
; an earlier revision of this script installed fe_symbol_table.json and 728
; fe_tables\*.bin into that tree.
;
; So the standing rule for this script: {app} is ours to manage, {userdocs}
; is not. Every [Files] entry targeting {userdocs} carries
; `onlyifdoesntexist uninsneveruninstall`, and [UninstallDelete] must never
; name a path under {userdocs}.
UsedUserAreasWarning=no

#if TargetArch == "x86"
OutputBaseFilename=spfy-setup-{#SpfyVersion}-x86
#else
OutputBaseFilename=spfy-setup-{#SpfyVersion}
#endif
OutputDir=dist
Compression=lzma2/ultra
SolidCompression=yes

; 64-bit installer behavior — needed so {syswow64}/regsvr32 resolves
; correctly when registering the 32-bit COM DLL on a 64-bit host.
; "x64compatible" is the modern Inno 6.3+ identifier (covers x64 and arm64
; running x64 binaries). Falls back to "x64" on older Inno.
;
; ⚠ ArchitecturesAllowed is a HARD REFUSAL, not a preference: Setup aborts on
; a machine that doesn't match, before touching anything. x64compatible alone
; is why this installer would not run on 32-bit Windows at all — the payload
; was always capable (spfy_sapi.dll and spfy_synth.exe are i686, min OS 4.0,
; msvcrt not UCRT, no api-set imports), the installer simply refused.
#if TargetArch == "x86"
ArchitecturesAllowed=x86compatible
; No ArchitecturesInstallIn64BitMode: there is no 64-bit mode to install in,
; and naming it here would be a compile error on a 32-bit-only build.
#else
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
#endif

UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={app}\spfy.ico
WizardStyle=modern

; Branding — single 256x256 multi-resolution .ico sourced from the
; installer/ directory. Used by:
;   * SetupIconFile         — the icon Explorer shows for spfy-setup-*.exe
;   * UninstallDisplayIcon  — the icon in Settings > Apps & features /
;                             Control Panel > Programs and Features
;   * [Icons] IconFilename  — Start Menu shortcuts that want app branding
;
; Wizard small image — the icon in the top-right corner of every
; wizard page after Welcome. BMP only (not ICO). Multiple comma-
; separated paths let Inno pick the best for the user's DPI:
;   1.00x  55x58
;   1.25x  64x68    (uncomment + add the file if you generate it)
;   1.50x  83x88
;   2.00x 110x116
;   2.50x 138x145
;   3.00x 164x174
; Recommend at least 1x + 2x to cover modern Windows hi-DPI displays.
WizardSmallImageFile=spfy_wizard_small.bmp,spfy_wizard_small_150.bmp,spfy_wizard_small_200.bmp

; Optional: the big banner on the Welcome and Finish pages. 164x314 px
; at 1x; same multi-DPI convention applies.
;   WizardImageFile=spfy_wizard.bmp,spfy_wizard_150.bmp,spfy_wizard_200.bmp

SetupIconFile=spfy.ico

; Show "Speechify (spfy)" in Add/Remove Programs.
AppContact={#MyAppURL}

; ---------------------------------------------------------------------
; [Languages]
; ---------------------------------------------------------------------

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

; ---------------------------------------------------------------------
; [Files]
; ---------------------------------------------------------------------

[Files]
; --- Binaries → {app} ---
; The 32-bit SAPI DLL gets regsvr32'd via [Run] below. We DON'T use
; Inno's `regserver` flag because:
;   (a) It always uses {sys}\regsvr32 which is 64-bit on x64; our DLL
;       is 32-bit and must register via SysWOW64\regsvr32.
;   (b) The 32-bit DLL's DllRegisterServer writes BOTH the 32- and
;       64-bit CLSID hives + voice tokens — running regsvr32 separately
;       on the 64-bit DLL would double-register.
;
; ⚠ The `32bit`/`64bit` flags select which VIEW of {app} / {sys} the entry
; resolves in, and `64bit` is INVALID unless the install is running in 64-bit
; mode. On the x86 build there is no such mode, so the 64-bit shim is not
; merely skipped — it must not appear in the script at all.
Source: "{#BuildDir}\src\sapi\spfy_sapi.dll";   DestDir: "{app}"; Flags: ignoreversion 32bit
#if TargetArch == "x64"
Source: "{#BuildDir}\src\sapi\spfy_sapi64.dll"; DestDir: "{app}"; Flags: ignoreversion 64bit
#endif
Source: "{#BuildDir}\src\cli\spfy_synth.exe";   DestDir: "{app}"; Flags: ignoreversion 32bit

; Helper batch — self-elevating regsvr32 wrapper for re-scanning the
; en-US folder after the user drops new voice directories in. Bundled
; so the Start Menu "Refresh SAPI Voices" shortcut and the post-install
; flow have something to point at.
Source: "refresh_voices.bat"; DestDir: "{app}"; Flags: ignoreversion

; App icon — bundled so UninstallDisplayIcon and Start Menu shortcuts
; can reference {app}\spfy.ico. SetupIconFile (above) reads it at
; compile time, this entry ships it for runtime references.
Source: "spfy.ico"; DestDir: "{app}"; Flags: ignoreversion

; --- NO shared FE data any more ---
;
; This installer used to lay out %USERPROFILE%\Documents\Speechify\spfy\
; with fe_symbol_table.json + fe_tables_a + fe_tables, because
; spfy_sapi.dll built literal paths to them. It no longer does: the DLL
; links spfy_embedded_assets (the same blob spfy_synth.exe has carried all
; along) and extracts to %TEMP%\spfy_assets_<dll-mtime> on first voice load.
;
; That removes a whole class of breakage — a relocated Documents folder, a
; second Windows account, or a partial uninstall used to kill every SAPI
; voice while the CLI carried on working, because only the DLL depended on
; the on-disk copy.
;
; Verified before this was deleted, not after: with all three RENAMED AWAY on
; disk, a 32-bit SAPI client selected "Speechify - tom" and produced a
; 72,918-byte WAV, and 728 files appeared under %TEMP%\spfy_assets_*. The DLL
; keeps an on-disk fallback for existing installs, so the rename is what
; proved the embedded path was the one being used.
;
; tom_hpclass.bin is gone too: hpclass is passed NULL on both the SAPI path
; and the CLI short form, so it is derived per-voice from the VIN. Its only
; readers are spfy_dump_voice --hpclass, spfy_anchor_replay and
; spfy_hp_score_test — none of which this installer ships.

; --- Pitch marks → the voice folder ---
; Needed only by Speechify 4 mode (spfy_synth --s4 / SPFY_4_MODE=1), which
; retargets F0 by TD-PSOLA and cannot start without them. They are ANALYSIS
; METADATA measured from the audio — periods in samples, one run per unit —
; not voice data, which is why they ship here while the VIN/VDB/VCF do not.
;
; They land in the voice folder because the pitch-mark stem is derived from
; the VDB path (tom8.vdb -> tom8), so they must sit beside it. Installing
; them before the user has dropped the voice in is fine and deliberate: the
; folder is created either way, and the marks are simply waiting when the
; voice arrives. CountVoiceDirs still needs the .vin/8.vdb/.vcf trio, so a
; folder holding only marks is correctly NOT counted as a voice.
;
; NO skipifsourcedoesntexist, deliberately: if these are missing the compile
; must fail loudly. Shipping an installer whose --s4 silently does nothing is
; worse than not shipping one.
; ⚠ FLAGS ARE LOAD-BEARING. Read before changing either line.
;
; onlyifdoesntexist  — never overwrite a file already sitting there. These
;   land in the USER'S OWN data folder, which may be a working copy of this
;   repository (it is on the maintainer's machine: {userdocs}\Speechify IS
;   the checkout). Clobbering a newer, locally regenerated set of pitch marks
;   with the ones frozen into the installer would be silent and unrecoverable
;   without a backup.
;
; uninsneveruninstall — never DELETE them on uninstall. Inno removes whatever
;   it installed, and that is exactly how an uninstall wiped tracked files out
;   of the working tree: an earlier build of this script also installed
;   fe_symbol_table.json and 728 fe_tables\*.bin under {userdocs}, so removing
;   the program removed source files too. The build then linked an EMPTY asset
;   blob.
;
; RULE FOR ANYTHING ADDED HERE LATER: inside {app} the installer owns the
; files and may delete them freely. Inside {userdocs} it is a GUEST — install
; only what is missing, and never take anything away.
Source: "{#SourceRoot}\en-US\tom\tom8.pmindex"; DestDir: "{userdocs}\Speechify\en-US\tom"; Flags: onlyifdoesntexist uninsneveruninstall
Source: "{#SourceRoot}\en-US\tom\tom8.pmdata";  DestDir: "{userdocs}\Speechify\en-US\tom"; Flags: onlyifdoesntexist uninsneveruninstall

; --- Documentation (best-effort, not all repos will have these) ---
Source: "{#SourceRoot}\SPFY_README.md"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceRoot}\SPEECHIFY_4_FINDINGS.md"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

; ---------------------------------------------------------------------
; [Icons] — Start Menu group
; ---------------------------------------------------------------------

[Icons]
; Primary user-facing action: re-scan the voices folder. The user runs
; this after dropping new voice folders (containing <name>.vin /
; <name>8.vdb / <name>.vcf) into %USERPROFILE%\Documents\Speechify\
; en-US\. The batch self-elevates via UAC and re-runs regsvr32.
Name: "{group}\Refresh SAPI Voices"; Filename: "{app}\refresh_voices.bat"; \
  WorkingDir: "{app}"; IconFilename: "{sys}\shell32.dll"; IconIndex: 238
Name: "{group}\Open Voices Folder"; Filename: "{userdocs}\Speechify\en-US"; \
  IconFilename: "{sys}\shell32.dll"; IconIndex: 4
Name: "{group}\Documentation"; Filename: "{app}\SPFY_README.md"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"

; ---------------------------------------------------------------------
; [Run] — post-install actions
; ---------------------------------------------------------------------

[Run]
; Register the 32-bit SAPI DLL. SysWOW64\regsvr32.exe is the 32-bit one
; (yes, the naming is backwards — Windows historical legacy). The DLL's
; DllRegisterServer writes both 32- and 64-bit registry views and
; auto-scans %USERPROFILE%\Documents\Speechify\en-US\ for voices.
;
; ⚠ THE ARCH SPLIT IS EXPLICIT ON PURPOSE. Inno documents {syswow64} as
; falling back to {sys} on 32-bit Windows, so one hardcoded line would
; probably have worked — but this is the step that actually registers the
; voices, and "probably" is not a property worth shipping in the one line
; whose failure leaves a silently voiceless install. {#RegSvrDir} is chosen
; at compile time in the preprocessor block at the top of this file.
Filename: "{#RegSvrDir}\regsvr32.exe"; \
  Parameters: "/s ""{app}\spfy_sapi.dll"""; \
  StatusMsg: "Registering SAPI voice DLL..."; \
  Flags: runascurrentuser waituntilterminated

; ---------------------------------------------------------------------
; [UninstallRun] — pre-uninstall actions
; ---------------------------------------------------------------------

[UninstallDelete]
; DllRegisterServer extracts the FE tables to {app}\fe_assets during the
; elevated post-install regsvr32. Inno only removes what it installed, so
; without this the directory (~730 files) is orphaned on uninstall.
; Wildcard: the directory name carries a content digest (fe_assets_<hex>), so
; an upgrade that changes the FE tables leaves the previous one behind too.
Type: filesandordirs; Name: "{app}\fe_assets_*"

; ---------------------------------------------------------------------
; [UninstallRun] — pre-uninstall actions
; ---------------------------------------------------------------------

[UninstallRun]
; Deregister BEFORE files are deleted (otherwise regsvr32 /u can't find
; the DLL to call DllUnregisterServer). The unregister sweep removes:
;   - HKLM\SOFTWARE\Classes\CLSID\{9C3A7D1E-...}  (both views)
;   - All Speechify_* tokens under
;     HKLM\SOFTWARE\Microsoft\Speech\Voices\Tokens (both views)
;   - Same under HKCU
Filename: "{syswow64}\regsvr32.exe"; \
  Parameters: "/u /s ""{app}\spfy_sapi.dll"""; \
  StatusMsg: "Deregistering SAPI voice DLL..."; \
  Flags: runascurrentuser waituntilterminated; \
  RunOnceId: "DeregisterSpfySapi"

; ---------------------------------------------------------------------
; [Code] — install-time sanity checks
; ---------------------------------------------------------------------

[Code]
function InitializeSetup(): Boolean;
var
  WinVersion: TWindowsVersion;
begin
  GetWindowsVersionEx(WinVersion);
  if not WinVersion.NTPlatform or (WinVersion.Major < 6) then
  begin
    MsgBox('This installer requires Windows Vista or newer.',
           mbError, MB_OK);
    Result := False;
    Exit;
  end;
  Result := True;
end;

function CountVoiceDirs(const Root: String): Integer;
var
  Rec: TFindRec;
  Voice: String;
begin
  Result := 0;
  if not DirExists(Root) then Exit;
  if FindFirst(Root + '\*', Rec) then
  try
    repeat
      if (Rec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
        if (Rec.Name <> '.') and (Rec.Name <> '..') then
        begin
          Voice := Root + '\' + Rec.Name + '\' + Rec.Name;
          { Count as a voice only if the canonical trio is present. }
          if FileExists(Voice + '.vin')
             and FileExists(Voice + '8.vdb')
             and FileExists(Voice + '.vcf') then
            Result := Result + 1;
        end;
    until not FindNext(Rec);
  finally
    FindClose(Rec);
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  VoicesDir: String;
  Found: Integer;
  Msg: String;
  Opened: Integer;
begin
  if CurStep = ssPostInstall then
  begin
    VoicesDir := ExpandConstant('{userdocs}\Speechify\en-US');
    ForceDirectories(VoicesDir);
    Found := CountVoiceDirs(VoicesDir);

    if Found = 0 then
    begin
      { Fresh install — no voices on disk yet. Explain the workflow
        and offer to open the folder. }
      Msg :=
        'Speechify is installed, but no SAPI voices have been registered yet.' + #13#10 + #13#10 +
        'SAPI voices need the raw SpeechWorks voice data (VIN/VDB/VCF), which is NOT bundled. You can find the voices in the GitHub repo (https://github.com/wagwan-piffting-blud/Speechify) or at Internet Archive (https://archive.org/details/SpeechifyTom).' + #13#10 + #13#10 +
        'To finish setup:' + #13#10 +
        '  1. Drop each voice folder into:' + #13#10 +
        '       ' + VoicesDir + #13#10 +
        '     (each folder must contain <name>.vin, <name>8.vdb, <name>.vcf)' + #13#10 + #13#10 +
        '  2. Run Start Menu > Speechify > "Refresh SAPI Voices"' + #13#10 +
        '     (or open an elevated cmd and run "%~dpsapi\refresh_voices.bat")' + #13#10 + #13#10 +
        'Open the voices folder in Explorer now?';
      if MsgBox(Msg, mbConfirmation, MB_YESNO) = IDYES then
      begin
        ShellExec('open', VoicesDir, '', '', SW_SHOWNORMAL, ewNoWait, Opened);
      end;
    end
    else
    begin
      { Voices already present (re-install / upgrade) — registration
        already picked them up. Just confirm count. }
      Msg :=
        'Speechify is installed and ' + IntToStr(Found) +
        ' voice(s) registered with SAPI.' + #13#10 + #13#10 +
        'Restart your SAPI client (Balabolka, Narrator, etc.) to see them.' + #13#10 + #13#10 +
        'If you add more voices later, run Start Menu > Speechify > "Refresh SAPI Voices".';
      MsgBox(Msg, mbInformation, MB_OK);
    end;
  end;
end;
