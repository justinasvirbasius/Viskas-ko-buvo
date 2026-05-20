/*
 * app_regtree.c — Registry tree browser
 *
 * Demonstrates:
 *   - WC_TREEVIEW with TVS_HASBUTTONS / TVS_LINESATROOT
 *   - Lazy population on TVN_ITEMEXPANDING (children inserted as the user
 *     drills in, not all at once)
 *   - RegOpenKeyExW + RegEnumKeyExW
 *   - Mapping HKEY constants to a small set of root nodes
 *
 * Read-only view; just expand and explore. The lParam of each tree item
 * encodes its parent HKEY + relative path so children can be enumerated
 * on demand.
 */

#include "shell.h"
#include <commctrl.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "advapi32.lib")

#define RT_PROP   L"MS_RT_STATE"
#define ID_TREE   11001

typedef struct {
    HKEY    root;
    wchar_t path[1024];  /* relative path under root; empty for root itself */
    BOOL    populated;
} RtNodeData;

typedef struct {
    HWND tree;
} RtState;

static WNDPROC g_origRtFrame = NULL;

static RtNodeData *Rt_NewNode(HKEY root, const wchar_t *path)
{
    RtNodeData *n = (RtNodeData *)calloc(1, sizeof(RtNodeData));
    if (!n) return NULL;
    n->root = root;
    if (path) wcsncpy_s(n->path, 1024, path, _TRUNCATE);
    return n;
}

static HTREEITEM Rt_Insert(HWND tree, HTREEITEM parent, const wchar_t *text,
                           RtNodeData *data, BOOL hasChildren)
{
    TVINSERTSTRUCTW ins;
    ZeroMemory(&ins, sizeof(ins));
    ins.hParent      = parent;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask    = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
    ins.item.pszText = (LPWSTR)text;
    ins.item.lParam  = (LPARAM)data;
    ins.item.cChildren = hasChildren ? 1 : 0;
    return (HTREEITEM)SendMessageW(tree, TVM_INSERTITEMW, 0, (LPARAM)&ins);
}

static void Rt_Populate(HWND tree, HTREEITEM hItem)
{
    TVITEMW it;
    RtNodeData *data;
    HKEY key;
    wchar_t name[256];
    DWORD i, nameLen, subkeys = 0;
    LONG rc;

    ZeroMemory(&it, sizeof(it));
    it.mask = TVIF_PARAM;
    it.hItem = hItem;
    if (!SendMessageW(tree, TVM_GETITEMW, 0, (LPARAM)&it)) return;
    data = (RtNodeData *)it.lParam;
    if (!data || data->populated) return;

    if (data->path[0] == 0) {
        key = data->root;
        rc  = ERROR_SUCCESS;
    } else {
        rc = RegOpenKeyExW(data->root, data->path, 0,
                           KEY_READ | KEY_ENUMERATE_SUB_KEYS, &key);
    }
    if (rc != ERROR_SUCCESS) {
        data->populated = TRUE;
        return;
    }
    RegQueryInfoKeyW(key, NULL, NULL, NULL, &subkeys, NULL, NULL,
                     NULL, NULL, NULL, NULL, NULL);

    for (i = 0; i < subkeys && i < 500; ++i) {  /* clamp */
        nameLen = 256;
        if (RegEnumKeyExW(key, i, name, &nameLen,
                          NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
            wchar_t childPath[1024];
            RtNodeData *childData;
            if (data->path[0]) {
                swprintf_s(childPath, 1024, L"%s\\%s", data->path, name);
            } else {
                wcscpy_s(childPath, 1024, name);
            }
            childData = Rt_NewNode(data->root, childPath);
            /* Mark as having children optimistically — refined when expanded */
            Rt_Insert(tree, hItem, name, childData, TRUE);
        }
    }
    if (key != data->root) RegCloseKey(key);
    data->populated = TRUE;
}

/* Recursively free node lParams on destruction */
static void Rt_FreeNode(HWND tree, HTREEITEM hItem)
{
    TVITEMW it;
    HTREEITEM child;

    if (!hItem) return;
    ZeroMemory(&it, sizeof(it));
    it.mask = TVIF_PARAM;
    it.hItem = hItem;
    if (SendMessageW(tree, TVM_GETITEMW, 0, (LPARAM)&it)) {
        RtNodeData *d = (RtNodeData *)it.lParam;
        if (d) free(d);
    }
    child = (HTREEITEM)SendMessageW(tree, TVM_GETNEXTITEM,
                                    TVGN_CHILD, (LPARAM)hItem);
    while (child) {
        HTREEITEM next = (HTREEITEM)SendMessageW(tree, TVM_GETNEXTITEM,
                                                 TVGN_NEXT, (LPARAM)child);
        Rt_FreeNode(tree, child);
        child = next;
    }
}

static LRESULT CALLBACK Rt_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    RtState *st = (RtState *)GetPropW(hwnd, RT_PROP);

    if (msg == WM_NOTIFY && st) {
        NMHDR *hdr = (NMHDR *)lp;
        if (hdr->idFrom == ID_TREE && hdr->code == TVN_ITEMEXPANDING) {
            NMTREEVIEWW *nm = (NMTREEVIEWW *)lp;
            if (nm->action == TVE_EXPAND) {
                Rt_Populate(st->tree, nm->itemNew.hItem);
            }
            return 0;
        }
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->tree, 4, 32, w - 8, h - 36, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        HTREEITEM root = (HTREEITEM)SendMessageW(st->tree, TVM_GETNEXTITEM,
                                                 TVGN_ROOT, 0);
        while (root) {
            HTREEITEM next = (HTREEITEM)SendMessageW(st->tree, TVM_GETNEXTITEM,
                                                     TVGN_NEXT, (LPARAM)root);
            Rt_FreeNode(st->tree, root);
            root = next;
        }
        free(st);
        RemovePropW(hwnd, RT_PROP);
    }
    return CallWindowProcW(g_origRtFrame, hwnd, msg, wp, lp);
}

static HWND RegTree_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    RtState *st;
    INITCOMMONCONTROLSEX icc;

    (void)self;
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_TREEVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"RegTree",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (RtState *)calloc(1, sizeof(RtState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->tree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, NULL,
        WS_CHILD | WS_VISIBLE | TVS_HASBUTTONS | TVS_LINESATROOT |
        TVS_HASLINES | TVS_SHOWSELALWAYS,
        4, 32, w - 8, h - 36, frame, (HMENU)(LONG_PTR)ID_TREE, hInstance, NULL);

    /* Insert the three commonly-explored root hives */
    Rt_Insert(st->tree, TVI_ROOT, L"HKEY_CURRENT_USER",
              Rt_NewNode(HKEY_CURRENT_USER, L""), TRUE);
    Rt_Insert(st->tree, TVI_ROOT, L"HKEY_LOCAL_MACHINE",
              Rt_NewNode(HKEY_LOCAL_MACHINE, L""), TRUE);
    Rt_Insert(st->tree, TVI_ROOT, L"HKEY_CLASSES_ROOT",
              Rt_NewNode(HKEY_CLASSES_ROOT, L""), TRUE);

    SetPropW(frame, RT_PROP, (HANDLE)st);
    if (!g_origRtFrame)
        g_origRtFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Rt_FrameProc);
    return frame;
}

MsApp g_AppRegTree = {
    L"RegTree",
    RegTree_Create,
    480, 420
};
