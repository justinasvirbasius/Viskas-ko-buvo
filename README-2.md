# MiniShell

A Windows desktop shell written in C using the Win32 API. Runs as a
full-screen "desktop" with a taskbar at the bottom; clicking a launcher opens
a movable app window. **25 demo apps** ship in the box, each chosen to
exercise a different corner of Win32.

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
              |  | events.c     |        |  apps/  (25 apps)       |
              |  | WndProcs     |        +-------------------------+
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
| **Clock** | Custom child class, GDI lines/ellipses, `SetTimer`, `localtime_s` |
| **Editor** | Multi-line `EDIT`, `WM_SETFONT`, frame subclass for resize |
| **Calc** | `BUTTON` grid, `WM_COMMAND` dispatch, per-window state via `SetProp` |

### Batch 2 — controls and GDI
| App | Win32 techniques |
|---|---|
| **Explorer** | `FindFirstFileW`/`FindNextFileW`, `LISTBOX` with `LBN_DBLCLK` |
| **Paint** | Off-screen `CreateCompatibleBitmap`, `SetCapture`, `BitBlt` |
| **Terminal** | Two `EDIT`s, subclassed input for `VK_RETURN`, in-process commands |
| **Note** | Multi-instance launching, `WM_CTLCOLOREDIT` tint |
| **SysMon** | `GlobalMemoryStatusEx`, `GetSystemTimes`, ring buffer, double-buffered chart |
| **Color** | `TRACKBAR_CLASSW`, `WM_HSCROLL`, live preview |

### Batch 3 — system services
| App | Win32 techniques |
|---|---|
| **ImageView** | `GetOpenFileNameW`, `LoadImageW`, `StretchBlt` (BMP only) |
| **Snake** | `QueryPerformanceCounter`, fixed-timestep game loop, `WM_KEYDOWN` |
| **Fetch** | Winsock (`WSAStartup`, `getaddrinfo`, `connect`, `recv`), worker thread, `PostMessage` from worker to UI |
| **Procs** | `CreateToolhelp32Snapshot`, `LISTVIEW` report mode + columns, `TrackPopupMenu`, `OpenProcess`/`TerminateProcess` |
| **Settings** | `WC_TABCONTROL`, registry persistence via `RegCreateKeyEx`/`RegSetValueEx`/`RegQueryValueEx` |
| **Clipboard** | `AddClipboardFormatListener` + `WM_CLIPBOARDUPDATE`, `GlobalAlloc(GMEM_MOVEABLE)` write |
| **Beeper** | `Beep()`, worker thread, cancellation via `CreateEventW` + `SetEvent` |
| **RegTree** | `WC_TREEVIEW`, lazy expansion on `TVN_ITEMEXPANDING`, `RegEnumKeyExW` |

### Batch 4 — advanced surfaces
| App | Win32 techniques |
|---|---|
| **GLCube** | OpenGL: `PIXELFORMATDESCRIPTOR`, `ChoosePixelFormat`, `wglCreateContext`, `SwapBuffers`, animated cube |
| **HexView** | `CreateFileMappingW` + `MapViewOfFile`, drag-and-drop via `DragAcceptFiles` + `WM_DROPFILES`, `WM_MOUSEWHEEL` |
| **CmdRun** | `CreatePipe`, `SetHandleInformation`, `CreateProcessW` with `STARTF_USESTDHANDLES`, real `cmd.exe /C` output capture |
| **Tray** | `Shell_NotifyIconW` (`NIM_ADD`/`NIM_MODIFY`/`NIM_DELETE`), balloon via `NIF_INFO`, callback message routing |
| **RichDoc** | RichEdit 5.0 (`Msftedit.dll`), `EM_SETCHARFORMAT` (bold/italic/underline), `EM_SETPARAFORMAT` (alignment), `TOOLBARCLASSNAME`, `STATUSCLASSNAME` |
| **PngView** | WIC from plain C: `CoCreateInstance(CLSID_WICImagingFactory)`, `CreateDecoderFromFilename`, `IWICFormatConverter` → BGRA32, `CreateDIBSection` |
| **HotKey** | `RegisterHotKey`/`UnregisterHotKey`, `WM_HOTKEY`, `FlashWindowEx` |
| **Progress** | `PROGRESS_CLASS` (`PBM_SETPOS`, `PBM_SETMARQUEE`), worker thread + cooperative cancellation event |

## Coverage

By library — every one of these is exercised by at least one app:
**user32, gdi32, kernel32, comctl32, shlwapi, comdlg32, advapi32, ws2_32, opengl32, shell32, windowscodecs, ole32.**

By pattern:
- Window subclassing (`SetWindowLongPtr` + `CallWindowProc`)
- Custom + standard window classes
- Per-window state via `SetProp` and `GWLP_USERDATA`
- Cross-thread UI updates via `PostMessage` with heap-allocated payloads (UI thread frees)
- Cooperative cancellation via auto-reset event + worker checks
- Custom non-client area (the app frame's title bar + close button)
- Lazy data population (tree expansion, list refresh on timer)
- COM from plain C (`CINTERFACE` + `lpVtbl->Method`)
- OpenGL rendering context creation on a child window
- Anonymous pipes for child-process I/O capture
- System tray icons with balloon notifications + popup menus
- System-wide keyboard hooks via `RegisterHotKey`

## Build

### MSVC (recommended)

Open an **x64 Native Tools Command Prompt for VS 2019/2022**, then:

```cmd
build.bat
```

Links: `user32 gdi32 comctl32 shlwapi comdlg32 advapi32 ws2_32 opengl32 shell32 windowscodecs ole32`.

### MinGW-w64 / GCC

```cmd
build_mingw.bat
```

## Run

```cmd
MiniShell.exe
```

- Click a taskbar button to launch an app. Each click spawns a new instance.
- Drag an app by its title bar. Red `X` closes.
- `Ctrl+Tab` from the desktop cycles foreground app. `Esc` exits.

### App tips

- **Snake** — click into the window first; arrow keys to play.
- **Paint** — left-drag to paint, click a swatch up top to change color, right-click clears.
- **Terminal** — `help` to list commands.
- **CmdRun** — runs actual `cmd.exe /C <command>`. Try `dir`, `ipconfig`, `echo %TEMP%`.
- **Explorer** — double-click `[folder]` to enter.
- **Procs** — right-click a row for context menu.
- **Fetch** — type a hostname (no `http://`), click Fetch. HTTP-only.
- **Settings** — Save writes to `HKCU\Software\MiniShell`; values restore next launch.
- **Clipboard** — copy text anywhere on your system; appears at top of list. Double-click an old entry to put it back.
- **Beeper** — click a slot to cycle notes. Hit Play.
- **RegTree** — expand a hive to lazily enumerate.
- **HexView** — drop any file onto the window or click Open. Arrow keys, Page Up/Down, mouse wheel to scroll.
- **Tray** — click "Add to tray", then look at the notification area. Left-click toggles window, right-click shows menu.
- **RichDoc** — select text, click B/I/U.
- **PngView** — opens PNG, JPEG, GIF, BMP, TIFF via WIC.
- **HotKey** — register Ctrl+Alt+M or Ctrl+Alt+L, then trigger from anywhere on the system.
- **Progress** — start the sieve; click Cancel mid-flight to see cooperative cancellation. Marquee toggle shows indeterminate mode.

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
- Taskbar buttons are getting tight at 25 apps; future work would paginate them.
- Most app state isn't persisted across restarts (Settings excepted).
- Fetch is HTTP only. HTTPS would need WinHTTP or SChannel.
- ImageView is BMP-only by design; PngView is the WIC variant.

## License

Public domain / CC0.
