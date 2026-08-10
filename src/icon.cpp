// The icon is drawn in code: a display whose screen carries a dark red
// gradient, matching the program's own domain of screen color and brightness.
//
// It is pure arithmetic, with no GDI+ and no image file, so the build needs no
// graphics tooling and the executable stays a single self-contained binary.
#include "common.h"

namespace zdisplay {

namespace {

struct Rgb { double r, g, b; };

inline uint32_t PackBgra(double r, double g, double b, double a) {
    const auto q = [](double v) -> uint32_t {
        return (uint32_t)Clamp((int)llround(v * 255.0), 0, 255);
    };
    return (q(a) << 24) | (q(r) << 16) | (q(g) << 8) | q(b);
}

/// Signed distance to a rounded rectangle: negative inside, positive outside.
/// The magnitude near the edge drives sub-pixel antialiasing.
struct RRect { double cx, cy, hx, hy, r; };

inline double SdRRect(double px, double py, const RRect& b) {
    const double qx = std::fabs(px - b.cx) - (b.hx - b.r);
    const double qy = std::fabs(py - b.cy) - (b.hy - b.r);
    const double ax = std::max(qx, 0.0);
    const double ay = std::max(qy, 0.0);
    return std::sqrt(ax * ax + ay * ay) +
           std::min(std::max(qx, qy), 0.0) - b.r;
}

/// Alpha 0..1 from a signed distance, with half-width `aa` in pixels.
inline double CoverageFromSd(double d, double aa) {
    return Clamp(0.5 - d / aa, 0.0, 1.0);
}

/// Composites src over bg in plain linear space (virtual premultiplication).
inline void Over(Rgb src, double sa, Rgb& bg, double& ba) {
    if (sa <= 0) return;
    const double a = sa + ba * (1.0 - sa);
    if (a <= 0) { bg = {0, 0, 0}; ba = 0; return; }
    bg.r = (src.r * sa + bg.r * ba * (1.0 - sa)) / a;
    bg.g = (src.g * sa + bg.g * ba * (1.0 - sa)) / a;
    bg.b = (src.b * sa + bg.b * ba * (1.0 - sa)) / a;
    ba = a;
}

}  // namespace

/// Fills `out` with `size*size` BGRA pixels, top-down.
///
/// Each part of the display is drawn first as a slightly larger silhouette in a
/// light metallic color and then as a dark body on top. The resulting light rim
/// keeps the icon legible on any background: the dark body carries the contrast
/// on white, the rim carries it on black.
static void RenderIconPixels(int size, std::vector<uint32_t>& out) {
    out.assign((size_t)size * size, 0);

    const double S = (double)size;
    const double aa = 0.85;                              // antialiasing, in pixels
    const double rimBig   = std::max(0.90, S * 0.022);   // bezel rim
    const double rimSmall = std::max(0.65, S * 0.015);   // base and neck rim

    // Geometry in pixel coordinates. The proportions hold at any size because
    // everything scales with S. Base and neck are oversized so they survive at
    // small icon sizes.
    const RRect base   = { 0.50 * S, 0.855 * S, 0.260 * S, 0.032 * S, 0.030 * S };
    const RRect neck   = { 0.50 * S, 0.755 * S, 0.070 * S, 0.055 * S, 0.016 * S };
    const RRect bezel  = { 0.50 * S, 0.400 * S, 0.445 * S, 0.295 * S, 0.062 * S };
    const RRect screen = { 0.50 * S, 0.390 * S, 0.390 * S, 0.240 * S, 0.030 * S };

    // Metallic rim: light grey with a slight cool tint.
    const Rgb rim{ 0.74, 0.74, 0.78 };

    // Lit power LED on the lower bezel, which reads as "display on" and makes
    // the red glow of the screen legible.
    const double ledCx = 0.50 * S;
    const double ledCy = bezel.cy + bezel.hy - 0.030 * S;
    const double ledR  = std::max(1.0, 0.019 * S);

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double px = x + 0.5;
            const double py = y + 0.5;

            Rgb col{0, 0, 0};
            double a = 0.0;

            // Base: light rim plus dark body on top.
            {
                const double d = SdRRect(px, py, base);
                const double rimM = CoverageFromSd(d - rimSmall, aa);
                if (rimM > 0) Over(rim, rimM, col, a);

                const double m = CoverageFromSd(d, aa);
                if (m > 0) {
                    const double t = Clamp((py - (base.cy - base.hy)) / (2 * base.hy), 0.0, 1.0);
                    const Rgb c{
                        Lerp(0.30, 0.12, t),
                        Lerp(0.30, 0.12, t),
                        Lerp(0.32, 0.13, t),
                    };
                    Over(c, m, col, a);
                }
            }

            // Neck: light rim plus body.
            {
                const double d = SdRRect(px, py, neck);
                const double rimM = CoverageFromSd(d - rimSmall, aa);
                if (rimM > 0) Over(rim, rimM, col, a);

                const double m = CoverageFromSd(d, aa);
                if (m > 0) {
                    const double t = Clamp((px - (neck.cx - neck.hx)) / (2 * neck.hx), 0.0, 1.0);
                    const double edge = 1.0 - std::fabs(2 * t - 1.0);  // 0 at the sides, 1 in the middle
                    const double v = 0.18 + 0.12 * edge;
                    const Rgb c{ v, v, v + 0.02 };
                    Over(c, m, col, a);
                }
            }

            // Bezel: thicker light rim, dark body with a vertical gradient, and
            // a metallic highlight on the inner edge against the screen.
            {
                const double d = SdRRect(px, py, bezel);
                const double rimM = CoverageFromSd(d - rimBig, aa);
                if (rimM > 0) Over(rim, rimM, col, a);

                const double m = CoverageFromSd(d, aa);
                if (m > 0) {
                    const double t = Clamp((py - (bezel.cy - bezel.hy)) / (2 * bezel.hy), 0.0, 1.0);
                    Rgb c{
                        Lerp(0.24, 0.09, t),
                        Lerp(0.24, 0.09, t),
                        Lerp(0.26, 0.10, t),
                    };
                    // Bezel pixels within about a pixel of the screen get a
                    // metallic edge, which separates bezel from screen.
                    const double dScreen = SdRRect(px, py, screen);
                    const double innerBand = std::max(0.9, S * 0.020);
                    if (dScreen > 0 && dScreen < innerBand) {
                        const double k = 0.60 * (1.0 - dScreen / innerBand);
                        c.r = Lerp(c.r, 0.62, k);
                        c.g = Lerp(c.g, 0.62, k);
                        c.b = Lerp(c.b, 0.64, k);
                    }
                    Over(c, m, col, a);
                }
            }

            // Screen: dark red gradient with the hotspot toward the top and a
            // diagonal reflection band.
            {
                const double d = SdRRect(px, py, screen);
                const double m = CoverageFromSd(d, aa);
                if (m > 0) {
                    const double gx = (px - screen.cx) / screen.hx;
                    const double gy = (py - (screen.cy - 0.35 * screen.hy)) / screen.hy;
                    double t = Clamp((gx * gx + gy * gy) / 1.45, 0.0, 1.0);
                    t = t * t;  // deepens the falloff toward the edges
                    Rgb c{
                        Lerp(0.92, 0.10, t),
                        Lerp(0.18, 0.01, t),
                        Lerp(0.20, 0.02, t),
                    };
                    // Subtle diagonal reflection, so the screen reads as glass.
                    const double diag = (px - screen.cx) + (py - (screen.cy - 0.55 * screen.hy));
                    const double band = std::exp(-(diag * diag) / (2.0 * (0.17 * S) * (0.17 * S)));
                    const double glow = band * 0.14;
                    c.r = Clamp(c.r + glow,        0.0, 1.0);
                    c.g = Clamp(c.g + glow * 0.30, 0.0, 1.0);
                    c.b = Clamp(c.b + glow * 0.30, 0.0, 1.0);

                    Over(c, m, col, a);
                }
            }

            // Lit power LED: bright red with a light core.
            {
                const double dx = px - ledCx;
                const double dy = py - ledCy;
                const double dist = std::sqrt(dx * dx + dy * dy);
                const double d = dist - ledR;
                const double m = CoverageFromSd(d, aa);
                if (m > 0) {
                    const double core = Clamp(1.0 - dist / (ledR * 0.6), 0.0, 1.0);
                    const Rgb c{
                        Lerp(0.95, 1.00, core),
                        Lerp(0.20, 0.55, core),
                        Lerp(0.18, 0.50, core),
                    };
                    Over(c, m, col, a);
                }
            }

            out[(size_t)y * size + x] = PackBgra(col.r, col.g, col.b, a);
        }
    }
}

/// Creates an HICON from the drawing. Used in the tray and the title bar.
HICON CreateAppIcon(int size) {
    std::vector<uint32_t> pixels;
    RenderIconPixels(size, pixels);

    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(BITMAPV5HEADER);
    bi.bV5Width = size;
    bi.bV5Height = -size;          // negative: rows run top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask   = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask  = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    HDC screen = ::GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP color = ::CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi),
                                       DIB_RGB_COLORS, &bits, nullptr, 0);
    ::ReleaseDC(nullptr, screen);
    if (!color || !bits) {
        if (color) ::DeleteObject(color);
        return ::LoadIconW(nullptr, IDI_APPLICATION);
    }
    memcpy(bits, pixels.data(), pixels.size() * sizeof(uint32_t));

    // Fully zeroed mask: the real transparency comes from the alpha channel.
    HBITMAP mask = ::CreateBitmap(size, size, 1, 1, nullptr);

    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;

    HICON icon = ::CreateIconIndirect(&ii);
    ::DeleteObject(color);
    ::DeleteObject(mask);
    return icon ? icon : ::LoadIconW(nullptr, IDI_APPLICATION);
}

/// Writes a classic .ico file (32 bpp plus mask) for the executable to embed at
/// build time. Invoked by "zdisplay.exe --make-icon".
bool WriteIcoFile(const std::wstring& path, int size) {
    std::vector<uint32_t> pixels;
    RenderIconPixels(size, pixels);

    const int maskStride = ((size + 31) / 32) * 4;   // 1 bpp rows aligned to 4 bytes
    const size_t xorSize = (size_t)size * size * 4;
    const size_t andSize = (size_t)maskStride * size;

    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = size;
    bih.biHeight = size * 2;      // doubled height: image plus mask
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = (DWORD)(xorSize + andSize);

    std::vector<uint8_t> file;
    const auto push = [&file](const void* data, size_t n) {
        const auto* p = static_cast<const uint8_t*>(data);
        file.insert(file.end(), p, p + n);
    };

    // ICONDIR
    const uint16_t reserved = 0, type = 1, count = 1;
    push(&reserved, 2); push(&type, 2); push(&count, 2);

    // ICONDIRENTRY
    const uint8_t w = (uint8_t)(size >= 256 ? 0 : size);
    const uint8_t h = w;
    const uint8_t palette = 0, res2 = 0;
    const uint16_t planes = 1, bits = 32;
    const uint32_t bytesInRes = (uint32_t)(sizeof(BITMAPINFOHEADER) + xorSize + andSize);
    const uint32_t offset = 22;
    push(&w, 1); push(&h, 1); push(&palette, 1); push(&res2, 1);
    push(&planes, 2); push(&bits, 2);
    push(&bytesInRes, 4); push(&offset, 4);

    // Bitmap header
    push(&bih, sizeof(bih));

    // Pixels bottom-up: the ICO format stores them inverted.
    for (int y = size - 1; y >= 0; --y)
        push(&pixels[(size_t)y * size], (size_t)size * 4);

    // Zeroed AND mask.
    std::vector<uint8_t> mask(andSize, 0);
    push(mask.data(), mask.size());

    HANDLE fh = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = ::WriteFile(fh, file.data(), (DWORD)file.size(), &written, nullptr) &&
                    written == file.size();
    ::CloseHandle(fh);
    return ok;
}

}  // namespace zdisplay
