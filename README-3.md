# MiniShell

A Windows desktop shell substitute written in C using the Win32 API. Runs as
a full-screen desktop with a **paginated taskbar** at the bottom. **45 demo
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
              |  | events.c     |        |  apps/  (45 apps)       |
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
| **Clock** | Custom child class, GDI lines/ellipses, `SetTimer` |
| **Editor** | Multi-line `EDIT`, frame subclass for resize |
| **Calc** | `BUTTON` grid, `WM_COMMAND` dispatch, per-window state via `SetProp` |

### Batch 2 — controls and GDI
| App | Win32 techniques |
|---|---|
| **Explorer** | `FindFirstFileW`/`FindNextFileW`, `LISTBOX` with `LBN_DBLCLK` |
| **Paint** | Off-screen `CreateCompatibleBitmap`, `SetCapture`, `BitBlt` |
| **Terminal** | Two `EDIT`s, subclassed input for `VK_RETURN`, in-process commands |
| **Note** | Multi-instance launching, `WM_CTLCOLOREDIT` tint |
| **SysMon** | `GlobalMemoryStatusEx`, `GetSystemTimes`, ring-buffer chart |
| **Color** | `TRACKBAR_CLASSW`, `WM_HSCROLL`, live preview |

### Batch 3 — system services
| App | Win32 techniques |
|---|---|
| **ImageView** | `GetOpenFileNameW`, `LoadImageW`, `StretchBlt` (BMP only) |
| **Snake** | `QueryPerformanceCounter`, fixed-timestep loop, `WM_KEYDOWN` |
| **Fetch** | Winsock (`WSAStartup`, `getaddrinfo`, `connect`, `recv`), worker thread |
| **Procs** | `CreateToolhelp32Snapshot`, `LISTVIEW`, `TrackPopupMenu`, `TerminateProcess` |
| **Settings** | `WC_TABCONTROL`, `RegCreateKeyEx`/`RegSetValueEx` persistence |
| **Clipboard** | `AddClipboardFormatListener` + `WM_CLIPBOARDUPDATE`, `GlobalAlloc` |
| **Beeper** | `Beep()`, worker thread, cancellation via event |
| **RegTree** | `WC_TREEVIEW`, lazy expansion on `TVN_ITEMEXPANDING`, `RegEnumKeyExW` |

### Batch 4 — advanced surfaces
| App | Win32 techniques |
|---|---|
| **GLCube** | OpenGL via `wglCreateContext` + `SwapBuffers`, animated cube |
| **HexView** | `CreateFileMappingW` + `MapViewOfFile`, drag-and-drop via `WM_DROPFILES` |
| **CmdRun** | `CreatePipe` + `CreateProcessW` with `STARTF_USESTDHANDLES` |
| **Tray** | `Shell_NotifyIconW`, balloon notifications, popup menu |
| **RichDoc** | RichEdit 5.0, `EM_SETCHARFORMAT`, toolbar, status bar |
| **PngView** | WIC (`CLSID_WICImagingFactory`, `IWICFormatConverter` → BGRA32) |
| **HotKey** | `RegisterHotKey`/`WM_HOTKEY`, `FlashWindowEx` |
| **Progress** | `PROGRESS_CLASS` (`PBM_SETPOS`, `PBM_SETMARQUEE`), cancellable worker |

### Batch 5 — modern stacks and IPC
| App | Win32 techniques |
|---|---|
| **D2D** | Direct2D + DirectWrite, hardware-accelerated 2D, `D2DERR_RECREATE_TARGET` handling |
| **Hasher** | BCrypt CNG (`BCRYPT_SHA256_ALGORITHM`), streamed file hash, drag-and-drop |
| **HttpsGet** | WinHTTP HTTPS (`WINHTTP_FLAG_SECURE`), worker thread |
| **PipeChat** | Named-pipe IPC (`CreateNamedPipeW`/`\\\\.\\pipe\\...`), auto server/client role |
| **Shared** | Page-file-backed shared memory (`CreateFileMappingW(INVALID_HANDLE_VALUE,...)`) + named mutex |
| **Services** | SCM (`OpenSCManagerW`, `EnumServicesStatusExW`, start/stop) |
| **Monitors** | `EnumDisplayMonitors`, MONITORINFOEX, scaled diagram |
| **GdiPlus** | GDI+ flat API in plain C, anti-aliased compound path + linear gradient |
| **Layered** | `WS_EX_LAYERED` + `SetLayeredWindowAttributes`, `AnimateWindow` |
| **DateBook** | `DATETIMEPICK_CLASSW` + `MONTHCAL_CLASSW`, two-way sync |
| **WavPlay** | `PlaySoundW(SND_MEMORY \| SND_LOOP)` with synthesized in-memory WAV |

### Batch 6 — deeper-water APIs
| App | Win32 techniques |
|---|---|
| **MailSlot** | `CreateMailslotW` (server) + `CreateFileW \\\\.\\mailslot\\…` (client), `GetMailslotInfo` poll |
| **Async** | Overlapped `ReadFile` with manual-reset event, *and* `CreateIoCompletionPort` + `GetQueuedCompletionStatus` worker pattern |
| **RawInput** | `RegisterRawInputDevices` (mouse HID), `WM_INPUT`, `GetRawInputData` |
| **ThreadPool** | `CreateThreadpoolWork`/`SubmitThreadpoolWork`, `InterlockedDecrement` countdown |
| **DpiAware** | `SetProcessDpiAwarenessContext` (dynamic), `GetDpiForWindow`, `WM_DPICHANGED` re-layout |
| **EventLog** | `OpenEventLogW`/`ReadEventLogW` (`EVENTLOG_BACKWARDS_READ`), record iteration |
| **Counters** | PDH (`PdhOpenQueryW`, `PdhAddCounterW`, `PdhGetFormattedCounterValue`) |
| **ShapedWin** | `CreateRoundRectRgn` + `CreateEllipticRgn` + `CombineRgn` + `SetWindowRgn` |
| **Atoms** | `GlobalAddAtomW` / `GlobalFindAtomW` / `GlobalGetAtomNameW` / `GlobalDeleteAtom` |

## Coverage

Libraries exercised: **user32, gdi32, kernel32, comctl32, shlwapi, comdlg32,
advapi32, ws2_32, opengl32, shell32, windowscodecs, ole32, uuid, d2d1, dwrite,
bcrypt, winhttp, gdiplus, winmm, pdh.**

Patterns and techniques:
- Window subclassing (`SetWindowLongPtr` + `CallWindowProc`)
- Custom + standard window classes
- Per-window state via `SetProp` and `GWLP_USERDATA`
- Cross-thread UI updates via `PostMessage` with heap-allocated payloads (UI thread frees)
- Cooperative cancellation via auto-reset event + worker checks
- Custom non-client area (app frame title bar + close button)
- Lazy data population (tree expansion, list refresh on timer)
- COM from plain C (`CINTERFACE` + `lpVtbl->Method`)
- OpenGL rendering context creation on a child window
- Direct2D + DirectWrite hardware-accelerated drawing
- Anonymous, named, and one-way (mailslot) IPC pipes
- Shared memory + named mutex
- Synchronous, overlapped, and IOCP I/O
- Win32 thread pool and bespoke worker threads
- Raw HID input
- System tray icons with balloon notifications + popup menus
- System-wide hotkeys (`RegisterHotKey`)
- Layered (translucent) windows and shaped (non-rectangular) windows
- Per-monitor DPI awareness with live `WM_DPICHANGED` re-layout
- BCrypt CNG hashing and WinHTTP HTTPS
- SCM service enumeration; Event Log reading; PDH performance counters
- Global atom table

## Build

### MSVC (recommended)

Open an **x64 Native Tools Command Prompt for VS 2019/2022**, then:

```cmd
build.bat
```

Links: `user32 gdi32 comctl32 shlwapi comdlg32 advapi32 ws2_32 opengl32 shell32 windowscodecs ole32 uuid d2d1 dwrite bcrypt winhttp gdiplus winmm pdh`.

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

- **PipeChat / MailSlot** — open two instances. The first becomes the server, the second the client.
- **Shared** — increment in one window; the change is visible in any other Shared window (or any other MiniShell process).
- **HttpsGet** — try `example.com` / `/`. Real TLS via WinHTTP.
- **Hasher** — drop any file. SHA-256 hash computed on a worker thread.
- **Async** — pick a large file and click both buttons to compare overlapped-with-event vs IOCP throughput.
- **RawInput** — hover the cursor over the window; watch HID-level deltas (independent of cursor acceleration).
- **ThreadPool** — submit N jobs; you'll see they finish nearly simultaneously in groups equal to your CPU count.
- **DpiAware** — drag the window between monitors with different scaling.
- **EventLog** — type `System` (default), `Application`, etc.
- **Counters** — live PDH counters. CPU is a rate counter so needs two ticks.
- **ShapedWin** — switch between rounded, oval, and ring shapes.
- **Atoms** — open two Atoms windows; ATOMs registered in one are visible in the other.
- **Services** — most users can list but not modify. Run elevated to start/stop.

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
- Fetch is HTTP only — HttpsGet is the TLS variant.
- ImageView is BMP-only by design — PngView is the WIC variant.
- Services modifications need elevation; informative error if denied.

## License

Public domain / CC0.
