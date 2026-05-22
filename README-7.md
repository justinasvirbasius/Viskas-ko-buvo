# MiniShell

A Windows desktop shell substitute written in C using the Win32 API. Runs as
a full-screen desktop with a **paginated taskbar** at the bottom. **75 demo
apps** ship in the box, each chosen to exercise a different corner of Win32.

## Architecture

```
         +---------------------+
         |       main.c        |   WinMain → Shell_Run
         +----------+----------+
                    |
         +----------v----------+      +----------------------+
         |       shell.c       +----->|   app_registry.c     |
         | desktop + taskbar   |      |  list of MsApp descs |
         +----+-----+----------+      +----------+-----------+
              |     |                            |
              |     v                            v
              |  +--------------+        +-------+-----------------+
              |  | events.c     |        |  apps/  (75 apps)       |
              |  | Wndprocs +   |        +-------------------------+
              |  | paginated    |
              |  | taskbar      |
              |  +------+-------+
              v         v
         +----+-----------------+
         |  window_manager.c    |   tracks live app HWNDs
         +----------------------+
```

Full module + library graph in `docs/architecture.mermaid`.

## Apps

### Batch 1 — fundamentals
| App | Win32 techniques |
|---|---|
| **Clock** | Custom child class, GDI, `SetTimer` |
| **Editor** | Multi-line `EDIT`, frame subclass for resize |
| **Calc** | `BUTTON` grid, `WM_COMMAND` dispatch, per-window state |

### Batch 2 — controls and GDI
| App | Win32 techniques |
|---|---|
| **Explorer** | `FindFirstFileW`, `LISTBOX` with `LBN_DBLCLK` |
| **Paint** | Off-screen bitmap, `SetCapture`, `BitBlt` |
| **Terminal** | Two `EDIT`s, subclassed input, in-process commands |
| **Note** | Multi-instance launching |
| **SysMon** | `GetSystemTimes`, double-buffered chart |
| **Color** | `TRACKBAR_CLASSW`, `WM_HSCROLL` |

### Batch 3 — system services
| App | Win32 techniques |
|---|---|
| **ImageView** | `GetOpenFileNameW`, `LoadImageW` (BMP only) |
| **Snake** | `QueryPerformanceCounter`, fixed-timestep loop |
| **Fetch** | Winsock client (HTTP, port 80) |
| **Procs** | `CreateToolhelp32Snapshot`, ListView, `TerminateProcess` |
| **Settings** | `WC_TABCONTROL` + registry persistence |
| **Clipboard** | `AddClipboardFormatListener` + `WM_CLIPBOARDUPDATE` |
| **Beeper** | `Beep()`, cancellable worker |
| **RegTree** | `WC_TREEVIEW`, lazy `RegEnumKeyExW` |

### Batch 4 — advanced surfaces
| App | Win32 techniques |
|---|---|
| **GLCube** | OpenGL `wglCreateContext`, animated cube |
| **HexView** | `MapViewOfFile` + drag-and-drop |
| **CmdRun** | `CreatePipe` + `CreateProcessW` `STARTF_USESTDHANDLES` |
| **Tray** | `Shell_NotifyIconW`, balloon, popup menu |
| **RichDoc** | RichEdit 5.0 + toolbar + status bar |
| **PngView** | WIC (`IWICFormatConverter` → BGRA32) |
| **HotKey** | `RegisterHotKey`, `FlashWindowEx` |
| **Progress** | `PROGRESS_CLASS` determinate + marquee |

### Batch 5 — modern stacks and IPC
| App | Win32 techniques |
|---|---|
| **D2D** | Direct2D + DirectWrite, `D2DERR_RECREATE_TARGET` |
| **Hasher** | BCrypt CNG SHA-256, streamed, drag-drop |
| **HttpsGet** | WinHTTP `WINHTTP_FLAG_SECURE` |
| **PipeChat** | Named pipes (`\\\\.\\pipe\\…`), auto server/client |
| **Shared** | Page-file shared memory + named mutex |
| **Services** | SCM enumeration + start/stop |
| **Monitors** | `EnumDisplayMonitors` diagram |
| **GdiPlus** | Flat C API, AA path + linear gradient |
| **Layered** | `WS_EX_LAYERED` + `AnimateWindow` |
| **DateBook** | DateTimePicker + MonthCal |
| **WavPlay** | `PlaySoundW(SND_MEMORY \| SND_LOOP)` synthesized WAV |

### Batch 6 — deeper-water APIs
| App | Win32 techniques |
|---|---|
| **MailSlot** | `CreateMailslotW` + client `CreateFileW \\\\.\\mailslot\\…` |
| **Async** | Overlapped + event *and* `CreateIoCompletionPort` worker pool |
| **RawInput** | `RegisterRawInputDevices` mouse HID, `WM_INPUT` |
| **ThreadPool** | `CreateThreadpoolWork`/`SubmitThreadpoolWork` |
| **DpiAware** | `SetProcessDpiAwarenessContext`, `WM_DPICHANGED` |
| **EventLog** | `ReadEventLogW EVENTLOG_BACKWARDS_READ` |
| **Counters** | PDH performance counters |
| **ShapedWin** | `CombineRgn` + `SetWindowRgn` |
| **Atoms** | Global atom table |

### Batch 7 — multimedia, modern dialogs, orphaned surfaces
| App | Win32 techniques |
|---|---|
| **D3D11** | Direct3D 11: `D3D11CreateDeviceAndSwapChain`, runtime HLSL via `D3DCompile`, vertex+constant buffers |
| **WasapiOut** | WASAPI shared mode: `IMMDeviceEnumerator`, `IAudioClient`, `IAudioRenderClient`, event-driven |
| **MicLevel** | `waveInOpen` `CALLBACK_FUNCTION`, double-buffered capture, peak-level meter |
| **TaskDlg** | `TaskDialogIndirect` with command links, footer, callback-driven progress |
| **OpenDlg** | `IFileOpenDialog` modern COM picker, `FOS_ALLOWMULTISELECT`, `FOS_PICKFOLDERS` |
| **SendIn** | `SendInput` synthesizing keystrokes into the app's own edit box |
| **Console** | `AllocConsole` attaching a console to the GUI process, colored `SetConsoleTextAttribute` |
| **Windows** | `EnumWindows` listing all visible top-level HWNDs; double-click to `SetForegroundWindow` |
| **Power** | `GetSystemPowerStatus`, `SetThreadExecutionState`, `WM_POWERBROADCAST` |
| **NetInfo** | `GetUserNameExW` (secur32), `GetComputerNameExW`, `NetWkstaGetInfo` (netapi32) |

### Batch 8 — system inspection, crypto, richer shell
| App | Win32 techniques |
|---|---|
| **AesCipher** | BCrypt AES-256-CBC, `BCryptGenerateSymmetricKey`, PKCS#7 padding, hex IV‖ciphertext |
| **Compress** | Windows Compression API (`CreateCompressor`/`Compress`) with LZMS algorithm |
| **MetaFile** | `CreateEnhMetaFileW` recording vector drawing, `PlayEnhMetaFile` resolution-independent replay |
| **Sessions** | `WTSEnumerateSessionsW` + `WTSQuerySessionInformationW` for terminal/logon sessions |
| **FileOps** | `SHFileOperationW` (`FO_COPY`) with double-NUL-terminated paths and shell progress UI |
| **Locales** | `EnumSystemLocalesEx` + `GetLocaleInfoEx` (display, ISO codes, currency, date format) |
| **Modules** | Toolhelp `CreateToolhelp32Snapshot(TH32CS_SNAPMODULE)`, `Module32First/Next` per-process DLL list |
| **Broadcast** | `SendMessageTimeoutW(HWND_BROADCAST, ...)` and receiving `WM_SETTINGCHANGE` / `WM_FONTCHANGE` |
| **DibClip** | Bitmap clipboard: `CF_DIB` with `GetDIBits`/`StretchDIBits`, mouse drawing canvas |
| **SysSpec** | `GlobalMemoryStatusEx`, `GetDiskFreeSpaceExW`, `GetSystemMetrics`, `SystemParametersInfoW` |

### Batch 9 — networking, devices, shell integration, identity, modern UI
| App | Win32 techniques |
|---|---|
| **TcpList** | `GetExtendedTcpTable` with `TCP_TABLE_OWNER_PID_ALL`, two-pass sizing, PID→process name resolve |
| **NetAdapt** | `GetAdaptersAddresses` walking unicast/gateway/DNS lists per NIC |
| **Devices** | `SetupDiGetClassDevsW(DIGCF_ALLCLASSES \| DIGCF_PRESENT)` + `SetupDiEnumDeviceInfo` |
| **HookKbd** | `SetWindowsHookExW(WH_KEYBOARD_LL, ...)` in-process global keyboard observer |
| **DragSrc** | OLE drag *source*: hand-written `IDropSource` + `IDataObject` vtables, `DoDragDrop` |
| **Dpapi** | `CryptProtectData` / `CryptUnprotectData` user-scope encryption, hex blob format |
| **DirWatch** | `ReadDirectoryChangesW` overlapped, worker-thread blocking on event, posts changes to UI |
| **ShellLnk** | `IShellLinkW` + `IPersistFile::Save`/`Load` to create and resolve `.lnk` shortcuts |
| **Fonts** | `EnumFontFamiliesExW` family enumeration with multi-size sample preview canvas |
| **DwmAttr** | `DwmSetWindowAttribute`: immersive dark mode, caption color, rounded corners, extended frame |

## Coverage

Libraries exercised: **user32, gdi32, kernel32, comctl32, shlwapi, comdlg32,
advapi32, ws2_32, opengl32, shell32, windowscodecs, ole32, uuid, d2d1, dwrite,
bcrypt, winhttp, gdiplus, winmm, pdh, d3d11, secur32, netapi32, cabinet,
wtsapi32, iphlpapi, setupapi, crypt32, dwmapi.**

Patterns and techniques covered:
- Window subclassing (`SetWindowLongPtr` + `CallWindowProc`)
- Custom + standard window classes
- Per-window state via `SetProp` and `GWLP_USERDATA`
- Cross-thread UI updates via `PostMessage` with heap-allocated payloads
- Cooperative cancellation via events checked in workers
- Custom non-client area, paginated taskbar
- Lazy data population (tree expansion, list refresh on timer)
- COM from plain C (`CINTERFACE` + `lpVtbl->Method`), `INITGUID` for self-emitted IIDs
- Hand-written COM server vtables (DragSrc's IDropSource + IDataObject)
- OpenGL legacy 3D and Direct3D 11 modern 3D
- Direct2D + DirectWrite hardware-accelerated 2D
- Enhanced metafile recording/replay; bitmap clipboard with CF_DIB
- Anonymous, named, and mailslot IPC; shared memory + mutex
- Sync, overlapped, IOCP I/O
- Win32 thread pool and bespoke worker threads
- Raw HID input; `SendInput` synthesis; system-wide hotkeys; low-level keyboard hook
- System tray + balloon + popup menu
- Layered (translucent) and shaped (non-rectangular) windows
- Per-monitor DPI awareness with `WM_DPICHANGED`
- DWM extended frame, immersive dark mode, caption color, rounded corners
- BCrypt CNG hashing **and** AES-256-CBC encryption; DPAPI user-scope encryption; WinHTTP HTTPS
- SCM service enumeration; Event Log reading; PDH counters
- Global atom table
- Modern TaskDialog and IFileOpenDialog COM
- WASAPI shared-mode playback (event-driven); `waveIn` capture metering
- AllocConsole attachment to a GUI app
- EnumWindows, GetSystemPowerStatus, SetThreadExecutionState
- secur32 / netapi32 identity & workstation info
- Windows Compression API (LZMS); Toolhelp module enumeration
- `SendMessageTimeout(HWND_BROADCAST, ...)` and receiving broadcasts
- WTS session enumeration; `SHFileOperation`; locale enumeration
- System metrics, memory, and disk inspection
- iphlpapi: TCP connection table + adapter address tree
- setupapi: PnP device enumeration with class properties
- ReadDirectoryChangesW for live folder watch
- IShellLinkW + IPersistFile for shortcut create/resolve
- EnumFontFamiliesExW for installed font discovery

## Build

### MSVC (recommended)

Open an **x64 Native Tools Command Prompt for VS 2019/2022**, then:

```cmd
build.bat
```

### MinGW-w64 / GCC

```cmd
build_mingw.bat
```

## Run

```cmd
MiniShell.exe
```

- The taskbar shows a page of launchers; click `<` or `>` to flip pages.
- Click a launcher to spawn an app. Each click starts a new instance.
- Drag an app by its title bar. Red `X` closes.
- `Ctrl+Tab` cycles foreground app. `Esc` exits the shell.

### Notable app tips

- **PipeChat / MailSlot** — open two instances. First becomes server, second client.
- **Shared** — increment in one window; the change appears in all other Shared windows.
- **HttpsGet** — try `example.com` / `/`. Real TLS via WinHTTP.
- **Hasher** — drop any file. SHA-256 on worker thread.
- **AesCipher** — enter a passphrase, type plaintext, Encrypt → hex; Decrypt with same passphrase.
- **Compress** — click *Fill sample* then *Run round-trip*; status reports ratio.
- **MetaFile** — *Record* then resize the window; the EMF rescales without pixelation.
- **D3D11** — modern 3D triangle; needs `d3dcompiler_47.dll` (ships with Win 10+).
- **WasapiOut** — continuous 440 Hz sine through the engine.
- **MicLevel** — live peak meter, no audio retained.
- **TaskDlg** — try all three: simple, command-link, progress.
- **OpenDlg** — modern multi-select file picker and folder picker.
- **SendIn** — Type/Hotkey/Clear synthesize input into the local edit box.
- **Console** — Allocate to pop a debug console; colored output via `SetConsoleTextAttribute`.
- **Windows** — list every visible top-level HWND; double-click to foreground it.
- **Power** — battery + AC info; Toggle keep-awake / display-on.
- **NetInfo** — user, computer, and domain identity.
- **Sessions** — usually shows session 0 (Services) and your interactive session.
- **FileOps** — picks a folder; shell renders its own copy progress.
- **Locales** — click any locale to inspect its short date, currency, etc.
- **Modules** — pick a process from the combo to list its loaded DLLs.
- **Broadcast** — open two instances; click a button in one to see the other receive it.
- **DibClip** — scribble with the mouse, Copy, paste into Paint or another DibClip.
- **SysSpec** — Refresh to re-poll memory load and disk free.
- **Services** — most users can list but not modify; run elevated to start/stop.
- **EventLog** — type `System` (default), `Application`, etc.
- **Counters** — live PDH. CPU is a rate counter so needs a second tick.
- **TcpList** — refreshes every 3 sec; system-owned (PID 0/4) sockets labeled as such.
- **NetAdapt** — shows every NIC including loopback and virtual; physical address as MAC.
- **Devices** — filter box does case-insensitive substring match on desc/class/manufacturer.
- **HookKbd** — counts only, never records key contents; Stop to unhook.
- **DragSrc** — type some text, then click-and-drag the blue area onto Notepad or any text field.
- **Dpapi** — Protect produces a hex blob; only this user's logon can Unprotect it.
- **DirWatch** — pick a folder, then create/delete/rename files in Explorer to see live events.
- **ShellLnk** — Create writes `MiniShell Demo Shortcut.lnk` to your Desktop pointing at notepad.
- **Fonts** — clicking a family name renders 12/16/24/36-point samples.
- **DwmAttr** — Dark toggles immersive dark title bar; Caption sets purple title (Win 11 only).

## Adding a new app

1. Create `src/apps/app_yours.c`:
   ```c
   static HWND Yours_Create(HWND parent, int x, int y, int w, int h, MsApp *self) {
       /* CreateWindowExW(... MS_CLASS_APPFRAME ...) and child controls */
   }
   MsApp g_AppYours = { L"Yours", Yours_Create, 400, 300 };
   ```
2. In `src/app_registry.c`: `extern MsApp g_AppYours;` and register in `Registry_Init`.
3. Add the source to `build.bat` and `build_mingw.bat`.

## Limitations

- No minimize/maximize, only close.
- Most app state isn't persisted across restarts (Settings excepted).
- Fetch is HTTP only; HttpsGet is the TLS variant.
- ImageView is BMP-only by design; PngView is WIC.
- Services modifications need elevation.
- D3D11 falls back to a friendly error message if `d3dcompiler_47.dll` is missing.
- Modules listing for non-owned processes may be denied without elevation.
- HookKbd: only one instance can hold the low-level hook at a time.
- DwmAttr caption color and rounded corners require Windows 11; on Win 10 the calls succeed silently.

## License

Public domain / CC0.
