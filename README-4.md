# MiniShell

A Windows desktop shell substitute written in C using the Win32 API. Runs as
a full-screen desktop with a **paginated taskbar** at the bottom. **55 demo
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
              |  | events.c     |        |  apps/  (55 apps)       |
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
| **D3D11** | Direct3D 11: `D3D11CreateDeviceAndSwapChain`, runtime HLSL via `D3DCompile`, vertex+constant buffers, animated triangle |
| **WasapiOut** | WASAPI shared mode: `IMMDeviceEnumerator`, `IAudioClient`, `IAudioRenderClient`, event-driven worker, mix-format respecting (float or PCM) |
| **MicLevel** | `waveInOpen` with `CALLBACK_FUNCTION`, double-buffered capture, peak-level meter — no audio retained |
| **TaskDlg** | `TaskDialogIndirect` with command links, footer, and a progress dialog that completes via `TDM_SET_PROGRESS_BAR_POS` from a `TDN_TIMER` callback |
| **OpenDlg** | `IFileOpenDialog` (modern COM), `FOS_ALLOWMULTISELECT`, `FOS_PICKFOLDERS`, `IShellItemArray` results |
| **SendIn** | `SendInput` synthesizing keystrokes into the app's own edit box, including Ctrl+A composite |
| **Console** | `AllocConsole` to attach a debug console to the GUI process, `SetConsoleTextAttribute` colored writes, `FreeConsole` |
| **Windows** | `EnumWindows` listing all visible top-level HWNDs system-wide, double-click to `SetForegroundWindow` |
| **Power** | `GetSystemPowerStatus`, `SetThreadExecutionState`, `WM_POWERBROADCAST` notifications |
| **NetInfo** | `GetUserNameExW` (secur32), `GetComputerNameExW`, `NetWkstaGetInfo` (netapi32) |

## Coverage

Libraries exercised: **user32, gdi32, kernel32, comctl32, shlwapi, comdlg32,
advapi32, ws2_32, opengl32, shell32, windowscodecs, ole32, uuid, d2d1, dwrite,
bcrypt, winhttp, gdiplus, winmm, pdh, d3d11, secur32, netapi32.**

Patterns and techniques:
- Window subclassing (`SetWindowLongPtr` + `CallWindowProc`)
- Custom + standard window classes
- Per-window state via `SetProp` and `GWLP_USERDATA`
- Cross-thread UI updates via `PostMessage` with heap-allocated payloads
- Cooperative cancellation via events checked in workers
- Custom non-client area, paginated taskbar
- Lazy data population (tree expansion, list refresh on timer)
- COM from plain C (`CINTERFACE` + `lpVtbl->Method`), `INITGUID` for self-emitted IIDs
- OpenGL legacy 3D and Direct3D 11 modern 3D
- Direct2D + DirectWrite hardware-accelerated 2D
- Anonymous, named, and mailslot IPC
- Page-file shared memory + mutex
- Sync, overlapped, IOCP I/O
- Win32 thread pool and bespoke worker threads
- Raw HID input; `SendInput` synthesis
- System tray + balloon + popup menu
- System-wide hotkeys
- Layered (translucent) and shaped (non-rectangular) windows
- Per-monitor DPI awareness with `WM_DPICHANGED`
- BCrypt CNG hashing; WinHTTP HTTPS
- SCM service enumeration; Event Log reading; PDH counters
- Global atom table
- Modern TaskDialog and IFileOpenDialog COM
- WASAPI shared-mode playback (event-driven); `waveIn` capture metering
- AllocConsole attachment to a GUI app
- EnumWindows, GetSystemPowerStatus, SetThreadExecutionState
- secur32 / netapi32 identity & workstation info

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
- **Shared** — increment in one window; the change appears in all other Shared windows (even across MiniShell processes).
- **HttpsGet** — try `example.com` / `/`. Real TLS via WinHTTP.
- **Hasher** — drop any file. SHA-256 on worker thread.
- **Async** — pick a large file and try both buttons to compare overlapped+event vs IOCP throughput.
- **RawInput** — hover the cursor over the window; HID-level deltas.
- **ThreadPool** — submit N jobs; they finish in groups equal to your CPU count.
- **DpiAware** — drag between monitors with different scaling.
- **EventLog** — type `System` (default), `Application`, etc.
- **Counters** — live PDH. CPU is a rate counter so needs a second tick.
- **ShapedWin** — switch between rounded, oval, and ring shapes.
- **Atoms** — open two; ATOMs registered in one are visible to the other.
- **Services** — most users can list but not modify; run elevated to start/stop.
- **D3D11** — modern 3D triangle. Needs `d3d11.dll` and `d3dcompiler_47.dll` (ships with Windows 10+).
- **WasapiOut** — continuous 440 Hz sine through the engine. Respects device mix format.
- **MicLevel** — live peak meter, no audio retained.
- **TaskDlg** — try all three: simple, command-link, progress.
- **OpenDlg** — modern multi-select file picker and folder picker.
- **SendIn** — Type/Hotkey/Clear synthesize input into the local edit box.
- **Console** — Allocate to pop a debug console; the colored output is via `SetConsoleTextAttribute`.
- **Windows** — list every visible top-level HWND; double-click to foreground it.
- **Power** — battery + AC info; Toggle keep-awake / display-on.
- **NetInfo** — user, computer, and domain identity.

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

## License

Public domain / CC0.
