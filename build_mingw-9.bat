@echo off
REM build_mingw.bat — MinGW-w64 build for MiniShell (146 apps, 16 batches)

setlocal
if not exist build mkdir build

set CFLAGS=-O2 -Wall -municode -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE -Iinclude

set LIBS=-luser32 -lgdi32 -lcomctl32 -lshlwapi -lcomdlg32 -ladvapi32 ^
         -lws2_32 -lopengl32 -lshell32 -lwindowscodecs -lole32 -loleaut32 ^
         -luuid -ld2d1 -ldwrite -lbcrypt -lwinhttp -lgdiplus -lwinmm ^
         -lpdh -ld3d11 -lsecur32 -lnetapi32 -lcabinet -lwtsapi32 ^
         -liphlpapi -lsetupapi -lcrypt32 -ldwmapi -lwinspool -lpropsys ^
         -lmfplat -lmfreadwrite -lmfuuid -lwbemuuid -lrstrtmgr -lpsapi ^
         -lmsi -lwlanapi -lpowrprof -luxtheme -limagehlp -lbits -ldxgi ^
         -lxmllite -lwinscard -lsensorsapi -lktmw32 -lhid

set SRC=src\main.c src\shell.c src\events.c src\window_manager.c src\app_registry.c ^
        src\apps\app_clock.c src\apps\app_editor.c src\apps\app_calc.c ^
        src\apps\app_explorer.c src\apps\app_paint.c src\apps\app_terminal.c ^
        src\apps\app_note.c src\apps\app_sysmon.c src\apps\app_color.c ^
        src\apps\app_imageview.c src\apps\app_snake.c src\apps\app_fetcher.c ^
        src\apps\app_procs.c src\apps\app_settings.c src\apps\app_clipboard.c ^
        src\apps\app_beeper.c src\apps\app_regtree.c ^
        src\apps\app_glcube.c src\apps\app_hexview.c src\apps\app_cmdrun.c ^
        src\apps\app_tray.c src\apps\app_richdoc.c src\apps\app_pngview.c ^
        src\apps\app_hotkey.c src\apps\app_progress.c ^
        src\apps\app_d2d.c src\apps\app_hasher.c src\apps\app_httpsget.c ^
        src\apps\app_pipechat.c src\apps\app_shared.c src\apps\app_services.c ^
        src\apps\app_monitors.c src\apps\app_gdiplus.c src\apps\app_layered.c ^
        src\apps\app_datebook.c src\apps\app_wavplay.c ^
        src\apps\app_mailslot.c src\apps\app_async.c src\apps\app_rawinput.c ^
        src\apps\app_threadpool.c src\apps\app_dpiaware.c src\apps\app_eventlog.c ^
        src\apps\app_counters.c src\apps\app_shapedwin.c src\apps\app_atoms.c ^
        src\apps\app_d3d11.c src\apps\app_wasapiout.c src\apps\app_miclevel.c ^
        src\apps\app_taskdlg.c src\apps\app_opendlg.c src\apps\app_sendin.c ^
        src\apps\app_console.c src\apps\app_windows.c src\apps\app_power.c ^
        src\apps\app_netinfo.c ^
        src\apps\app_aescipher.c src\apps\app_compressor.c src\apps\app_metafile.c ^
        src\apps\app_sessions.c src\apps\app_fileops.c src\apps\app_locales.c ^
        src\apps\app_modules.c src\apps\app_broadcast.c src\apps\app_dibclip.c ^
        src\apps\app_sysspec.c ^
        src\apps\app_tcplist.c src\apps\app_netadapt.c src\apps\app_devices.c ^
        src\apps\app_hookkbd.c src\apps\app_dragsrc.c src\apps\app_dpapi.c ^
        src\apps\app_dirwatch.c src\apps\app_shelllnk.c src\apps\app_fonts.c ^
        src\apps\app_dwmattr.c ^
        src\apps\app_tokeninfo.c src\apps\app_jumplist.c src\apps\app_topology.c ^
        src\apps\app_printenum.c src\apps\app_inifile.c src\apps\app_guidgen.c ^
        src\apps\app_curves.c src\apps\app_netwatch.c src\apps\app_sehprobe.c ^
        src\apps\app_dibdraw.c ^
        src\apps\app_winevent.c src\apps\app_taskbar3.c src\apps\app_perpixalpha.c ^
        src\apps\app_shellprops.c src\apps\app_keymap.c src\apps\app_timezones.c ^
        src\apps\app_devnotify.c src\apps\app_sesshook.c src\apps\app_wsaevent.c ^
        src\apps\app_encodings.c ^
        src\apps\app_mediafndr.c src\apps\app_wmiquery.c src\apps\app_uiauto.c ^
        src\apps\app_pastetgt.c src\apps\app_oldwatch.c src\apps\app_mmcssaud.c ^
        src\apps\app_savedlg.c src\apps\app_certstore.c src\apps\app_rstrtmgr.c ^
        src\apps\app_clipmon.c ^
        src\apps\app_psapi.c src\apps\app_msilist.c src\apps\app_symlink.c ^
        src\apps\app_guiinfo.c src\apps\app_shellwins.c src\apps\app_wlaninfo.c ^
        src\apps\app_powersch.c src\apps\app_uxtheme.c src\apps\app_peinfo.c ^
        src\apps\app_rotlist.c ^
        src\apps\app_bitstask.c src\apps\app_d3d12.c src\apps\app_dxgivbl.c ^
        src\apps\app_printhook.c src\apps\app_winsock2.c src\apps\app_setthrdesc.c ^
        src\apps\app_daclookup.c src\apps\app_touchinj.c src\apps\app_adjpriv.c ^
        src\apps\app_apprestart.c ^
        src\apps\app_wssocket.c src\apps\app_dcomp.c src\apps\app_magnify.c ^
        src\apps\app_bluetooth.c src\apps\app_efscrypt.c src\apps\app_xmllite.c ^
        src\apps\app_setupapidrv.c src\apps\app_smartcard.c src\apps\app_gpupref.c ^
        src\apps\app_ptrinput.c src\apps\app_appcntr.c ^
        src\apps\app_animgr.c src\apps\app_xaudio2.c src\apps\app_sensor.c ^
        src\apps\app_hid.c src\apps\app_fwpolicy.c src\apps\app_txreg.c ^
        src\apps\app_inkreco.c src\apps\app_procmit.c src\apps\app_d2deff.c ^
        src\apps\app_audiosess.c

gcc %CFLAGS% -o build\MiniShell.exe %SRC% %LIBS% -mwindows

if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
echo.
echo Built build\MiniShell.exe
endlocal
