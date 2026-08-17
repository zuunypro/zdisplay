// The panel's own colour registers: RGB gain (VCP 0x16/0x18/0x1A) and
// saturation (0x8A).
//
// These reach the monitor over the video cable, so they cost no tonal range and
// survive anything that resets the gamma ramp. They live in a window of their
// own because the Adjustments tab already runs to the bottom of a frame sized
// for a 1366x768 laptop, and because they carry a condition none of the other
// controls do: nearly every panel accepts a gain write only while its colour
// preset is the user one.
//
// The window is modal against the settings window. The values belong to the
// profile and monitor selected there, and blocking that selection while the
// window is open is what keeps the sliders from editing something else halfway
// through a drag.
#include "ui.h"
#include "ui_ids.h"
#include "ui_dpi.h"
#include "ui_theme.h"

namespace zdisplay {

namespace {

const wchar_t* kColorClass = L"ZdisplayMonitorColorWindow";
using dpi::S;

/// The three gain rows, in the order they are laid out. Saturation follows them
/// but is a group of its own, so it is not part of this table.
constexpr AdjField kGainFields[3] = { F_HWRGAIN, F_HWGGAIN, F_HWBGAIN };

void SetText(HWND h, const std::wstring& s) {
    if (h) ::SetWindowTextW(h, s.c_str());
}

/// The panel whose own values the sliders start from: the one selected, or the
/// first that answers when the selection is every monitor at once.
std::wstring SeedMonitor(DdcciBackend* ddc, const std::wstring& selected) {
    if (!selected.empty() || !ddc) return selected;
    for (const auto& m : monitors::All())
        if (ddc->SupportsGain(m.key) || ddc->SupportsSaturation(m.key)) return m.key;
    return std::wstring();
}

/// MCCS colour presets 0x0B, 0x0C and 0x0D are the user ones. They are the only
/// presets under which a panel keeps a gain the host writes; every other preset
/// answers the command and holds its own factory values.
bool IsUserPreset(int value) { return value >= 0x0B && value <= 0x0D; }

}  // namespace

void App::ShowMonitorColor() {
    if (colorWnd_ && ::IsWindow(colorWnd_)) {
        ::SetForegroundWindow(colorWnd_);
        return;
    }
    if (!settings_ || !::IsWindow(settings_)) return;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ColorProc;
        wc.hInstance = inst_;
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = theme::BaseBrush();
        wc.lpszClassName = kColorClass;
        wc.hIcon = icon_;
        wc.hIconSm = icon_;
        if (!::RegisterClassExW(&wc)) return;
        registered = true;
    }

    colorMonitorKey_ = SelectedMonitorKey();

    // Sized from the client area at the DPI of the settings window, which is
    // the window this one is anchored to.
    dpi::DetectFor(settings_);
    const int cw = S(470);
    const int ch = S(366);

    RECT want{0, 0, cw, ch};
    const DWORD style = WS_POPUPWINDOW | WS_CAPTION | WS_CLIPCHILDREN;
    ::AdjustWindowRectEx(&want, style, FALSE, 0);
    const int w = want.right - want.left;
    const int h = want.bottom - want.top;

    RECT parent{};
    ::GetWindowRect(settings_, &parent);
    const RECT work = dpi::WorkAreaForCursor();
    int x = parent.left + ((parent.right - parent.left) - w) / 2;
    int y = parent.top + ((parent.bottom - parent.top) - h) / 3;
    x = Clamp(x, (int)work.left, (int)(std::max)((LONG)work.left, work.right - w));
    y = Clamp(y, (int)work.top, (int)(std::max)((LONG)work.top, work.bottom - h));

    colorWnd_ = ::CreateWindowExW(0, kColorClass, T(L"Monitor colour"), style,
                                  x, y, w, h, settings_, nullptr, inst_, nullptr);
    if (!colorWnd_) return;
    theme::ApplyWindowBackdrop(colorWnd_);

    const int mx = S(16);
    const int rowW = cw - mx * 2;
    int y0 = S(12);

    colorStatus_ = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     mx, y0, rowW, S(34), colorWnd_, nullptr, inst_, nullptr);
    y0 += S(38);
    // Three lines tall: the line naming the preset the monitor is actually on
    // is the longest text in this window.
    colorPresetNote_ = ::CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT, mx, y0, rowW, S(50),
        colorWnd_, nullptr, inst_, nullptr);
    y0 += S(58);

    colorGainCheck_ = ::CreateWindowExW(0, L"BUTTON", T(L"Control the monitor's RGB gain"),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        mx, y0, rowW, S(20), colorWnd_, (HMENU)(INT_PTR)IDC_HWCOLOR_GAIN, inst_, nullptr);
    y0 += S(26);

    const wchar_t* gainNames[3] = { T(L"Red gain"), T(L"Green gain"), T(L"Blue gain") };
    for (int c = 0; c < 3; ++c) {
        sliders_[kGainFields[c]].Create(colorWnd_, gainNames[c], kGainFields[c],
                                        mx, y0, rowW, 0, 100, 50, 1, L"%", 0,
                                        IDC_SLIDER_RESET_BASE + kGainFields[c]);
        y0 += S(32);
    }
    y0 += S(8);

    colorSatCheck_ = ::CreateWindowExW(0, L"BUTTON", T(L"Control the monitor's saturation"),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        mx, y0, rowW, S(20), colorWnd_, (HMENU)(INT_PTR)IDC_HWCOLOR_SAT, inst_, nullptr);
    y0 += S(26);
    sliders_[F_HWSAT].Create(colorWnd_, T(L"Saturation"), F_HWSAT,
                             mx, y0, rowW, 0, 100, 50, 1, L"%", 0,
                             IDC_SLIDER_RESET_BASE + F_HWSAT);
    y0 += S(40);

    ::CreateWindowExW(0, L"BUTTON", T(L"Close"),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        cw - mx - S(110), y0, S(110), S(28), colorWnd_,
        (HMENU)(INT_PTR)IDC_HWCOLOR_CLOSE, inst_, nullptr);

    // Font at the DPI the controls were laid out with; font_ follows the system
    // DPI and overflows them on a mixed-DPI setup.
    if (colorFont_) { ::DeleteObject(colorFont_); colorFont_ = nullptr; }
    colorFont_ = dpi::MessageFontFor(dpi::Current());
    ::EnumChildWindows(colorWnd_, [](HWND child, LPARAM fp) -> BOOL {
        ::SendMessageW(child, WM_SETFONT, (WPARAM)fp, TRUE);
        return TRUE;
    }, (LPARAM)(colorFont_ ? colorFont_ : font_));

    for (int i = F_HWCOLOR_FIRST; i < F_COUNT; ++i) sliders_[i].Show(true);

    LoadMonitorColor();

    // Modal by hand: the settings window stays visible, since the sliders here
    // are read against the profile and monitor shown there.
    ::EnableWindow(settings_, FALSE);
    ::ShowWindow(colorWnd_, SW_SHOW);
    ::SetForegroundWindow(colorWnd_);
}

void App::CloseMonitorColor() {
    if (!colorWnd_) return;
    HWND wnd = colorWnd_;
    colorWnd_ = nullptr;

    if (settings_ && ::IsWindow(settings_)) {
        ::EnableWindow(settings_, TRUE);
        ::SetForegroundWindow(settings_);
    }
    if (::IsWindow(wnd)) ::DestroyWindow(wnd);

    for (int i = F_HWCOLOR_FIRST; i < F_COUNT; ++i) sliders_[i] = SliderRow{};
    colorGainCheck_ = colorSatCheck_ = nullptr;
    colorStatus_ = colorPresetNote_ = nullptr;
    colorMonitorKey_.clear();
    if (colorFont_) { ::DeleteObject(colorFont_); colorFont_ = nullptr; }
}

void App::LoadMonitorColor() {
    if (!colorWnd_ || !::IsWindow(colorWnd_)) return;
    Adjustments* a = CurrentAdjustments();
    if (!a) return;

    const bool wasLoading = loadingUi_;
    loadingUi_ = true;

    // Capability is per monitor and per register. With every monitor selected,
    // a register counts as available when at least one panel answers it, the
    // same rule the physical brightness follows.
    bool gainOk = false, satOk = false;
    if (!colorMonitorKey_.empty()) {
        gainOk = engine_->Ddc()->SupportsGain(colorMonitorKey_);
        satOk  = engine_->Ddc()->SupportsSaturation(colorMonitorKey_);
    } else {
        for (const auto& m : monitors::All()) {
            gainOk = gainOk || engine_->Ddc()->SupportsGain(m.key);
            satOk  = satOk  || engine_->Ddc()->SupportsSaturation(m.key);
        }
    }

    const std::wstring seedKey = SeedMonitor(engine_->Ddc(), colorMonitorKey_);

    const double values[F_COUNT - F_HWCOLOR_FIRST] = {
        a->hwRedGain, a->hwGreenGain, a->hwBlueGain, a->hwSaturation
    };
    for (int i = F_HWCOLOR_FIRST; i < F_COUNT; ++i) {
        // An unmanaged register shows the value the panel itself is carrying,
        // so switching the group on is a no-op until a slider is moved.
        double shown = values[i - F_HWCOLOR_FIRST];
        if (shown < 0) {
            const int own = i == F_HWSAT
                ? engine_->Ddc()->OriginalSaturation(seedKey)
                : engine_->Ddc()->OriginalGain(seedKey, i - F_HWCOLOR_FIRST);
            shown = own >= 0 ? (double)own : 50.0;
        }
        sliders_[i].Set(shown);
    }

    const bool gainOn = a->hwRedGain >= 0 || a->hwGreenGain >= 0 || a->hwBlueGain >= 0;
    const bool satOn  = a->hwSaturation >= 0;
    ::SendMessageW(colorGainCheck_, BM_SETCHECK, gainOn ? BST_CHECKED : BST_UNCHECKED, 0);
    ::SendMessageW(colorSatCheck_,  BM_SETCHECK, satOn  ? BST_CHECKED : BST_UNCHECKED, 0);
    ::EnableWindow(colorGainCheck_, gainOk);
    ::EnableWindow(colorSatCheck_, satOk);
    for (int c = 0; c < 3; ++c) sliders_[kGainFields[c]].Enable(gainOk && gainOn);
    sliders_[F_HWSAT].Enable(satOk && satOn);

    const MonitorTarget* target = colorMonitorKey_.empty()
        ? nullptr : monitors::ByKey(colorMonitorKey_);
    Profile* p = EditingProfile();
    std::wstring status = Format(T(L"Profile '%s' on %s."),
                                 p ? p->name.c_str() : L"-",
                                 target ? target->friendlyName.c_str()
                                        : T(L"every monitor"));
    // Worded without naming the monitor: the same line serves the selection of
    // every monitor at once, where more than one panel is being described.
    if (!gainOk && !satOk)
        status += T(L"  Neither RGB gain nor saturation is available here.");
    else if (!gainOk)
        status += T(L"  Only saturation is available here.");
    else if (!satOk)
        status += T(L"  Only RGB gain is available here.");
    SetText(colorStatus_, status);

    // The preset the panel is actually on, when the feature probe has already
    // read it. Naming it turns the warning above the sliders from a rule the
    // user has to check by hand into a statement about this monitor.
    std::wstring note = T(L"A monitor applies these only while its colour preset is the user "
                          L"one. Set that preset first, on the panel or in Monitor commands.");
    if (gainOk && !colorMonitorKey_.empty() &&
        engine_->Ddc()->FeaturesProbed(colorMonitorKey_)) {
        for (const auto& f : engine_->Ddc()->Features(colorMonitorKey_)) {
            if (f.code != kVcpColorPreset || f.current < 0) continue;
            if (IsUserPreset(f.current)) {
                note = T(L"The monitor is on a user colour preset, so it keeps what is "
                         L"written here.");
            } else {
                std::wstring name = VcpValueName(kVcpColorPreset, (unsigned char)f.current);
                if (name.empty()) name = Format(T(L"value 0x%02X"), (unsigned)f.current);
                note = Format(T(L"The monitor is on the '%s' preset and will hold its own "
                                L"values: switch it to a user preset, under Monitor "
                                L"commands, before these sliders have any effect."),
                              name.c_str());
            }
            break;
        }
    }
    SetText(colorPresetNote_, note);

    loadingUi_ = wasLoading;
}

void App::ToggleMonitorColorGroup(bool gain) {
    Adjustments* a = CurrentAdjustments();
    if (!a) return;

    const bool on = ::SendMessageW(gain ? colorGainCheck_ : colorSatCheck_,
                                   BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (gain) {
        for (int c = 0; c < 3; ++c) {
            // Switching the group on adopts what the sliders already show, which
            // LoadMonitorColor seeded with the panel's own values.
            *FieldPtr(*a, kGainFields[c]) = on ? sliders_[kGainFields[c]].Get() : -1;
            sliders_[kGainFields[c]].Enable(on);
        }
    } else {
        a->hwSaturation = on ? sliders_[F_HWSAT].Get() : -1;
        sliders_[F_HWSAT].Enable(on);
    }

    ApplyLive();

    // Turning a group off gives the monitor its own values back. That is the
    // backend's job: it holds what the registers carried before Zdisplay wrote
    // to them, and only it knows whether anything was ever written.
    //
    // After ApplyLive and not before: applying REPLACES what is queued for the
    // panel, and a profile that no longer manages these registers says nothing
    // about them, so a restore queued first would be dropped and the monitor
    // would keep the last value written.
    if (!on && engine_) engine_->Ddc()->RestoreColor(colorMonitorKey_, gain);
    MarkDirty();
}

LRESULT CALLBACK App::ColorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* self = App::Get();
    if (!self) return ::DefWindowProcW(hwnd, msg, wp, lp);
    return self->OnColorMessage(hwnd, msg, wp, lp);
}

LRESULT App::OnColorMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            // IsDialogMessage turns Esc into IDCANCEL and Enter into the default
            // button; both close the window, which is its only exit.
            if (id == IDCANCEL || id == IDC_HWCOLOR_CLOSE) {
                ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
            if (loadingUi_) return 0;
            if (id == IDC_HWCOLOR_GAIN) { ToggleMonitorColorGroup(true); return 0; }
            if (id == IDC_HWCOLOR_SAT)  { ToggleMonitorColorGroup(false); return 0; }

            // Per-slider reset: back to the value the panel itself carried.
            if (id >= IDC_SLIDER_RESET_BASE + F_HWCOLOR_FIRST &&
                id < IDC_SLIDER_RESET_BASE + F_COUNT) {
                const AdjField f = (AdjField)(id - IDC_SLIDER_RESET_BASE);
                Adjustments* a = CurrentAdjustments();
                if (!a) return 0;
                const std::wstring key = SeedMonitor(engine_->Ddc(), colorMonitorKey_);
                const int own = f == F_HWSAT
                    ? engine_->Ddc()->OriginalSaturation(key)
                    : engine_->Ddc()->OriginalGain(key, (int)f - F_HWCOLOR_FIRST);
                sliders_[f].Set(own >= 0 ? (double)own : 50.0);
                if (*FieldPtr(*a, f) >= 0) {
                    *FieldPtr(*a, f) = sliders_[f].Get();
                    ApplyLive();
                    MarkDirty();
                }
                return 0;
            }
            return 0;
        }

        case WM_HSCROLL:
            if (lp) OnSlider((HWND)lp, LOWORD(wp) == TB_THUMBTRACK);
            return 0;

        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(lp);
            if (hdr && hdr->code == NM_CUSTOMDRAW) {
                LRESULT r = 0;
                if (theme::HandleCustomDraw(lp, &r)) return r;
            }
            break;
        }

        case WM_ERASEBKGND:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLOREDIT: {
            LRESULT r = 0;
            if (theme::HandleColorMessage(hwnd, msg, wp, lp, &r)) return r;
            break;
        }

        case WM_DRAWITEM:
            if (theme::HandleDrawItem(reinterpret_cast<const DRAWITEMSTRUCT*>(lp)))
                return TRUE;
            break;

        case WM_CLOSE:
            CloseMonitorColor();
            return 0;

        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace zdisplay
