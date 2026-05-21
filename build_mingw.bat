@echo off
REM Build with MinGW-w64 (gcc)
setlocal
set OUT=MiniShell.exe
set CFLAGS=-O2 -Wall -municode -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -Iinclude
set LIBS=-luser32 -lgdi32 -lcomctl32 -lshlwapi -lcomdlg32 -ladvapi32 -lws2_32 -lopengl32 -lshell32 -lwindowscodecs -lole32 -luuid -ld2d1 -ldwrite -lbcrypt -lwinhttp -lgdiplus -lwinmm -lpdh -ld3d11 -lsecur32 -lnetapi32

set SOURCES=src\main.c src\shell.c src\window_manager.c src\events.c src\app_registry.c src\apps\app_clock.c src\apps\app_editor.c src\apps\app_calc.c src\apps\app_explorer.c src\apps\app_paint.c src\apps\app_terminal.c src\apps\app_note.c src\apps\app_sysmon.c src\apps\app_color.c src\apps\app_imageview.c src\apps\app_snake.c src\apps\app_fetcher.c src\apps\app_procs.c src\apps\app_settings.c src\apps\app_clipboard.c src\apps\app_beeper.c src\apps\app_regtree.c src\apps\app_glcube.c src\apps\app_hexview.c src\apps\app_cmdrun.c src\apps\app_tray.c src\apps\app_richdoc.c src\apps\app_pngview.c src\apps\app_hotkey.c src\apps\app_progress.c src\apps\app_d2d.c src\apps\app_hasher.c src\apps\app_httpsget.c src\apps\app_pipechat.c src\apps\app_shared.c src\apps\app_services.c src\apps\app_monitors.c src\apps\app_gdiplus.c src\apps\app_layered.c src\apps\app_datebook.c src\apps\app_wavplay.c src\apps\app_mailslot.c src\apps\app_async.c src\apps\app_rawinput.c src\apps\app_threadpool.c src\apps\app_dpiaware.c src\apps\app_eventlog.c src\apps\app_counters.c src\apps\app_shapedwin.c src\apps\app_atoms.c src\apps\app_d3d11.c src\apps\app_wasapiout.c src\apps\app_miclevel.c src\apps\app_taskdlg.c src\apps\app_opendlg.c src\apps\app_sendin.c src\apps\app_console.c src\apps\app_windows.c src\apps\app_power.c src\apps\app_netinfo.c

gcc %CFLAGS% %SOURCES% -o %OUT% %LIBS% -mwindows
if errorlevel 1 (
    echo *** Build failed ***
    exit /b 1
)
echo Built %OUT%.
endlocal
