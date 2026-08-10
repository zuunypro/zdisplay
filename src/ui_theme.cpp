#include "ui_theme.h"
#include "ui_dpi.h"

#include <uxtheme.h>
#include <vssym32.h>

namespace zdisplay {
namespace theme {

namespace {

// Cool, near-neutral greys keep the background from competing with on-screen
// content. The accent red appears only where there is action or value: active
// button, filled part of a slider, selected tab.
const Palette kDark = {
    RGB(24, 24, 27),     // base
    RGB(34, 34, 39),     // surface
    RGB(45, 45, 51),     // surfaceAlt
    RGB(56, 56, 63),     // hover
    RGB(64, 64, 72),     // line
    RGB(233, 233, 238),  // text
    RGB(150, 150, 160),  // textDim
    RGB(214, 45, 50),    // accent
    RGB(240, 74, 79),    // accentHot
    RGB(132, 28, 32),    // accentDim
};

// Integer values are stored as window properties. The bias keeps the encoded
// HANDLE non-null even for ranges that allow negative values (hue runs
// -180..180), and nothing is allocated.
const wchar_t* const kTrackPosProp = L"Zdisplay.TrackPos";
const wchar_t* const kTrackMinProp = L"Zdisplay.TrackMin";
const wchar_t* const kTrackMaxProp = L"Zdisplay.TrackMax";
const INT_PTR kTrackPropBias = 0x10000000;

HANDLE EncodeTrackValue(int value) {
    return reinterpret_cast<HANDLE>((INT_PTR)value + kTrackPropBias);
}

bool DecodeTrackValue(HWND bar, const wchar_t* name, int* value) {
    const HANDLE encoded = ::GetPropW(bar, name);
    if (!encoded) return false;
    if (value) *value = (int)(reinterpret_cast<INT_PTR>(encoded) - kTrackPropBias);
    return true;
}

HBRUSH g_base = nullptr, g_surface = nullptr, g_surfaceAlt = nullptr;
bool   g_glass = false;

/// Window property marking a label as dimmed (see SetDimmed). It lives on the
/// control itself, so it is released together with the control.
const wchar_t* const kDimProp = L"ZdisplayDim";

// DWM: dark window frame and glass backdrop

constexpr DWORD kDarkModeOld       = 19;  ///< Windows 10 before 20H1
constexpr DWORD kDarkMode          = 20;
constexpr DWORD kSystemBackdrop    = 38;  ///< Windows 11 22H2 and later
constexpr DWORD kBackdropAcrylic   = 3;   ///< DWMSBT_TRANSIENTWINDOW

typedef HRESULT (WINAPI *PfnDwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
typedef HRESULT (WINAPI *PfnSetWindowTheme)(HWND, LPCWSTR, LPCWSTR);

PfnDwmSetWindowAttribute DwmSet() {
    static PfnDwmSetWindowAttribute p = [] {
        HMODULE h = ::LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return h ? (PfnDwmSetWindowAttribute)(void*)::GetProcAddress(h, "DwmSetWindowAttribute")
                 : nullptr;
    }();
    return p;
}

PfnSetWindowTheme WindowTheme() {
    static PfnSetWindowTheme p = [] {
        HMODULE h = ::LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return h ? (PfnSetWindowTheme)(void*)::GetProcAddress(h, "SetWindowTheme") : nullptr;
    }();
    return p;
}

std::wstring ClassOf(HWND h) {
    wchar_t name[64] = {};
    ::GetClassNameW(h, name, 63);
    return name;
}

/// Fills a rectangle and draws a one-pixel border around it.
void FillWithBorder(HDC dc, const RECT& r, COLORREF fill, COLORREF border) {
    HBRUSH b = ::CreateSolidBrush(fill);
    ::FillRect(dc, &r, b);
    ::DeleteObject(b);

    HBRUSH e = ::CreateSolidBrush(border);
    ::FrameRect(dc, &r, e);
    ::DeleteObject(e);
}

/// Draws the control text with the font the control already uses.
void DrawControlText(HDC dc, HWND ctl, const RECT& r, COLORREF color, UINT format) {
    wchar_t text[256] = {};
    ::GetWindowTextW(ctl, text, 255);
    if (!text[0]) return;

    HFONT font = (HFONT)::SendMessageW(ctl, WM_GETFONT, 0, 0);
    HGDIOBJ old = font ? ::SelectObject(dc, font) : nullptr;
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, color);
    RECT t = r;
    ::DrawTextW(dc, text, -1, &t, format);
    if (old) ::SelectObject(dc, old);
}

// Buttons

bool DrawButton(const NMCUSTOMDRAW* cd, LRESULT* result) {
    if (cd->dwDrawStage != CDDS_PREPAINT) return false;

    const HWND ctl = cd->hdr.hwndFrom;
    const LONG style = ::GetWindowLongW(ctl, GWL_STYLE);
    const LONG type = style & 0xFu;
    const bool disabled = (cd->uItemState & CDIS_DISABLED) != 0;
    const bool hot = (cd->uItemState & CDIS_HOT) != 0;
    const bool pressed = (cd->uItemState & CDIS_SELECTED) != 0;

    if (type == BS_AUTOCHECKBOX || type == BS_CHECKBOX ||
        type == BS_AUTORADIOBUTTON || type == BS_RADIOBUTTON) {
        // The box and the radio circle are owner-drawn: the stock control
        // paints a white square that no colour message reaches.
        HBRUSH bg = BaseBrush();
        RECT full = cd->rc;
        ::FillRect(cd->hdc, &full, bg);

        const int side = dpi::S(14);
        RECT box{ cd->rc.left, 0, cd->rc.left + side, 0 };
        box.top = cd->rc.top + ((cd->rc.bottom - cd->rc.top) - side) / 2;
        box.bottom = box.top + side;

        const bool checked = ::SendMessageW(ctl, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const COLORREF fill = checked ? (disabled ? kDark.accentDim : kDark.accent)
                                      : kDark.surfaceAlt;
        const COLORREF edge = hot && !disabled ? kDark.accentHot : kDark.line;
        FillWithBorder(cd->hdc, box, fill, edge);

        if (checked) {
            HPEN pen = ::CreatePen(PS_SOLID, dpi::S(2), kDark.text);
            HGDIOBJ oldPen = ::SelectObject(cd->hdc, pen);
            const int x = box.left, y = box.top, s = side;
            ::MoveToEx(cd->hdc, x + s / 4, y + s / 2, nullptr);
            ::LineTo(cd->hdc, x + s / 2 - 1, y + s - s / 4);
            ::LineTo(cd->hdc, x + s - s / 5, y + s / 4);
            ::SelectObject(cd->hdc, oldPen);
            ::DeleteObject(pen);
        }

        RECT label = cd->rc;
        label.left = box.right + dpi::S(7);
        DrawControlText(cd->hdc, ctl, label, disabled ? kDark.textDim : kDark.text,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_WORD_ELLIPSIS);
        *result = CDRF_SKIPDEFAULT;
        return true;
    }

    if (type == BS_GROUPBOX) {
        // The group box frame becomes a thin line with the caption over it.
        RECT r = cd->rc;
        HBRUSH bg = BaseBrush();
        ::FillRect(cd->hdc, &r, bg);

        RECT frame = r;
        frame.top += dpi::S(7);
        HBRUSH e = ::CreateSolidBrush(kDark.line);
        ::FrameRect(cd->hdc, &frame, e);
        ::DeleteObject(e);

        wchar_t text[128] = {};
        ::GetWindowTextW(ctl, text, 127);
        if (text[0]) {
            HFONT font = (HFONT)::SendMessageW(ctl, WM_GETFONT, 0, 0);
            HGDIOBJ old = font ? ::SelectObject(cd->hdc, font) : nullptr;
            SIZE sz{};
            ::GetTextExtentPoint32W(cd->hdc, text, (int)wcslen(text), &sz);
            RECT cap{ r.left + dpi::S(9), r.top,
                      r.left + dpi::S(9) + sz.cx + dpi::S(8), r.top + sz.cy + dpi::S(2) };
            ::FillRect(cd->hdc, &cap, bg);
            ::SetBkMode(cd->hdc, TRANSPARENT);
            ::SetTextColor(cd->hdc, kDark.textDim);
            ::TextOutW(cd->hdc, r.left + dpi::S(13), r.top, text, (int)wcslen(text));
            if (old) ::SelectObject(cd->hdc, old);
        }
        *result = CDRF_SKIPDEFAULT;
        return true;
    }

    // Push button.
    COLORREF fill = kDark.surfaceAlt, edge = kDark.line, fg = kDark.text;
    if (disabled)      { fill = kDark.surface;    edge = kDark.line;      fg = kDark.textDim; }
    else if (pressed)  { fill = kDark.accentDim;  edge = kDark.accentDim; }
    else if (hot)      { fill = kDark.hover;      edge = kDark.accent; }

    FillWithBorder(cd->hdc, cd->rc, fill, edge);
    DrawControlText(cd->hdc, ctl, cd->rc, fg,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    *result = CDRF_SKIPDEFAULT;
    return true;
}

// Sliders

bool DrawTrackbar(const NMCUSTOMDRAW* cd, LRESULT* result) {
    // The whole trackbar is painted in a single CDDS_PREPAINT pass. This
    // control does not emit the per-part item stages (TBCD_CHANNEL,
    // TBCD_THUMB), so the part rectangles are queried from the control itself
    // through documented messages.
    if (cd->dwDrawStage != CDDS_PREPAINT) return false;

    const HWND bar = cd->hdr.hwndFrom;
    const bool off = ::IsWindowEnabled(bar) == FALSE;

    RECT client{};
    ::GetClientRect(bar, &client);
    ::FillRect(cd->hdc, &client, BaseBrush());

    // The thumb position is derived from the value, not from TBM_GETTHUMBRECT.
    // CDRF_SKIPDEFAULT stops the control from running its own paint, which is
    // where it recomputes that rectangle, so the reported rectangle would stay
    // at its initial position.
    int pos = 0, lo = 0, hi = 0;
    const bool cached = DecodeTrackValue(bar, kTrackPosProp, &pos) &&
                        DecodeTrackValue(bar, kTrackMinProp, &lo) &&
                        DecodeTrackValue(bar, kTrackMaxProp, &hi);
    if (!cached) {
        // Fallback for controls created outside SliderRow.
        pos = (int)::SendMessageW(bar, TBM_GETPOS, 0, 0);
        lo  = (int)::SendMessageW(bar, TBM_GETRANGEMIN, 0, 0);
        hi  = (int)::SendMessageW(bar, TBM_GETRANGEMAX, 0, 0);
    }
    const double frac = hi > lo
        ? Clamp((double)(pos - lo) / (double)(hi - lo), 0.0, 1.0)
        : 0.0;

    const int gripW = dpi::S(9);
    const int gripH = (std::max)(dpi::S(14), (int)(client.bottom - client.top) - dpi::S(6));
    const int mid   = (client.top + client.bottom) / 2;

    // The rail uses the control's own channel so the drawn thumb lands where a
    // click is interpreted. The channel is computed on WM_SIZE rather than
    // during painting; the client rectangle stands in while it is still empty.
    RECT channel{};
    ::SendMessageW(bar, TBM_GETCHANNELRECT, 0, (LPARAM)&channel);
    const bool channelOk = channel.right > channel.left &&
                           channel.left >= client.left && channel.right <= client.right;
    const int railLeft  = channelOk ? channel.left  : client.left;
    const int railRight = channelOk ? channel.right : client.right;

    // The thumb centre travels between the rail ends, inset by half a thumb
    // width on each side so the thumb never overhangs the rail.
    const int runLeft  = railLeft + gripW / 2;
    const int runRight = (std::max)(runLeft, railRight - gripW / 2);
    const int center   = runLeft + (int)llround(frac * (runRight - runLeft));

    const int half = dpi::S(2);
    RECT track{ railLeft, mid - half, railRight, mid + half };

    HBRUSH rail = ::CreateSolidBrush(off ? kDark.surface : kDark.surfaceAlt);
    ::FillRect(cd->hdc, &track, rail);
    ::DeleteObject(rail);

    // The stretch left of the thumb is filled with the accent colour to show at
    // a glance how much of the range is applied.
    if (center > track.left) {
        RECT filled{ track.left, track.top, center, track.bottom };
        HBRUSH on = ::CreateSolidBrush(off ? kDark.accentDim : kDark.accent);
        ::FillRect(cd->hdc, &filled, on);
        ::DeleteObject(on);
    }

    RECT grip{ center - gripW / 2, mid - gripH / 2,
               center + gripW / 2, mid + gripH / 2 };

    const bool hot = (cd->uItemState & (CDIS_HOT | CDIS_SELECTED)) != 0;
    FillWithBorder(cd->hdc, grip,
                   off ? kDark.surfaceAlt : (hot ? kDark.accentHot : kDark.text),
                   off ? kDark.line : kDark.accentDim);

    *result = CDRF_SKIPDEFAULT;
    return true;
}

void SyncTrackbarVisualImpl(HWND bar);

/// The trackbar currently being dragged. A single HWND is enough because there
/// is only one mouse, and this keeps the state out of per-control properties
/// that would have to be cleared on every exit path.
HWND g_trackDrag = nullptr;

/// Maps a mouse X coordinate to the matching value and applies it.
///
/// The thumb travels between the extreme thumb centres, not between the rail
/// edges, so half a thumb width is excluded at each end; ignoring that skews
/// the value near the ends and makes the minimum and maximum hard to hit.
void TrackSetFromX(HWND bar, int x) {
    RECT ch{};
    ::SendMessageW(bar, TBM_GETCHANNELRECT, 0, (LPARAM)&ch);
    const int thumb = (int)::SendMessageW(bar, TBM_GETTHUMBLENGTH, 0, 0);
    const int lo = (int)::SendMessageW(bar, TBM_GETRANGEMIN, 0, 0);
    const int hi = (int)::SendMessageW(bar, TBM_GETRANGEMAX, 0, 0);

    const int left  = ch.left + thumb / 2;
    const int right = ch.right - thumb / 2;
    if (right <= left || hi <= lo) return;

    double t = (double)(x - left) / (double)(right - left);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;

    const int pos = lo + (int)llround(t * (double)(hi - lo));
    if (pos == (int)::SendMessageW(bar, TBM_GETPOS, 0, 0)) return;

    ::SendMessageW(bar, TBM_SETPOS, TRUE, (LPARAM)(LONG)pos);
    SyncTrackbarVisualImpl(bar);
    ::InvalidateRect(bar, nullptr, FALSE);

    // The parent applies the adjustment on this notification. TB_THUMBTRACK
    // marks the drag as still in progress, so the parent throttles the write
    // instead of committing one per mouse pixel.
    if (HWND parent = ::GetParent(bar))
        ::SendMessageW(parent, WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, pos), (LPARAM)bar);
}

LRESULT CALLBACK TrackbarSubclass(HWND wnd, UINT msg, WPARAM wp, LPARAM lp,
                                  UINT_PTR, DWORD_PTR) {
    switch (msg) {
        case WM_ERASEBKGND:
            // DrawTrackbar covers the whole client area. Erasing first with
            // the control brush produces an intermediate frame without the
            // rail, which reads as flicker during fast movement.
            return 1;

        // The stock trackbar starts a drag only when the click lands on the
        // thumb, which is a few pixels wide, and a click on the rail pages by
        // one step instead of moving to the click point. Here a click anywhere
        // moves the value to that point and engages the drag, so dragging is
        // 1:1 with the mouse.
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            ::SetFocus(wnd);
            g_trackDrag = wnd;
            ::SetCapture(wnd);
            TrackSetFromX(wnd, (int)(short)LOWORD(lp));
            return 0;

        case WM_MOUSEMOVE:
            if (g_trackDrag == wnd) {
                TrackSetFromX(wnd, (int)(short)LOWORD(lp));
                return 0;
            }
            break;

        case WM_LBUTTONUP:
            if (g_trackDrag == wnd) {
                g_trackDrag = nullptr;
                ::ReleaseCapture();
                // TB_ENDTRACK tells the parent the drag is over: that is
                // where throttling stops and the final value is persisted.
                if (HWND parent = ::GetParent(wnd))
                    ::SendMessageW(parent, WM_HSCROLL, MAKEWPARAM(TB_ENDTRACK, 0),
                                   (LPARAM)wnd);
                return 0;
            }
            break;

        // Alt+Tab, a dialog taking focus or Esc drop the capture without a
        // WM_LBUTTONUP. Clearing the drag state here keeps the trackbar from
        // following the mouse after the button is released.
        case WM_CAPTURECHANGED:
            if (g_trackDrag == wnd) {
                g_trackDrag = nullptr;
                if (HWND parent = ::GetParent(wnd))
                    ::SendMessageW(parent, WM_HSCROLL, MAKEWPARAM(TB_ENDTRACK, 0),
                                   (LPARAM)wnd);
            }
            break;

        case WM_NCDESTROY:
            if (g_trackDrag == wnd) g_trackDrag = nullptr;
            ::RemovePropW(wnd, kTrackPosProp);
            ::RemovePropW(wnd, kTrackMinProp);
            ::RemovePropW(wnd, kTrackMaxProp);
            ::RemoveWindowSubclass(wnd, TrackbarSubclass, 4);
            break;
    }
    return ::DefSubclassProc(wnd, msg, wp, lp);
}

void SyncTrackbarVisualImpl(HWND bar) {
    if (!bar || !::IsWindow(bar)) return;
    const int pos = (int)::SendMessageW(bar, TBM_GETPOS, 0, 0);
    const int lo  = (int)::SendMessageW(bar, TBM_GETRANGEMIN, 0, 0);
    const int hi  = (int)::SendMessageW(bar, TBM_GETRANGEMAX, 0, 0);
    ::SetPropW(bar, kTrackPosProp, EncodeTrackValue(pos));
    ::SetPropW(bar, kTrackMinProp, EncodeTrackValue(lo));
    ::SetPropW(bar, kTrackMaxProp, EncodeTrackValue(hi));
}

// Tabs

LRESULT CALLBACK TabSubclass(HWND wnd, UINT msg, WPARAM wp, LPARAM lp,
                             UINT_PTR, DWORD_PTR) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;                       // everything is painted in WM_PAINT

        case WM_PAINT: {
            // The whole strip is painted here, without the default drawing.
            // TCS_OWNERDRAWFIXED hands over only the interior of each tab
            // through WM_DRAWITEM, leaving the tab frames and the content
            // border in system colours that no message reaches.
            PAINTSTRUCT ps{};
            HDC dc = ::BeginPaint(wnd, &ps);

            RECT client{};
            ::GetClientRect(wnd, &client);
            ::FillRect(dc, &client, BaseBrush());

            const int count = (int)::SendMessageW(wnd, TCM_GETITEMCOUNT, 0, 0);
            const int sel = (int)::SendMessageW(wnd, TCM_GETCURSEL, 0, 0);

            HFONT font = (HFONT)::SendMessageW(wnd, WM_GETFONT, 0, 0);
            HGDIOBJ oldFont = font ? ::SelectObject(dc, font) : nullptr;
            ::SetBkMode(dc, TRANSPARENT);

            int stripBottom = 0;
            for (int i = 0; i < count; ++i) {
                RECT r{};
                if (!::SendMessageW(wnd, TCM_GETITEMRECT, i, (LPARAM)&r)) continue;
                if (r.bottom > stripBottom) stripBottom = r.bottom;

                const bool on = (i == sel);
                HBRUSH bg = ::CreateSolidBrush(on ? kDark.surfaceAlt : kDark.base);
                ::FillRect(dc, &r, bg);
                ::DeleteObject(bg);

                // Accent strip under the active tab marks it without a frame.
                if (on) {
                    RECT accent{ r.left, r.bottom - dpi::S(3), r.right, r.bottom };
                    HBRUSH a = ::CreateSolidBrush(kDark.accent);
                    ::FillRect(dc, &accent, a);
                    ::DeleteObject(a);
                }

                wchar_t text[64] = {};
                TCITEMW item{};
                item.mask = TCIF_TEXT;
                item.pszText = text;
                item.cchTextMax = 63;
                ::SendMessageW(wnd, TCM_GETITEMW, i, (LPARAM)&item);

                ::SetTextColor(dc, on ? kDark.text : kDark.textDim);
                ::DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }

            // Thin line separating the tab strip from the content.
            if (stripBottom > 0) {
                RECT line{ client.left, stripBottom, client.right, stripBottom + dpi::S(1) };
                HBRUSH l = ::CreateSolidBrush(kDark.line);
                ::FillRect(dc, &line, l);
                ::DeleteObject(l);
            }

            if (oldFont) ::SelectObject(dc, oldFont);
            ::EndPaint(wnd, &ps);
            return 0;
        }

        case WM_NCDESTROY:
            ::RemoveWindowSubclass(wnd, TabSubclass, 1);
            break;
    }
    return ::DefSubclassProc(wnd, msg, wp, lp);
}

/// Interior background colour of a control, so its border can match it.
COLORREF InnerColorOf(const std::wstring& cls) {
    if (IEquals(cls, L"SysListView32") || IEquals(cls, L"SysTreeView32"))
        return kDark.surface;
    return kDark.surfaceAlt;
}

/// Repaints the non-client border of a field.
///
/// WS_EX_CLIENTEDGE reserves two pixels outside the client area, which Windows
/// paints with the light classic bevel. That band is not client area, so no
/// colour message reaches it; WM_NCPAINT is intercepted and it is drawn here.
LRESULT CALLBACK BorderSubclass(HWND wnd, UINT msg, WPARAM wp, LPARAM lp,
                                UINT_PTR, DWORD_PTR) {
    switch (msg) {
        case WM_SETFOCUS:
        case WM_KILLFOCUS: {
            // The accent highlight on the border follows focus, so the border
            // is redrawn when focus enters or leaves.
            const LRESULT r = ::DefSubclassProc(wnd, msg, wp, lp);
            ::RedrawWindow(wnd, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW);
            return r;
        }

        case WM_NCPAINT: {
            // The default handler is deliberately not called: it is what draws
            // the light bevel. Painting over it afterwards leaves the inner
            // line, because the bevel has two layers and the inner one is
            // redrawn by the control theme.
            HDC dc = ::GetWindowDC(wnd);
            if (!dc) return 0;

            RECT box{};
            ::GetWindowRect(wnd, &box);
            ::OffsetRect(&box, -box.left, -box.top);   // window coordinates

            const HWND focus = ::GetFocus();
            const bool hot = focus == wnd || (focus && ::GetParent(focus) == wnd);

            wchar_t cls[64] = {};
            ::GetClassNameW(wnd, cls, 63);

            HBRUSH outer = ::CreateSolidBrush(hot ? kDark.accent : kDark.line);
            ::FrameRect(dc, &box, outer);
            ::DeleteObject(outer);

            // Of the two pixels WS_EX_CLIENTEDGE reserves, the outer becomes
            // the theme line and the inner blends into the field background.
            HBRUSH inner = ::CreateSolidBrush(InnerColorOf(cls));
            ::InflateRect(&box, -1, -1);
            ::FrameRect(dc, &box, inner);
            ::DeleteObject(inner);

            ::ReleaseDC(wnd, dc);
            return 0;
        }

        case WM_NCDESTROY:
            ::RemoveWindowSubclass(wnd, BorderSubclass, 2);
            break;
    }
    return ::DefSubclassProc(wnd, msg, wp, lp);
}

/// Draws the column header of a list view.
///
/// The subclass has to sit on the list rather than on the dialog: the header is
/// a child of the list and sends NM_CUSTOMDRAW there, never to the dialog.
LRESULT CALLBACK ListViewSubclass(HWND wnd, UINT msg, WPARAM wp, LPARAM lp,
                                  UINT_PTR, DWORD_PTR) {
    if (msg == WM_NOTIFY) {
        auto* nm = reinterpret_cast<NMHDR*>(lp);
        if (nm && nm->code == NM_CUSTOMDRAW && IEquals(ClassOf(nm->hwndFrom), L"SysHeader32")) {
            auto* cd = reinterpret_cast<NMCUSTOMDRAW*>(lp);
            if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                const bool pressed = (cd->uItemState & CDIS_SELECTED) != 0;
                HBRUSH bg = ::CreateSolidBrush(pressed ? kDark.hover : kDark.surfaceAlt);
                ::FillRect(cd->hdc, &cd->rc, bg);
                ::DeleteObject(bg);

                // Thin divider on the right in place of the bevelled frame.
                RECT sep{ cd->rc.right - 1, cd->rc.top + dpi::S(4),
                          cd->rc.right, cd->rc.bottom - dpi::S(4) };
                HBRUSH line = ::CreateSolidBrush(kDark.line);
                ::FillRect(cd->hdc, &sep, line);
                ::DeleteObject(line);

                wchar_t text[128] = {};
                HDITEMW item{};
                item.mask = HDI_TEXT;
                item.pszText = text;
                item.cchTextMax = 127;
                ::SendMessageW(nm->hwndFrom, HDM_GETITEMW, cd->dwItemSpec, (LPARAM)&item);

                RECT r = cd->rc;
                r.left += dpi::S(7);
                r.right -= dpi::S(4);
                HFONT font = (HFONT)::SendMessageW(nm->hwndFrom, WM_GETFONT, 0, 0);
                HGDIOBJ old = font ? ::SelectObject(cd->hdc, font) : nullptr;
                ::SetBkMode(cd->hdc, TRANSPARENT);
                ::SetTextColor(cd->hdc, kDark.text);
                ::DrawTextW(cd->hdc, text, -1, &r,
                            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                if (old) ::SelectObject(cd->hdc, old);
                return CDRF_SKIPDEFAULT;
            }
        }
    }
    if (msg == WM_NCDESTROY) ::RemoveWindowSubclass(wnd, ListViewSubclass, 3);
    return ::DefSubclassProc(wnd, msg, wp, lp);
}

/// Enables dark mode at process level.
///
/// Without it the "DarkMode_Explorer" theme has no effect: scroll bars of edits
/// and lists and their sunken frames stay light. The entry point is exported by
/// ordinal only, so it is called on builds that have it (before 17763 ordinal
/// 135 is an unrelated function) and the build number comes from RtlGetVersion,
/// which is not subject to the GetVersionEx compatibility shims.
void EnableProcessDarkMode() {
    static bool done = false;
    if (done) return;
    done = true;

    HMODULE nt = ::GetModuleHandleW(L"ntdll.dll");
    if (!nt) return;
    typedef LONG (WINAPI *PfnRtlGetVersion)(RTL_OSVERSIONINFOW*);
    auto version = (PfnRtlGetVersion)(void*)::GetProcAddress(nt, "RtlGetVersion");
    if (!version) return;

    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (version(&vi) != 0) return;
    if (vi.dwMajorVersion < 10 || vi.dwBuildNumber < 17763) return;

    HMODULE ux = ::LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;

    // Both historical forms take a single int-sized argument
    // (AllowDarkModeForApp(BOOL) in 1809, SetPreferredAppMode(enum) from 1903
    // on), so one call covers both. 2 means force dark.
    typedef int (WINAPI *PfnPreferredAppMode)(int);
    if (auto mode = (PfnPreferredAppMode)(void*)::GetProcAddress(ux, MAKEINTRESOURCEA(135)))
        mode(2);
    typedef void (WINAPI *PfnFlushMenuThemes)();
    if (auto flush = (PfnFlushMenuThemes)(void*)::GetProcAddress(ux, MAKEINTRESOURCEA(136)))
        flush();
}

}  // namespace

void SyncTrackbarVisual(HWND bar) {
    SyncTrackbarVisualImpl(bar);
}

const Palette& Colors() { return kDark; }
bool GlassActive() { return g_glass; }

void InitializeProcess() {
    EnableProcessDarkMode();
}

void SetDimmed(HWND ctl, bool dim) {
    if (!ctl) return;
    if (dim) ::SetPropW(ctl, kDimProp, (HANDLE)1);
    else     ::RemovePropW(ctl, kDimProp);
    ::InvalidateRect(ctl, nullptr, TRUE);
}

HBRUSH BaseBrush() {
    if (!g_base) g_base = ::CreateSolidBrush(kDark.base);
    return g_base;
}
HBRUSH SurfaceBrush() {
    if (!g_surface) g_surface = ::CreateSolidBrush(kDark.surface);
    return g_surface;
}
HBRUSH SurfaceAltBrush() {
    if (!g_surfaceAlt) g_surfaceAlt = ::CreateSolidBrush(kDark.surfaceAlt);
    return g_surfaceAlt;
}

void Shutdown() {
    if (g_base)       { ::DeleteObject(g_base);       g_base = nullptr; }
    if (g_surface)    { ::DeleteObject(g_surface);    g_surface = nullptr; }
    if (g_surfaceAlt) { ::DeleteObject(g_surfaceAlt); g_surfaceAlt = nullptr; }
}

bool ApplyWindowBackdrop(HWND hwnd) {
    InitializeProcess();

    auto set = DwmSet();
    if (!set) return false;

    const BOOL on = TRUE;
    // 20 is the current attribute; 19 applied to early Windows 10 and is
    // ignored on newer builds. Sending both covers all versions without
    // version detection, since the unsupported one only returns an error.
    set(hwnd, kDarkMode, &on, sizeof(on));
    set(hwnd, kDarkModeOld, &on, sizeof(on));

    // Acrylic applies to the frame, the shadow and the title bar; the client
    // area stays opaque deliberately. The compositor only shows the backdrop
    // where the client area was left unpainted, and GDI cannot return an
    // already painted region to a translucent state, so an unpainted
    // background makes every label whose text changes draw over the previous
    // text instead of erasing it.
    const DWORD backdrop = kBackdropAcrylic;
    g_glass = SUCCEEDED(set(hwnd, kSystemBackdrop, &backdrop, sizeof(backdrop)));
    return g_glass;
}

void ApplyToControls(HWND parent) {
    auto theme = WindowTheme();

    struct Ctx { PfnSetWindowTheme theme; };
    Ctx ctx{ theme };

    ::EnumChildWindows(parent, [](HWND child, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        const std::wstring cls = ClassOf(child);

        // "DarkMode_Explorer" swaps scroll bars and inner borders for their
        // dark variants; without it scrollable lists and fields keep a white
        // band in the corner.
        if (c->theme) {
            // Each control family accepts its own dark theme name. The column
            // header is a separate control inside the list and carries its own
            // theme, so it needs a line of its own.
            if (IEquals(cls, L"SysHeader32"))
                c->theme(child, L"DarkMode_ItemsView", nullptr);
            else if (IEquals(cls, L"EDIT") || IEquals(cls, L"ComboBox"))
                c->theme(child, L"DarkMode_CFD", nullptr);
            else if (IEquals(cls, L"SysListView32") || IEquals(cls, L"SysTreeView32") ||
                     IEquals(cls, L"tooltips_class32"))
                c->theme(child, L"DarkMode_Explorer", nullptr);
        }

        // Sunken-edge fields: the light bevel is outside every colour message
        // and has to be repainted on WM_NCPAINT.
        if ((::GetWindowLongW(child, GWL_EXSTYLE) & WS_EX_CLIENTEDGE) != 0)
            ::SetWindowSubclass(child, BorderSubclass, 2, 0);

        if (IEquals(cls, L"SysListView32")) {
            ::SendMessageW(child, LVM_SETBKCOLOR, 0, (LPARAM)kDark.surface);
            ::SendMessageW(child, LVM_SETTEXTBKCOLOR, 0, (LPARAM)kDark.surface);
            ::SendMessageW(child, LVM_SETTEXTCOLOR, 0, (LPARAM)kDark.text);
            ::SetWindowSubclass(child, ListViewSubclass, 3, 0);
        } else if (IEquals(cls, L"msctls_trackbar32")) {
            ::SetWindowSubclass(child, TrackbarSubclass, 4, 0);
        } else if (IEquals(cls, L"msctls_statusbar32")) {
            ::SendMessageW(child, SB_SETBKCOLOR, 0, (LPARAM)kDark.base);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
}

bool HandleColorMessage(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result) {
    switch (msg) {
        case WM_ERASEBKGND: {
            RECT r{};
            ::GetClientRect(wnd, &r);
            ::FillRect((HDC)wp, &r, BaseBrush());
            *result = 1;
            return true;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC dc = (HDC)wp;
            const HWND ctl = (HWND)lp;
            const bool dim = !::IsWindowEnabled(ctl) || ::GetPropW(ctl, kDimProp) != nullptr;
            ::SetTextColor(dc, dim ? kDark.textDim : kDark.text);
            // Opaque background, not transparent. With a hollow brush a static
            // stops erasing its own background, and text that changes, such as
            // slider values during a drag, is drawn over the previous text.
            ::SetBkColor(dc, kDark.base);
            ::SetBkMode(dc, OPAQUE);
            *result = (LRESULT)BaseBrush();
            return true;
        }

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC dc = (HDC)wp;
            ::SetTextColor(dc, kDark.text);
            ::SetBkColor(dc, kDark.surfaceAlt);
            *result = (LRESULT)SurfaceAltBrush();
            return true;
        }
    }
    return false;
}

bool HandleCustomDraw(LPARAM lp, LRESULT* result) {
    auto* cd = reinterpret_cast<NMCUSTOMDRAW*>(lp);
    if (!cd || cd->hdr.code != NM_CUSTOMDRAW) return false;

    const std::wstring cls = ClassOf(cd->hdr.hwndFrom);
    if (IEquals(cls, L"Button"))          return DrawButton(cd, result);
    if (IEquals(cls, L"msctls_trackbar32")) return DrawTrackbar(cd, result);
    return false;
}

bool HandleDrawItem(const DRAWITEMSTRUCT* dis) {
    if (!dis || dis->CtlType != ODT_LISTBOX) return false;
    // itemID == -1 marks an empty list; clearing the background avoids leftover
    // pixels.
    if (dis->itemID == (UINT)-1) {
        ::FillRect(dis->hDC, &dis->rcItem, SurfaceAltBrush());
        return true;
    }

    const bool selected = (dis->itemState & ODS_SELECTED) != 0;
    HBRUSH bg = ::CreateSolidBrush(selected ? kDark.accent : kDark.surfaceAlt);
    ::FillRect(dis->hDC, &dis->rcItem, bg);
    ::DeleteObject(bg);

    wchar_t text[256] = {};
    if (::SendMessageW(dis->hwndItem, LB_GETTEXTLEN, dis->itemID, 0) < 255)
        ::SendMessageW(dis->hwndItem, LB_GETTEXT, dis->itemID, (LPARAM)text);

    RECT r = dis->rcItem;
    r.left += dpi::S(6);
    ::SetBkMode(dis->hDC, TRANSPARENT);
    ::SetTextColor(dis->hDC, selected ? RGB(255, 255, 255) : kDark.text);
    ::DrawTextW(dis->hDC, text, -1, &r,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    return true;
}

void SubclassTabControl(HWND tabs) {
    if (tabs) ::SetWindowSubclass(tabs, TabSubclass, 1, 0);
}

}  // namespace theme
}  // namespace zdisplay
