// main.cpp — anx1ous Launcher (Graphite minimal edition)

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#  define UNICODE
#endif
#ifndef _UNICODE
#  define _UNICODE
#endif

// IApplicationActivationManager and GetPackageFamilyName are gated behind Win8 in
// the SDK headers; the launcher targets Windows 10 anyway.
#ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0A00
#endif
#ifndef NTDDI_VERSION
#  define NTDDI_VERSION 0x0A000000
#endif
#include <windows.h>
#include <windowsx.h>
#include <tlhelp32.h>
#include <aclapi.h>
#include <sddl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <urlmon.h>
#include <wininet.h>
#include <wincrypt.h>
#include <appmodel.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shldisp.h>
#include <exdisp.h>
#include <gdiplus.h>
#include <math.h>
#include <wchar.h>
#include <string.h>
#include <stdlib.h>

using namespace Gdiplus;
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "wininet.lib")

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#  define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

#define TIMER_ANIM        1001
#define WM_APP_DONE       (WM_APP + 1)

static const int WIN_W = 480;
static const int WIN_H = 220;

// Minecraft: Windows 10 Edition 1.1.5 — the only build the client supports.
//
// The 1.1.5 package is a re-signed side-by-side build with its own identity, not
// the Store's Microsoft.MinecraftUWP. That is what lets it live next to a modern
// Minecraft: different Identity Name, different publisher hash, separate data
// folder. Installing it therefore never touches the Store copy.
static const wchar_t* MC_PACKAGE = L"Microsoft.Minecraft115";
static const wchar_t* MC_FAMILY  = L"Microsoft.Minecraft115_ekx664bjj63nr";
static const wchar_t* MC_AUMID   = L"Microsoft.Minecraft115_ekx664bjj63nr!App";
static const wchar_t* URL_APPX =
    L"https://github.com/qcountel/anx1ous/releases/download/minecraft/mcpe.appx";
static const wchar_t* URL_CERT =
    L"https://github.com/qcountel/anx1ous/releases/download/minecraft/mcpe.cer";

// Hit-test rects
static const RECT RC_PLAY  = { 150,  96, 330, 140 };
static const RECT RC_CLOSE = { 458,  12, 474,  28 };
static const RECT RC_MIN   = { 440,  12, 456,  28 };

// Globals
static HWND    g_hWnd      = nullptr;
static bool    g_isBusy    = false;
static bool    g_fail      = false;   // last run failed (game not found / exited)
static bool    g_hoverPlay = false;
static bool    g_hoverClose= false;
static bool    g_hoverMin  = false;
static float   g_animPlay  = 0.0f;
static float   g_animClose = 0.0f;
static float   g_animMin   = 0.0f;

// What the worker is doing right now, shown on the status line. Written by the
// worker thread, read by the paint handler — a torn read would only ever show a
// stale caption for one frame, so a lock would buy nothing here.
static wchar_t g_status[128] = L"ready";

static void SetStatus(const wchar_t* text) {
    wcsncpy_s(g_status, text, _TRUNCATE);
    if (g_hWnd) InvalidateRect(g_hWnd, nullptr, FALSE);
}

// ── GDI+ helpers ──────────────────────────────────────────────────────────────
static void AddRoundedRect(GraphicsPath& path, RectF r, float radius) {
    float d = radius * 2.0f;
    path.AddArc(r.X,            r.Y,            d, d, 180.0f, 90.0f);
    path.AddArc(r.X+r.Width-d,  r.Y,            d, d, 270.0f, 90.0f);
    path.AddArc(r.X+r.Width-d,  r.Y+r.Height-d, d, d,   0.0f, 90.0f);
    path.AddArc(r.X,            r.Y+r.Height-d,  d, d,  90.0f, 90.0f);
    path.CloseFigure();
}

static BYTE LerpB(BYTE a, BYTE b, float t) { return (BYTE)(a+(b-a)*t); }
static Color LerpC(Color a, Color b, float t) {
    return Color(LerpB(a.GetA(),b.GetA(),t), LerpB(a.GetR(),b.GetR(),t),
                 LerpB(a.GetG(),b.GetG(),t), LerpB(a.GetB(),b.GetB(),t));
}

// ── Shell out to PowerShell ───────────────────────────────────────────────────
// Package queries and Add-AppxPackage have no practical Win32 equivalent that is
// worth hand-rolling here, so the launcher drives them through PowerShell with
// no window and, where needed, captures stdout.

static bool RunHidden(const wchar_t* args, DWORD timeoutMs, DWORD* exitCode) {
    wchar_t cmd[2048];
    // %ls, not %s: in MinGW the wide printf family treats %s as a *narrow* string
    // (POSIX semantics), so a wchar_t* would stop at the first embedded NUL byte
    // and only the first character would survive.
    swprintf(cmd, 2048, L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass %ls",
             args);

    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags     = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;

    const DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);
    if (exitCode) {
        *exitCode = 1;
        if (wait == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, exitCode);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return wait == WAIT_OBJECT_0;
}

static bool RunCapture(const wchar_t* args, wchar_t* out, size_t outChars, DWORD timeoutMs) {
    if (out && outChars) out[0] = 0;

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    wchar_t cmd[2048];
    swprintf(cmd, 2048, L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass %ls",
             args);

    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput  = wr;
    si.hStdError   = wr;
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        return false;
    }
    CloseHandle(wr);   // the child owns the write end now

    char buf[1024] = {};
    DWORD total = 0, got = 0;
    while (total < sizeof(buf) - 1 &&
           ReadFile(rd, buf + total, (DWORD)(sizeof(buf) - 1 - total), &got, nullptr) && got)
        total += got;
    buf[total] = 0;

    WaitForSingleObject(pi.hProcess, timeoutMs);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);

    if (out && outChars)
        MultiByteToWideChar(CP_UTF8, 0, buf, -1, out, (int)outChars);
    return true;
}

// ── Minecraft 1.1.5 detection ─────────────────────────────────────────────────
// Presence of the side-by-side package is the whole test: its identity already
// pins the build, so there is no version string to compare.
//
// Asked through the packaging API rather than PowerShell. Scraping stdout for a
// keyword is not safe here — an error message that echoes the command would
// contain the keyword too, and the launcher would decide the game was installed
// when it was not: no download, a launch that cannot work, and Explorer falling
// back to opening a folder.
static bool HasMinecraft115() {
    UINT32 count = 0, bufferChars = 0;
    const LONG rc = GetPackagesByPackageFamily(MC_FAMILY, &count, nullptr,
                                               &bufferChars, nullptr);

    // Nothing installed under this family: ERROR_SUCCESS with a count of zero.
    if (rc == ERROR_SUCCESS) return count > 0;
    if (rc != ERROR_INSUFFICIENT_BUFFER) return false;
    return count > 0;
}

// ── Certificate → Trusted Root ────────────────────────────────────────────────
// The appx is signed by a certificate Windows does not know, so the store has to
// trust it before Add-AppxPackage will accept the package.
static bool InstallRootCert(const wchar_t* cerPath) {
    HANDLE f = CreateFileW(cerPath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;

    DWORD size = GetFileSize(f, nullptr);
    if (!size || size > 64 * 1024) { CloseHandle(f); return false; }

    BYTE* blob = (BYTE*)malloc(size);
    DWORD read = 0;
    if (!blob || !ReadFile(f, blob, size, &read, nullptr) || read != size) {
        free(blob); CloseHandle(f); return false;
    }
    CloseHandle(f);

    PCCERT_CONTEXT ctx = CertCreateCertificateContext(
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, blob, read);
    free(blob);
    if (!ctx) return false;

    // LOCAL_MACHINE so every account trusts it; needs the admin rights the
    // manifest already requests.
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                     CERT_SYSTEM_STORE_LOCAL_MACHINE, L"ROOT");
    if (!store) { CertFreeCertificateContext(ctx); return false; }

    const BOOL ok = CertAddCertificateContextToStore(
        store, ctx, CERT_STORE_ADD_REPLACE_EXISTING, nullptr);

    CertCloseStore(store, 0);
    CertFreeCertificateContext(ctx);
    return ok != 0;
}

// ── Download with progress ────────────────────────────────────────────────────
// The appx is ~136 MB; without a percentage the launcher looks hung.
class DownloadProgress : public IBindStatusCallback {
public:
    explicit DownloadProgress(const wchar_t* caption) : m_caption(caption) {}

    // This lives on the stack for the duration of the download, so refcounting
    // is deliberately inert.
    STDMETHOD_(ULONG, AddRef)()  override { return 1; }
    STDMETHOD_(ULONG, Release)() override { return 1; }

    STDMETHOD(QueryInterface)(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IBindStatusCallback) {
            *out = static_cast<IBindStatusCallback*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHOD(OnProgress)(ULONG done, ULONG total, ULONG, LPCWSTR) override {
        if (total) {
            const int percent = (int)((done * 100ULL) / total);
            const int mbDone  = (int)(done  / (1024 * 1024));
            const int mbTotal = (int)(total / (1024 * 1024));
            wchar_t line[128];
            swprintf(line, 128, L"%ls  %d%%  (%d/%d MB)", m_caption, percent, mbDone, mbTotal);
            SetStatus(line);
        }
        return S_OK;
    }

    STDMETHOD(OnStartBinding)(DWORD, IBinding*)              override { return S_OK; }
    STDMETHOD(GetPriority)(LONG*)                            override { return E_NOTIMPL; }
    STDMETHOD(OnLowResource)(DWORD)                          override { return S_OK; }
    STDMETHOD(OnStopBinding)(HRESULT, LPCWSTR)               override { return S_OK; }
    STDMETHOD(GetBindInfo)(DWORD*, BINDINFO*)                override { return S_OK; }
    STDMETHOD(OnDataAvailable)(DWORD, DWORD, FORMATETC*, STGMEDIUM*) override { return S_OK; }
    STDMETHOD(OnObjectAvailable)(REFIID, IUnknown*)          override { return S_OK; }

private:
    const wchar_t* m_caption;
};

static bool Download(const wchar_t* url, const wchar_t* dest, const wchar_t* caption) {
    DeleteFileW(dest);
    DownloadProgress progress(caption);
    return URLDownloadToFileW(nullptr, url, dest, 0, &progress) == S_OK;
}

// ── Install Minecraft 1.1.5 ───────────────────────────────────────────────────
// Downloads the signed package plus its certificate, trusts the certificate and
// hands the package to the deployment service.
static bool InstallMinecraft() {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);

    wchar_t cer[MAX_PATH], appx[MAX_PATH];
    swprintf(cer,  MAX_PATH, L"%lsmcpe.cer",  dir);
    swprintf(appx, MAX_PATH, L"%lsmcpe.appx", dir);

    SetStatus(L"downloading certificate");
    if (!Download(URL_CERT, cer, L"downloading certificate")) {
        SetStatus(L"failed - could not download the certificate");
        return false;
    }

    SetStatus(L"trusting certificate");
    if (!InstallRootCert(cer)) {
        SetStatus(L"failed - could not add the certificate (run as administrator)");
        return false;
    }

    if (!Download(URL_APPX, appx, L"downloading Minecraft")) {
        SetStatus(L"failed - could not download Minecraft");
        return false;
    }

    SetStatus(L"installing Minecraft (this takes a while)");

    // No -ForceUpdateFromAnyVersion on purpose: the package has its own identity,
    // so this is a side-by-side install, never a downgrade. Were the asset ever
    // swapped for one carrying the Store identity, the flag would silently
    // replace the player's modern Minecraft — without it that case just fails.
    wchar_t args[MAX_PATH + 160];
    swprintf(args, MAX_PATH + 160,
             L"-Command \"Add-AppxPackage -Path '%ls' -ForceApplicationShutdown\"", appx);

    DWORD exitCode = 1;
    const bool ran = RunHidden(args, 15 * 60 * 1000, &exitCode);

    DeleteFileW(cer);
    DeleteFileW(appx);

    if (!ran || exitCode != 0) {
        SetStatus(L"failed - Minecraft could not be installed");
        return false;
    }
    if (!HasMinecraft115()) {
        SetStatus(L"failed - installed build is not 1.1.5");
        return false;
    }
    return true;
}

// ── Resolve latest DLL URL from GitHub ────────────────────────────────────────
// Reads a URL into memory. Used for the releases API, which is small JSON.
static bool HttpGetText(const wchar_t* url, char* out, DWORD outBytes) {
    if (!out || outBytes == 0) return false;
    out[0] = 0;

    HINTERNET inet = InternetOpenW(L"anx1ous-launcher/1.0",
                                   INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!inet) return false;

    HINTERNET conn = InternetOpenUrlW(inet, url, nullptr, 0,
                                      INTERNET_FLAG_RELOAD|INTERNET_FLAG_SECURE|
                                      INTERNET_FLAG_NO_CACHE_WRITE|INTERNET_FLAG_NO_UI, 0);
    if (!conn) { InternetCloseHandle(inet); return false; }

    DWORD total = 0, got = 0;
    while (total < outBytes - 1 &&
           InternetReadFile(conn, out + total, outBytes - 1 - total, &got) && got)
        total += got;
    out[total] = 0;

    InternetCloseHandle(conn);
    InternetCloseHandle(inet);
    return total > 0;
}

// Finds the newest release that actually ships anx1ous.dll.
//
// "latest" cannot be trusted: GitHub picks it by publish date across all
// releases, so an unrelated release (the Minecraft package, say) becomes latest
// and the download 404s. Walk the release list in order instead — the API already
// returns newest first — and take the first asset named anx1ous.dll.
static void FetchLatestDllUrl(wchar_t* outUrl, DWORD outLen) {
    static char json[262144];

    if (HttpGetText(L"https://api.github.com/repos/qcountel/anx1ous/releases?per_page=30",
                    json, sizeof(json))) {
        const char* needle = "\"browser_download_url\"";
        for (const char* p = strstr(json, needle); p; p = strstr(p + 1, needle)) {
            const char* colon = strchr(p, ':');
            if (!colon) break;
            const char* open = strchr(colon, '"');
            if (!open) break;
            const char* close = strchr(open + 1, '"');
            if (!close) break;

            const size_t len = (size_t)(close - open - 1);
            if (len == 0 || len >= 512) continue;

            char url[512];
            memcpy(url, open + 1, len);
            url[len] = 0;

            // Assets keep their upload name, so the suffix identifies the DLL.
            const size_t urlLen = strlen(url);
            const char* suffix = "/anx1ous.dll";
            const size_t suffixLen = strlen(suffix);
            if (urlLen >= suffixLen && _stricmp(url + urlLen - suffixLen, suffix) == 0) {
                MultiByteToWideChar(CP_UTF8, 0, url, -1, outUrl, (int)outLen);
                return;
            }
        }
    }

    // Offline or the API refused us: the redirect endpoint is still worth a try.
    swprintf(outUrl,
        L"https://github.com/qcountel/anx1ous/releases/latest/download/anx1ous.dll");
}

// ── Worker thread ─────────────────────────────────────────────────────────────
// Shell activation and URLDownloadToFile both need an apartment on this thread,
// and the worker has several early exits — so tie the lifetime to the scope.
struct ComScope {
    bool owned;
    ComScope() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        owned = SUCCEEDED(hr) && hr != S_FALSE;
    }
    ~ComScope() { if (owned) CoUninitialize(); }
};

static DWORD WINAPI PlayWorker(LPVOID) {
    ComScope com;

    SetStatus(L"checking Minecraft");
    if (HasMinecraft115()) {
        g_isBusy = false;
        SetStatus(L"Minecraft 1.1.5 is already installed");
        PostMessageW(g_hWnd, WM_APP_DONE, 0, 0);
        return 0;
    }

    // InstallMinecraft explains its own failures through the status line.
    if (!InstallMinecraft()) {
        g_isBusy = false;
        PostMessageW(g_hWnd, WM_APP_DONE, 2, 0);
        return 1;
    }

    g_isBusy = false;
    SetStatus(L"installed - Minecraft 1.1.5 is ready");
    PostMessageW(g_hWnd, WM_APP_DONE, 0, 0);
    return 0;
}

static void OnPlay() {
    if (g_isBusy) return;
    g_isBusy = true;
    g_fail   = false;
    SetStatus(L"working");
    HANDLE t = CreateThread(nullptr, 0, PlayWorker, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

// ── Render ────────────────────────────────────────────────────────────────────
static void RenderLauncher(HDC hdcTarget, RECT rcClient) {
    int w = rcClient.right - rcClient.left;
    int h = rcClient.bottom- rcClient.top;

    Bitmap bb(w, h, PixelFormat32bppARGB);
    Graphics g(&bb);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    // ── Background: flat graphite ────────────────────────────────────────────
    SolidBrush bgBr(Color(255, 15, 15, 17));                 // #0F0F11
    g.FillRectangle(&bgBr, 0, 0, w, h);

    // Hairline border
    Pen borderPen(Color(255, 26, 26, 30), 1.0f);             // #1A1A1E
    g.DrawRectangle(&borderPen, 0, 0, w-1, h-1);

    // ── Fonts ────────────────────────────────────────────────────────────────
    Font fTitle (L"Consolas", 12.0f, FontStyleBold,    UnitPixel);
    Font fBtn   (L"Consolas", 12.0f, FontStyleBold,    UnitPixel);
    Font fStat  (L"Consolas", 10.0f, FontStyleRegular, UnitPixel);
    Font fGlyph (L"Consolas", 12.0f, FontStyleBold,    UnitPixel);

    // ── Title: tracked caps ──────────────────────────────────────────────────
    SolidBrush titleBr(Color(255, 138, 138, 142));           // #8A8A8E
    g.DrawString(L"A N X 1 O U S", -1, &fTitle, PointF(20.0f, 14.0f), &titleBr);

    // ── Window controls: bare glyphs, no chrome ──────────────────────────────
    StringFormat gf;
    gf.SetAlignment(StringAlignmentCenter);
    gf.SetLineAlignment(StringAlignmentCenter);

    Color minC   = LerpC(Color(255, 85, 85, 90), Color(255, 220, 220, 224), g_animMin);
    Color closeC = LerpC(Color(255, 85, 85, 90), Color(255, 229, 72, 77),   g_animClose);

    SolidBrush minBr(minC);
    g.DrawString(L"–", -1, &fGlyph, RectF(440.0f, 12.0f, 16.0f, 16.0f), &gf, &minBr);

    SolidBrush closeBr(closeC);
    g.DrawString(L"×", -1, &fGlyph, RectF(458.0f, 12.0f, 16.0f, 16.0f), &gf, &closeBr);

    // ── Play button ───────────────────────────────────────────────────────────
    {
        bool busy = g_isBusy;

        float bx = (float)RC_PLAY.left;
        float by = (float)RC_PLAY.top;
        float bw = (float)(RC_PLAY.right  - RC_PLAY.left);
        float bh = (float)(RC_PLAY.bottom - RC_PLAY.top);

        GraphicsPath btnPath;
        AddRoundedRect(btnPath, RectF(bx+0.5f, by+0.5f, bw-1.0f, bh-1.0f), 4.0f);
        SolidBrush btnBr(busy ? Color(255, 18, 18, 21)      // #121215
                              : Color(255, 26, 26, 29));     // #1A1A1D
        g.FillPath(&btnBr, &btnPath);
        Pen btnPen(Color(busy ? 200 : 255, 42, 42, 46), 1.0f);   // #2A2A2E
        g.DrawPath(&btnPen, &btnPath);

        // Hover: crimson underline grows from the center
        if (!busy && g_animPlay > 0.01f) {
            float lw = (bw - 28.0f) * g_animPlay;
            Pen acc(Color((BYTE)(200.0f * g_animPlay), 229, 72, 77), 2.0f);  // #E5484D
            g.DrawLine(&acc,
                bx + (bw - lw) * 0.5f, by + bh - 6.0f,
                bx + (bw + lw) * 0.5f, by + bh - 6.0f);
        }

        StringFormat sfC;
        sfC.SetAlignment(StringAlignmentCenter);
        sfC.SetLineAlignment(StringAlignmentCenter);

        if (busy) {
            DWORD tick = GetTickCount();
            for (int i = 0; i < 3; ++i) {
                bool on = ((tick / 300 + i) % 3) == 0;
                SolidBrush dotBr(on ? Color(255, 154, 154, 160)   // #9A9AA0
                                    : Color(255, 58, 58, 64));    // #3A3A40
                float cx2 = bx + bw * 0.5f + (i - 1) * 14.0f;
                float cy2 = by + bh * 0.5f;
                g.FillEllipse(&dotBr, cx2 - 4.0f, cy2 - 4.0f, 8.0f, 8.0f);
            }
        } else {
            Color txtC = LerpC(Color(255, 170, 170, 176), Color(255, 240, 240, 244), g_animPlay);
            SolidBrush btnTxt(txtC);
            g.DrawString(L"I N S T A L L", -1, &fBtn,
                RectF(bx, by, bw, bh), &sfC, &btnTxt);
        }
    }

    // ── Status line, bottom-left ──────────────────────────────────────────────
    {
        const wchar_t* txt = g_status;
        Color dotC, txtC;
        if (g_isBusy) {
            BYTE pulse = (BYTE)(128.0f + 120.0f * sinf(GetTickCount() / 260.0f));
            dotC = Color(pulse, 229, 72, 77);                 // #E5484D
            txtC = Color(255, 138, 138, 142);                 // #8A8A8E
        } else if (g_fail) {
            dotC = Color(255, 229, 72, 77);
            txtC = Color(255, 190, 110, 116);
        } else {
            dotC = Color(255, 74, 74, 78);                    // #4A4A4E
            txtC = Color(255, 85, 85, 90);                    // #55555A
        }
        SolidBrush dotBr(dotC);
        g.FillEllipse(&dotBr, 20.0f, 193.0f, 5.0f, 5.0f);
        SolidBrush stBr(txtC);
        g.DrawString(txt, -1, &fStat, PointF(32.0f, 189.0f), &stBr);
    }

    // ── Blit ─────────────────────────────────────────────────────────────────
    Graphics gScreen(hdcTarget);
    gScreen.DrawImage(&bb, 0, 0);
}

// ── WndProc ───────────────────────────────────────────────────────────────────
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_hWnd = hwnd;
        SetTimer(hwnd, TIMER_ANIM, 16, nullptr);
        return 0;

    case WM_TIMER:
        if (wp == TIMER_ANIM) {
            bool dirty = g_isBusy; // repaint for spinner when busy
            auto step = [&](float& cur, float tgt) {
                if (fabsf(cur-tgt) > 0.001f) { cur += (tgt-cur)*0.14f; dirty=true; }
                else cur = tgt;
            };
            step(g_animPlay,  g_hoverPlay && !g_isBusy ? 1.0f : 0.0f);
            step(g_animClose, g_hoverClose ? 1.0f : 0.0f);
            step(g_animMin,   g_hoverMin   ? 1.0f : 0.0f);
            if (dirty) InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_NCHITTEST: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        if (PtInRect(&RC_PLAY,pt)||PtInRect(&RC_CLOSE,pt)||PtInRect(&RC_MIN,pt))
            return HTCLIENT;
        if (pt.y < 40) return HTCAPTION;
        return HTCLIENT;
    }

    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        g_hoverPlay  = PtInRect(&RC_PLAY,pt)  && !g_isBusy;
        g_hoverClose = PtInRect(&RC_CLOSE,pt);
        g_hoverMin   = PtInRect(&RC_MIN,pt);
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSELEAVE:
        g_hoverPlay = g_hoverClose = g_hoverMin = false;
        return 0;

    case WM_LBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if      (PtInRect(&RC_CLOSE,pt)) PostQuitMessage(0);
        else if (PtInRect(&RC_MIN,pt))   ShowWindow(hwnd, SW_MINIMIZE);
        else if (PtInRect(&RC_PLAY,pt))  OnPlay();
        return 0;
    }

    case WM_APP_DONE:
        // 0 = done, 1 = generic failure, 2 = failure the worker already described
        g_fail = (wp != 0);
        if (wp == 1) SetStatus(L"failed - game not found or exited");
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        RenderLauncher(hdc, rc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND: return 1;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_ANIM);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── Entry ─────────────────────────────────────────────────────────────────────
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    // Single-instance guard (named mutex — blocks second launch)
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"anx1ousLauncherSingletonMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Find existing window and bring to front
        HWND hExist = FindWindowW(L"anx1ousGdiPlusWnd", nullptr);
        if (hExist) {
            ShowWindow(hExist, SW_RESTORE);
            SetForegroundWindow(hExist);
        }
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    ULONG_PTR gdiplusToken;
    GdiplusStartupInput gsi;
    GdiplusStartup(&gdiplusToken, &gsi, nullptr);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style         = CS_HREDRAW|CS_VREDRAW|CS_DROPSHADOW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hi;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"anx1ousGdiPlusWnd";
    wc.hIcon   = LoadIconW(hi, MAKEINTRESOURCEW(1));
    wc.hIconSm = LoadIconW(hi, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW,
        L"anx1ousGdiPlusWnd", L"anx1ous Launcher",
        WS_POPUP|WS_VISIBLE|WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, WIN_W, WIN_H,
        nullptr, nullptr, hi, nullptr);

    DWORD cp = 2;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cp, sizeof(cp));

    RECT rcWork;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);
    int cx = (rcWork.right -rcWork.left - WIN_W)/2;
    int cy = (rcWork.bottom-rcWork.top  - WIN_H)/2;
    SetWindowPos(hwnd, nullptr, cx, cy, WIN_W, WIN_H, SWP_NOZORDER|SWP_SHOWWINDOW);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m); DispatchMessageW(&m);
    }

    GdiplusShutdown(gdiplusToken);
    ReleaseMutex(hMutex); CloseHandle(hMutex);
    return (int)m.wParam;
}
