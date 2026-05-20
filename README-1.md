# MiniShell

A small Windows desktop shell written in C using the Win32 API. It runs as a
full-screen "desktop" with a taskbar at the bottom; clicking a launcher opens
a movable app window. **17 demo apps** ship in the box, each chosen to
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
              |  | events.c     |        |  apps/  (17 apps)       |
              |  | WndProcs     |        +-------------------------+
              |  +------+-------+
              v         v
         +----+-----------------+
         |  window_manager.c    |   tracks live app HWNDs
         +----------------------+
```

Full module + library graph in `docs/architecture.mermaid`.

## Apps and what they cover

| App | Win32 techniques |
|---|---|
| **Clock** | Custom child window class, GDI line/ellipse, `SetTimer`, `localtime_s` |
| **Editor** | Multi-line `EDIT` control, `WM_SETFONT`, frame subclass for resize |
| **Calc** | Grid of `BUTTON` controls, `WM_COMMAND` dispatch, per-window state via `SetProp` |
| **Explorer** | `FindFirstFileW`/`FindNextFileW`, `LISTBOX` with `LBN_DBLCLK`, path normalization |
| **Paint** | Off-screen `CreateCompatibleBitmap`, `SetCapture`, `BitBlt`, color palette |
| **Terminal** | Two `EDIT` controls, subclassed input for `VK_RETURN`, in-process command dispatch |
| **Note** | Multi-instance launching, `WM_CTLCOLOREDIT` tint |
| **SysMon** | `GlobalMemoryStatusEx`, `GetSystemTimes`, ring buffer, double-buffered chart |
| **Color** | `TRACKBAR_CLASSW` from comctl32, `WM_HSCROLL`, live preview |
| **ImageView** | Common file dialog (`GetOpenFileNameW`), `LoadImageW`, `StretchBlt` |
| **Snake** | `QueryPerformanceCounter`, `WM_KEYDOWN`, fixed-timestep game loop |
| **Fetch** | Winsock (`WSAStartup`, `getaddrinfo`, `connect`, `recv`), worker thread, `PostMessage` from worker to UI |
| **Procs** | `CreateToolhelp32Snapshot`, `LISTVIEW` in report mode with columns, `CreatePopupMenu` + `TrackPopupMenu`, `OpenProcess`/`TerminateProcess` |
| **Settings** | `WC_TABCONTROL` with pages, `RegCreateKeyExW`, `RegSetValueExW`/`RegQueryValueExW` persistence under HKCU |
| **Clipboard** | `AddClipboardFormatListener`, `WM_CLIPBOARDUPDATE`, `OpenClipboard`/`SetClipboardData` with `GlobalAlloc(GMEM_MOVEABLE)` |
| **Beeper** | `Beep()`, worker thread, cancellation via `CreateEventW` + `SetEvent` |
| **RegTree** | `WC_TREEVIEW`, lazy node population on `TVN_ITEMEXPANDING`, `RegEnumKeyExW` |

## Win32 surface area touched

By library:
- **user32**: windows, input, clipboard, menus, listbox/edit/button controls
- **gdi32**: pens, brushes, bitmaps, BitBlt, StretchBlt, text drawing, double buffering
- **kernel32**: timers, threads, events, files (FindFirstFile, Toolhelp32), `Beep`, high-resolution timing, memory
- **comctl32**: trackbar, tab, list-view (report), tree-view, status bar reuse
- **comdlg32**: open file dialog
- **advapi32**: registry — `RegCreateKeyEx`, `RegSetValueEx`, `RegQueryValueEx`, `RegEnumKeyEx`
- **ws2_32**: TCP client end-to-end
- **shlwapi**: linked but used only for path helpers in Explorer

By pattern:
- Window subclassing (`SetWindowLongPtr`/`CallWindowProc`)
- Custom + standard window classes
- Per-window state via `SetProp` and via `GWLP_USERDATA`
- Cross-thread UI updates via `PostMessage` with heap-allocated payloads
- Lazy data population (tree expansion, list refresh on timer)
- Cancellation via auto-reset event + worker thread cooperative checks
- Custom non-client area (the app frame's title bar + close button)

## Build

### MSVC (recommended)

Open an **x64 Native Tools Command Prompt for VS 2019/2022**, then:

```cmd
build.bat
```

Links against `user32`, `gdi32`, `comctl32`, `shlwapi`, `comdlg32`, `advapi32`, `ws2_32`.

### MinGW-w64 / GCC

```cmd
build_mingw.bat
```

## Run

```cmd
MiniShell.exe
```

- Click a taskbar button to launch an app. Each click creates a new instance.
- Drag an app by its title bar. Click the red `X` to close.
- `Ctrl+Tab` from the desktop cycles foreground app. `Esc` exits.

### App tips

- **Snake** — click into the window first to give it focus, then arrow keys.
- **Paint** — click a swatch up top to change color; right-click clears.
- **Terminal** — `help` to list commands.
- **Explorer** — double-click `[folder]` to enter, `[..]` to go up.
- **Procs** — right-click a row for the context menu.
- **Fetch** — type a hostname (no `http://`), click Fetch.
- **Settings** — Save writes to `HKCU\Software\MiniShell`; values restore on next launch.
- **Clipboard** — copy text anywhere on your system; it appears at the top of the list. Double-click an old entry to put it back on the clipboard.
- **Beeper** — click a slot to cycle `C D E F G A B C+ --`. Hit Play.
- **RegTree** — expand any hive to lazily enumerate subkeys.

## Adding a new app

1. Create `src/apps/app_yours.c` with a creation function and descriptor:

   ```c
   static HWND Yours_Create(HWND parent, int x, int y, int w, int h, MsApp *self) {
       /* CreateWindowExW(... MS_CLASS_APPFRAME ...) and child controls */
   }
   MsApp g_AppYours = { L"Yours", Yours_Create, 400, 300 };
   ```

2. In `src/app_registry.c`, declare `extern MsApp g_AppYours;` and register it in `Registry_Init`.

3. Add the source file to `build.bat` and `build_mingw.bat`. Done.

## Limitations

- No minimize/maximize, just close.
- Taskbar shows launchers only, not running apps.
- No persistence except for Settings.
- Terminal commands are built-in (no `CreateProcess`/pipes).
- ImageView only opens BMP. Adding PNG/JPEG would mean WIC (`IWICImagingFactory`).
- Fetch does HTTP (port 80) only; HTTPS needs SChannel or WinHTTP.

## License

Public domain / CC0.
