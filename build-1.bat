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
set LIBS=user32.lib gdi32.lib

set SOURCES=^
  src\main.c ^
  src\shell.c ^
  src\window_manager.c ^
  src\events.c ^
  src\app_registry.c ^
  src\apps\app_clock.c ^
  src\apps\app_editor.c ^
  src\apps\app_calc.c

cl %CFLAGS% %SOURCES% /Fe:%OUT% /link %LIBS%
if errorlevel 1 (
    echo.
    echo *** Build failed ***
    exit /b 1
)

echo.
echo Built %OUT%.
endlocal
