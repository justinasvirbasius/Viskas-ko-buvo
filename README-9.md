# MiniShell

A Win32 desktop-shell substitute, in C. Each "app" is a small,
self-contained C file under `src/apps/` that exercises a distinct
Win32 API surface. A paginated taskbar at the bottom of the screen
launches them.

The project is deliberately *broad* rather than deep — the goal is to
demonstrate as many corners of the Windows API as possible in working,
buildable code that compiles with either MSVC or MinGW.

**115 apps across 13 batches, 42 libraries.**

## Build

**MSVC** — open an *x64 Native Tools Command Prompt for VS* and run:

```
build.bat
```

**MinGW** — from a shell with `gcc` on the PATH:

```
build_mingw.bat
```

Output: `MiniShell.exe`.

## Architecture

```
main.c → shell.c ─┬─ window_manager.c
                  ├─ events.c (paginated taskbar)
                  └─ app_registry.c → 115 apps in src/apps/
```

Each app is a `MsApp` descriptor (title, factory, default size). The
shell creates a borderless desktop window, hosts a paginated taskbar,
and lets the user launch any registered app. Apps are popup child
windows owned by the desktop.

See `docs/architecture.mermaid` for the full graph.

## Apps by batch

### Batch 1 — fundamentals
| App | Surface |
|---|---|
| Clock | `WM_TIMER`, `GetLocalTime`, GDI text rendering |
| Editor | `EDIT` control, `WM_SETTEXT`, undo/cut/copy/paste |
| Calc | `BUTTON` grid, basic eval |

### Batch 2 — controls and GDI
| App | Surface |
|---|---|
| Explorer | `LISTVIEW` with `FindFirstFileW` / `FindNextFileW` |
| Paint | GDI drawing, `WM_LBUTTONDOWN` / `WM_MOUSEMOVE` |
| Terminal | Edit + GDI raster output |
| Note | Multi-line edit + file I/O |
| SysMon | `GlobalMemoryStatusEx`, `GetTickCount64` |
| Color | RGB sliders, `CreateSolidBrush` |

### Batch 3 — system services
| App | Surface |
|---|---|
| ImageView | `LoadImageW` BMP |
| Snake | Game loop with `WM_TIMER` |
| Fetcher | Worker thread, posted messages |
| Procs | `CreateToolhelp32Snapshot` Process32First |
| Settings | `SystemParametersInfoW` |
| Clipboard | `OpenClipboard`, `CF_UNICODETEXT` |
| Beeper | `Beep`, `MessageBeep` |
| RegTree | `RegOpenKeyExW`, `RegEnumKeyExW` |

### Batch 4 — advanced surfaces
| App | Surface |
|---|---|
| GLCube | OpenGL via `wglCreateContext`, `CS_OWNDC` |
| HexView | Custom GDI hex viewer |
| CmdRun | `CreateProcessW` + redirected pipes |
| Tray | `Shell_NotifyIconW`, balloon tips |
| RichDoc | `RICHEDIT50W`, `LoadLibrary("Msftedit.dll")` |
| PngView | WIC (`IWICImagingFactory`) |
| HotKey | `RegisterHotKey`, `WM_HOTKEY` |
| Progress | `PROGRESS_CLASS` + marquee |

### Batch 5 — modern stacks and IPC
| App | Surface |
|---|---|
| D2D | Direct2D + DirectWrite from C |
| Hasher | BCrypt SHA-256 streamed |
| HttpsGet | WinHTTP HTTPS GET |
| PipeChat | Named pipes (server + client) |
| Shared | Page-file shared memory, named mutex |
| Services | SCM `EnumServicesStatusExW`, start/stop |
| Monitors | `EnumDisplayMonitors` |
| GdiPlus | GDI+ flat C API |
| Layered | `WS_EX_LAYERED`, `SetLayeredWindowAttributes` |
| DateBook | `DateTimePicker`, `MonthCal` |
| WavPlay | `PlaySoundW` SND_MEMORY |

### Batch 6 — deeper-water APIs
| App | Surface |
|---|---|
| MailSlot | `CreateMailslotW`, `WriteFile` broadcast |
| Async | Overlapped + event AND IOCP |
| RawInput | `RegisterRawInputDevices` HID |
| ThreadPool | `CreateThreadpoolWork` / `SubmitThreadpoolWork` |
| DpiAware | `SetProcessDpiAwarenessContext`, per-monitor v2 |
| EventLog | `OpenEventLogW`, `ReadEventLogW` backwards |
| Counters | PDH (`PdhAddCounterW`, `PdhCollectQueryData`) |
| ShapedWin | `CombineRgn`, `SetWindowRgn` |
| Atoms | `GlobalAddAtomW`, atom table |

### Batch 7 — multimedia, modern dialogs, orphaned surfaces
| App | Surface |
|---|---|
| D3D11 | `D3D11CreateDevice`, dynamic `d3dcompiler_47.dll` |
| WasapiOut | `IAudioClient` event-driven output |
| MicLevel | `waveInOpen` peak meter |
| TaskDlg | `TaskDialogIndirect` |
| OpenDlg | `IFileOpenDialog` |
| SendIn | `SendInput` synthesized keystrokes |
| Console | `AllocConsole`, attach/detach |
| Windows | `EnumWindows`, hierarchy walk |
| Power | `GetSystemPowerStatus`, `SetThreadExecutionState` |
| NetInfo | `GetUserNameExW`, `NetWkstaGetInfo` |

### Batch 8 — system inspection, crypto, richer shell
| App | Surface |
|---|---|
| AesCipher | BCrypt AES-256-CBC roundtrip |
| Compress | Compression API LZMS |
| MetaFile | `CreateEnhMetaFileW`, record/replay |
| Sessions | `WTSEnumerateSessionsW` |
| FileOps | `SHFileOperationW` double-NUL list |
| Locales | `EnumSystemLocalesEx` |
| Modules | Toolhelp `TH32CS_SNAPMODULE` |
| Broadcast | `SendMessageTimeoutW(HWND_BROADCAST, ...)` |
| DibClip | Clipboard `CF_DIB` round-trip |
| SysSpec | System metrics, memory, disks |

### Batch 9 — networking, devices, shell integration, identity, modern UI
| App | Surface |
|---|---|
| TcpList | `GetExtendedTcpTable` PID-keyed |
| NetAdapt | `GetAdaptersAddresses` |
| Devices | `SetupDiGetClassDevsW` |
| HookKbd | `WH_KEYBOARD_LL` global hook |
| DragSrc | OLE drag *source* (`IDropSource` + `IDataObject` hand-built) |
| Dpapi | `CryptProtectData` / `CryptUnprotectData` |
| DirWatch | `ReadDirectoryChangesW` overlapped |
| ShellLnk | `IShellLinkW` + `IPersistFile` |
| Fonts | `EnumFontFamiliesExW` |
| DwmAttr | DWM dark mode, caption color, rounded corners |

### Batch 10 — security, taskbar, hardware, drawing, INI/GUID/SEH
| App | Surface |
|---|---|
| TokenInfo | `OpenProcessToken`, SID/integrity/groups/privileges |
| JumpList | `ICustomDestinationList`, `IObjectCollection` |
| Topology | `GetLogicalProcessorInformationEx` |
| PrintEnum | `EnumPrintersW` level 2 |
| IniFile | `GetPrivateProfileStringW` round-trip |
| GuidGen | `CoCreateGuid` *and* `BCryptGenRandom` |
| Curves | `Pie`, `Chord`, `Arc`, `AngleArc`, `PolyBezier`, paths |
| NetWatch | `NotifyAddrChange` overlapped |
| SehProbe | `__try` / `__except`, `SetUnhandledExceptionFilter` |
| DibDraw | `CreateDIBSection`, direct BGRA Mandelbrot |

### Batch 11 — accessibility events, shell modernity, sessions, encoding
| App | Surface |
|---|---|
| WinEvent | `SetWinEventHook` global accessibility events |
| TaskBar3 | `ITaskbarList3` progress, overlay icon |
| PerPixAlpha | `UpdateLayeredWindow` with 32-bit premultiplied BGRA |
| ShellProps | `SHCreateItemFromParsingName` + `IPropertyStore` enumeration |
| KeyMap | `MapVirtualKey`, `ToUnicodeEx`, `GetKeyboardLayoutList` |
| TimeZones | `EnumDynamicTimeZoneInformation`, multi-zone clocks |
| DevNotify | `RegisterDeviceNotificationW` for USB + volumes |
| SessHook | `WTSRegisterSessionNotification`, `WM_WTSSESSION_CHANGE` |
| WsaEvent | `WSAEventSelect` event-based async TCP |
| Encodings | `WideCharToMultiByte` codepage survey |

### Batch 12 — Media Foundation, WMI, automation, OLE target, MMCSS, certs, restart manager
| App | Surface |
|---|---|
| MediaFndr | `IMFSourceReader` + stream introspection |
| WmiQuery | `IWbemLocator` + `IWbemServices::ExecQuery` WQL |
| UIAuto | `IUIAutomation::GetFocusedElement` polling |
| PasteTgt | OLE drop target via `RegisterDragDrop` + `IDropTarget` |
| OldWatch | `FindFirstChangeNotificationW` |
| MmcssAud | `AvSetMmThreadCharacteristicsW(L"Pro Audio")` |
| SaveDlg | `IFileSaveDialog` with file types |
| CertStore | `CertOpenSystemStoreW` + cert enumeration |
| RstrtMgr | `RmStartSession` + `RmGetList` |
| ClipMon | `AddClipboardFormatListener` push notifications |

### Batch 13 — process introspection, install/network metadata, themes, PE, COM ROT
| App | Surface |
|---|---|
| Psapi | `EnumProcesses` + `QueryFullProcessImageNameW` + `GetProcessMemoryInfo` |
| MsiList | `MsiEnumProductsW` + `MsiGetProductInfoW` |
| Symlink | `CreateSymbolicLinkW` + `FSCTL_GET_REPARSE_POINT` |
| GuiInfo | `GetGUIThreadInfo` cross-process focus/caret/menu state |
| ShellWins | `IShellWindows` enumerate Explorer windows via COM |
| WlanInfo | `WlanOpenHandle` + `WlanGetAvailableNetworkList` |
| PowerSch | `PowerEnumerate` + `PowerGetActiveScheme` |
| UxTheme | `OpenThemeData` + `DrawThemeBackground` |
| PeInfo | Memory-mapped PE header walk + imports via `imagehlp` |
| RotList | `GetRunningObjectTable` + `IEnumMoniker` |

## Libraries linked (42)

`user32`, `gdi32`, `comctl32`, `shlwapi`, `comdlg32`, `advapi32`,
`ws2_32`, `opengl32`, `shell32`, `windowscodecs`, `ole32`, `oleaut32`,
`uuid`, `d2d1`, `dwrite`, `bcrypt`, `winhttp`, `gdiplus`, `winmm`,
`pdh`, `d3d11`, `secur32`, `netapi32`, `cabinet`, `wtsapi32`,
`iphlpapi`, `setupapi`, `crypt32`, `dwmapi`, `winspool`, `propsys`,
`mfplat`, `mfreadwrite`, `mfuuid`, `wbemuuid`, `rstrtmgr`, `psapi`,
`msi`, `wlanapi`, `powrprof`, `uxtheme`, `imagehlp`. Plus `avrt.dll`
loaded dynamically by MmcssAud, and `kernel32` implicit.

## Notes

- All code is C (no C++). COM is consumed via `CINTERFACE` +
  `COBJMACROS` (e.g. `ITaskbarList3_SetProgressValue(p, ...)`).
- Unicode throughout: `UNICODE` / `_UNICODE` defined globally.
- `_WIN32_WINNT=0x0A00` so Windows 10+ surfaces are visible.
- Worker threads talk back to the UI via `PostMessageW` with heap
  `wchar_t*` payloads; the UI thread frees them after appending.
- Each app is *isolated*: deleting `src/apps/app_X.c` and removing
  the matching extern/init line in `app_registry.c` is all you need
  to drop a feature.
