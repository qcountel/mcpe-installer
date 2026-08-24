// gen_icon.cpp — generates anx1ous.ico in the launcher's visual style.
// Renders 256/48/32/16 px images with GDI+ and packs them into one .ico.

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#  define UNICODE
#endif
#ifndef _UNICODE
#  define _UNICODE
#endif
#include <windows.h>
#include <wtypes.h>   // PROPID — needed by MinGW's gdiplus headers
#include <gdiplus.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

using namespace Gdiplus;

static void AddRound(GraphicsPath& path, RectF r, float radius) {
    float d = radius * 2.0f;
    path.AddArc(r.X,            r.Y,            d, d, 180.0f, 90.0f);
    path.AddArc(r.X+r.Width-d,  r.Y,            d, d, 270.0f, 90.0f);
    path.AddArc(r.X+r.Width-d,  r.Y+r.Height-d, d, d,   0.0f, 90.0f);
    path.AddArc(r.X,            r.Y+r.Height-d, d, d,  90.0f, 90.0f);
    path.CloseFigure();
}

static int GetPngClsid(CLSID* out) {
    UINT num = 0, size = 0;
    if (GetImageEncodersSize(&num, &size) != Ok || !size) return -1;
    ImageCodecInfo* arr = (ImageCodecInfo*)malloc(size);
    if (!arr) return -1;
    GetImageEncoders(num, size, arr);
    int rc = -1;
    for (UINT i = 0; i < num; ++i)
        if (wcscmp(arr[i].MimeType, L"image/png") == 0) { *out = arr[i].Clsid; rc = 0; break; }
    free(arr);
    return rc;
}

static void RenderIcon(Bitmap* bm, int S) {
    Graphics g(bm);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias); // no ClearType fringes on alpha
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);

    float fS = (float)S;
    GraphicsPath rr;
    AddRound(rr, RectF(0.5f, 0.5f, fS-1.0f, fS-1.0f), fS*0.085f);

    // ── Background: flat graphite + hairline border ────────────────────────
    SolidBrush bgBr(Color(255, 15, 15, 17));                     // #0F0F11
    g.FillPath(&bgBr, &rr);
    Pen borderPen(Color(255, 42, 42, 46), S >= 96 ? 2.0f : 1.0f); // #2A2A2E
    g.DrawPath(&borderPen, &rr);

    // ── «A» — strict caps, light gray, centered ────────────────────────────
    StringFormat cFmt;
    cFmt.SetAlignment(StringAlignmentCenter);
    cFmt.SetLineAlignment(StringAlignmentCenter);

    float fs = fS * 0.60f;
    {   // probe with a trial font, shrink if the glyph overflows
        Font probe(L"Consolas", fs, FontStyleBold, UnitPixel);
        RectF bb;
        g.MeasureString(L"A", -1, &probe, RectF(0,0,fS,fS), &cFmt, &bb);
        if (bb.Width > fS*0.66f) fs *= fS*0.66f / bb.Width;
    }
    Font font(L"Consolas", fs, FontStyleBold, UnitPixel);
    SolidBrush grayBr(Color(255, 216, 216, 220));                // #D8D8DC
    g.DrawString(L"A", -1, &font, RectF(0, 0, fS, fS), &cFmt, &grayBr);

    // ── Red dot, top-right corner — the single accent ──────────────────────
    float r = S >= 48 ? fS*0.048f : fS*0.075f;
    SolidBrush redBr(Color(255, 229, 72, 77));                   // #E5484D
    g.FillEllipse(&redBr, fS*0.78f - r, fS*0.22f - r, r*2.0f, r*2.0f);
}

static bool SavePngMem(Bitmap* bm, const CLSID& clsid,
                       unsigned char** outData, size_t* outLen) {
    IStream* st = nullptr;
    if (CreateStreamOnHGlobal(nullptr, TRUE, &st) != S_OK) return false;
    if (bm->Save(st, &clsid, nullptr) != Ok) { st->Release(); return false; }
    HGLOBAL hg = nullptr;
    if (GetHGlobalFromStream(st, &hg) != S_OK) { st->Release(); return false; }
    SIZE_T sz = GlobalSize(hg);
    void* p = GlobalLock(hg);
    *outData = (unsigned char*)malloc(sz);
    memcpy(*outData, p, sz);
    *outLen = (size_t)sz;
    GlobalUnlock(hg);
    st->Release();
    return true;
}

static void Put16(unsigned char* p, unsigned v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; }
static void Put32(unsigned char* p, unsigned long v) {
    p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF;
}

int main(int argc, char** argv) {
    const char* outPath = (argc > 1) ? argv[1] : "anx1ous.ico";
    const int sizes[] = { 256, 48, 32, 16 };
    const int nSizes = 4;

    ULONG_PTR tok;
    GdiplusStartupInput gsi;
    if (GdiplusStartup(&tok, &gsi, nullptr) != Ok) { fwprintf(stderr, L"GdiplusStartup failed\n"); return 1; }

    CLSID pngClsid;
    if (GetPngClsid(&pngClsid) != 0) { fwprintf(stderr, L"PNG encoder not found\n"); return 1; }

    unsigned char* blobs[8] = {};
    size_t        lens [8]  = {};

    for (int i = 0; i < nSizes; ++i) {
        int S = sizes[i];
        Bitmap bm(S, S, PixelFormat32bppARGB);
        RenderIcon(&bm, S);
        if (!SavePngMem(&bm, pngClsid, &blobs[i], &lens[i])) {
            fwprintf(stderr, L"PNG save failed for %d\n", S);
            return 1;
        }
        wprintf(L"rendered %d px -> %zu bytes\n", S, lens[i]);
    }

    FILE* f = fopen(outPath, "wb");
    if (!f) { fwprintf(stderr, L"cannot open %hs\n", outPath); return 1; }

    unsigned char hdr[6] = {0,0, 1,0, (unsigned char)nSizes, 0};
    fwrite(hdr, 1, 6, f);

    unsigned long offset = 6 + 16 * nSizes;
    for (int i = 0; i < nSizes; ++i) {
        unsigned char e[16];
        int S = sizes[i];
        e[0]  = (unsigned char)(S >= 256 ? 0 : S);   // width  (0 = 256)
        e[1]  = (unsigned char)(S >= 256 ? 0 : S);   // height (0 = 256)
        e[2]  = 0;                                    // palette colors
        e[3]  = 0;                                    // reserved
        Put16 (e + 4, 1);                             // color planes
        Put16 (e + 6, 32);                            // bits per pixel
        Put32 (e + 8, (unsigned long)lens[i]);        // data size
        Put32 (e +12, offset);                        // data offset
        fwrite(e, 1, 16, f);
        offset += (unsigned long)lens[i];
    }
    for (int i = 0; i < nSizes; ++i) fwrite(blobs[i], 1, lens[i], f);
    fclose(f);

    for (int i = 0; i < nSizes; ++i) free(blobs[i]);
    GdiplusShutdown(tok);
    wprintf(L"written: %hs\n", outPath);
    return 0;
}
