/*
 * app_topology.c — CPU topology, cache hierarchy, NUMA layout
 *
 * Demonstrates GetLogicalProcessorInformationEx, which (unlike its older
 * non-Ex sibling) reports the full CPU layout with affinity masks wide enough
 * for systems beyond 64 logical cores:
 *   - Two-pass sizing (NULL buffer returns ERROR_INSUFFICIENT_BUFFER + cb)
 *   - SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX is variable-length; walk by
 *     adding ->Size to the pointer each step
 *   - Per relation: Processor (cores+SMT), Cache (level + line size + size +
 *     type), NumaNode (node number + affinity), ProcessorPackage (sockets)
 *
 * Output is a textual report. Numbers are shown with both decimal and hex
 * forms for the affinity masks so power users can verify topology.
 */

#include "shell.h"
#include <stdlib.h>
#include <stdio.h>

#define TP_PROP    L"MS_TP_STATE"
#define ID_TP_OUT  66001
#define ID_TP_REF  66002

typedef struct { HWND output, refBtn; } TpState;
static WNDPROC g_origTpFrame = NULL;

static void Tp_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
}

static int Tp_PopCount(ULONG_PTR x)
{
    int n = 0;
    while (x) { n += (int)(x & 1); x >>= 1; }
    return n;
}

static const wchar_t *Tp_CacheType(PROCESSOR_CACHE_TYPE t)
{
    switch (t) {
    case CacheUnified:     return L"Unified";
    case CacheInstruction: return L"Instruction";
    case CacheData:        return L"Data";
    case CacheTrace:       return L"Trace";
    }
    return L"?";
}

static void Tp_Refresh(TpState *st)
{
    DWORD cb = 0;
    BYTE *buf = NULL;
    BYTE *p, *end;
    int cores = 0, logical = 0, packages = 0, numaNodes = 0;
    int caches[4] = {0};   /* L1, L2, L3, L4 counts */

    SetWindowTextW(st->output, L"");

    GetLogicalProcessorInformationEx(RelationAll, NULL, &cb);
    if (cb == 0) {
        Tp_Append(st->output, L"GetLogicalProcessorInformationEx sizing failed.\r\n");
        return;
    }
    buf = (BYTE *)malloc(cb);
    if (!buf) return;
    if (!GetLogicalProcessorInformationEx(RelationAll,
            (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)buf, &cb)) {
        free(buf);
        Tp_Append(st->output, L"GetLogicalProcessorInformationEx failed.\r\n");
        return;
    }

    /* First pass: counters */
    p = buf; end = buf + cb;
    while (p < end) {
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *info =
            (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)p;
        switch (info->Relationship) {
        case RelationProcessorCore: {
            ++cores;
            /* GroupMask[0].Mask: logical processors that share this core */
            logical += Tp_PopCount(info->Processor.GroupMask[0].Mask);
            break;
        }
        case RelationCache: {
            int lvl = info->Cache.Level;
            if (lvl >= 1 && lvl <= 4) ++caches[lvl - 1];
            break;
        }
        case RelationNumaNode:        ++numaNodes;  break;
        case RelationProcessorPackage:++packages;   break;
        default: break;
        }
        p += info->Size;
    }

    {
        wchar_t buf2[400];
        swprintf_s(buf2, 400,
            L"== Summary ==\r\n"
            L"  packages (sockets) : %d\r\n"
            L"  NUMA nodes         : %d\r\n"
            L"  physical cores     : %d\r\n"
            L"  logical processors : %d\r\n"
            L"  cache counts       : L1=%d  L2=%d  L3=%d  L4=%d\r\n\r\n",
            packages, numaNodes, cores, logical,
            caches[0], caches[1], caches[2], caches[3]);
        Tp_Append(st->output, buf2);
    }

    /* Second pass: detail */
    Tp_Append(st->output, L"== Detail ==\r\n");
    p = buf;
    while (p < end) {
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *info =
            (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)p;
        wchar_t line[300];
        switch (info->Relationship) {
        case RelationProcessorCore: {
            int n = Tp_PopCount(info->Processor.GroupMask[0].Mask);
            swprintf_s(line, 300,
                L"  core         : mask=0x%016llx  (%d threads, SMT=%s)\r\n",
                (unsigned long long)info->Processor.GroupMask[0].Mask, n,
                (info->Processor.Flags & LTP_PC_SMT) ? L"yes" : L"no");
            Tp_Append(st->output, line);
            break;
        }
        case RelationCache: {
            const CACHE_RELATIONSHIP *c = &info->Cache;
            swprintf_s(line, 300,
                L"  cache L%u %-12s : %5u KB, line %u bytes, assoc=%u\r\n",
                c->Level, Tp_CacheType(c->Type),
                c->CacheSize / 1024,
                c->LineSize, c->Associativity);
            Tp_Append(st->output, line);
            break;
        }
        case RelationNumaNode: {
            swprintf_s(line, 300,
                L"  NUMA node %lu : mask=0x%016llx\r\n",
                info->NumaNode.NodeNumber,
                (unsigned long long)info->NumaNode.GroupMask.Mask);
            Tp_Append(st->output, line);
            break;
        }
        case RelationProcessorPackage: {
            ULONG_PTR mask = info->Processor.GroupMask[0].Mask;
            swprintf_s(line, 300,
                L"  package      : mask=0x%016llx (%d logical)\r\n",
                (unsigned long long)mask, Tp_PopCount(mask));
            Tp_Append(st->output, line);
            break;
        }
        default: break;
        }
        p += info->Size;
    }
    free(buf);
}

static LRESULT CALLBACK Tp_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    TpState *st = (TpState *)GetPropW(hwnd, TP_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_TP_REF) { Tp_Refresh(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->refBtn, 8, 34, 100, 24, TRUE);
        MoveWindow(st->output, 8, 64, w - 16, h - 72, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, TP_PROP); }
    return CallWindowProcW(g_origTpFrame, hwnd, msg, wp, lp);
}

static HWND Topology_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    TpState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Topology",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (TpState *)calloc(1, sizeof(TpState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->refBtn = CreateWindowExW(0, L"BUTTON", L"Refresh",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        8, 34, 100, 24, frame, (HMENU)(LONG_PTR)ID_TP_REF, hInstance, NULL);
    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 64, w - 16, h - 72, frame, (HMENU)(LONG_PTR)ID_TP_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, TP_PROP, (HANDLE)st);
    if (!g_origTpFrame) g_origTpFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Tp_FrameProc);
    Tp_Refresh(st);
    return frame;
}

MsApp g_AppTopology = { L"Topology", Topology_Create, 700, 480 };
