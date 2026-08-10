// Zdisplay installer: the user interface.
//
// A single frameless window with no Windows controls. Shapes are rasterized
// into a 32-bit buffer with distance-field antialiasing; text is drawn with GDI
// afterwards.
//
// Standard controls paint themselves from the Windows visual theme, so
// darkening them requires a different path per Windows version. Drawing the six
// clickable regions directly keeps the appearance identical everywhere and
// allows true rounded corners.
#include "setup.h"

#include <math.h>
#include <stdint.h>
#include <windowsx.h>
#include <shellapi.h>
#include <objbase.h>
#include <vector>

namespace {

using namespace setup;

// Palette
// Same values as src/ui_theme.cpp, so the installer and the application share a
// single appearance.
const COLORREF kBase       = RGB(24, 24, 27);
const COLORREF kSurface    = RGB(34, 34, 39);
const COLORREF kSurfaceAlt = RGB(45, 45, 51);
const COLORREF kHover      = RGB(56, 56, 63);
const COLORREF kLine       = RGB(64, 64, 72);
const COLORREF kText       = RGB(233, 233, 238);
const COLORREF kTextDim    = RGB(150, 150, 160);
const COLORREF kAccent     = RGB(214, 45, 50);
const COLORREF kAccentHot  = RGB(240, 74, 79);
const COLORREF kAccentDim  = RGB(132, 28, 32);

// Canvas
// Rounded rectangles and circles come from a signed distance field: the
// distance to the edge becomes 0..1 coverage in the outermost pixel.
struct Canvas {
    int w = 0, h = 0;
    uint32_t* px = nullptr;

    inline void Blend(int x, int y, COLORREF c, float a) {
        if (a <= 0.0f || x < 0 || y < 0 || x >= w || y >= h) return;
        if (a > 1.0f) a = 1.0f;
        uint32_t& d = px[y * w + x];
        const int dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
        const int sr = GetRValue(c), sg = GetGValue(c), sb = GetBValue(c);
        const int r = dr + (int)((sr - dr) * a + 0.5f);
        const int g = dg + (int)((sg - dg) * a + 0.5f);
        const int b = db + (int)((sb - db) * a + 0.5f);
        d = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }

    void Clear(COLORREF c) {
        const uint32_t v = ((uint32_t)GetRValue(c) << 16) | ((uint32_t)GetGValue(c) << 8) | GetBValue(c);
        for (int i = 0, n = w * h; i < n; ++i) px[i] = v;
    }

    /// Coverage of a rounded rectangle at pixel (x,y).
    static float RoundCoverage(float px_, float py_, float cx, float cy,
                               float halfW, float halfH, float r) {
        if (r > halfW) r = halfW;
        if (r > halfH) r = halfH;
        float dx = fabsf(px_ - cx) - (halfW - r);
        float dy = fabsf(py_ - cy) - (halfH - r);
        if (dx < 0) dx = 0;
        if (dy < 0) dy = 0;
        const float d = sqrtf(dx * dx + dy * dy) - r;
        const float cov = 0.5f - d;
        return cov < 0 ? 0.0f : (cov > 1 ? 1.0f : cov);
    }

    void FillRound(float x, float y, float ww, float hh, float r, COLORREF c, float alpha = 1.0f) {
        const float cx = x + ww * 0.5f, cy = y + hh * 0.5f;
        const int x0 = (int)floorf(x) - 1, x1 = (int)ceilf(x + ww) + 1;
        const int y0 = (int)floorf(y) - 1, y1 = (int)ceilf(y + hh) + 1;
        for (int py_ = y0; py_ <= y1; ++py_)
            for (int pxi = x0; pxi <= x1; ++pxi) {
                const float cov = RoundCoverage(pxi + 0.5f, py_ + 0.5f, cx, cy,
                                                ww * 0.5f, hh * 0.5f, r);
                if (cov > 0) Blend(pxi, py_, c, cov * alpha);
            }
    }

    /// One-pixel outline: outer coverage minus inner coverage.
    void StrokeRound(float x, float y, float ww, float hh, float r, COLORREF c,
                     float thick = 1.0f, float alpha = 1.0f) {
        const float cx = x + ww * 0.5f, cy = y + hh * 0.5f;
        const int x0 = (int)floorf(x) - 1, x1 = (int)ceilf(x + ww) + 1;
        const int y0 = (int)floorf(y) - 1, y1 = (int)ceilf(y + hh) + 1;
        for (int py_ = y0; py_ <= y1; ++py_)
            for (int pxi = x0; pxi <= x1; ++pxi) {
                const float outer = RoundCoverage(pxi + 0.5f, py_ + 0.5f, cx, cy,
                                                  ww * 0.5f, hh * 0.5f, r);
                const float inner = RoundCoverage(pxi + 0.5f, py_ + 0.5f, cx, cy,
                                                  ww * 0.5f - thick, hh * 0.5f - thick,
                                                  r - thick > 0 ? r - thick : 0);
                const float cov = outer - inner;
                if (cov > 0) Blend(pxi, py_, c, cov * alpha);
            }
    }

    void FillCircle(float cx, float cy, float r, COLORREF c, float alpha = 1.0f) {
        FillRound(cx - r, cy - r, r * 2, r * 2, r, c, alpha);
    }

    /// Segment with rounded caps, used for the close glyph and the checkbox
    /// tick, which must stay crisp at any DPI.
    void Line(float ax, float ay, float bx, float by, float thick, COLORREF c, float alpha = 1.0f) {
        const float half = thick * 0.5f;
        const int x0 = (int)floorf((ax < bx ? ax : bx) - half - 1);
        const int x1 = (int)ceilf((ax > bx ? ax : bx) + half + 1);
        const int y0 = (int)floorf((ay < by ? ay : by) - half - 1);
        const int y1 = (int)ceilf((ay > by ? ay : by) + half + 1);
        const float vx = bx - ax, vy = by - ay;
        const float len2 = vx * vx + vy * vy;
        for (int py_ = y0; py_ <= y1; ++py_)
            for (int pxi = x0; pxi <= x1; ++pxi) {
                const float qx = pxi + 0.5f - ax, qy = py_ + 0.5f - ay;
                float t = len2 > 0 ? (qx * vx + qy * vy) / len2 : 0.0f;
                if (t < 0) t = 0;
                if (t > 1) t = 1;
                const float dx = qx - vx * t, dy = qy - vy * t;
                const float cov = 0.5f - (sqrtf(dx * dx + dy * dy) - half);
                if (cov > 0) Blend(pxi, py_, c, (cov > 1 ? 1.0f : cov) * alpha);
            }
    }

    /// Diffuse glow made of several growing outlines at low opacity: cheaper
    /// than a real blur and indistinguishable from one on a dark background.
    void Glow(float x, float y, float ww, float hh, float r, COLORREF c, float strength, int spread) {
        for (int i = spread; i >= 1; --i) {
            const float f = (float)i / spread;
            FillRound(x - i, y - i * 0.6f + 1, ww + i * 2, hh + i * 1.2f, r + i,
                      c, strength * (1.0f - f) * 0.10f);
        }
    }

    void VGradient(int x, int y, int ww, int hh, COLORREF top, COLORREF bottom) {
        for (int i = 0; i < hh; ++i) {
            const float t = hh > 1 ? (float)i / (hh - 1) : 0.0f;
            const int r = (int)(GetRValue(top) + (GetRValue(bottom) - GetRValue(top)) * t);
            const int g = (int)(GetGValue(top) + (GetGValue(bottom) - GetGValue(top)) * t);
            const int b = (int)(GetBValue(top) + (GetBValue(bottom) - GetBValue(top)) * t);
            for (int j = 0; j < ww; ++j) Blend(x + j, y + i, RGB(r, g, b), 1.0f);
        }
    }

    /// Header halo that falls off along both axes: strongest near the mark at
    /// the top left, gone before the window body, never a solid band.
    void HeaderGradient(int x, int y, int ww, int hh, COLORREF accent) {
        for (int i = 0; i < hh; ++i) {
            const float fy = hh > 1 ? (float)i / (hh - 1) : 0.0f;
            // Inverted smoothstep: no step at the top, no edge at the bottom.
            const float v = 1.0f - fy * fy * (3.0f - 2.0f * fy);
            for (int j = 0; j < ww; ++j) {
                const float fx = ww > 1 ? (float)j / (ww - 1) : 0.0f;
                const float dx = fx - 0.22f;
                const float focus = 1.0f / (1.0f + dx * dx * 4.5f);
                Blend(x + j, y + i, accent, v * (0.18f + focus * 0.20f));
            }
        }
    }
};

// Geometry
// Measurements are in logical pixels (96 dpi); Scale() converts to device
// pixels. The height must clear four checkboxes: RCheck(3) ends at 352 and the
// primary button starts at kWinH-78, so anything below 430 overlaps.
const int kWinW = 560, kWinH = 462;

struct Rect4 { float x, y, w, h; };

bool Inside(const Rect4& r, int px_, int py_) {
    return px_ >= r.x && px_ < r.x + r.w && py_ >= r.y && py_ < r.y + r.h;
}

// `Count` closes the list and sizes the animation arrays, so the loops in
// Animate() cover every hit region without a hand-written bound.
enum class Hit { None, Close, Browse, Check0, Check1, Check2, Check3, Primary, Count };

/// Largest number of checkboxes on any screen (the install screen).
const int kMaxChecks = 4;
enum class Stage { Ask, Working, Done, Failed };

// State

struct App {
    HWND wnd = nullptr;
    HINSTANCE inst = nullptr;
    int dpi = 96;

    bool uninstallMode = false;
    bool upgrade = false;          ///< an installation already exists on this machine
    Options opt;
    Stage stage = Stage::Ask;

    // Work runs on another thread; the interface only reads what it publishes.
    HANDLE worker = nullptr;
    CRITICAL_SECTION lock;
    int  workPercent = 0;
    wchar_t workStep[128] = L"";
    Result result;

    float shownPercent = 0.0f;
    float hover[(int)Hit::Count] = {};  ///< animated opacity of each region
    float checkAnim[kMaxChecks] = {};
    float stageFade = 0.0f;
    Hit hot = Hit::None;
    Hit pressed = Hit::None;

    // Drawing buffer
    HDC memDC = nullptr;
    HBITMAP bmp = nullptr, oldBmp = nullptr;
    Canvas canvas;
    int bmpW = 0, bmpH = 0;

    HFONT fTitle = nullptr, fBody = nullptr, fSmall = nullptr, fLabel = nullptr, fButton = nullptr;
    HICON icon = nullptr;

    int Scale(int v) const { return ::MulDiv(v, dpi, 96); }
    float ScaleF(float v) const { return v * dpi / 96.0f; }
};

App g;

const UINT WM_WORK_PROGRESS = WM_APP + 1;
const UINT WM_WORK_DONE     = WM_APP + 2;

void HardenCurrentProcess() {
    // DEP and ASLR come from the PE header; these policies close image-load
    // paths a self-contained installer does not need. They are best-effort so
    // that older Windows versions still run.
    ::HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);
    ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);

    typedef BOOL (WINAPI *PfnSetMitigation)(PROCESS_MITIGATION_POLICY, PVOID, SIZE_T);
    HMODULE kernel = ::GetModuleHandleW(L"kernel32.dll");
    PfnSetMitigation set = kernel
        ? (PfnSetMitigation)(void*)::GetProcAddress(kernel, "SetProcessMitigationPolicy")
        : nullptr;
    if (set) {
        PROCESS_MITIGATION_IMAGE_LOAD_POLICY image = {};
        image.NoRemoteImages = 1;
        image.NoLowMandatoryLabelImages = 1;
        image.PreferSystem32Images = 1;
        set(ProcessImageLoadPolicy, &image, sizeof(image));
    }
}

// Fonts

HFONT MakeFont(int px, int weight) {
    LOGFONTW lf = {};
    lf.lfHeight = -g.Scale(px);
    lf.lfWeight = weight;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfCharSet = DEFAULT_CHARSET;
    ::lstrcpynW(lf.lfFaceName, L"Segoe UI", LF_FACESIZE);
    return ::CreateFontIndirectW(&lf);
}

void ReleaseFonts() {
    HFONT* all[] = {&g.fTitle, &g.fBody, &g.fSmall, &g.fLabel, &g.fButton};
    for (HFONT* f : all) {
        if (*f) { ::DeleteObject(*f); *f = nullptr; }
    }
}

void BuildFonts() {
    ReleaseFonts();
    g.fTitle  = MakeFont(25, FW_SEMIBOLD);
    g.fBody   = MakeFont(14, FW_NORMAL);
    g.fSmall  = MakeFont(12, FW_NORMAL);
    g.fLabel  = MakeFont(11, FW_SEMIBOLD);
    g.fButton = MakeFont(15, FW_SEMIBOLD);
}

// Layout

Rect4 RClose()  { return {kWinW - 46.0f, 14.0f, 32.0f, 32.0f}; }
Rect4 RPath()   { return {32.0f, 146.0f, kWinW - 64.0f, 44.0f}; }
Rect4 RBrowse() { const Rect4 p = RPath(); return {p.x + p.w - 8 - 86, p.y + 7, 86, 30}; }
Rect4 RCheck(int i) { return {32.0f, 212.0f + i * 38.0f, (float)kWinW - 64.0f, 26.0f}; }
Rect4 RCheckBox(int i) { const Rect4 r = RCheck(i); return {r.x, r.y + 3, 20, 20}; }
Rect4 RPrimary() { return {kWinW - 32.0f - 156.0f, kWinH - 32.0f - 46.0f, 156.0f, 46.0f}; }

int CheckCount() { return g.uninstallMode ? 1 : 4; }

const wchar_t* CheckLabel(int i) {
    if (g.uninstallMode) return L"Apagar também as configurações e os perfis";
    switch (i) {
        case 0: return L"Iniciar junto com o Windows";
        case 1: return L"Criar atalho na área de trabalho";
        case 2: return L"Abrir o Zdisplay ao terminar";
        // The label states both the cost and the benefit: without the full
        // range Windows silently dilutes the ramp and only about half of the
        // shadow adjustment reaches the display.
        default: return L"Liberar a faixa completa de gama — pede administrador";
    }
}

bool* CheckValue(int i) {
    if (g.uninstallMode) return &g.opt.removeSettings;
    switch (i) {
        case 0: return &g.opt.autostart;
        case 1: return &g.opt.desktopIcon;
        case 2: return &g.opt.launchAfter;
        default: return &g.opt.fullGammaRange;
    }
}

Hit HitTest(int px_, int py_) {
    // Hit testing happens in logical pixels, so undo the scale first.
    const float fx = px_ * 96.0f / g.dpi, fy = py_ * 96.0f / g.dpi;
    if (Inside(RClose(), (int)fx, (int)fy)) return Hit::Close;
    if (g.stage != Stage::Ask) {
        if (Inside(RPrimary(), (int)fx, (int)fy) && g.stage != Stage::Working) return Hit::Primary;
        return Hit::None;
    }
    if (Inside(RBrowse(), (int)fx, (int)fy)) return Hit::Browse;
    if (Inside(RPath(), (int)fx, (int)fy)) return Hit::Browse;
    for (int i = 0; i < CheckCount(); ++i)
        if (Inside(RCheck(i), (int)fx, (int)fy)) return (Hit)((int)Hit::Check0 + i);
    if (Inside(RPrimary(), (int)fx, (int)fy)) return Hit::Primary;
    return Hit::None;
}

// Painting

void EnsureBuffer(int w, int h) {
    if (g.memDC && g.bmpW == w && g.bmpH == h) return;
    if (g.memDC) {
        ::SelectObject(g.memDC, g.oldBmp);
        ::DeleteObject(g.bmp);
        ::DeleteDC(g.memDC);
        g.memDC = nullptr;
        g.bmp = nullptr;
    }
    HDC screen = ::GetDC(nullptr);
    g.memDC = ::CreateCompatibleDC(screen);
    ::ReleaseDC(nullptr, screen);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    g.bmp = ::CreateDIBSection(g.memDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    g.oldBmp = (HBITMAP)::SelectObject(g.memDC, g.bmp);
    g.canvas.w = w;
    g.canvas.h = h;
    g.canvas.px = static_cast<uint32_t*>(bits);
    g.bmpW = w;
    g.bmpH = h;
}

void TextAt(HFONT font, COLORREF color, const wchar_t* s, Rect4 r, UINT format, int tracking = 0) {
    ::SelectObject(g.memDC, font);
    ::SetTextColor(g.memDC, color);
    ::SetBkMode(g.memDC, TRANSPARENT);
    ::SetTextCharacterExtra(g.memDC, g.Scale(tracking));
    RECT rc = {g.Scale((int)r.x), g.Scale((int)r.y),
               g.Scale((int)(r.x + r.w)), g.Scale((int)(r.y + r.h))};
    ::DrawTextW(g.memDC, s, -1, &rc, format);
    ::SetTextCharacterExtra(g.memDC, 0);
}

COLORREF Mix(COLORREF a, COLORREF b, float t) {
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return RGB((int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t),
               (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t),
               (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t));
}

/// Converts a logical rectangle to the scaled canvas.
Rect4 S(const Rect4& r) {
    return {g.ScaleF(r.x), g.ScaleF(r.y), g.ScaleF(r.w), g.ScaleF(r.h)};
}

void DrawCheckMark(Canvas& c, Rect4 box, COLORREF color, float alpha) {
    const float x = box.x, y = box.y, w = box.w, h = box.h;
    const float t = g.ScaleF(2.2f);
    // Two strokes: the first descends to the vertex, the second rises.
    c.Line(x + w * 0.24f, y + h * 0.52f, x + w * 0.43f, y + h * 0.71f, t, color, alpha);
    c.Line(x + w * 0.43f, y + h * 0.71f, x + w * 0.77f, y + h * 0.30f, t, color, alpha);
}

void PaintAskBody(Canvas& c) {
    // Install folder
    const Rect4 path = S(RPath());
    c.FillRound(path.x, path.y, path.w, path.h, g.ScaleF(11), kSurfaceAlt);
    c.StrokeRound(path.x, path.y, path.w, path.h, g.ScaleF(11), kLine, g.ScaleF(1));

    const Rect4 br = S(RBrowse());
    const float bh = g.hover[(int)Hit::Browse];
    c.FillRound(br.x, br.y, br.w, br.h, g.ScaleF(8), Mix(kHover, kAccent, bh * 0.85f));

    // Checkboxes
    for (int i = 0; i < CheckCount(); ++i) {
        const Rect4 box = S(RCheckBox(i));
        const float on = g.checkAnim[i];
        const float hv = g.hover[(int)Hit::Check0 + i];
        const float r = g.ScaleF(6);
        if (on > 0.01f) {
            c.FillRound(box.x, box.y, box.w, box.h, r, Mix(kSurfaceAlt, kAccent, on));
            if (hv > 0.01f)
                c.FillRound(box.x, box.y, box.w, box.h, r, kAccentHot, hv * 0.35f * on);
        } else {
            c.FillRound(box.x, box.y, box.w, box.h, r, Mix(kSurfaceAlt, kHover, hv));
        }
        c.StrokeRound(box.x, box.y, box.w, box.h, r,
                      Mix(kLine, kAccent, on), g.ScaleF(1), 1.0f - on * 0.4f);
        if (on > 0.05f) DrawCheckMark(c, box, RGB(255, 255, 255), on);
    }
}

void PaintProgress(Canvas& c) {
    const float x = g.ScaleF(32), w = g.ScaleF(kWinW - 64);
    const float y = g.ScaleF(252), h = g.ScaleF(6);
    c.FillRound(x, y, w, h, h * 0.5f, kSurfaceAlt);
    const float filled = w * (g.shownPercent / 100.0f);
    if (filled > 1) {
        c.Glow(x, y, filled, h, h * 0.5f, kAccent, 1.0f, g.Scale(10));
        c.FillRound(x, y, filled, h, h * 0.5f, kAccent);
        // The lighter tip suggests motion without animating a texture.
        c.FillRound(x + filled - h * 2, y, h * 2, h, h * 0.5f, kAccentHot, 0.9f);
    }
}

void PaintDoneMark(Canvas& c, bool ok) {
    const float cx = g.ScaleF(kWinW * 0.5f), cy = g.ScaleF(196);
    const float r = g.ScaleF(34);
    const COLORREF color = ok ? kAccent : kAccentDim;
    c.Glow(cx - r, cy - r, r * 2, r * 2, r, color, 1.6f, g.Scale(16));
    c.FillCircle(cx, cy, r, color);
    if (ok) {
        DrawCheckMark(c, {cx - r, cy - r, r * 2, r * 2}, RGB(255, 255, 255), 1.0f);
    } else {
        const float t = g.ScaleF(4);
        c.Line(cx, cy - r * 0.42f, cx, cy + r * 0.12f, t, RGB(255, 255, 255));
        c.FillCircle(cx, cy + r * 0.38f, t * 0.62f, RGB(255, 255, 255));
    }
}

void Paint(HDC dc, const RECT& client) {
    const int w = client.right - client.left, h = client.bottom - client.top;
    EnsureBuffer(w, h);
    ::GdiFlush();  // text from the previous frame may still be queued

    Canvas& c = g.canvas;
    c.Clear(kBase);

    // The red accent starts at the top and dissolves into the background in a
    // single pass, which keeps the title readable instead of laying down a
    // near-solid band.
    c.HeaderGradient(0, 0, w, g.Scale(104), kAccent);
    c.FillRound(0, 0, (float)w, g.ScaleF(2), 0, kAccentDim, 0.82f);
    c.FillRound(0, g.ScaleF(2), (float)w, g.ScaleF(1), 0, kAccentHot, 0.20f);

    // Close button
    {
        const Rect4 r = S(RClose());
        const float hv = g.hover[(int)Hit::Close];
        if (hv > 0.01f)
            c.FillRound(r.x, r.y, r.w, r.h, g.ScaleF(9), kAccent, hv);
        const float m = r.w * 0.34f;
        const COLORREF col = Mix(kTextDim, RGB(255, 255, 255), hv);
        c.Line(r.x + m, r.y + m, r.x + r.w - m, r.y + r.h - m, g.ScaleF(1.7f), col);
        c.Line(r.x + r.w - m, r.y + m, r.x + m, r.y + r.h - m, g.ScaleF(1.7f), col);
    }

    if (g.stage == Stage::Ask) PaintAskBody(c);
    else if (g.stage == Stage::Working) PaintProgress(c);
    else PaintDoneMark(c, g.stage == Stage::Done);

    // Primary button
    const bool showPrimary = (g.stage != Stage::Working);
    if (showPrimary) {
        const Rect4 b = S(RPrimary());
        const float hv = g.hover[(int)Hit::Primary];
        const bool down = (g.pressed == Hit::Primary && g.hot == Hit::Primary);
        COLORREF fill = Mix(kAccent, kAccentHot, hv);
        if (down) fill = kAccentDim;
        c.Glow(b.x, b.y, b.w, b.h, g.ScaleF(11), kAccent, 1.0f + hv, g.Scale(14));
        c.FillRound(b.x, b.y, b.w, b.h, g.ScaleF(11), fill);
    }

    // Text
    // GDI draws after the pixels, so nothing needs synchronizing in between.
    if (g.icon) {
        const int s = g.Scale(46);
        ::DrawIconEx(g.memDC, g.Scale(32), g.Scale(28), g.icon, s, s, 0, nullptr, DI_NORMAL);
    }

    const wchar_t* title = g.uninstallMode ? L"Desinstalar o Zdisplay"
                                           : (g.upgrade ? L"Atualizar o Zdisplay" : L"Zdisplay");
    TextAt(g.fTitle, kText, title, {88, 26, kWinW - 140.0f, 34}, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

    {
        std::wstring sub = L"Brilho, cor e saturação  ·  versão ";
        sub += kVersionStr;
        TextAt(g.fSmall, kTextDim, sub.c_str(), {90, 62, kWinW - 140.0f, 22},
               DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    }

    if (g.stage == Stage::Ask) {
        TextAt(g.fLabel, kTextDim,
               g.uninstallMode ? L"SERÁ REMOVIDO DE" : L"PASTA DE INSTALAÇÃO",
               {34, 122, kWinW - 68.0f, 18}, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX, 1);

        const Rect4 p = RPath();
        TextAt(g.fBody, kText, g.opt.dir.c_str(),
               {p.x + 14, p.y, p.w - 118, p.h},
               DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS | DT_NOPREFIX);

        if (!g.uninstallMode) {
            const Rect4 br = RBrowse();
            TextAt(g.fSmall, kText, L"Alterar", br,
                   DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        }

        for (int i = 0; i < CheckCount(); ++i) {
            const Rect4 r = RCheck(i);
            TextAt(g.fBody, *CheckValue(i) ? kText : kTextDim, CheckLabel(i),
                   {r.x + 34, r.y, r.w - 34, r.h},
                   DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        }

        std::wstring foot = L"Integridade SHA-256  ·  sem administrador";
        const DWORD bytes = PayloadSize();
        if (bytes) {
            wchar_t mb[48];
            ::wsprintfW(mb, L"  ·  %lu,%lu MB", bytes / (1024 * 1024),
                        (bytes % (1024 * 1024)) * 10 / (1024 * 1024));
            foot += mb;
        }
        TextAt(g.fSmall, kTextDim, foot.c_str(),
               {32, RPrimary().y, kWinW - 96.0f - RPrimary().w, RPrimary().h},
               DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    } else if (g.stage == Stage::Working) {
        wchar_t step[128];
        ::EnterCriticalSection(&g.lock);
        ::lstrcpynW(step, g.workStep, 128);
        ::LeaveCriticalSection(&g.lock);
        TextAt(g.fBody, kText, step, {32, 206, kWinW - 64.0f, 26},
               DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);

        wchar_t pct[16];
        ::wsprintfW(pct, L"%d%%", (int)(g.shownPercent + 0.5f));
        TextAt(g.fSmall, kTextDim, pct, {32, 272, kWinW - 64.0f, 22},
               DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else {
        const bool ok = (g.stage == Stage::Done);
        const wchar_t* head = ok ? (g.uninstallMode ? L"Zdisplay removido" : L"Zdisplay instalado")
                                 : L"Não deu certo";
        TextAt(g.fTitle, kText, head, {32, 250, kWinW - 64.0f, 34},
               DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);

        std::wstring detail;
        if (ok && !g.uninstallMode)
            detail = g.opt.launchAfter
                         ? L"Ele já está rodando na bandeja, ao lado do relógio."
                         : L"Procure por Zdisplay no menu Iniciar.";
        else if (ok)
            detail = L"Os arquivos e os atalhos foram apagados.";
        else
            detail = g.result.message;

        TextAt(g.fSmall, kTextDim, detail.c_str(), {56, 288, kWinW - 112.0f, 56},
               DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
    }

    if (showPrimary) {
        const wchar_t* label;
        if (g.stage == Stage::Ask)
            label = g.uninstallMode ? L"Desinstalar" : (g.upgrade ? L"Atualizar" : L"Instalar");
        else if (g.stage == Stage::Done)
            label = L"Concluir";
        else
            label = L"Fechar";
        TextAt(g.fButton, RGB(255, 255, 255), label, RPrimary(),
               DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }

    ::GdiFlush();
    ::BitBlt(dc, 0, 0, w, h, g.memDC, 0, 0, SRCCOPY);
}

// Worker

void CALLBACK OnProgress(void* ctx, int percent, const wchar_t* step) {
    ::EnterCriticalSection(&g.lock);
    g.workPercent = percent;
    ::lstrcpynW(g.workStep, step ? step : L"", 128);
    ::LeaveCriticalSection(&g.lock);
    if (g.wnd) ::PostMessageW(g.wnd, WM_WORK_PROGRESS, 0, 0);
}

DWORD WINAPI WorkerMain(LPVOID) {
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const Result r = g.uninstallMode ? Uninstall(g.opt, OnProgress, nullptr)
                                     : Install(g.opt, OnProgress, nullptr);
    ::CoUninitialize();
    ::EnterCriticalSection(&g.lock);
    g.result = r;
    g.workPercent = 100;
    ::LeaveCriticalSection(&g.lock);
    if (g.wnd) ::PostMessageW(g.wnd, WM_WORK_DONE, 0, 0);
    return 0;
}

void StartWork() {
    g.stage = Stage::Working;
    g.shownPercent = 0.0f;
    ::EnterCriticalSection(&g.lock);
    g.workPercent = 0;
    ::lstrcpynW(g.workStep, L"Preparando...", 128);
    ::LeaveCriticalSection(&g.lock);
    g.worker = ::CreateThread(nullptr, 0, WorkerMain, nullptr, 0, nullptr);
    if (!g.worker) {
        g.result.ok = false;
        g.result.message = L"Não consegui iniciar a instalação.";
        g.stage = Stage::Failed;
    }
    ::InvalidateRect(g.wnd, nullptr, FALSE);
}

// Window

void ApplyRoundedFrame(HWND wnd) {
    // Windows 11 rounds the frame through DWM and keeps the correct shadow.
    // Earlier versions fall back to a manual window region: hard-edged, but
    // still rounded.
    typedef HRESULT (WINAPI *PfnDwmSet)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE dwm = ::LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    bool done = false;
    if (dwm) {
        PfnDwmSet set = (PfnDwmSet)(void*)::GetProcAddress(dwm, "DwmSetWindowAttribute");
        if (set) {
            const DWORD kCornerPreference = 33, kRound = 2;
            done = SUCCEEDED(set(wnd, kCornerPreference, &kRound, sizeof(kRound)));
        }
        ::FreeLibrary(dwm);
    }
    if (!done) {
        RECT rc;
        ::GetWindowRect(wnd, &rc);
        const int r = ::MulDiv(12, g.dpi, 96);
        HRGN rgn = ::CreateRoundRectRgn(0, 0, rc.right - rc.left + 1,
                                        rc.bottom - rc.top + 1, r * 2, r * 2);
        ::SetWindowRgn(wnd, rgn, TRUE);  // the window now owns the region
    }
}

void Relayout(HWND wnd, bool center) {
    const int w = ::MulDiv(kWinW, g.dpi, 96), h = ::MulDiv(kWinH, g.dpi, 96);
    if (center) {
        POINT cur;
        ::GetCursorPos(&cur);
        HMONITOR mon = ::MonitorFromPoint(cur, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi = {sizeof(mi)};
        ::GetMonitorInfoW(mon, &mi);
        const int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
        const int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;
        ::SetWindowPos(wnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        ::SetWindowPos(wnd, nullptr, 0, 0, w, h, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
    }
    ApplyRoundedFrame(wnd);
}

void LoadAppIcon() {
    const int s = ::MulDiv(48, g.dpi, 96);
    g.icon = (HICON)::LoadImageW(g.inst, MAKEINTRESOURCEW(1), IMAGE_ICON, s, s, LR_DEFAULTCOLOR);
}

void Animate() {
    bool dirty = false;
    const float step = 0.22f;

    for (int i = 0; i < (int)Hit::Count; ++i) {
        const float target = ((Hit)i == g.hot && g.stage != Stage::Working) ? 1.0f : 0.0f;
        const float d = (target - g.hover[i]) * step;
        if (fabsf(target - g.hover[i]) > 0.004f) { g.hover[i] += d; dirty = true; }
        else if (g.hover[i] != target) { g.hover[i] = target; dirty = true; }
    }

    for (int i = 0; i < kMaxChecks; ++i) {
        const float target = (i < CheckCount() && *CheckValue(i)) ? 1.0f : 0.0f;
        if (fabsf(target - g.checkAnim[i]) > 0.004f) {
            g.checkAnim[i] += (target - g.checkAnim[i]) * 0.30f;
            dirty = true;
        } else if (g.checkAnim[i] != target) { g.checkAnim[i] = target; dirty = true; }
    }

    if (g.stage == Stage::Working) {
        ::EnterCriticalSection(&g.lock);
        const float target = (float)g.workPercent;
        ::LeaveCriticalSection(&g.lock);
        if (g.shownPercent < target) {
            // The real work takes about 200 ms; without these bounds the bar
            // would jump from 0 to 100 with nothing readable in between.
            float d = (target - g.shownPercent) * 0.20f;
            if (d > 2.4f) d = 2.4f;
            if (d < 0.45f) d = 0.45f;
            g.shownPercent += d;
            if (g.shownPercent > target) g.shownPercent = target;
            dirty = true;
        }
        if (g.worker && g.shownPercent >= 99.5f) {
            if (::WaitForSingleObject(g.worker, 0) == WAIT_OBJECT_0) {
                ::CloseHandle(g.worker);
                g.worker = nullptr;
                ::EnterCriticalSection(&g.lock);
                const Result r = g.result;
                ::LeaveCriticalSection(&g.lock);
                g.stage = r.ok ? Stage::Done : Stage::Failed;

                // Requested after the copy and from the UI thread: the
                // elevation prompt needs an owner window, and the worker thread
                // is already gone. Declining elevation still leaves a working
                // per-user install, only with a diluted shadow adjustment.
                if (r.ok && !g.uninstallMode && g.opt.fullGammaRange) {
                    std::wstring err;
                    if (!RequestFullGammaRange(g.wnd, &err)) {
                        ::MessageBoxW(g.wnd,
                                      (L"O Zdisplay foi instalado, mas a faixa completa "
                                       L"de gama não foi liberada.\n\n" + err +
                                       L"\n\nO programa funciona assim mesmo; o ajuste de "
                                       L"sombras é que chega mais fraco. Dá para liberar "
                                       L"depois rodando o instalador de novo.").c_str(),
                                      L"Zdisplay", MB_OK | MB_ICONINFORMATION);
                    } else {
                        ::MessageBoxW(g.wnd,
                                      L"Faixa completa de gama liberada.\n\n"
                                      L"Ela só passa a valer no próximo login do Windows. "
                                      L"Até lá o ajuste de sombras continua diluído.",
                                      L"Zdisplay", MB_OK | MB_ICONINFORMATION);
                    }
                }

                g.hot = HitTest(0, 0);
                dirty = true;
            }
        }
    }

    if (dirty) ::InvalidateRect(g.wnd, nullptr, FALSE);
}

void Finish() {
    if (g.stage == Stage::Done && !g.uninstallMode && g.opt.launchAfter)
        LaunchInstalled(g.opt.dir);
    ::DestroyWindow(g.wnd);
}

void OnClick(Hit what) {
    switch (what) {
        case Hit::Close:
            if (g.stage == Stage::Working) return;  // never abort mid-install
            ::DestroyWindow(g.wnd);
            return;

        case Hit::Browse: {
            if (g.stage != Stage::Ask || g.uninstallMode) return;
            std::wstring picked;
            if (PickFolder(g.wnd, g.opt.dir, &picked)) {
                // The dialog picks where the install folder goes, not the
                // folder itself, so selecting an existing folder does not
                // scatter the program directly into it.
                const size_t cut = picked.find_last_of(L"\\/");
                const std::wstring leaf = cut == std::wstring::npos ? picked : picked.substr(cut + 1);
                if (::lstrcmpiW(leaf.c_str(), L"Zdisplay") == 0) g.opt.dir = picked;
                else g.opt.dir = picked + L"\\Zdisplay";
                ::InvalidateRect(g.wnd, nullptr, FALSE);
            }
            return;
        }

        case Hit::Check0:
        case Hit::Check1:
        case Hit::Check2:
        case Hit::Check3: {
            if (g.stage != Stage::Ask) return;
            const int i = (int)what - (int)Hit::Check0;
            if (i < CheckCount()) *CheckValue(i) = !*CheckValue(i);
            return;
        }

        case Hit::Primary:
            if (g.stage == Stage::Ask) StartWork();
            else if (g.stage == Stage::Done) Finish();
            else if (g.stage == Stage::Failed) ::DestroyWindow(g.wnd);
            return;

        default:
            return;
    }
}

LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            g.wnd = wnd;
            g.dpi = 96;
            {
                typedef UINT (WINAPI *PfnGetDpi)(HWND);
                HMODULE user = ::GetModuleHandleW(L"user32.dll");
                PfnGetDpi getDpi = user ? (PfnGetDpi)(void*)::GetProcAddress(user, "GetDpiForWindow") : nullptr;
                if (getDpi) {
                    const UINT d = getDpi(wnd);
                    if (d) g.dpi = (int)d;
                } else {
                    HDC dc = ::GetDC(wnd);
                    g.dpi = ::GetDeviceCaps(dc, LOGPIXELSX);
                    ::ReleaseDC(wnd, dc);
                }
            }
            BuildFonts();
            LoadAppIcon();
            ::SetTimer(wnd, 1, 16, nullptr);
            return 0;

        case WM_DPICHANGED: {
            g.dpi = HIWORD(wp);
            BuildFonts();
            if (g.icon) { ::DestroyIcon(g.icon); g.icon = nullptr; }
            LoadAppIcon();
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            ::SetWindowPos(wnd, nullptr, r->left, r->top, r->right - r->left,
                           r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            ApplyRoundedFrame(wnd);
            ::InvalidateRect(wnd, nullptr, FALSE);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = ::BeginPaint(wnd, &ps);
            RECT rc;
            ::GetClientRect(wnd, &rc);
            Paint(dc, rc);
            ::EndPaint(wnd, &ps);
            return 0;
        }

        case WM_TIMER:
            Animate();
            return 0;

        case WM_MOUSEMOVE: {
            const Hit h = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (h != g.hot) {
                g.hot = h;
                TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, wnd, 0};
                ::TrackMouseEvent(&tme);
                ::InvalidateRect(wnd, nullptr, FALSE);
            }
            ::SetCursor(::LoadCursorW(nullptr, h == Hit::None ? IDC_ARROW : IDC_HAND));
            return 0;
        }

        case WM_MOUSELEAVE:
            g.hot = Hit::None;
            ::InvalidateRect(wnd, nullptr, FALSE);
            return 0;

        case WM_LBUTTONDOWN: {
            const Hit h = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            g.pressed = h;
            if (h == Hit::None) {
                // Dragging the empty header area is the only way to move the
                // window, since there is no title bar.
                ::ReleaseCapture();
                ::SendMessageW(wnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                g.pressed = Hit::None;
                return 0;
            }
            ::SetCapture(wnd);
            ::InvalidateRect(wnd, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONUP: {
            const Hit h = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            const Hit was = g.pressed;
            g.pressed = Hit::None;
            ::ReleaseCapture();
            ::InvalidateRect(wnd, nullptr, FALSE);
            if (was != Hit::None && was == h) OnClick(h);
            return 0;
        }

        case WM_KEYDOWN:
            if (wp == VK_ESCAPE && g.stage != Stage::Working) {
                if (g.stage == Stage::Done) Finish();
                else ::DestroyWindow(wnd);
            } else if (wp == VK_RETURN || wp == VK_SPACE) {
                if (g.stage != Stage::Working) OnClick(Hit::Primary);
            }
            return 0;

        case WM_WORK_PROGRESS:
            return 0;  // the animation reads the value on the next frame

        case WM_WORK_DONE:
            return 0;  // the final stage is entered once the bar reaches 100%

        case WM_CLOSE:
            if (g.stage == Stage::Working) return 0;
            ::DestroyWindow(wnd);
            return 0;

        case WM_DESTROY:
            ::KillTimer(wnd, 1);
            ::PostQuitMessage(0);
            return 0;
    }
    return ::DefWindowProcW(wnd, msg, wp, lp);
}

// Command line

struct CmdLine {
    bool uninstall = false;
    bool silent = false;
    bool verify = false;
    bool gammaRange = false;  ///< internal mode: run elevated only to write the registry value
    bool hasDir = false;
    std::wstring dir;
};

CmdLine ParseCommandLine() {
    CmdLine out;
    int count = 0;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &count);
    if (!argv) return out;
    for (int i = 1; i < count; ++i) {
        const wchar_t* a = argv[i];
        if (!::lstrcmpiW(a, L"/uninstall") || !::lstrcmpiW(a, L"--uninstall") ||
            !::lstrcmpiW(a, L"-uninstall"))
            out.uninstall = true;
        else if (!::lstrcmpiW(a, L"/S") || !::lstrcmpiW(a, L"--silent"))
            out.silent = true;
        else if (!::lstrcmpiW(a, L"/verify") || !::lstrcmpiW(a, L"--verify"))
            out.verify = true;
        else if (!::lstrcmpiW(a, L"--faixa-gama"))
            out.gammaRange = true;
        else if (!::_wcsnicmp(a, L"/D=", 3) && a[3]) {
            out.hasDir = true;
            out.dir = a + 3;
        }
    }
    ::LocalFree(argv);
    return out;
}

void TrimTrailingSlash(std::wstring* s) {
    while (s->size() > 3 && (s->back() == L'\\' || s->back() == L'/')) s->pop_back();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    HardenCurrentProcess();
    g.inst = inst;
    ::InitializeCriticalSection(&g.lock);
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const CmdLine cmd = ParseCommandLine();
    g.uninstallMode = cmd.uninstall;

    if (cmd.verify) {
        const Result r = VerifyEmbeddedPayload();
        ::CoUninitialize();
        ::DeleteCriticalSection(&g.lock);
        return r.ok ? 0 : 1;
    }

    // Internal mode: this instance was relaunched elevated only to write the
    // HKLM value. It shows no window and reports through its exit code, and it
    // runs before any other decision so it never mixes with an install flow.
    if (cmd.gammaRange) {
        const bool ok = WriteFullGammaRange(nullptr);
        ::CoUninitialize();
        ::DeleteCriticalSection(&g.lock);
        return ok ? 0 : 1;
    }

    std::wstring installed;
    g.upgrade = FindInstalled(&installed) && !installed.empty();

    if (g.uninstallMode) {
        // The uninstaller lives inside the installation itself; the registry
        // only breaks the tie when the file was copied elsewhere.
        const std::wstring self = SelfPath();
        const size_t cut = self.find_last_of(L"\\/");
        const std::wstring name = cut == std::wstring::npos ? self : self.substr(cut + 1);
        g.opt.dir = (::lstrcmpiW(name.c_str(), kUninstName) == 0)
                        ? SelfDir()
                        : (g.upgrade ? installed : SelfDir());
        g.opt.removeSettings = false;
    } else if (cmd.hasDir) {
        g.opt.dir = cmd.dir;
    } else {
        g.opt.dir = g.upgrade ? installed : DefaultInstallDir();
    }
    TrimTrailingSlash(&g.opt.dir);
    if (cmd.silent) g.opt.launchAfter = false;

    if (cmd.silent) {
        const Result r = g.uninstallMode ? Uninstall(g.opt, nullptr, nullptr)
                                         : Install(g.opt, nullptr, nullptr);
        if (r.ok && !g.uninstallMode && g.opt.launchAfter) LaunchInstalled(g.opt.dir);
        ::CoUninitialize();
        ::DeleteCriticalSection(&g.lock);
        return r.ok ? 0 : 1;
    }

    if (!g.uninstallMode && PayloadSize() == 0) {
        ::MessageBoxW(nullptr,
                      L"Este instalador foi montado sem o Zdisplay dentro dele.\n"
                      L"Gere-o de novo com  build.bat setup.",
                      L"Zdisplay", MB_ICONERROR | MB_OK);
        ::CoUninitialize();
        ::DeleteCriticalSection(&g.lock);
        return 1;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = (HICON)::LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    wc.lpszClassName = L"ZdisplaySetupWindow";
    ::RegisterClassExW(&wc);

    HWND wnd = ::CreateWindowExW(
        WS_EX_APPWINDOW, wc.lpszClassName, L"Zdisplay",
        WS_POPUP | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
        kWinW, kWinH, nullptr, nullptr, inst, nullptr);
    if (!wnd) {
        ::CoUninitialize();
        ::DeleteCriticalSection(&g.lock);
        return 1;
    }

    Relayout(wnd, true);
    ::ShowWindow(wnd, SW_SHOW);
    ::SetForegroundWindow(wnd);

    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    if (g.worker) {
        ::WaitForSingleObject(g.worker, 10000);
        ::CloseHandle(g.worker);
    }
    if (g.icon) ::DestroyIcon(g.icon);
    ReleaseFonts();
    if (g.memDC) {
        ::SelectObject(g.memDC, g.oldBmp);
        ::DeleteObject(g.bmp);
        ::DeleteDC(g.memDC);
    }
    ::CoUninitialize();
    ::DeleteCriticalSection(&g.lock);
    return 0;
}
