@echo off
REM Build with MinGW-w64 (gcc)
REM Requires gcc on PATH (e.g. via MSYS2 or w64devkit)

setlocal
set OUT=MiniShell.exe
set CFLAGS=-O2 -Wall -municode -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -Iinclude
set LIBS=-luser32 -lgdi32

set SOURCES=src\main.c src\shell.c src\window_manager.c src\events.c src\app_registry.c src\apps\app_clock.c src\apps\app_editor.c src\apps\app_calc.c

gcc %CFLAGS% %SOURCES% -o %OUT% %LIBS% -mwindows
if errorlevel 1 (
    echo *** Build failed ***
    exit /b 1
)
echo Built %OUT%.
endlocal
