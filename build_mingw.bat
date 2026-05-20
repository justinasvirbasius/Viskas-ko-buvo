@echo off
REM Build with MinGW-w64 (gcc)
REM Requires gcc on PATH (e.g. via MSYS2 or w64devkit)

setlocal
set OUT=MiniShell.exe
set CFLAGS=-O2 -Wall -municode -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -Iinclude
set LIBS=-luser32 -lgdi32 -lcomctl32 -lshlwapi -lcomdlg32 -ladvapi32 -lws2_32

set SOURCES=src\main.c src\shell.c src\window_manager.c src\events.c src\app_registry.c src\apps\app_clock.c src\apps\app_editor.c src\apps\app_calc.c src\apps\app_explorer.c src\apps\app_paint.c src\apps\app_terminal.c src\apps\app_note.c src\apps\app_sysmon.c src\apps\app_color.c src\apps\app_imageview.c src\apps\app_snake.c src\apps\app_fetcher.c src\apps\app_procs.c src\apps\app_settings.c src\apps\app_clipboard.c src\apps\app_beeper.c src\apps\app_regtree.c

gcc %CFLAGS% %SOURCES% -o %OUT% %LIBS% -mwindows
if errorlevel 1 (
    echo *** Build failed ***
    exit /b 1
)
echo Built %OUT%.
endlocal
