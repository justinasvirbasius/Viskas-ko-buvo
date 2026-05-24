/*
 * app_xmllite.c — XML parsing and emission via xmllite
 *
 * Demonstrates IXmlReader / IXmlWriter from xmllite.dll — the
 * Microsoft-recommended pull parser / streamed writer that doesn't
 * require MSXML/COM apartments. Used by Windows itself for the SxS
 * manifest reader, XPS, and various OS components.
 *
 *   - CreateXmlReader(&IID_IXmlReader, &reader, NULL) builds a pull parser
 *   - CreateXmlWriter(&IID_IXmlWriter, &writer, NULL) builds a streamed
 *     writer
 *   - Either binds to an IStream; SHCreateMemStream wraps a byte buffer
 *   - IXmlReader::Read returns one node type per call; IXmlReader::GetNodeType,
 *     ::GetLocalName, ::GetValue, ::MoveToFirstAttribute / MoveToNextAttribute
 *     walk the document
 *   - IXmlWriter::WriteStartDocument / WriteStartElement /
 *     WriteAttributeString / WriteString / WriteEndElement / WriteEndDocument
 *     emit a tree
 *
 * We write a small document into a memory stream, then re-read it back
 * and dump every node — proving both halves.
 */

#define COBJMACROS
#define CINTERFACE
#define INITGUID

#include "shell.h"
#include <xmllite.h>
#include <shlwapi.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "xmllite.lib")
#pragma comment(lib, "shlwapi.lib")

#define XL_PROP   L"MS_XL_STATE"
#define ID_XL_GO  116001
#define ID_XL_OUT 116002

typedef struct { HWND output; } XlState;
static WNDPROC g_origXlFrame = NULL;

static void Xl_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
}

static const wchar_t *Xl_NodeTypeName(XmlNodeType nt)
{
    switch (nt) {
    case XmlNodeType_Element:                return L"Element";
    case XmlNodeType_Attribute:              return L"Attribute";
    case XmlNodeType_Text:                   return L"Text";
    case XmlNodeType_CDATA:                  return L"CDATA";
    case XmlNodeType_ProcessingInstruction:  return L"PI";
    case XmlNodeType_Comment:                return L"Comment";
    case XmlNodeType_DocumentType:           return L"DocType";
    case XmlNodeType_Whitespace:             return L"Whitespace";
    case XmlNodeType_EndElement:             return L"EndElement";
    case XmlNodeType_XmlDeclaration:         return L"XmlDecl";
    }
    return L"?";
}

static IStream *Xl_WriteDoc(XlState *st)
{
    IStream *stream = SHCreateMemStream(NULL, 0);
    IXmlWriter *writer = NULL;
    HRESULT hr;
    LARGE_INTEGER zero;

    if (!stream) return NULL;
    hr = CreateXmlWriter(&IID_IXmlWriter, (void **)&writer, NULL);
    if (FAILED(hr)) { IStream_Release(stream); return NULL; }

    IXmlWriter_SetOutput(writer, (IUnknown *)stream);
    IXmlWriter_SetProperty(writer, XmlWriterProperty_Indent, TRUE);

    IXmlWriter_WriteStartDocument(writer, XmlStandalone_Omit);
    IXmlWriter_WriteComment(writer, L" MiniShell xmllite demo ");
    IXmlWriter_WriteStartElement(writer, NULL, L"library", NULL);
    {
        IXmlWriter_WriteStartElement(writer, NULL, L"book", NULL);
        IXmlWriter_WriteAttributeString(writer, NULL, L"id", NULL, L"42");
        IXmlWriter_WriteAttributeString(writer, NULL, L"genre", NULL, L"sci-fi");
        IXmlWriter_WriteElementString(writer, NULL, L"title", NULL, L"The Stars My Destination");
        IXmlWriter_WriteElementString(writer, NULL, L"author", NULL, L"Alfred Bester");
        IXmlWriter_WriteEndElement(writer);
    }
    {
        IXmlWriter_WriteStartElement(writer, NULL, L"book", NULL);
        IXmlWriter_WriteAttributeString(writer, NULL, L"id", NULL, L"43");
        IXmlWriter_WriteElementString(writer, NULL, L"title", NULL, L"Dune");
        IXmlWriter_WriteElementString(writer, NULL, L"author", NULL, L"Frank Herbert");
        IXmlWriter_WriteEndElement(writer);
    }
    IXmlWriter_WriteEndElement(writer);
    IXmlWriter_WriteEndDocument(writer);
    IXmlWriter_Flush(writer);
    IXmlWriter_Release(writer);

    /* Print the bytes */
    {
        STATSTG s;
        if (SUCCEEDED(IStream_Stat(stream, &s, STATFLAG_NONAME))) {
            char *buf = (char *)malloc((size_t)s.cbSize.QuadPart + 1);
            wchar_t hdr[80];
            ULONG read = 0;
            zero.QuadPart = 0;
            IStream_Seek(stream, zero, STREAM_SEEK_SET, NULL);
            if (buf) {
                IStream_Read(stream, buf, (ULONG)s.cbSize.QuadPart, &read);
                buf[read] = 0;
                {
                    wchar_t *w = (wchar_t *)malloc((read + 1) * sizeof(wchar_t));
                    if (w) {
                        int n = MultiByteToWideChar(CP_UTF8, 0, buf, read, w, read);
                        w[n] = 0;
                        swprintf_s(hdr, 80, L"== Generated XML (%lu bytes) ==\r\n", read);
                        Xl_Append(st->output, hdr);
                        Xl_Append(st->output, w);
                        Xl_Append(st->output, L"\r\n");
                        free(w);
                    }
                }
                free(buf);
            }
        }
    }
    zero.QuadPart = 0;
    IStream_Seek(stream, zero, STREAM_SEEK_SET, NULL);
    return stream;
}

static void Xl_ReadDoc(XlState *st, IStream *stream)
{
    IXmlReader *reader = NULL;
    HRESULT hr;
    XmlNodeType nt;

    hr = CreateXmlReader(&IID_IXmlReader, (void **)&reader, NULL);
    if (FAILED(hr)) return;
    IXmlReader_SetInput(reader, (IUnknown *)stream);

    Xl_Append(st->output, L"\r\n== Parsed back via IXmlReader ==\r\n");

    while (S_OK == IXmlReader_Read(reader, &nt)) {
        const wchar_t *name = NULL;
        const wchar_t *value = NULL;
        UINT nameLen = 0, valLen = 0;
        wchar_t line[400];
        IXmlReader_GetLocalName(reader, &name, &nameLen);
        IXmlReader_GetValue(reader, &value, &valLen);

        swprintf_s(line, 400, L"  %-12s  name='%s'  value='%s'\r\n",
                   Xl_NodeTypeName(nt),
                   name  ? name  : L"",
                   value ? value : L"");
        Xl_Append(st->output, line);

        if (nt == XmlNodeType_Element) {
            /* Walk attributes */
            if (IXmlReader_MoveToFirstAttribute(reader) == S_OK) {
                do {
                    const wchar_t *aName, *aVal;
                    UINT aNameLen, aValLen;
                    IXmlReader_GetLocalName(reader, &aName, &aNameLen);
                    IXmlReader_GetValue(reader, &aVal, &aValLen);
                    swprintf_s(line, 400, L"      @%s = '%s'\r\n",
                               aName ? aName : L"", aVal ? aVal : L"");
                    Xl_Append(st->output, line);
                } while (IXmlReader_MoveToNextAttribute(reader) == S_OK);
                IXmlReader_MoveToElement(reader);
            }
        }
    }
    IXmlReader_Release(reader);
}

static void Xl_RunDemo(XlState *st)
{
    IStream *stream;
    SetWindowTextW(st->output, L"");
    stream = Xl_WriteDoc(st);
    if (!stream) {
        Xl_Append(st->output, L"Failed to create stream.\r\n");
        return;
    }
    Xl_ReadDoc(st, stream);
    IStream_Release(stream);
}

static LRESULT CALLBACK Xl_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    XlState *st = (XlState *)GetPropW(hwnd, XL_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_XL_GO) { Xl_RunDemo(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, XL_PROP); }
    return CallWindowProcW(g_origXlFrame, hwnd, msg, wp, lp);
}

static HWND XmlLite_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    XlState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"XmlLite",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (XlState *)calloc(1, sizeof(XlState));
    if (!st) { DestroyWindow(frame); return NULL; }

    CreateWindowExW(0, L"BUTTON", L"Write then parse",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 160, 26, frame, (HMENU)(LONG_PTR)ID_XL_GO, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"Generates a small XML doc with IXmlWriter, then re-parses it\r\n"
        L"with IXmlReader and dumps every node.\r\n",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_XL_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, XL_PROP, (HANDLE)st);
    if (!g_origXlFrame) g_origXlFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Xl_FrameProc);
    return frame;
}

MsApp g_AppXmlLite = { L"XmlLite", XmlLite_Create, 780, 560 };
