# MiniShell

A tiny Windows desktop shell written in C using the Win32 API. It runs as a
full-screen "desktop" with a taskbar at the bottom; clicking a launcher opens
a movable app window. Three apps ship in the box: an analog clock, a text
editor, and a calculator.

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
             |  +--------------+        +-------+---------+
             |  | events.c     |        |  apps/          |
             |  | WndProcs     |        |   app_clock.c   |
             |  +------+-------+        |   app_editor.c  |
             |         |                |   app_calc.c    |
             v         v                +-----------------+
        +----+-----------------+
        |  window_manager.c    |   tracks live app HWNDs
        +----------------------+
```

Full diagram in `docs/architecture.mermaid`.

### Modules

| File | Role |
|---|---|
| `src/main.c` | `wWinMain` entry, delegates to `Shell_Run`. |
| `src/shell.c` | Registers window classes, creates the desktop, runs the message loop. |
| `src/events.c` | `Shell_WndProc` (desktop paint, taskbar clicks) and `AppFrame_WndProc` (title bar, close button, drag). |
| `src/window_manager.c` | Fixed-size table of live app windows; lookup, register, unregister, focus cycling. |
| `src/app_registry.c` | Holds the list of available `MsApp` descriptors and launches new instances. |
| `src/apps/app_clock.c` | Analog clock: child window, GDI, 1-second timer. |
| `src/apps/app_editor.c` | Text editor: wraps a multi-line `EDIT` control. |
| `src/apps/app_calc.c` | Calculator: button grid + read-only display, simple operator state machine. |
| `include/shell.h` | Public types (`MsApp`, `MsAppWindow`), constants, function declarations. |

### Why this shape

- **One desktop, many app frames.** The shell window owns the screen and draws the taskbar. Each app is a top-level popup that the shell parents — that keeps focus and Z-order behaving naturally.
- **App descriptor + create function.** Adding a new app is just one new `MsApp g_AppX = { ... }` plus an extern in `app_registry.c`. No central enum to update, no recompile of unrelated modules.
- **Frame procedure shared, content procedure per app.** All apps use `AppFrame_WndProc` for the title bar / close button. Apps that need their own messages (calculator's `WM_COMMAND`, editor's `WM_SIZE`) install a *subclass* on top, preserving the frame behavior underneath.

## Build

### MSVC (recommended)

Open an **x64 Native Tools Command Prompt for VS 2019/2022**, then:

```cmd
build.bat
```

This produces `MiniShell.exe` in the project root.

### MinGW-w64 / GCC

With `gcc` on PATH (e.g. MSYS2 mingw64 shell or w64devkit):

```cmd
build_mingw.bat
```

## Run

```cmd
MiniShell.exe
```

- Click a taskbar button to launch an app.
- Drag an app by its title bar.
- Click the red `X` to close.
- `Ctrl+Tab` from the desktop cycles foreground app.
- `Esc` on the desktop exits.

## Adding a new app

1. Create `src/apps/app_yours.c`. Export a creation function and a descriptor:

   ```c
   static HWND Yours_Create(HWND parent, int x, int y, int w, int h, MsApp *self) {
       /* CreateWindowExW(... MS_CLASS_APPFRAME ...) and child controls */
   }

   MsApp g_AppYours = {
       L"Yours",
       Yours_Create,
       400, 300  /* default width, height */
   };
   ```

2. In `src/app_registry.c`, add:

   ```c
   extern MsApp g_AppYours;
   /* and inside Registry_Init: */
   g_apps[g_app_count++] = &g_AppYours;
   ```

3. Add the source file to `build.bat` (and `build_mingw.bat`). Done.

## Limitations / next steps

- No minimize/maximize buttons on app frames — only close.
- The taskbar only shows launchers, not currently-running apps.
- Window manager is a fixed-size table (`MS_MAX_APP_WINDOWS = 16`). Swap to a list if needed.
- No persistence: closing the shell loses all app state (editor content, calc value).
- Wallpaper is a solid color. A `LoadImage` + `BitBlt` in `Shell_WndProc`'s `WM_PAINT` would add a real backdrop.

## License

Public domain / CC0. Use freely.
