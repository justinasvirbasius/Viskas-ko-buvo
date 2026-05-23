/*
 * app_peinfo.c — PE file inspector
 *
 * Demonstrates direct PE-format parsing — opens an EXE/DLL via memory
 * mapping and walks the headers in place:
 *   - CreateFileMappingW + MapViewOfFile gives us the file as a byte array
 *   - IMAGE_DOS_HEADER at offset 0; e_lfanew points to IMAGE_NT_HEADERS
 *   - IMAGE_NT_HEADERS{64} contains the optional header + data directories
 *   - IMAGE_DIRECTORY_ENTRY_IMPORT / _EXPORT point at the import/export
 *     descriptor tables
 *   - ImageRvaToVa (imagehlp) translates RVAs to file offsets for mapped
 *     images (the imports/exports tables use RVAs into the image)
 *   - IMAGE_IMPORT_DESCRIPTOR has Name (RVA → DLL name) and OriginalFirstThunk
 *     (RVA → array of IMAGE_THUNK_DATA → IMAGE_IMPORT_BY_NAME)
 *
 * We dump machine type, characteristics, sections, and the first few
 * imported DLLs with their function names.
 */

#include "shell.h"
#include <imagehlp.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "imagehlp.lib")

#define PE_PROP    L"MS_PE_STATE"
#define ID_PE_PATH 100001
#define ID_PE_BR   100002
#define ID_PE_GO   100003
#define ID_PE_OUT  100004

typedef struct { HWND pathEdit, browseBtn, goBtn, output; } PeState;
static WNDPROC g_origPeFrame = NULL;

static void Pe_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
}

static const wchar_t *Pe_Machine(WORD m)
{
    switch (m) {
    case IMAGE_FILE_MACHINE_I386:  return L"x86";
    case IMAGE_FILE_MACHINE_AMD64: return L"x64";
    case IMAGE_FILE_MACHINE_ARM:   return L"ARM";
    case IMAGE_FILE_MACHINE_ARM64: return L"ARM64";
    case IMAGE_FILE_MACHINE_IA64:  return L"IA64";
    }
    return L"?";
}

static void Pe_Inspect(PeState *st)
{
    wchar_t path[MAX_PATH];
    HANDLE  hFile, hMap;
    void    *base;
    LARGE_INTEGER fileSize;
    PIMAGE_DOS_HEADER dos;
    PIMAGE_NT_HEADERS nt;
    LOADED_IMAGE li;
    BOOL    loaded;
    char    cPath[MAX_PATH];

    SetWindowTextW(st->output, L"");
    GetWindowTextW(st->pathEdit, path, MAX_PATH);
    if (!path[0]) {
        Pe_Append(st->output, L"Choose a PE file.\r\n");
        return;
    }

    hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        Pe_Append(st->output, L"CreateFile failed.\r\n");
        return;
    }
    GetFileSizeEx(hFile, &fileSize);
    hMap = CreateFileMappingW(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hMap) { CloseHandle(hFile); Pe_Append(st->output, L"Map failed.\r\n"); return; }
    base = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!base) { CloseHandle(hMap); CloseHandle(hFile); Pe_Append(st->output, L"View failed.\r\n"); return; }

    dos = (PIMAGE_DOS_HEADER)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        Pe_Append(st->output, L"Not a DOS executable (MZ magic missing).\r\n");
        goto cleanup;
    }
    nt = (PIMAGE_NT_HEADERS)((BYTE *)base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        Pe_Append(st->output, L"Not a PE file (PE magic missing).\r\n");
        goto cleanup;
    }

    {
        wchar_t line[600];
        swprintf_s(line, 600,
            L"== PE header ==\r\n"
            L"  Machine          : 0x%04x (%s)\r\n"
            L"  Sections         : %u\r\n"
            L"  Characteristics  : 0x%04x\r\n"
            L"  TimeStamp        : 0x%08lx\r\n"
            L"  SizeOfOptional   : %u\r\n"
            L"  Magic (optional) : 0x%04x\r\n"
            L"  Subsystem        : %u\r\n"
            L"  EntryPoint RVA   : 0x%08lx\r\n"
            L"  ImageBase        : 0x%016llx\r\n",
            nt->FileHeader.Machine, Pe_Machine(nt->FileHeader.Machine),
            nt->FileHeader.NumberOfSections,
            nt->FileHeader.Characteristics,
            nt->FileHeader.TimeDateStamp,
            nt->FileHeader.SizeOfOptionalHeader,
            nt->OptionalHeader.Magic,
            nt->OptionalHeader.Subsystem,
            nt->OptionalHeader.AddressOfEntryPoint,
            (ULONGLONG)nt->OptionalHeader.ImageBase);
        Pe_Append(st->output, line);
    }

    /* Sections */
    {
        PIMAGE_SECTION_HEADER sec = IMAGE_FIRST_SECTION(nt);
        int i;
        Pe_Append(st->output, L"\r\n== Sections ==\r\n");
        for (i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            wchar_t line[300];
            char    n8[9];
            wchar_t name[16];
            memcpy(n8, sec->Name, 8); n8[8] = 0;
            MultiByteToWideChar(CP_UTF8, 0, n8, -1, name, 16);
            swprintf_s(line, 300,
                L"  %-9s  vsize=0x%08lx  rsize=0x%08lx  raw=0x%08lx  char=0x%08lx\r\n",
                name, sec->Misc.VirtualSize, sec->SizeOfRawData,
                sec->PointerToRawData, sec->Characteristics);
            Pe_Append(st->output, line);
        }
    }

    /* Imports via imagehlp */
    WideCharToMultiByte(CP_UTF8, 0, path, -1, cPath, MAX_PATH, NULL, NULL);
    loaded = MapAndLoad(cPath, NULL, &li, FALSE, TRUE);
    if (loaded) {
        ULONG sz = 0;
        PIMAGE_IMPORT_DESCRIPTOR imp =
            (PIMAGE_IMPORT_DESCRIPTOR)ImageDirectoryEntryToData(
                li.MappedAddress, FALSE, IMAGE_DIRECTORY_ENTRY_IMPORT, &sz);
        if (imp) {
            int n = 0;
            Pe_Append(st->output, L"\r\n== Imports ==\r\n");
            while (imp->Name && n < 20) {
                char *dllName = (char *)ImageRvaToVa(
                    li.FileHeader, li.MappedAddress, imp->Name, NULL);
                wchar_t wName[200] = L"<?>";
                wchar_t line[300];
                int fnCount = 0;
                if (dllName) MultiByteToWideChar(CP_UTF8, 0, dllName, -1, wName, 200);

                /* Count functions */
                {
                    PIMAGE_THUNK_DATA thunk = (PIMAGE_THUNK_DATA)ImageRvaToVa(
                        li.FileHeader, li.MappedAddress,
                        imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk,
                        NULL);
                    if (thunk) {
                        while (thunk->u1.AddressOfData) { ++fnCount; ++thunk; }
                    }
                }
                swprintf_s(line, 300, L"  %-40s  %d function(s)\r\n", wName, fnCount);
                Pe_Append(st->output, line);
                ++imp; ++n;
            }
        }
        UnMapAndLoad(&li);
    }

cleanup:
    UnmapViewOfFile(base);
    CloseHandle(hMap);
    CloseHandle(hFile);
}

static void Pe_Browse(PeState *st)
{
    OPENFILENAMEW ofn;
    wchar_t fn[MAX_PATH] = L"";
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = fn;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"PE files\0*.exe;*.dll;*.sys;*.ocx\0All files\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) SetWindowTextW(st->pathEdit, fn);
}

static LRESULT CALLBACK Pe_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    PeState *st = (PeState *)GetPropW(hwnd, PE_PROP);
    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_PE_GO) { Pe_Inspect(st); return 0; }
        if (LOWORD(wp) == ID_PE_BR) { Pe_Browse(st);  return 0; }
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->pathEdit,  12, 38, w - 224, 24, TRUE);
        MoveWindow(st->browseBtn, w - 208, 38, 96, 24, TRUE);
        MoveWindow(st->goBtn,     w - 108, 38, 90, 24, TRUE);
        MoveWindow(st->output,    8, 70, w - 16, h - 78, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, PE_PROP); }
    return CallWindowProcW(g_origPeFrame, hwnd, msg, wp, lp);
}

static HWND PeInfo_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    PeState *st;
    HFONT mono;
    wchar_t defaultPath[MAX_PATH];
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"PeInfo",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (PeState *)calloc(1, sizeof(PeState));
    if (!st) { DestroyWindow(frame); return NULL; }

    ExpandEnvironmentStringsW(L"%windir%\\notepad.exe", defaultPath, MAX_PATH);

    st->pathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", defaultPath,
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        12, 38, w - 224, 24, frame, (HMENU)(LONG_PTR)ID_PE_PATH, hInstance, NULL);
    st->browseBtn = CreateWindowExW(0, L"BUTTON", L"Browse...",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        w - 208, 38, 96, 24, frame, (HMENU)(LONG_PTR)ID_PE_BR, hInstance, NULL);
    st->goBtn = CreateWindowExW(0, L"BUTTON", L"Inspect",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        w - 108, 38, 90, 24, frame, (HMENU)(LONG_PTR)ID_PE_GO, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 70, w - 16, h - 78, frame, (HMENU)(LONG_PTR)ID_PE_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, PE_PROP, (HANDLE)st);
    if (!g_origPeFrame) g_origPeFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Pe_FrameProc);
    Pe_Inspect(st);
    return frame;
}

MsApp g_AppPeInfo = { L"PeInfo", PeInfo_Create, 780, 500 };
