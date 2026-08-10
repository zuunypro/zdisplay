// DPI scaling shared by every window.
//
// Scaling is resolved per window rather than once at startup: windows can sit on
// monitors with different DPI values, so a single startup value would leave
// layout coordinates and fonts disagreeing on a mixed-DPI setup.
#pragma once
#include "common.h"

namespace zdisplay {
namespace dpi {

inline int& Current() {
    static int value = 96;
    return value;
}

/// Converts design units (96 dpi) into pixels for the current DPI.
inline int S(int v) { return ::MulDiv(v, Current(), 96); }

inline void DetectFor(HWND hwnd) {
    typedef UINT (WINAPI *PfnGetDpiForWindow)(HWND);
    static PfnGetDpiForWindow pGetDpi = [] {
        HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
        return user32 ? (PfnGetDpiForWindow)(void*)::GetProcAddress(user32, "GetDpiForWindow")
                      : nullptr;
    }();

    int& value = Current();
    if (pGetDpi && hwnd) {
        const UINT d = pGetDpi(hwnd);
        if (d >= 72 && d <= 480) { value = (int)d; return; }
    }
    HDC dc = ::GetDC(nullptr);
    if (dc) { value = ::GetDeviceCaps(dc, LOGPIXELSX); ::ReleaseDC(nullptr, dc); }
    if (value < 72 || value > 480) value = 96;
}

/// DPI of the monitor holding the cursor. Sizes a window before it exists, since
/// a window that has not been created yet has no DPI to query.
inline int ForCursor() {
    typedef UINT (WINAPI *PfnGetDpiForMonitor)(HMONITOR, int, UINT*, UINT*);
    static PfnGetDpiForMonitor pGetDpi = [] {
        HMODULE shcore = ::LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return shcore ? (PfnGetDpiForMonitor)(void*)::GetProcAddress(shcore, "GetDpiForMonitor")
                      : nullptr;
    }();

    POINT pt{};
    ::GetCursorPos(&pt);
    HMONITOR mon = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (pGetDpi && mon) {
        UINT x = 0, y = 0;
        if (pGetDpi(mon, 0 /* MDT_EFFECTIVE_DPI */, &x, &y) == S_OK && x >= 72 && x <= 480)
            return (int)x;
    }
    return Current();
}

/// System message font at the requested DPI. The caller owns the HFONT.
///
/// A window sized for one monitor's DPI needs its font at that same DPI:
/// SystemParametersInfoW reports only the system DPI, which oversizes text
/// relative to the scaled controls on a mixed-DPI setup.
inline HFONT MessageFontFor(int dpiValue) {
    typedef BOOL (WINAPI *PfnSpiForDpi)(UINT, UINT, PVOID, UINT, UINT);
    static PfnSpiForDpi pSpi = [] {
        HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
        return user32 ? (PfnSpiForDpi)(void*)::GetProcAddress(user32, "SystemParametersInfoForDpi")
                      : nullptr;
    }();

    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    bool got = pSpi &&
               pSpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, (UINT)dpiValue) != FALSE;
    if (!got) {
        if (!::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
            return nullptr;
        // Windows without the per-DPI API: rescale the system-DPI font.
        HDC dc = ::GetDC(nullptr);
        int sys = dc ? ::GetDeviceCaps(dc, LOGPIXELSY) : 96;
        if (dc) ::ReleaseDC(nullptr, dc);
        if (sys < 72) sys = 96;
        ncm.lfMessageFont.lfHeight = ::MulDiv(ncm.lfMessageFont.lfHeight, dpiValue, sys);
    }
    return ::CreateFontIndirectW(&ncm.lfMessageFont);
}

/// Work area (taskbar excluded) of the monitor holding the cursor.
inline RECT WorkAreaForCursor() {
    POINT pt{};
    ::GetCursorPos(&pt);
    HMONITOR mon = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (mon && ::GetMonitorInfoW(mon, &mi)) return mi.rcWork;

    RECT fallback{0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN)};
    return fallback;
}

}  // namespace dpi
}  // namespace zdisplay
