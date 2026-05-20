@echo off
REM ============================================================
REM  build.bat — Compile MiniShell with the MSVC toolchain
REM
REM  Run from an "x64 Native Tools Command Prompt for VS" so that
REM  cl.exe and link.exe are on the PATH.
REM ============================================================

setlocal
set OUT=MiniShell.exe
set INC=/I include
set DEFS=/DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN
set CFLAGS=/nologo /W3 /O2 %DEFS% %INC%
set LIBS=user32.lib gdi32.lib comctl32.lib shlwapi.lib comdlg32.lib advapi32.lib ws2_32.lib opengl32.lib shell32.lib windowscodecs.lib ole32.lib uuid.lib

set SOURCES=^
  src\main.c ^
  src\shell.c ^
  src\window_manager.c ^
  src\events.c ^
  src\app_registry.c ^
  src\apps\app_clock.c ^
  src\apps\app_editor.c ^
  src\apps\app_calc.c ^
  src\apps\app_explorer.c ^
  src\apps\app_paint.c ^
  src\apps\app_terminal.c ^
  src\apps\app_note.c ^
  src\apps\app_sysmon.c ^
  src\apps\app_color.c ^
  src\apps\app_imageview.c ^
  src\apps\app_snake.c ^
  src\apps\app_fetcher.c ^
  src\apps\app_procs.c ^
  src\apps\app_settings.c ^
  src\apps\app_clipboard.c ^
  src\apps\app_beeper.c ^
  src\apps\app_regtree.c ^
  src\apps\app_glcube.c ^
  src\apps\app_hexview.c ^
  src\apps\app_cmdrun.c ^
  src\apps\app_tray.c ^
  src\apps\app_richdoc.c ^
  src\apps\app_pngview.c ^
  src\apps\app_hotkey.c ^
  src\apps\app_progress.c

cl %CFLAGS% %SOURCES% /Fe:%OUT% /link %LIBS%
if errorlevel 1 (
    echo.
    echo *** Build failed ***
    exit /b 1
)

echo.
echo Built %OUT%.
endlocal
