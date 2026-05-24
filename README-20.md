# MiniShell

A Win32 desktop-shell-substitute written in C — a paginated taskbar
hosting **146 demo apps**, each exercising a distinct Win32 API surface.

> ~36,000+ lines of pure C across 16 batches. Builds with MSVC or
> MinGW-w64; no third-party libraries.

---

## Building

### MSVC (recommended)

From an *x64 Native Tools Command Prompt for VS 2019/2022*:

```bat
build.bat
```

Output: `build\MiniShell.exe`.

### MinGW-w64

```bat
build_mingw.bat
```

---

## Running

```bat
build\MiniShell.exe
```

A bottom taskbar appears. Apps are paginated; use the `‹` `›` arrows on
the right of the taskbar to switch pages. Click an app button to
launch it.

---

## Architecture

```
src/
├── main.c               Process entry / window-class registration
├── shell.c              Desktop window class + Shell_Run loop
├── events.c             Paginated taskbar (hit-testing, paging)
├── window_manager.c     Tracks live MsAppWindow handles
└── app_registry.c       Externs + init order for all 146 apps
src/apps/                One file per app (app_<name>.c)
include/shell.h          MsApp interface, layout constants
docs/architecture.mermaid Module / library dependency diagram
```

Each app declares `MsApp g_App<Name> = { title, create, w, h };`. The
registry calls `create(parent, x, y, w, h, self)` to instantiate.

---

## App catalog by batch

### Batch 1 — fundamentals (3)
Clock · Editor · Calc

### Batch 2 — controls and GDI (6)
Explorer · Paint · Terminal · Note · SysMon · Color

### Batch 3 — system services (8)
ImageView · Snake · Fetcher · Procs · Settings · Clipboard · Beeper · RegTree

### Batch 4 — advanced surfaces (8)
GlCube · HexView · CmdRun · Tray · RichDoc · PngView · HotKey · Progress

### Batch 5 — modern stacks & IPC (11)
D2D · Hasher · HttpsGet · PipeChat · Shared · Services · Monitors · GdiPlus · Layered · DateBook · WavPlay

### Batch 6 — deeper-water APIs (9)
MailSlot · Async · RawInput · ThreadPool · DpiAware · EventLog · Counters · ShapedWin · Atoms

### Batch 7 — multimedia, dialogs (10)
D3D11 · WasapiOut · MicLevel · TaskDlg · OpenDlg · SendIn · Console · Windows · Power · NetInfo

### Batch 8 — system inspection, crypto (10)
AesCipher · Compressor · MetaFile · Sessions · FileOps · Locales · Modules · Broadcast · DibClip · SysSpec

### Batch 9 — networking, devices, shell (10)
TcpList · NetAdapt · Devices · HookKbd · DragSrc · Dpapi · DirWatch · ShellLnk · Fonts · DwmAttr

### Batch 10 — security, taskbar, drawing (10)
TokenInfo · JumpList · Topology · PrintEnum · IniFile · GuidGen · Curves · NetWatch · SehProbe · DibDraw

### Batch 11 — accessibility, shell, sessions (10)
WinEvent · TaskBar3 · PerPixAlpha · ShellProps · KeyMap · TimeZones · DevNotify · SessHook · WsaEvent · Encodings

### Batch 12 — MF, WMI, UIA, OLE (10)
MediaFndr · WmiQuery · UIAuto · PasteTgt · OldWatch · MmcssAud · SaveDlg · CertStore · RstrtMgr · ClipMon

### Batch 13 — process / install / theme / PE / COM (10)
Psapi · MsiList · Symlink · GuiInfo · ShellWins · WlanInfo · PowerSch · UxTheme · PeInfo · RotList

### Batch 14 — BITS, D3D12, raw sockets, ACLs (10)
BitsTask · D3D12 · DXGIVbl · PrintHook · WinSock2 · SetThrDesc · DacLookup · TouchInj · AdjPriv · AppRestart

### Batch 15 — modern Win 10 surfaces (11)
WsSocket · DComp · Magnify · Bluetooth · EfsCrypt · XmlLite · SetupApiDrv · SmartCard · GpuPref · PtrInput · AppCntr

### Batch 16 — **NEW** (10)

| App | Win32 surface |
|---|---|
| **AniMgr** | `IUIAnimationManager` + `ScheduleTransition` + per-frame `Update`/`GetValue` |
| **XAudio2** | `XAudio2Create` + `CreateMasteringVoice` + `CreateSourceVoice` + `SubmitSourceBuffer` |
| **Sensor** | `ISensorManager::GetSensorsByCategory` + `ISensor::GetFriendlyName`/`GetState` |
| **Hid** | `HidD_GetHidGuid` + SetupDi enum + `HidD_GetAttributes` + `HidP_GetCaps` |
| **FwPolicy** | `INetFwPolicy2` + `get_Rules` + IEnumVARIANT walk |
| **TxReg** | `CreateTransaction` + `RegCreateKeyTransactedW` + Commit / Rollback |
| **InkReco** | `CLSID_InkRecognizers` + `IInkRecognizer::get_Name`/`get_Languages` |
| **ProcMit** | `GetProcessMitigationPolicy` (10 policy types) |
| **D2dEff** | `ID2D1Factory1::CreateDevice` + `ID2D1DeviceContext::CreateEffect` (GaussianBlur) |
| **AudioSess** | `IAudioSessionManager2::GetSessionEnumerator` + `IAudioSessionControl2` |

---

## Libraries linked (49)

`user32` `gdi32` `comctl32` `shlwapi` `comdlg32` `advapi32` `ws2_32`
`opengl32` `shell32` `windowscodecs` `ole32` `oleaut32` `uuid` `d2d1`
`dwrite` `bcrypt` `winhttp` `gdiplus` `winmm` `pdh` `d3d11` `secur32`
`netapi32` `cabinet` `wtsapi32` `iphlpapi` `setupapi` `crypt32`
`dwmapi` `winspool` `propsys` `mfplat` `mfreadwrite` `mfuuid`
`wbemuuid` `rstrtmgr` `psapi` `msi` `wlanapi` `powrprof` `uxtheme`
`imagehlp` `bits` `dxgi` `xmllite` `winscard` `sensorsapi` `ktmw32`
`hid`

**Loaded dynamically** (not linked): `xaudio2_9.dll` / `xaudio2_8.dll`
(XAudio2), `hid.dll` (Hid), `kernel32.dll` (ProcMit, for
`GetProcessMitigationPolicy`), plus several from prior batches:
`avrt.dll` (MmcssAud), `dcomp.dll`, `Magnification.dll`, `bthprops.cpl`,
`userenv.dll`.

---

## Technical notes (cross-cutting)

- COM consumed from C via `CINTERFACE` + `COBJMACROS` + `INITGUID`
- `_WIN32_WINNT=0x0A00` (Win 10 surface) defined in both build scripts
- Worker→UI updates use `PostMessage` with heap `wchar_t*` payloads
  freed by the UI thread
- Cooperative cancellation via auto-reset event +
  `WaitForSingleObject(evt, 0)` checks
- Most apps subclass the `MS_CLASS_APPFRAME` window class via
  `SetWindowLongPtr(GWLP_WNDPROC)` and hold per-instance state in a
  `GetProp(prop_name)` slot
- DPI policy is per-monitor (set via `SetProcessDpiAwarenessContext`),
  see DpiAware in Batch 6

---

## Status

| | |
|---|---|
| Apps | 146 |
| Batches | 16 |
| Source files | 151 (5 core + 146 apps) |
| Lines of C | ~36,000+ |
| Linked libraries | 49 |
| Dynamically loaded DLLs | 8+ |
| Build flavors | MSVC + MinGW-w64 |
