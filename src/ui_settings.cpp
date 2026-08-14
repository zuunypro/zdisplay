// Settings window in plain Win32. Created on demand and destroyed on close, so
// that at rest the program is nothing but the tray icon.
#include "ui.h"
#include "ui_ids.h"
#include "ui_dpi.h"
#include "ui_theme.h"
#include "version.h"

namespace zdisplay {

namespace {

const wchar_t* kSettingsClass = L"ZdisplaySettingsWindow";

using dpi::S;

void SetText(HWND h, const std::wstring& s) {
    if (h) ::SetWindowTextW(h, s.c_str());
}

void SetChecked(HWND h, bool on) {
    if (h) ::SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

int ComboIndex(HWND h) {
    return h ? (int)::SendMessageW(h, CB_GETCURSEL, 0, 0) : -1;
}

std::wstring ComboText(HWND h) {
    const int i = ComboIndex(h);
    if (i < 0) return L"";
    const int len = (int)::SendMessageW(h, CB_GETLBTEXTLEN, i, 0);
    if (len <= 0) return L"";
    std::wstring s((size_t)len, L'\0');
    ::SendMessageW(h, CB_GETLBTEXT, i, (LPARAM)&s[0]);
    return s;
}

void ComboSelectText(HWND h, const std::wstring& text) {
    if (!h) return;
    const int n = (int)::SendMessageW(h, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < n; ++i) {
        const int len = (int)::SendMessageW(h, CB_GETLBTEXTLEN, i, 0);
        if (len <= 0) continue;
        std::wstring s((size_t)len, L'\0');
        ::SendMessageW(h, CB_GETLBTEXT, i, (LPARAM)&s[0]);
        if (IEquals(s, text)) { ::SendMessageW(h, CB_SETCURSEL, i, 0); return; }
    }
    if (n > 0) ::SendMessageW(h, CB_SETCURSEL, 0, 0);
}

SYSTEMTIME MinutesLater(const SYSTEMTIME& value, int minutes) {
    FILETIME ft{};
    if (!::SystemTimeToFileTime(&value, &ft)) return value;
    ULARGE_INTEGER ticks{};
    ticks.LowPart = ft.dwLowDateTime;
    ticks.HighPart = ft.dwHighDateTime;
    ticks.QuadPart += (ULONGLONG)minutes * 60ULL * 10000000ULL;
    ft.dwLowDateTime = ticks.LowPart;
    ft.dwHighDateTime = ticks.HighPart;
    SYSTEMTIME out{};
    return ::FileTimeToSystemTime(&ft, &out) ? out : value;
}

}  // namespace

// Fields

double* FieldPtr(Adjustments& a, AdjField f) {
    switch (f) {
        case F_BRIGHT:      return &a.brightness;
        case F_CONTRAST:    return &a.contrast;
        case F_GAMMA:       return &a.gamma;
        case F_TEMP:        return &a.temperature;
        case F_SHADOWS:     return &a.shadows;
        case F_CLARITY:     return &a.clarity;
        case F_SAT:         return &a.saturation;
        case F_VIB:         return &a.vibrance;
        case F_HUE:         return &a.hue;
        case F_DIM:         return &a.dim;
        case F_RGAIN:       return &a.redGain;
        case F_GGAIN:       return &a.greenGain;
        case F_BGAIN:       return &a.blueGain;
        case F_BLUEBLOCK:   return &a.blueBlock;
        case F_HWBRIGHT:    return &a.hwBrightness;
        case F_HWCONTRAST:  return &a.hwContrast;
        default:            return &a.brightness;
    }
}

// Slider row

void SliderRow::Create(HWND parent, const wchar_t* caption, AdjField f,
                       int x, int y, int width,
                       double minV, double maxV, double defV,
                       double scaleFactor, const wchar_t* suffixText, int decimalPlaces,
                       int resetId) {
    field = f;
    scale = scaleFactor;
    defValue = defV;
    suffix = suffixText;
    decimals = decimalPlaces;

    HINSTANCE inst = (HINSTANCE)::GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    const int labelW = S(120);
    const int valueW = S(66);
    const int resetW = S(24);
    const int barW = width - labelW - valueW - resetW - S(14);

    label = ::CreateWindowExW(0, L"STATIC", caption, WS_CHILD | SS_LEFT,
                              x, y + S(5), labelW, S(18), parent, nullptr, inst, nullptr);

    bar = ::CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                            WS_CHILD | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                            x + labelW, y, barW, S(26), parent, nullptr, inst, nullptr);
    const int lo = (int)llround(minV * scale);
    const int hi = (int)llround(maxV * scale);
    ::SendMessageW(bar, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));

    // Step sizes derive from the range WIDTH, not from the scale factor. Scale
    // varies per slider, so a scale-derived step covers the whole range on a
    // slider such as gamma, which uses scale 100 over a span of 270 units.
    const int span = (std::max)(1, hi - lo);
    const int line = (std::max)(1, span / 100);   // arrow key: 1% of the range
    const int page = (std::max)(line * 2, span / 20);  // Page Up: 5% of the range
    ::SendMessageW(bar, TBM_SETPAGESIZE, 0, (LPARAM)(LONG)page);
    ::SendMessageW(bar, TBM_SETLINESIZE, 0, (LPARAM)(LONG)line);

    value = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_RIGHT,
                              x + labelW + barW + S(6), y + S(5), valueW, S(18),
                              parent, nullptr, inst, nullptr);

    reset = ::CreateWindowExW(0, L"BUTTON", L"↺", WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                              x + width - resetW, y + S(2), resetW, S(22),
                              parent, (HMENU)(INT_PTR)resetId, inst, nullptr);

    Set(defV);
}

double SliderRow::Get() const {
    if (!bar) return defValue;
    return (double)::SendMessageW(bar, TBM_GETPOS, 0, 0) / scale;
}

void SliderRow::Set(double v) {
    if (!bar) return;
    // The native partial redraw only knows the thumb, not the owner-drawn fill
    // painted up to the current position. The position is set without painting
    // and the whole bar is invalidated so the frame stays coherent.
    ::SendMessageW(bar, TBM_SETPOS, FALSE, (LPARAM)(LONG)llround(v * scale));
    theme::SyncTrackbarVisual(bar);
    ::InvalidateRect(bar, nullptr, FALSE);
    UpdateValueLabel();
}

void SliderRow::UpdateValueLabel() {
    if (!value) return;
    wchar_t buf[64];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%.*f%s", decimals, Get(), suffix);
    ::SetWindowTextW(value, buf);
}

void SliderRow::Enable(bool on) {
    ::EnableWindow(bar, on);
    ::EnableWindow(reset, on);

    // Labels stay enabled on purpose. Windows draws a disabled static embossed,
    // painting the text twice — light offset over dark — which is meant for a
    // light grey background and smears over a dark one. The row is marked dimmed
    // instead, and the theme paints it in the secondary color.
    theme::SetDimmed(label, !on);
    theme::SetDimmed(value, !on);
}

void SliderRow::Show(bool on) {
    const int cmd = on ? SW_SHOW : SW_HIDE;
    ::ShowWindow(label, cmd);
    ::ShowWindow(bar, cmd);
    ::ShowWindow(value, cmd);
    ::ShowWindow(reset, cmd);
}

// Window creation

void App::ShowSettings() {
    if (settings_ && ::IsWindow(settings_)) {
        ::ShowWindow(settings_, SW_SHOW);
        if (::IsIconic(settings_)) ::ShowWindow(settings_, SW_RESTORE);
        ::SetForegroundWindow(settings_);
        return;
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = SettingsProc;
        wc.hInstance = inst_;
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        // The dark brush belongs to the class, which matters before the first
        // WM_PAINT: DWM can compose the freshly created surface between
        // ShowWindow and the children painting, and a null brush leaves that
        // surface white for one frame.
        wc.hbrBackground = theme::BaseBrush();
        wc.lpszClassName = kSettingsClass;
        wc.hIcon = icon_;
        wc.hIconSm = icon_;
        if (!::RegisterClassExW(&wc)) {
            KLOG_E(L"Could not register the settings window class.");
            return;
        }
        registered = true;
    }

    // Created hidden so the DPI of the screen it will appear on can be measured.
    // WS_EX_COMPOSITED is omitted on purpose: the double buffering it enables
    // does not work with owner-draw controls, which is nearly every control
    // here, and leaves the window blank. Flicker is handled instead by
    // WS_CLIPCHILDREN and by suspending redraw in ShowTab.
    settings_ = ::CreateWindowExW(
        0, kSettingsClass, T(L"Zdisplay — settings"),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
        nullptr, nullptr, inst_, nullptr);

    if (!settings_) {
        KLOG_E(L"Could not create the settings window (error %lu).", ::GetLastError());
        return;
    }

    // Placed on the monitor holding the cursor, at that monitor's DPI and inside
    // its work area. Centering by SM_CXSCREEN would always target the primary
    // monitor and would ignore the taskbar.
    dpi::Current() = dpi::ForCursor();
    RecreateUiFonts();
    theme::ApplyWindowBackdrop(settings_);

    // 660, not 620: the System tab grew by the language and performance rows,
    // and its left column runs to y=612. The window is still short enough that
    // the frame fits the work area of a 1366x768 laptop, which is the smallest
    // screen this is meant for.
    RECT want{0, 0, S(880), S(660)};
    ::AdjustWindowRectEx(&want, (DWORD)::GetWindowLongPtrW(settings_, GWL_STYLE), FALSE, 0);
    const int w = want.right - want.left;
    const int h = want.bottom - want.top;
    const RECT work = dpi::WorkAreaForCursor();
    int x = work.left + ((work.right - work.left) - w) / 2;
    int y = work.top + ((work.bottom - work.top) - h) / 2;
    x = Clamp(x, (int)work.left, (int)(std::max)((LONG)work.left, work.right - w));
    y = Clamp(y, (int)work.top, (int)(std::max)((LONG)work.top, work.bottom - h));
    ::SetWindowPos(settings_, nullptr, x, y, w, h, SWP_NOZORDER);

    CreateSettingsControls(settings_);
    ReloadAll();
    ShowTab(0);

    settingsBuilt_ = true;

    ::ShowWindow(settings_, SW_SHOW);
    // ShowWindow only schedules the first paint. The dark frame is completed
    // within this call, before control returns to the compositor, so the window
    // is never visible carrying the initial white surface.
    ::RedrawWindow(settings_, nullptr, nullptr,
                   RDW_INVALIDATE | RDW_ERASE | RDW_FRAME |
                   RDW_ALLCHILDREN | RDW_UPDATENOW);
    ::SetForegroundWindow(settings_);
}

void App::RebuildSettingsForDpi() {
    pendingDpiChange_ = false;
    if (!settings_ || !::IsWindow(settings_)) return;

    const int tab = tabs_ ? (int)::SendMessageW(tabs_, TCM_GETCURSEL, 0, 0) : 0;
    CommitProfileEditor();          // keep an edit in progress
    const RECT r = pendingDpiRect_;

    // Recreation is the relayout: the window is destroyed (WM_DESTROY clears the
    // members) and ShowSettings rebuilds everything at the DPI of the monitor it
    // moved to, since the cursor is there along with the window.
    HWND old = settings_;
    settings_ = nullptr;
    settingsBuilt_ = false;
    ::DestroyWindow(old);
    ShowSettings();

    if (settings_ && tabs_ && tab > 0) {
        ::SendMessageW(tabs_, TCM_SETCURSEL, tab, 0);
        ShowTab(tab);
    }
    // Move back to the position Windows suggested; the new size already comes
    // from ShowSettings at the correct DPI.
    if (settings_ && r.right > r.left)
        ::SetWindowPos(settings_, nullptr, r.left, r.top, 0, 0,
                       SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
}

LRESULT CALLBACK App::SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* self = App::Get();
    if (self) return self->OnSettingsMessage(hwnd, msg, wp, lp);
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// Helpers

namespace {

struct Maker {
    HWND parent;
    HINSTANCE inst;
    HFONT font;
    HFONT fontBold;
    std::vector<HWND>* sink;

    HWND Add(const wchar_t* cls, const wchar_t* text, DWORD style,
             int x, int y, int w, int h, int id = 0, DWORD ex = 0, bool bold = false) const {
        HWND c = ::CreateWindowExW(ex, cls, text, WS_CHILD | style,
                                   x, y, w, h, parent, (HMENU)(INT_PTR)id, inst, nullptr);
        if (c) {
            ::SendMessageW(c, WM_SETFONT, (WPARAM)(bold ? fontBold : font), TRUE);
            if (sink) sink->push_back(c);
        }
        return c;
    }

    HWND Label(const wchar_t* text, int x, int y, int w, int h = 0) const {
        return Add(L"STATIC", text, SS_LEFT, x, y, w, h ? h : S(18));
    }
    HWND Section(const wchar_t* text, int x, int y, int w) const {
        return Add(L"STATIC", text, SS_LEFT, x, y, w, S(20), 0, 0, true);
    }
    HWND Hint(const wchar_t* text, int x, int y, int w, int h) const {
        return Add(L"STATIC", text, SS_LEFT, x, y, w, h);
    }
    HWND Button(const wchar_t* text, int id, int x, int y, int w, int h) const {
        return Add(L"BUTTON", text, WS_TABSTOP | BS_PUSHBUTTON, x, y, w, h, id);
    }
    HWND Check(const wchar_t* text, int id, int x, int y, int w) const {
        return Add(L"BUTTON", text, WS_TABSTOP | BS_AUTOCHECKBOX, x, y, w, S(20), id);
    }
    HWND Edit(int id, int x, int y, int w, DWORD extra = 0) const {
        return Add(L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | extra,
                   x, y, w, S(22), id, WS_EX_CLIENTEDGE);
    }
    HWND Combo(int id, int x, int y, int w, int dropH = 240) const {
        return Add(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                   x, y, w, S(dropH), id);
    }
    /// Drop-down that also accepts typed text. Used where the list is a
    /// convenience rather than a constraint: the process name, for instance,
    /// accepts a wildcard ('cs*') and programs that are not running.
    HWND ComboEdit(int id, int x, int y, int w, int dropH = 240) const {
        return Add(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL,
                   x, y, w, S(dropH), id);
    }
    HWND List(int id, int x, int y, int w, int h) const {
        // LBS_OWNERDRAWFIXED: the selected row of a list box takes the system
        // highlight color and no color message reaches it. Drawing each row
        // lets the selection use the theme color instead.
        // LBS_NOINTEGRALHEIGHT: without it the list box resizes itself to fit a
        // whole number of items and shrinks to the height of its rows instead of
        // filling the column.
        return Add(L"LISTBOX", L"", WS_TABSTOP | WS_BORDER | WS_VSCROLL |
                   LBS_NOTIFY | LBS_HASSTRINGS | LBS_OWNERDRAWFIXED |
                   LBS_NOINTEGRALHEIGHT,
                   x, y, w, h, id, WS_EX_CLIENTEDGE);
    }
    HWND ListView(int id, int x, int y, int w, int h) const {
        HWND c = Add(WC_LISTVIEWW, L"", WS_TABSTOP | WS_BORDER | LVS_REPORT |
                     LVS_SINGLESEL | LVS_SHOWSELALWAYS, x, y, w, h, id, WS_EX_CLIENTEDGE);
        if (c) ListView_SetExtendedListViewStyle(c, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES);
        return c;
    }
};

// Press-and-hold subclass for the Compare button. BN_PUSHED / BN_UNPUSHED are
// 16-bit-era notifications kept only for compatibility and do not arrive under
// visual styles v6 with owner-draw theming, so mouse and keyboard messages are
// read directly.
LRESULT CALLBACK CompareProc(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                             UINT_PTR id, DWORD_PTR) {
    const auto preview = [](bool on) {
        if (App* app = App::Get()) app->GetEngine().PreviewOriginal(on);
    };
    switch (msg) {
        case WM_LBUTTONDOWN:
            preview(true);
            break;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:   // dragged outside and released
        case WM_KILLFOCUS:        // Alt+Tab while held
            preview(false);
            break;
        // The button is also reachable by Tab, where space is the press. Bit 30
        // of lParam marks an auto-repeat, which would otherwise send one
        // preview(true) per keyboard repeat.
        case WM_KEYDOWN:
            if (wp == VK_SPACE && !(lp & 0x40000000)) preview(true);
            break;
        case WM_KEYUP:
            if (wp == VK_SPACE) preview(false);
            break;
        case WM_NCDESTROY:
            preview(false);
            ::RemoveWindowSubclass(h, CompareProc, id);
            break;
        default:
            break;
    }
    return ::DefSubclassProc(h, msg, wp, lp);
}

void AddColumn(HWND lv, int index, const wchar_t* title, int width) {
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.pszText = const_cast<wchar_t*>(title);
    col.cx = width;
    col.iSubItem = index;
    ListView_InsertColumn(lv, index, &col);
}

}  // namespace

void App::AddTip(HWND control, const wchar_t* text) {
    if (!tooltip_ || !control) return;
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = settings_;
    ti.uId = (UINT_PTR)control;
    ti.lpszText = const_cast<wchar_t*>(text);
    ::SendMessageW(tooltip_, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

void App::AddTip(int id, const wchar_t* text) {
    AddTip(::GetDlgItem(settings_, id), text);
}

void App::FactoryReset() {
    // The screen is restored before anything else, so a failure part way through
    // leaves the plain Windows screen rather than an orphaned adjustment that no
    // profile claims any more.
    engine_->ResetAll();

    const std::wstring path = ConfigPath();

    // A copy is kept before deleting: a factory reset need not be irreversible,
    // and the cost is a file of a few kilobytes.
    ::CopyFileW(path.c_str(), (path + L".before-reset").c_str(), FALSE);

    // Config{} restores every field to the value written in the struct
    // declaration and SeedDefaults() recreates the shipped profiles. Engine
    // holds a pointer to config_ rather than a copy, so assigning here is
    // enough: the address does not change.
    config_ = Config{};
    config_.SeedDefaults();

    // Start-with-Windows lives in the registry, outside the config file, so it
    // survives the reset unless it is cleared explicitly.
    startup::Set(config_.startWithWindows);

    SaveConfig(config_);

    // Leftovers that would revive the old configuration at the next start:
    // LoadConfig falls back to the .bak when the main file yields nothing. This
    // has to run AFTER SaveConfig, because deleting first lets SaveConfig
    // recreate the .bak from the old file.
    ::DeleteFileW((path + L".bak").c_str());
    ::DeleteFileW((path + L".invalid").c_str());

    // Engine and interface start over. The manual profile is cleared first
    // because it points at a name that probably no longer exists.
    engine_->ClearManualProfile();
    engine_->OnProfilesChanged();
    if (!engine_->Enabled()) engine_->SetEnabled(true);
    engine_->Recompute(false);
    engine_->ApplyNow();

    RegisterHotkeys();
    ScheduleBreakReminder();
    ReloadAll();
    ShowTab(activeTab_);
    UpdateTrayTip();
    UpdateStatusBar();

    KLOG_I(L"Factory defaults restored: %d profile(s), no rules. "
           L"Copy of the previous file at %s.before-reset",
           (int)config_.profiles.size(), path.c_str());
}

void App::CreateSettingsControls(HWND hwnd) {
    // Creating controls fires notifications: sliders take an initial value and
    // checkboxes take state. The flag keeps those from being read as user edits
    // and written to the profile.
    loadingUi_ = true;

    // Tooltip control shared by every control in the window.
    tooltip_ = ::CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                 WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                 CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                 hwnd, nullptr, inst_, nullptr);
    if (tooltip_) {
        // Without this, long text becomes a single line clipped at the screen edge.
        ::SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, S(420));
        ::SendMessageW(tooltip_, TTM_SETDELAYTIME, TTDT_AUTOPOP, 20000);
    }

    RECT rc;
    ::GetClientRect(hwnd, &rc);
    const int cw = rc.right;
    const int ch = rc.bottom;

    const int margin = S(10);
    const int statusH = S(22);

    // WS_TABSTOP makes the tab control reachable from the keyboard; once it has
    // focus, the arrow keys switch tabs natively.
    tabs_ = ::CreateWindowExW(0, WC_TABCONTROLW, L"",
                              WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP | TCS_TABS,
                              margin, margin, cw - margin * 2,
                              ch - margin * 2 - statusH, hwnd,
                              (HMENU)(INT_PTR)IDC_TABS, inst_, nullptr);
    ::SendMessageW(tabs_, WM_SETFONT, (WPARAM)font_, TRUE);
    theme::SubclassTabControl(tabs_);

    const wchar_t* tabNames[] = { T(L"Adjustments"), T(L"Vision"), T(L"Profiles"),
                                  T(L"Automation"), T(L"System"), T(L"Diagnostics") };
    for (int i = 0; i < 6; ++i) {
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<wchar_t*>(tabNames[i]);
        ::SendMessageW(tabs_, TCM_INSERTITEMW, i, (LPARAM)&item);
    }

    statusBar_ = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   margin + S(4), ch - statusH, cw - margin * 2, S(18),
                                   hwnd, (HMENU)(INT_PTR)IDC_STATUS, inst_, nullptr);
    ::SendMessageW(statusBar_, WM_SETFONT, (WPARAM)font_, TRUE);

    // Usable area inside the tabs.
    const int ax = margin + S(12);
    const int ay = margin + S(34);
    const int aw = cw - margin * 2 - S(24);

    Maker mk{hwnd, inst_, font_, fontBold_, nullptr};

    // A label is the name of a field, and the name is where the pointer lands
    // first. NameField() creates the label and records which field it names, so
    // the tooltip block at the end can register the same text on both the field
    // and its label.
    auto Section = [&](const wchar_t* title, int x, int y, int w, const wchar_t* hint) {
        AddTip(mk.Section(title, x, y, w), hint);
    };

    std::vector<std::pair<int, HWND>> labels;
    auto NameField = [&](int fieldId, const wchar_t* text, int x, int y, int w) {
        labels.emplace_back(fieldId, mk.Label(text, x, y, w));
    };

    // Adjustments tab
    mk.sink = &tabControls_[0];
    {
        int y = ay;
        NameField(IDC_PROFILE_COMBO, T(L"Profile"), ax, y + S(4), S(46));
        profileCombo_ = mk.Combo(IDC_PROFILE_COMBO, ax + S(50), y, S(210));
        mk.Button(T(L"Automatic"), IDC_AUTO_BTN, ax + S(268), y, S(94), S(24));
        pauseButton_ = mk.Button(T(L"Pause"), IDC_PAUSE_BTN, ax + S(368), y, S(80), S(24));

        y += S(32);
        NameField(IDC_MONITOR_COMBO, T(L"Monitor"), ax, y + S(4), S(50));
        monitorCombo_ = mk.Combo(IDC_MONITOR_COMBO, ax + S(50), y, S(300));
        perMonitorCheck_ = mk.Check(T(L"This monitor has its own settings"), IDC_PER_MONITOR,
                                    ax + S(362), y + S(2), S(240));

        const int colW = (aw - S(24)) / 2;
        const int lx = ax;
        const int rx = ax + colW + S(24);
        int ly = ay + S(70);
        int ry = ly;

        // Left column: gamma ramp.
        Section(T(L"Light and tone"), lx, ly, colW,
              T(L"Everything here goes through the graphics card's gamma ramp: it works "
                L"on any GPU, on any monitor, and applies inside fullscreen games too. "
                L"It does not reduce the panel's light — it only interprets color."));
        ly += S(24);
        sliders_[F_BRIGHT].Create(hwnd, T(L"Brightness"), F_BRIGHT, lx, ly, colW,
                                  10, 150, 100, 1, L"%", 0, IDC_SLIDER_RESET_BASE + F_BRIGHT);
        ly += S(32);
        sliders_[F_CONTRAST].Create(hwnd, T(L"Contrast"), F_CONTRAST, lx, ly, colW,
                                    0, 200, 100, 1, L"%", 0, IDC_SLIDER_RESET_BASE + F_CONTRAST);
        ly += S(32);
        sliders_[F_GAMMA].Create(hwnd, T(L"Gamma"), F_GAMMA, lx, ly, colW,
                                 0.3, 3.0, 1.0, 100, L"", 2, IDC_SLIDER_RESET_BASE + F_GAMMA);
        ly += S(32);
        sliders_[F_TEMP].Create(hwnd, T(L"Temperature"), F_TEMP, lx, ly, colW,
                                1500, 10000, 6500, 0.02, L" K", 0, IDC_SLIDER_RESET_BASE + F_TEMP);
        ly += S(32);
        sliders_[F_BLUEBLOCK].Create(hwnd, T(L"Blue light filter"), F_BLUEBLOCK, lx, ly, colW,
                                     0, 100, 0, 1, L"%", 0, IDC_SLIDER_RESET_BASE + F_BLUEBLOCK);
        ly += S(40);

        Section(T(L"Shadow detail"), lx, ly, colW,
              T(L"Brightens the dark without washing out the rest of the image. Shadows "
                L"raises the black floor; Clarity separates the near-black tones so the "
                L"lift does not flatten them. Together they reveal what was hidden."));
        ly += S(24);
        sliders_[F_SHADOWS].Create(hwnd, T(L"Shadows"), F_SHADOWS, lx, ly, colW,
                                   0, 100, 0, 1, L"%", 0, IDC_SLIDER_RESET_BASE + F_SHADOWS);
        ly += S(32);
        sliders_[F_CLARITY].Create(hwnd, T(L"Clarity"), F_CLARITY, lx, ly, colW,
                                   0, 100, 0, 1, L"%", 0, IDC_SLIDER_RESET_BASE + F_CLARITY);
        ly += S(36);

        Section(T(L"White balance"), lx, ly, colW,
              T(L"A ceiling for each channel, to match two displays side by side. To warm "
                L"or cool the whole image, use Temperature — it is more predictable."));
        ly += S(24);
        sliders_[F_RGAIN].Create(hwnd, T(L"Red gain"), F_RGAIN, lx, ly, colW,
                                 50, 100, 100, 1, L"%", 0, IDC_SLIDER_RESET_BASE + F_RGAIN);
        ly += S(32);
        sliders_[F_GGAIN].Create(hwnd, T(L"Green gain"), F_GGAIN, lx, ly, colW,
                                 50, 100, 100, 1, L"%", 0, IDC_SLIDER_RESET_BASE + F_GGAIN);
        ly += S(32);
        sliders_[F_BGAIN].Create(hwnd, T(L"Blue gain"), F_BGAIN, lx, ly, colW,
                                 50, 100, 100, 1, L"%", 0, IDC_SLIDER_RESET_BASE + F_BGAIN);
        ly += S(44);

        // Two distinct actions kept apart: one edits the profile, the other
        // restores the screen.
        mk.Button(T(L"Reset this profile"), IDC_RESET_ALL, lx, ly, S(170), S(26));
        mk.Button(T(L"Restore the display now"), IDC_RESTORE_SCREEN,
                  lx + S(176), ly, S(170), S(26));
        ly += S(32);
        compareButton_ = mk.Add(L"BUTTON", T(L"Compare (hold)"),
                                WS_TABSTOP | BS_PUSHBUTTON,
                                lx, ly, S(346), S(26), IDC_COMPARE);
        ::SetWindowSubclass(compareButton_, CompareProc, 1, 0);

        // Right column: color, dimming and hardware.
        Section(T(L"Color"), rx, ry, colW,
              T(L"Saturation through the universal matrix, which gives the same result "
                L"on any PC, and vibrance through the GPU, which lifts the weak colors "
                L"without blowing out the strong ones."));
        ry += S(24);
        sliders_[F_SAT].Create(hwnd, T(L"Saturation"), F_SAT, rx, ry, colW,
                               0, 200, 100, 1, L"%", 0, IDC_SLIDER_RESET_BASE + F_SAT);
        ry += S(32);
        sliders_[F_VIB].Create(hwnd, T(L"Vibrance (GPU)"), F_VIB, rx, ry, colW,
                               0, 100, 0, 1, L"%", 0, IDC_SLIDER_RESET_BASE + F_VIB);
        ry += S(32);
        sliders_[F_HUE].Create(hwnd, T(L"Hue"), F_HUE, rx, ry, colW,
                               -180, 180, 0, 1, L"°", 0, IDC_SLIDER_RESET_BASE + F_HUE);
        ry += S(30);
        invertCheck_ = mk.Check(T(L"Invert colors"), IDC_INVERT, rx, ry, S(200));
        ry += S(34);

        Section(T(L"Extra dimming"), rx, ry, colW,
              T(L"For when the monitor at its minimum is still too bright. It is a black "
                L"layer over the screen: it works, but it washes out contrast and shows "
                L"up in screenshots."));
        ry += S(24);
        sliders_[F_DIM].Create(hwnd, T(L"Dim"), F_DIM, rx, ry, colW,
                               0, 90, 0, 1, L"%", 0, IDC_SLIDER_RESET_BASE + F_DIM);
        ry += S(40);

        Section(T(L"Monitor hardware"), rx, ry, colW,
              T(L"Talks to the panel over the video cable and reduces light for real, "
                L"without washing out contrast. This is the right way to lower "
                L"brightness — the gamma ramp only darkens the image."));
        ry += S(24);
        manageHwBright_ = mk.Check(T(L"Control the physical brightness"), IDC_MANAGE_HWBRIGHT,
                                   rx, ry, S(260));
        ry += S(24);
        sliders_[F_HWBRIGHT].Create(hwnd, T(L"Physical brightness"), F_HWBRIGHT, rx, ry, colW,
                                    0, 100, 70, 1, L"%", 0, IDC_SLIDER_RESET_BASE + F_HWBRIGHT);
        ry += S(32);
        manageHwContrast_ = mk.Check(T(L"Control the physical contrast"), IDC_MANAGE_HWCONTRAST,
                                     rx, ry, S(260));
        ry += S(24);
        sliders_[F_HWCONTRAST].Create(hwnd, T(L"Physical contrast"), F_HWCONTRAST, rx, ry, colW,
                                      0, 100, 50, 1, L"%", 0, IDC_SLIDER_RESET_BASE + F_HWCONTRAST);
        ry += S(40);
        NameField(IDC_DDC_MONITOR_MODE, T(L"DDC mode (needs a restart)"), rx, ry + S(4), S(190));
        ddcModeCombo_ = mk.Combo(IDC_DDC_MONITOR_MODE, rx + S(196), ry, S(180), 120);
        ry += S(34);

        // Monitor commands sit outside the profile on purpose: switching input
        // or powering the panel off is a one-shot action, not a value worth
        // storing and reapplying on every profile change.
        //
        // Laid out as three columns rather than three label+list rows, because
        // the right column ends a few pixels above the status bar. The section
        // header doubles as the status line, stating why the lists are empty.
        monFeaturesLabel_ = mk.Section(T(L"Monitor commands"),
                                       rx, ry, colW);
        ry += S(22);

        const int cmdW  = (colW - S(16)) / 3;
        const int cmdX2 = rx + cmdW + S(8);
        const int cmdX3 = rx + (cmdW + S(8)) * 2;
        NameField(IDC_MON_INPUT, T(L"Input"), rx, ry, cmdW);
        NameField(IDC_MON_PRESET, T(L"Preset"), cmdX2, ry, cmdW);
        NameField(IDC_MON_POWER, T(L"Power"), cmdX3, ry, cmdW);
        ry += S(18);

        monInputCombo_ = mk.Combo(IDC_MON_INPUT, rx, ry, cmdW, 240);
        monPresetCombo_ = mk.Combo(IDC_MON_PRESET, cmdX2, ry, cmdW, 240);
        monPowerCombo_ = mk.Combo(IDC_MON_POWER, cmdX3, ry, cmdW, 200);

        // Slider rows are not created through Maker, so their handles are
        // registered by hand.
        for (int i = 0; i < F_COUNT; ++i) {
            tabControls_[0].push_back(sliders_[i].label);
            tabControls_[0].push_back(sliders_[i].bar);
            tabControls_[0].push_back(sliders_[i].value);
            tabControls_[0].push_back(sliders_[i].reset);
            ::SendMessageW(sliders_[i].label, WM_SETFONT, (WPARAM)font_, TRUE);
            ::SendMessageW(sliders_[i].value, WM_SETFONT, (WPARAM)font_, TRUE);
            ::SendMessageW(sliders_[i].reset, WM_SETFONT, (WPARAM)font_, TRUE);
        }
    }

    // Vision tab: one switch plus four values, each with its explanation
    // alongside it.
    mk.sink = &tabControls_[1];
    {
        int y = ay;
        visEnable_ = mk.Check(T(L"Adjust the display automatically through the day"),
                              IDC_VIS_ENABLE, ax, y, S(400));
        ::SendMessageW(visEnable_, WM_SETFONT, (WPARAM)fontBold_, TRUE);
        y += S(26);

        mk.Hint(T(L"The display warms on its own as the sun goes down and returns to "
                  L"normal in the morning. It works on top of any profile — you do not "
                  L"have to create a profile or a schedule rule for it."),
                ax, y, aw, S(34));
        y += S(40);

        visStatus_ = mk.Add(L"STATIC", L"", WS_CHILD | SS_LEFT,
                            ax, y, aw, S(36), IDC_VIS_STATUS);
        ::SendMessageW(visStatus_, WM_SETFONT, (WPARAM)fontBold_, TRUE);
        y += S(44);

        const int fx = ax + S(230);
        const int hintX = ax + S(330);
        const int hintW = aw - S(330);

        Section(T(L"Display color"), ax, y, S(300),
                T(L"The color target at each end of the day. Zdisplay walks between the "
                  L"two as the hours pass, instead of switching all at once.")); y += S(26);

        NameField(IDC_VIS_DAY_TEMP, T(L"Daytime temperature"), ax, y + S(4), S(220));
        visDayTemp_ = mk.Edit(IDC_VIS_DAY_TEMP, fx, y, S(80), ES_NUMBER);
        mk.Hint(T(L"6500 K is neutral white. Leave it there if daytime already looks right."),
                hintX, y + S(2), hintW, S(20));
        y += S(30);

        NameField(IDC_VIS_NIGHT_TEMP, T(L"Night temperature"), ax, y + S(4), S(220));
        visNightTemp_ = mk.Edit(IDC_VIS_NIGHT_TEMP, fx, y, S(80), ES_NUMBER);
        mk.Hint(T(L"3400 K is the color of an incandescent bulb. The lower it goes, the "
                  L"more orange — and the less blue reaching your eyes at night."),
                hintX, y + S(2), hintW, S(34));
        y += S(38);

        NameField(IDC_VIS_NIGHT_BRIGHT, T(L"Night brightness (% of the profile)"), ax, y + S(4), S(220));
        visNightBright_ = mk.Edit(IDC_VIS_NIGHT_BRIGHT, fx, y, S(80), ES_NUMBER);
        mk.Hint(T(L"100 leaves brightness alone. It matters as much as the color does: "
                  L"what tires the eyes is the display being far brighter than the room."),
                hintX, y + S(2), hintW, S(34));
        y += S(42);

        Section(T(L"When"), ax, y, S(300),
                T(L"The times that separate day from night, and how long the crossing takes.")); y += S(26);

        NameField(IDC_VIS_NIGHT_START, T(L"Night starts"), ax, y + S(4), S(220));
        visNightStart_ = mk.Edit(IDC_VIS_NIGHT_START, fx, y, S(80));
        NameField(IDC_VIS_DAY_START, T(L"Day starts"), ax + S(330), y + S(4), S(110));
        visDayStart_ = mk.Edit(IDC_VIS_DAY_START, ax + S(444), y, S(80));
        y += S(30);

        NameField(IDC_VIS_TRANSITION, T(L"Transition length (min)"), ax, y + S(4), S(220));
        visTransition_ = mk.Edit(IDC_VIS_TRANSITION, fx, y, S(80), ES_NUMBER);
        mk.Hint(T(L"The change happens gradually around the time, half before and half "
                  L"after. An hour is enough that the display is never caught changing."),
                hintX, y + S(2), hintW, S(34));
        y += S(40);

        mk.Hint(T(L"Accepts a clock time (22:00) or the sun: 'sunset', 'sunrise', "
                  L"'sunset-30'. With no location Zdisplay uses 20:00 and 07:00; fill in "
                  L"Latitude and Longitude on the Automation tab and it follows the sun."),
                ax, y, aw, S(34));
        y += S(42);

        Section(T(L"Eye break"), ax, y, S(300),
                T(L"Independent of the color adjustment: it works even with the switch "
                  L"on this tab turned off.")); y += S(26);

        NameField(IDC_VIS_BREAK, T(L"Reminder interval (min)"), ax, y + S(4), S(220));
        visBreak_ = mk.Edit(IDC_VIS_BREAK, fx, y, S(80), ES_NUMBER);
        mk.Hint(T(L"0 turns it off. Every 20 minutes Zdisplay reminds you to look at "
                  L"something far away, about 6 metres, for 20 seconds. It is the one "
                  L"recommendation against screen eye strain with real clinical support "
                  L"behind it — more than any color adjustment has."),
                hintX, y + S(2), hintW, S(48));
        y += S(50);

        mk.Button(T(L"Test the reminder"), IDC_VIS_TEST_BREAK,
                  ax, y, S(180), S(26));
        mk.Hint(T(L"The reminder works even with the color adjustment turned off."),
                ax + S(190), y + S(5), aw - S(190), S(20));
        y += S(34);

        mk.Button(T(L"Preview day (5 s)"), IDC_VIS_PREVIEW_DAY,
                  ax, y, S(170), S(26));
        mk.Button(T(L"Preview night (5 s)"), IDC_VIS_PREVIEW_NIGHT,
                  ax + S(178), y, S(170), S(26));
        mk.Hint(T(L"Click once; the display returns on its own after five seconds."),
                ax + S(356), y + S(5), aw - S(356), S(20));
    }

    // Profiles tab
    mk.sink = &tabControls_[2];
    {
        const int y = ay;
        profileList_ = mk.List(IDC_PROFILE_LIST, ax, y, S(240), S(300));

        const int fx = ax + S(260);
        const int fieldX = fx + S(130);
        int fy = y + S(2);

        NameField(IDC_PROFILE_NAME, T(L"Name"), fx, fy + S(4), S(126));
        profileNameEdit_ = mk.Edit(IDC_PROFILE_NAME, fieldX, fy, S(240));
        fy += S(32);

        NameField(IDC_PROFILE_HOTKEY, T(L"Global hotkey"), fx, fy + S(4), S(126));
        profileHotkeyEdit_ = mk.Edit(IDC_PROFILE_HOTKEY, fieldX, fy, S(240));
        fy += S(32);

        NameField(IDC_PROFILE_TRANSITION, T(L"Transition (ms)"), fx, fy + S(4), S(126));
        transitionEdit_ = mk.Edit(IDC_PROFILE_TRANSITION, fieldX, fy, S(90), ES_NUMBER);
        fy += S(32);

        NameField(IDC_PROFILE_SATENGINE, T(L"Saturation engine"), fx, fy + S(4), S(126));
        satEngineCombo_ = mk.Combo(IDC_PROFILE_SATENGINE, fieldX, fy, S(240), 160);
        fy += S(34);

        defaultLabel_ = mk.Label(L"", fx, fy, S(380));
        fy += S(26);

        mk.Hint(T(L"Automatic: the GPU handles vibrance and the universal matrix handles "
                  L"saturation, which keeps the result the same on any machine."),
                fx, fy, S(380), S(50));

        int by = y + S(312);
        mk.Button(T(L"New"), IDC_PROFILE_NEW, ax, by, S(76), S(26));
        mk.Button(T(L"Duplicate"), IDC_PROFILE_DUP, ax + S(82), by, S(82), S(26));
        mk.Button(T(L"Delete"), IDC_PROFILE_DELETE, ax + S(170), by, S(76), S(26));
        mk.Button(T(L"Make default"), IDC_PROFILE_DEFAULT, ax + S(252), by, S(110), S(26));
        by += S(32);
        mk.Button(T(L"Export profiles..."), IDC_PROFILE_EXPORT, ax, by, S(130), S(26));
        mk.Button(T(L"Import profiles..."), IDC_PROFILE_IMPORT, ax + S(136), by, S(130), S(26));
    }

    // Automation tab
    mk.sink = &tabControls_[3];
    {
        int y = ay;
        AddTip(mk.Label(T(L"Rules by application"), ax, y, aw),
               T(L"They switch profile when the program comes to the foreground. They "
                 L"take precedence over the schedule rules: while one of them matches, "
                 L"the time of day is not consulted."));
        y += S(22);
        appListView_ = mk.ListView(IDC_APP_LIST, ax, y, aw, S(150));
        AddColumn(appListView_, 0, T(L"Process"), S(260));
        AddColumn(appListView_, 1, T(L"Profile"), S(200));
        AddColumn(appListView_, 2, T(L"Priority"), S(90));
        y += S(158);

        NameField(IDC_APP_PROCESS, T(L"Process"), ax, y + S(4), S(70));
        // Drop-down listing the programs running now, still typable for
        // wildcards ('cs*') and for programs that are not running.
        appProcessEdit_ = mk.ComboEdit(IDC_APP_PROCESS, ax + S(72), y, S(180));
        NameField(IDC_APP_PROFILE, T(L"Profile"), ax + S(262), y + S(4), S(40));
        appProfileCombo_ = mk.Combo(IDC_APP_PROFILE, ax + S(304), y, S(160));
        NameField(IDC_APP_PRIORITY, T(L"Prio."), ax + S(474), y + S(4), S(38));
        appPriorityEdit_ = mk.Edit(IDC_APP_PRIORITY, ax + S(514), y, S(50), ES_NUMBER);
        appEnabledCheck_ = mk.Check(T(L"Enabled"), IDC_APP_ENABLED, ax + S(576), y + S(2), S(70));
        y += S(30);

        mk.Button(T(L"Add"), IDC_APP_ADD, ax, y, S(90), S(26));
        mk.Button(T(L"Update"), IDC_APP_UPDATE, ax + S(96), y, S(90), S(26));
        mk.Button(T(L"Remove"), IDC_APP_DELETE, ax + S(192), y, S(90), S(26));
        mk.Button(T(L"Use the program in focus"), IDC_APP_PICK, ax + S(288), y, S(190), S(26));
        y += S(30);

        mk.Hint(T(L"The Process list shows the programs open right now. It can also be "
                  L"typed into, including with '*' (cs* catches cs2 and csgo)."),
                ax, y, aw, S(18));
        y += S(24);

        AddTip(mk.Label(T(L"Rules by time of day"), ax, y, aw),
               T(L"They apply when no application rule matches. If no range covers the "
                 L"current time, the default profile takes over."));
        y += S(22);
        schedList_ = mk.ListView(IDC_SCHED_LIST, ax, y, aw, S(110));
        AddColumn(schedList_, 0, T(L"Start"), S(120));
        AddColumn(schedList_, 1, T(L"End"), S(120));
        AddColumn(schedList_, 2, T(L"Profile"), S(220));
        // Priority is listed because it decides between two overlapping ranges.
        AddColumn(schedList_, 3, T(L"Priority"), S(90));
        y += S(118);

        NameField(IDC_SCHED_START, T(L"Start"), ax, y + S(4), S(46));
        schedStartEdit_ = mk.Edit(IDC_SCHED_START, ax + S(48), y, S(70));
        NameField(IDC_SCHED_END, T(L"End"), ax + S(128), y + S(4), S(34));
        schedEndEdit_ = mk.Edit(IDC_SCHED_END, ax + S(164), y, S(70));
        NameField(IDC_SCHED_PROFILE, T(L"Profile"), ax + S(244), y + S(4), S(40));
        schedProfileCombo_ = mk.Combo(IDC_SCHED_PROFILE, ax + S(286), y, S(160));
        // Updating rebuilds the whole rule, so priority needs a field of its own
        // here to survive a round trip through the dialog.
        NameField(IDC_SCHED_PRIORITY, T(L"Prio."), ax + S(452), y + S(4), S(40));
        schedPriorityEdit_ = mk.Edit(IDC_SCHED_PRIORITY, ax + S(494), y, S(50));
        schedEnabledCheck_ = mk.Check(T(L"Enabled"), IDC_SCHED_ENABLED, ax + S(556), y + S(2), S(70));
        y += S(30);

        mk.Button(T(L"Add"), IDC_SCHED_ADD, ax, y, S(90), S(26));
        mk.Button(T(L"Update"), IDC_SCHED_UPDATE, ax + S(96), y, S(90), S(26));
        mk.Button(T(L"Remove"), IDC_SCHED_DELETE, ax + S(192), y, S(90), S(26));
        y += S(36);

        mk.Hint(T(L"Start and End accept a clock time (22:00) or the sun itself: "
                  L"'sunset', 'sunrise', and with an offset such as 'sunset-30' or "
                  L"'sunrise+45'. Sunset moves by more than two hours across the year, "
                  L"so a fixed range is wrong for half the months."),
                ax, y, aw, S(34));
        y += S(38);

        AddTip(mk.Label(T(L"Location"), ax, y + S(4), S(80)),
               T(L"Used only to work out sunrise and sunset. It stays on your PC, in "
                 L"zdisplay.ini — Zdisplay does not reach the network."));
        NameField(IDC_LATITUDE, T(L"Latitude"), ax + S(84), y + S(4), S(56));
        latitudeEdit_ = mk.Edit(IDC_LATITUDE, ax + S(142), y, S(90));
        NameField(IDC_LONGITUDE, T(L"Longitude"), ax + S(242), y + S(4), S(66));
        longitudeEdit_ = mk.Edit(IDC_LONGITUDE, ax + S(310), y, S(90));
    }

    // System tab
    mk.sink = &tabControls_[4];
    {
        const int colW = (aw - S(30)) / 2;
        const int lx = ax;
        const int rx = ax + colW + S(30);
        int ly = ay, ry = ay;

        Section(T(L"Behaviour"), lx, ly, colW,
              T(L"How Zdisplay behaves at start, on exit and while it is open.")); ly += S(26);
        checkStartup_   = mk.Check(T(L"Start with Windows"), IDC_CHK_STARTUP, lx, ly, colW); ly += S(24);
        checkMinimized_ = mk.Check(T(L"Start minimised to the tray"), IDC_CHK_MINIMIZED, lx, ly, colW); ly += S(24);
        checkAppRules_  = mk.Check(T(L"Switch profile by the program in focus"), IDC_CHK_APPRULES, lx, ly, colW); ly += S(24);
        checkSchedule_  = mk.Check(T(L"Switch profile by the time of day"), IDC_CHK_SCHEDULE, lx, ly, colW); ly += S(24);
        checkRestore_   = mk.Check(T(L"Restore the display on exit"), IDC_CHK_RESTORE, lx, ly, colW); ly += S(24);
        checkConfirmDark_ = mk.Check(T(L"Ask for confirmation when the adjustments go too dark"),
                                     IDC_CHK_CONFIRM_DARK, lx, ly, colW); ly += S(24);
        checkMirrorKeys_ = mk.Check(T(L"Brightness keys also reach the external monitors"),
                                    IDC_CHK_MIRROR_KEYS, lx, ly, colW);
        ly += S(30);
        // The confirmation once repeated its own tooltip here, word for word.
        // The tooltip on the checkbox says it, and the column has no room to
        // say it twice.

        // The label column is S(196) rather than S(170): "Reapply the adjustments
        // every (s)" measures 179 px in the interface font, where the Portuguese
        // it replaced measured 142. All three rows move together so the fields
        // stay in one line.
        NameField(IDC_LANGUAGE, T(L"Language"), lx, ly + S(4), S(196));
        languageCombo_ = mk.Combo(IDC_LANGUAGE, lx + S(202), ly, S(170));
        ly += S(26);
        mk.Hint(T(L"The language is applied when this window is reopened."),
                lx + S(4), ly, colW - S(4), S(20));
        ly += S(28);

        NameField(IDC_PERFORMANCE, T(L"Performance"), lx, ly + S(4), S(196));
        performanceCombo_ = mk.Combo(IDC_PERFORMANCE, lx + S(202), ly, S(170));
        ly += S(26);
        // Kept as a handle: the text follows the selected mode, so the window
        // states what the mode does instead of making the user try all three.
        performanceHint_ = mk.Hint(L"", lx + S(4), ly, colW - S(4), S(34));
        ly += S(40);

        NameField(IDC_WATCHDOG, T(L"Reapply the adjustments every (s)"), lx, ly + S(4), S(196));
        watchdogEdit_ = mk.Edit(IDC_WATCHDOG, lx + S(202), ly, S(60), ES_NUMBER);
        // Same again: the tooltip on the field already carries this sentence.
        ly += S(34);

        Section(T(L"Backends"), lx, ly, colW,
              T(L"The paths through which Zdisplay reaches the screen. Turning one off "
                L"is useful only to isolate a problem. Changes take effect at the next "
                L"start.")); ly += S(26);
        checkVendor_    = mk.Check(T(L"Vendor APIs (NVIDIA NVAPI / AMD ADL)"), IDC_CHK_VENDOR, lx, ly, colW); ly += S(24);
        checkMagnify_   = mk.Check(T(L"Universal color matrix (Magnification API)"), IDC_CHK_MAGNIFY, lx, ly, colW); ly += S(24);
        checkDdc_       = mk.Check(T(L"Monitor hardware over DDC/CI"), IDC_CHK_DDC, lx, ly, colW); ly += S(24);
        checkBacklight_ = mk.Check(T(L"Laptop backlight (WMI)"), IDC_CHK_BACKLIGHT, lx, ly, colW); ly += S(24);
        checkOverlay_   = mk.Check(T(L"Dimming layer"), IDC_CHK_OVERLAY, lx, ly, colW); ly += S(32);

        unlockButton_ = mk.Button(T(L"Unlock the full gamma range (admin)"), IDC_UNLOCK_GAMMA,
                                  lx, ly, S(280), S(28));
        ly += S(34);
        mk.Button(T(L"Open the configuration folder"), IDC_OPEN_FOLDER, lx, ly, S(220), S(26));
        mk.Button(T(L"Factory reset..."), IDC_FACTORY_RESET,
                  lx + S(226), ly, S(174), S(26));

        Section(T(L"Global hotkeys"), rx, ry, colW,
              T(L"They work from anywhere in Windows, even with this window closed. "
                L"Format: Ctrl+Alt+K, Ctrl+Shift+F5, Win+Alt+Up. Empty turns one off.")); ry += S(26);
        const wchar_t* hkLabels[7] = {
            T(L"Brightness up"), T(L"Brightness down"),
            T(L"Saturation up"), T(L"Saturation down"),
            T(L"Pause / resume"), T(L"Open this window"),
            T(L"EMERGENCY: give the screen back"),
        };
        for (int i = 0; i < 7; ++i) {
            NameField(IDC_HK_BASE + i, hkLabels[i], rx, ry + S(4), S(150));
            hkEdits_[i] = mk.Edit(IDC_HK_BASE + i, rx + S(156), ry, S(180));
            ry += S(28);
        }
        ry += S(6);
        NameField(IDC_HK_STEP, T(L"Hotkey step"), rx, ry + S(4), S(150));
        stepEdit_ = mk.Edit(IDC_HK_STEP, rx + S(156), ry, S(60), ES_NUMBER);
        ry += S(34);
        mk.Hint(T(L"Format: Ctrl+Alt+K, Ctrl+Shift+F5, Win+Alt+Up. Leave a field empty "
                  L"to turn that hotkey off."), rx, ry, colW, S(40));
        ry += S(42);
        // Stays empty while every hotkey registers; it reports the combinations
        // Windows refused, which the edit fields would otherwise still show as
        // if they worked.
        hotkeyWarning_ = mk.Hint(L"", rx, ry, colW, S(46));
    }

    // Diagnostics tab
    mk.sink = &tabControls_[5];
    {
        diagEdit_ = mk.Add(L"EDIT", L"",
                           WS_TABSTOP | WS_BORDER | WS_VSCROLL | ES_MULTILINE |
                           ES_READONLY | ES_AUTOVSCROLL,
                           ax, ay, aw, ch - ay - S(76), IDC_DIAG_TEXT, WS_EX_CLIENTEDGE);

        // Held in a member so WM_DESTROY can delete it; a local would leak one
        // HFONT per open/close cycle of the window.
        if (fontMono_) { ::DeleteObject(fontMono_); fontMono_ = nullptr; }
        fontMono_ = ::CreateFontW(-S(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        if (fontMono_) ::SendMessageW(diagEdit_, WM_SETFONT, (WPARAM)fontMono_, TRUE);

        const int by = ch - S(66);
        mk.Button(T(L"Refresh"), IDC_DIAG_REFRESH, ax, by, S(100), S(26));
        mk.Button(T(L"Copy"), IDC_DIAG_COPY, ax + S(106), by, S(90), S(26));
        mk.Button(T(L"Open the log"), IDC_DIAG_OPENLOG, ax + S(202), by, S(110), S(26));
        mk.Button(T(L"Read capabilities..."), IDC_DIAG_CAPS,
                              ax + S(318), by, S(150), S(26));
        mk.Button(T(L"Test the monitor"), IDC_DIAG_ROUNDTRIP,
                                   ax + S(474), by, S(140), S(26));
        mk.Button(T(L"Clear the DDC quarantine"), IDC_DIAG_DDC_RESET,
                                  ax + S(620), by, S(180), S(26));
    }

    // Tooltips. Every control has one; the label names the field in a couple of
    // words, the tooltip states what changing it does and what it costs. They
    // are grouped here instead of being spread through the layout above so the
    // whole interface can be read as text in one place.
    {
        // Same tooltip on the field and on its label.
        auto Tip = [&](int id, const wchar_t* text) {
            AddTip(id, text);
            for (const auto& r : labels)
                if (r.first == id) AddTip(r.second, text);
        };

        // Sliders: the same tooltip on the label, the bar and the value.
        const struct { AdjField field; const wchar_t* text; } sliderTips[] = {
        { F_BRIGHT,
          T(L"Brightness in software, through the gamma ramp. It works on any GPU and "
            L"inside fullscreen games too. It does not dim the panel's backlight — use "
            L"the physical brightness for that.") },
        { F_CONTRAST,
          T(L"Contrast around mid grey. High values clip white and black, losing "
            L"detail at both ends.") },
        { F_GAMMA,
          T(L"The response curve. Below 1 the midtones darken, above 1 they lighten. "
            L"It touches neither black nor white.") },
        { F_TEMP,
          T(L"Color temperature. 6500 K is neutral; below that the image warms, as it "
            L"does under Windows Night light.") },
        { F_BLUEBLOCK,
          T(L"Cuts the blue band and leaves the rest. Unlike temperature, which "
            L"rebalances all three colors — here the image yellows faster, in exchange "
            L"for blocking more blue.") },
        { F_SHADOWS,
          T(L"Raises only the bottom of the curve: black gains a floor and the effect "
            L"dies out before the midtones, so highlights, strong colors and overall "
            L"contrast stay intact. This is what gaming monitors call Black eQualizer. "
            L"It applies inside fullscreen games too.") },
        { F_CLARITY,
          T(L"Pushes the near-black tones apart before the lift, giving back the "
            L"detail the lift would flatten. On its own it brightens nothing: raise it "
            L"together with Shadows. It is not edge sharpening — it is contrast in the "
            L"low tones, which is what makes dark detail reappear.") },
        { F_SAT,
          T(L"Saturation through the universal color matrix, which gives the same "
            L"result on any PC — including one with no dedicated GPU.") },
        { F_VIB,
          T(L"GPU vibrance: it lifts weak colors without blowing out the strong ones. "
            L"Uses NVAPI on NVIDIA and ADL on AMD.") },
        { F_HUE,
          T(L"Rotates every color around the color wheel. It is for correcting a panel "
            L"that pulls to one side; on a normal image it just breaks the color.") },
        { F_DIM,
          T(L"Darkens with a black layer over the screen. It goes below the panel's "
            L"minimum, but it washes out contrast and shows up in screenshots.") },
        { F_RGAIN,
          T(L"Ceiling for the red channel. It is for matching two displays side by "
            L"side; to warm or cool the image, use Temperature.") },
        { F_GGAIN, T(L"Ceiling for the green channel. See Red gain.") },
        { F_BGAIN, T(L"Ceiling for the blue channel. See Red gain.") },
        { F_HWBRIGHT,
          T(L"The monitor's physical brightness, over DDC/CI or through the laptop "
            L"backlight. It reduces light for real, without washing out contrast.") },
        { F_HWCONTRAST,
          T(L"The panel's own contrast, over DDC/CI. It acts on the hardware, so it "
            L"applies with HDR on as well.") },
        };
        for (const auto& d : sliderTips) {
            AddTip(sliders_[d.field].bar,   d.text);
            AddTip(sliders_[d.field].label, d.text);
            AddTip(sliders_[d.field].value, d.text);
        }

        // Adjustments tab.
        Tip(IDC_PROFILE_COMBO,
               T(L"A named set of adjustments. Choosing one here pins the profile by "
                 L"hand and stops the automatic switching until you click Automatic."));
        Tip(IDC_AUTO_BTN,
               T(L"Hands control back to the rules: the program in focus, the time of "
                 L"day or the default profile apply again, in that order."));
        Tip(IDC_PAUSE_BTN,
               T(L"Returns the display to its original state and stops reapplying. "
                 L"Nothing is lost — on resume everything comes back as it was."));
        Tip(IDC_MONITOR_COMBO,
               T(L"Chooses which display the adjustments below refer to. Under 'All', "
                 L"whatever you change applies to every one of them."));
        Tip(IDC_INVERT,
               T(L"Replaces each color with its opposite. Useful for reading on a light "
                 L"screen and as an accessibility feature."));
        Tip(IDC_MANAGE_HWBRIGHT,
               T(L"Lets the profile drive the monitor's own brightness. While it is "
                 L"unchecked, Zdisplay leaves whatever is set on the panel alone."));
        Tip(IDC_MANAGE_HWCONTRAST,
               T(L"The same for the panel's contrast. Few monitors accept it, and the "
                 L"factory value is usually the best one."));
        Tip(IDC_RESET_ALL,
               T(L"Returns EVERY slider in this profile to neutral. It changes the "
                 L"profile, not just the screen — and it is saved."));
        Tip(IDC_RESTORE_SCREEN,
               T(L"Returns the display to its original state without touching the "
                 L"profile. Use it when another program messes up the color."));
        for (int i = 0; i < F_COUNT; ++i)
            Tip(IDC_SLIDER_RESET_BASE + i, T(L"Returns this slider to its neutral value."));

        Tip(IDC_PER_MONITOR,
             T(L"Check this for the monitor to hold its own values, independent of the "
               L"others. Unchecked, it follows the profile's common adjustment."));
        Tip(IDC_COMPARE,
             T(L"Hold it down to see the display as it was before the adjustments. "
               L"Release and the adjustment comes back."));
        Tip(IDC_DDC_MONITOR_MODE,
             T(L"Automatic uses the normal interval between commands. Slow waits "
               L"longer, which helps on unstable docks and adapters. Never use excludes "
               L"this monitor from any DDC/CI probing. Takes effect at the next start."));
        Tip(IDC_MON_INPUT,
             T(L"Switches the monitor's video input (HDMI, DisplayPort, USB-C). It is "
               L"the same as using the buttons on the panel — useful when two machines "
               L"share one display."));
        Tip(IDC_MON_PRESET,
             T(L"The monitor's own color preset. It acts on the hardware, so it keeps "
               L"working with HDR on, where the gamma ramp does not."));
        Tip(IDC_MON_POWER,
             T(L"Turns the monitor off or suspends it. Bringing it back may need the "
               L"button on the panel: not every monitor answers DDC/CI while it is "
               L"off."));


        // Vision tab.
        Tip(IDC_VIS_ENABLE,
               T(L"A layer that acts on top of the active profile, whichever it is. It "
                 L"does not replace profiles or schedule rules: it adds to them."));
        Tip(IDC_VIS_DAY_TEMP,
               T(L"The display color during the day, in kelvin. 6500 K is the neutral "
                 L"white of sRGB — leave it there if daytime already looks right."));
        Tip(IDC_VIS_NIGHT_TEMP,
               T(L"The display color at night. 3400 K is the color of an incandescent "
                 L"bulb; the lower it goes, the less blue reaches your eyes."));
        Tip(IDC_VIS_NIGHT_BRIGHT,
               T(L"The percentage of the profile's brightness applied at night. 100 "
                 L"changes nothing. A display too bright for the room tires the eyes "
                 L"more than its color does."));
        Tip(IDC_VIS_NIGHT_START,
               T(L"A clock time (22:00) or the sun: 'sunset', 'sunset-30', 'sunset+45'. "
                 L"Without latitude and longitude on the Automation tab, 20:00 applies."));
        Tip(IDC_VIS_DAY_START,
               T(L"A clock time (07:00) or the sun: 'sunrise', 'sunrise+45'. With no "
                 L"location, 07:00 applies."));
        Tip(IDC_VIS_TRANSITION,
               T(L"How many minutes the change takes, half before and half after the "
                 L"time. An hour is enough for it to pass unnoticed."));
        Tip(IDC_VIS_BREAK,
               T(L"0 turns it off. Every so many minutes a reminder appears to look at "
                 L"something about 6 metres away for 20 seconds — the one recommendation "
                 L"against eye strain with real clinical support behind it."));
        Tip(IDC_VIS_TEST_BREAK,
               T(L"Shows the reminder now, without waiting for the interval. If Windows "
                 L"notifications are off, it says so instead of vanishing silently."));
        Tip(IDC_VIS_PREVIEW_DAY,
               T(L"Applies the daytime color for five seconds and returns on its own. "
                 L"With daytime at 6500 K there is nothing to see: 6500 K is neutral."));
        Tip(IDC_VIS_PREVIEW_NIGHT,
               T(L"Applies the night color for five seconds and returns on its own."));

        // Profiles tab.
        Tip(IDC_PROFILE_LIST,
               T(L"Every saved profile. The selected one is what the fields beside it "
                 L"edit — selecting here applies nothing to the display."));
        Tip(IDC_PROFILE_NAME,
               T(L"Renaming updates the application and schedule rules that point at "
                 L"this profile, on its own."));
        Tip(IDC_PROFILE_HOTKEY,
               T(L"The combination that activates this profile from anywhere in "
                 L"Windows. For example Ctrl+Alt+1. Empty turns it off."));
        Tip(IDC_PROFILE_TRANSITION,
               T(L"How long the display takes to reach this profile. 0 switches at "
                 L"once; a few hundred ms hide the jump."));
        Tip(IDC_PROFILE_SATENGINE,
               T(L"Who performs the saturation. Automatic uses the universal matrix, "
                 L"which gives the same result on any machine, and leaves vibrance to "
                 L"the GPU."));
        Tip(IDC_PROFILE_NEW,      T(L"Creates a profile at neutral."));
        Tip(IDC_PROFILE_DUP,      T(L"Copies the selected profile, with the same values."));
        Tip(IDC_PROFILE_DELETE,
               T(L"Deletes the selected profile. The rules that pointed at it stop "
                 L"having any effect until you correct them."));
        Tip(IDC_PROFILE_DEFAULT,
               T(L"The profile used when no application or schedule rule matches."));
        Tip(IDC_PROFILE_EXPORT,
               T(L"Writes the profiles to a text file, to carry to another PC or to "
                 L"keep before experimenting."));
        Tip(IDC_PROFILE_IMPORT,
               T(L"Reads profiles from an exported file. A repeated name comes in as a "
                 L"copy; nothing is overwritten."));

        // Automation tab.
        Tip(IDC_APP_LIST,
               T(L"They switch profile when the program comes to the foreground. The "
                 L"box on each row turns the rule on and off without deleting it."));
        Tip(IDC_APP_PROCESS,
               T(L"The executable name, without .exe. The list shows what is open right "
                 L"now, and it can be typed with '*' — 'cs*' catches cs2 and csgo."));
        Tip(IDC_APP_PROFILE,   T(L"The profile applied while that program is in focus."));
        Tip(IDC_APP_PRIORITY,
               T(L"Breaks the tie when two patterns catch the same program. Higher wins."));
        Tip(IDC_APP_ENABLED,   T(L"Turns the rule off without deleting it."));
        Tip(IDC_APP_ADD,       T(L"Creates a rule from what is in the fields above."));
        Tip(IDC_APP_UPDATE,    T(L"Saves the changes to the rule selected in the list."));
        Tip(IDC_APP_DELETE,    T(L"Deletes the selected rule."));
        Tip(IDC_APP_PICK,
               T(L"Fills the Process field with the program in the foreground right "
                 L"now — it saves you finding the executable name."));
        Tip(IDC_SCHED_LIST,
               T(L"They apply when no application rule matches. The box on each row "
                 L"turns the range on and off without deleting it."));
        Tip(IDC_SCHED_START,
               T(L"A clock time (22:00) or the sun: 'sunset', 'sunrise', 'sunset-30', "
                 L"'sunrise+45'. Sunset moves by more than two hours across the year, so "
                 L"a fixed range is wrong for half the months."));
        Tip(IDC_SCHED_END,      T(L"Same format as Start. A range may cross midnight."));
        Tip(IDC_SCHED_PROFILE,  T(L"The profile applied inside the range."));
        Tip(IDC_SCHED_PRIORITY, T(L"Breaks the tie between overlapping ranges. Higher wins."));
        Tip(IDC_SCHED_ENABLED,  T(L"Turns the range off without deleting it."));
        Tip(IDC_SCHED_ADD,      T(L"Creates a range from what is in the fields above."));
        Tip(IDC_SCHED_UPDATE,   T(L"Saves the changes to the range selected in the list."));
        Tip(IDC_SCHED_DELETE,   T(L"Deletes the selected range."));

        Tip(IDC_LATITUDE,
             T(L"In decimal degrees, positive to the north. Berlin: 52.52. Left empty, "
               L"the rules using 'sunrise' and 'sunset' never fire — switching profile "
               L"on the clock of a place you are not in would be worse than not "
               L"switching at all."));
        Tip(IDC_LONGITUDE,
             T(L"In decimal degrees, positive to the east. Berlin: 13.40."));

        // System tab.
        Tip(IDC_CHK_STARTUP,
               T(L"Registers Zdisplay to open at login. It applies only to this Windows "
                 L"account and does not ask for administrator."));
        Tip(IDC_CHK_MINIMIZED,
               T(L"Opens straight to the tray, without this window. The adjustments are "
                 L"applied just the same."));
        Tip(IDC_CHK_APPRULES,
               T(L"The master switch for the application rules. Off, Zdisplay stops "
                 L"following which program is in focus."));
        Tip(IDC_CHK_SCHEDULE, T(L"The master switch for the schedule rules."));
        Tip(IDC_CHK_RESTORE,
               T(L"On exit, returns the display to its original state. Unchecked, the "
                 L"last adjustment stays on the screen after closing."));
        Tip(IDC_CHK_CONFIRM_DARK,
               T(L"Applies the adjustment and undoes it on its own after 15 s if you do "
                 L"not confirm. It is what stops someone locking themselves out behind "
                 L"a black screen."));
        Tip(IDC_WATCHDOG,
               T(L"How many seconds pass between one reapplication and the next. 0 "
                 L"turns it off. It guards against Night light, drivers and games that "
                 L"overwrite the gamma ramp."));
        Tip(IDC_CHK_VENDOR,
               T(L"NVAPI on NVIDIA, ADL on AMD. It is the only path to the card's real "
                 L"vibrance."));
        Tip(IDC_CHK_MAGNIFY,
               T(L"A color matrix applied by the Windows compositor. It gives the same "
                 L"saturation and hue on any machine, with or without a dedicated card."));
        Tip(IDC_CHK_DDC,
               T(L"Talks to the monitor over the video cable. It is what allows changing "
                 L"the physical brightness, the contrast and the input."));
        Tip(IDC_CHK_BACKLIGHT,
               T(L"The backlight of a laptop's internal panel, over WMI."));
        Tip(IDC_CHK_OVERLAY,
               T(L"A translucent black window over everything. It darkens below the "
                 L"panel's minimum, but it washes out contrast and shows up in "
                 L"screenshots."));
        Tip(IDC_UNLOCK_GAMMA,
               T(L"Windows halves the strength of the gamma ramp. Unlocking the full "
                 L"range requires administrator and writes a system key; it applies to "
                 L"every program, not only Zdisplay."));
        Tip(IDC_RELOCK_GAMMA,
               T(L"Gives the gamma ramp back the standard Windows limit. Requires "
                 L"administrator and applies to every program."));
        Tip(IDC_FACTORY_RESET,
             T(L"Erases everything you have configured — profiles, rules, hotkeys, "
               L"location and the options on this tab — and returns Zdisplay to its "
               L"freshly installed state. It asks for confirmation and keeps a copy of "
               L"the current file before erasing."));
        Tip(IDC_OPEN_FOLDER,
               T(L"Opens the folder holding zdisplay.ini, the log and the copy of the "
                 L"display's original state."));
        Tip(IDC_HK_BASE + 0, T(L"Raises the active profile's brightness, from anywhere in Windows."));
        Tip(IDC_HK_BASE + 1, T(L"Lowers the active profile's brightness."));
        Tip(IDC_HK_BASE + 2, T(L"Raises the active profile's saturation."));
        Tip(IDC_HK_BASE + 3, T(L"Lowers the active profile's saturation."));
        Tip(IDC_HK_BASE + 4, T(L"Pauses and resumes without opening this window."));
        Tip(IDC_HK_BASE + 5, T(L"Brings this window to the front."));
        Tip(IDC_HK_STEP,
               T(L"How far the brightness and saturation hotkeys move per press, in "
                 L"percentage points."));

        Tip(IDC_CHK_MIRROR_KEYS,
             T(L"On a docked laptop, the brightness keys only reach the internal panel. "
               L"With this on, Zdisplay carries the same change to the external "
               L"monitors over DDC/CI. A profile that already manages the physical "
               L"brightness stays in charge, so the two do not fight."));
        Tip(IDC_HK_BASE + 6,
             T(L"Returns the display to its original state and pauses Zdisplay, from "
               L"anywhere in Windows. Clear this field and the default comes back on "
               L"its own — it is the emergency exit."));

        // Diagnostics tab.
        Tip(IDC_DIAG_TEXT,
               T(L"A portrait of what Zdisplay sees: monitors, graphics card, EDID and "
                 L"which backends came up. This is what is worth attaching to a problem "
                 L"report."));
        Tip(IDC_DIAG_REFRESH, T(L"Reads everything again now, without restarting the program."));

        // Labels held in members, which have no control ID.
        AddTip(monFeaturesLabel_,
               T(L"They act at once and do not enter the profile: switching the input or "
                 L"turning the display off is a one-off action, not an adjustment worth "
                 L"reapplying on every profile change."));
        AddTip(statusBar_,
               T(L"The active profile, how many adjustment paths came up and how many "
                 L"monitors Zdisplay sees right now."));
        AddTip(visStatus_,
               T(L"What is in effect at this moment and what comes next. It updates on "
                 L"its own as the day passes."));
        AddTip(defaultLabel_,
               T(L"The default profile is the one used when no application or schedule "
                 L"rule matches."));
        AddTip(hotkeyWarning_,
               T(L"It appears when Windows refuses a hotkey because another program has "
                 L"already taken it. Empty means every one was accepted."));
        Tip(IDC_DIAG_COPY,    T(L"Copies this text to the clipboard."));
        Tip(IDC_DIAG_CAPS,
             T(L"Asks each monitor which DDC/CI features it declares. It is not done on "
               L"its own: there is a Windows defect in which a malformed answer — from "
               L"the generic monitor driver, of all things — brings the system down. It "
               L"is only for diagnosis; Zdisplay does not need it."));
        Tip(IDC_DIAG_ROUNDTRIP,
             T(L"Proves the monitor really obeys: it moves the brightness one step, "
               L"reads it back, checks it and restores the value that was there. Some "
               L"panels accept the command, answer success and change nothing — only "
               L"this test separates that case from an adjustment that worked."));
        Tip(IDC_DIAG_DDC_RESET,
             T(L"Releases monitors whose capability read was blocked after a crash. It "
               L"starts no read of its own."));
        Tip(IDC_DIAG_OPENLOG,
               T(L"Opens zdisplay.log in the default editor. That is where the errors "
                 L"and the history of the last runs live."));
    }

    // Applies list colors, dark scroll bars and the status bar. Must run last
    // because it walks the children that already exist.
    theme::ApplyToControls(hwnd);

    loadingUi_ = false;
}

void App::ShowTab(int index) {
    activeTab_ = Clamp(index, 0, 5);

    // Redraw is suspended while tabs swap. A switch touches dozens of controls
    // and each ShowWindow repaints immediately, so without this the tab
    // assembles piece by piece instead of appearing at once.
    ::SendMessageW(settings_, WM_SETREDRAW, FALSE, 0);
    for (int t = 0; t < 6; ++t) {
        const int cmd = (t == activeTab_) ? SW_SHOW : SW_HIDE;
        for (HWND h : tabControls_[t])
            if (h && ::IsWindow(h)) ::ShowWindow(h, cmd);
    }
    ::SendMessageW(settings_, WM_SETREDRAW, TRUE, 0);
    ::RedrawWindow(settings_, nullptr, nullptr,
                   RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_ERASE);

    if (activeTab_ == 5) LoadDiagnostics();
    if (activeTab_ == 1) LoadVision();
}

// Loading

void App::ReloadAll() {
    loadingUi_ = true;

    ReloadProfileCombos();
    ReloadMonitorCombo();
    ReloadRunningApps();
    // Hotkeys are registered at startup, before this window exists, so the
    // warning has to be refreshed here rather than on the first field change.
    UpdateHotkeyWarning();

    // Profiles tab
    ::SendMessageW(satEngineCombo_, CB_RESETCONTENT, 0, 0);
    const wchar_t* engines[] = {
        T(L"Automatic (recommended)"),
        T(L"Force the vendor GPU"),
        T(L"Force universal (any PC)"),
        T(L"Leave saturation alone"),
    };
    for (const wchar_t* e : engines)
        ::SendMessageW(satEngineCombo_, CB_ADDSTRING, 0, (LPARAM)e);

    ::SendMessageW(ddcModeCombo_, CB_RESETCONTENT, 0, 0);
    const wchar_t* ddcModes[] = {
        T(L"Automatic (recommended)"), T(L"Slow DDC (dock/adapter)"),
        T(L"Never use DDC on this monitor")
    };
    for (const wchar_t* mode : ddcModes)
        ::SendMessageW(ddcModeCombo_, CB_ADDSTRING, 0, (LPARAM)mode);

    // System tab
    ::SendMessageW(languageCombo_, CB_RESETCONTENT, 0, 0);
    // Language names stay in their own language: someone looking for their own
    // looks for the name they know it by, not for a translation of it.
    const wchar_t* languages[] = { T(L"Automatic (from Windows)"), L"English", L"Português" };
    for (const wchar_t* l : languages)
        ::SendMessageW(languageCombo_, CB_ADDSTRING, 0, (LPARAM)l);
    ::SendMessageW(languageCombo_, CB_SETCURSEL, LanguageIndexOf(config_.language), 0);

    ::SendMessageW(performanceCombo_, CB_RESETCONTENT, 0, 0);
    const wchar_t* modes[] = {
        T(L"Quality"), T(L"Balanced"), T(L"Light")
    };
    for (const wchar_t* m : modes)
        ::SendMessageW(performanceCombo_, CB_ADDSTRING, 0, (LPARAM)m);
    ::SendMessageW(performanceCombo_, CB_SETCURSEL, PerformanceIndexOf(config_.performance), 0);
    SetText(performanceHint_, PerformanceHintFor(config_.performance));

    SetChecked(checkStartup_,   config_.startWithWindows);
    SetChecked(checkMinimized_, config_.startMinimized);
    SetChecked(checkAppRules_,  config_.enableAppRules);
    SetChecked(checkSchedule_,  config_.enableSchedule);
    SetChecked(checkRestore_,   config_.restoreOnExit);
    SetText(watchdogEdit_, std::to_wstring(config_.watchdogSeconds));
    SetChecked(checkVendor_,    config_.enableVendorApis);
    SetChecked(checkMagnify_,   config_.enableMagnification);
    SetChecked(checkDdc_,       config_.enableDdcCi);
    SetChecked(checkBacklight_, config_.enableBacklight);
    SetChecked(checkOverlay_,   config_.enableOverlay);

    SetText(hkEdits_[0], config_.hkBrightnessUp);
    SetText(hkEdits_[1], config_.hkBrightnessDown);
    SetText(hkEdits_[2], config_.hkSaturationUp);
    SetText(hkEdits_[3], config_.hkSaturationDown);
    SetText(hkEdits_[4], config_.hkToggle);
    SetText(hkEdits_[5], config_.hkShow);
    SetText(hkEdits_[6], config_.hkPanic);
    SetText(stepEdit_, FormatDouble(config_.hotkeyStep));
    SetChecked(checkConfirmDark_, config_.confirmDarkSettings);
    SetChecked(checkMirrorKeys_, config_.mirrorInternalBrightness);
    LoadVision();

    // Left empty when there is no location, so the field does not show the 999
    // sentinel value.
    SetText(latitudeEdit_,  config_.HasLocation() ? FormatDouble(config_.latitude, 4) : L"");
    SetText(longitudeEdit_, config_.HasLocation() ? FormatDouble(config_.longitude, 4) : L"");

    if (GammaBackend::RangeUnlocked()) {
        // Once the range is unlocked, the button becomes the undo action.
        SetText(unlockButton_, T(L"Restore the standard Windows range (admin)"));
        ::SetWindowLongPtrW(unlockButton_, GWLP_ID, IDC_RELOCK_GAMMA);
    }

    // Controls this machine cannot honor are dimmed, rather than left responsive
    // with no effect.
    const bool vendor = engine_->Nvidia()->Available() || engine_->Amd()->Available();
    if (!vendor) {
        SetText(sliders_[F_VIB].label, T(L"Vibrance (no GPU)"));
    }

    SetText(pauseButton_, engine_->Enabled() ? T(L"Pause") : T(L"Resume"));

    LoadRuleLists();
    LoadProfileEditor();
    LoadAdjustments();
    UpdateStatusBar();

    loadingUi_ = false;
}

void App::ReloadRunningApps() {
    if (!appProcessEdit_ || !::IsWindow(appProcessEdit_)) return;

    // Preserves what was already typed: the list is a convenience and must not
    // clear the text.
    wchar_t typed[256] = {};
    ::GetWindowTextW(appProcessEdit_, typed, _countof(typed));

    ::SendMessageW(appProcessEdit_, CB_RESETCONTENT, 0, 0);
    for (const auto& app : ListRunningApps())
        ::SendMessageW(appProcessEdit_, CB_ADDSTRING, 0, (LPARAM)app.process.c_str());

    SetText(appProcessEdit_, typed);
}

void App::ReloadProfileCombos() {
    const bool wasLoading = loadingUi_;
    loadingUi_ = true;

    const std::wstring active = engine_->Active() ? engine_->Active()->name : L"";

    HWND combos[] = { profileCombo_, appProfileCombo_, schedProfileCombo_ };
    for (HWND c : combos) {
        if (!c) continue;
        ::SendMessageW(c, CB_RESETCONTENT, 0, 0);
        for (const auto& p : config_.profiles)
            ::SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)p.name.c_str());
        ::SendMessageW(c, CB_SETCURSEL, 0, 0);
    }
    if (!active.empty()) ComboSelectText(profileCombo_, active);

    if (profileList_) {
        const int sel = (int)::SendMessageW(profileList_, LB_GETCURSEL, 0, 0);
        ::SendMessageW(profileList_, LB_RESETCONTENT, 0, 0);
        for (const auto& p : config_.profiles)
            ::SendMessageW(profileList_, LB_ADDSTRING, 0, (LPARAM)p.name.c_str());
        const int count = (int)config_.profiles.size();
        ::SendMessageW(profileList_, LB_SETCURSEL,
                       (WPARAM)Clamp(sel < 0 ? 0 : sel, 0, (std::max)(0, count - 1)), 0);
    }

    loadingUi_ = wasLoading;
}

void App::ReloadMonitorCombo() {
    if (!monitorCombo_) return;
    const bool wasLoading = loadingUi_;
    loadingUi_ = true;

    const int prev = ComboIndex(monitorCombo_);
    ::SendMessageW(monitorCombo_, CB_RESETCONTENT, 0, 0);
    ::SendMessageW(monitorCombo_, CB_ADDSTRING, 0, (LPARAM)T(L"All monitors"));
    for (const auto& m : monitors::All()) {
        const std::wstring label = m.isPrimary
            ? m.friendlyName + L"  " + T(L"(primary)") : m.friendlyName;
        ::SendMessageW(monitorCombo_, CB_ADDSTRING, 0, (LPARAM)label.c_str());
    }
    const int count = (int)monitors::All().size() + 1;
    ::SendMessageW(monitorCombo_, CB_SETCURSEL,
                   (WPARAM)Clamp(prev < 0 ? 0 : prev, 0, count - 1), 0);

    loadingUi_ = wasLoading;
}

Profile* App::EditingProfile() {
    const std::wstring name = ComboText(profileCombo_);
    if (Profile* p = config_.Find(name)) return p;
    return engine_->Active();
}

std::wstring App::SelectedMonitorKey() {
    const int i = ComboIndex(monitorCombo_);
    if (i <= 0) return L"";   // index 0 is the all-monitors entry
    const auto& list = monitors::All();
    const size_t index = (size_t)(i - 1);
    return index < list.size() ? list[index].key : L"";
}

Adjustments* App::CurrentAdjustments() {
    Profile* p = EditingProfile();
    if (!p) return nullptr;
    const std::wstring key = SelectedMonitorKey();
    if (key.empty()) return &p->global;
    Adjustments* a = p->Find(key);
    return a ? a : &p->global;
}

void App::LoadMonitorFeatures() {
    if (!monInputCombo_) return;

    const bool wasLoading = loadingUi_;
    loadingUi_ = true;

    struct Slot { HWND combo; unsigned char code; std::vector<unsigned char>* values; };
    const Slot slots[] = {
        { monInputCombo_,  kVcpInputSource, &monInputValues_  },
        { monPresetCombo_, kVcpColorPreset, &monPresetValues_ },
        { monPowerCombo_,  kVcpPowerMode,   &monPowerValues_  },
    };

    for (const auto& s : slots) {
        ::SendMessageW(s.combo, CB_RESETCONTENT, 0, 0);
        s.values->clear();
        ::EnableWindow(s.combo, FALSE);
    }

    // The section header doubles as the status line stating why the lists are
    // empty. The texts are short on purpose: the column has a fixed width and a
    // static clips anything past it.
    const std::wstring key = SelectedMonitorKey();
    if (key.empty()) {
        // The command targets a single panel, so the all-monitors selection has
        // no meaning here, unlike the color adjustments.
        SetText(monFeaturesLabel_, T(L"Monitor commands — choose a monitor above"));
        loadingUi_ = wasLoading;
        return;
    }

    if (!engine_->Ddc()->Supports(key)) {
        SetText(monFeaturesLabel_, T(L"Monitor commands — this one does not answer DDC/CI"));
        loadingUi_ = wasLoading;
        return;
    }

    if (!engine_->Ddc()->FeaturesProbed(key)) {
        // The probe costs several slow commands and completes asynchronously, so
        // the header states that the empty lists are pending rather than
        // unsupported.
        SetText(monFeaturesLabel_, T(L"Monitor commands — asking the monitor..."));
        engine_->Ddc()->RequestFeatureProbe();
        loadingUi_ = wasLoading;
        return;
    }

    const auto features = engine_->Ddc()->Features(key);
    int offered = 0;
    for (const auto& s : slots) {
        const DdcciBackend::Feature* found = nullptr;
        for (const auto& f : features)
            if (f.code == s.code) { found = &f; break; }
        if (!found) continue;

        // With no declared value list, offer the values that have a known name
        // and fit within the feature maximum, instead of all of 0..255.
        std::vector<unsigned char> candidates = found->values;
        if (candidates.empty()) {
            for (int v = 0; v <= found->maximum && v <= 255; ++v)
                if (!VcpValueName(s.code, (unsigned char)v).empty())
                    candidates.push_back((unsigned char)v);
        }
        if (candidates.empty()) continue;

        int select = -1;
        for (size_t i = 0; i < candidates.size(); ++i) {
            const unsigned char v = candidates[i];
            std::wstring name = VcpValueName(s.code, v);
            if (name.empty()) name = Format(T(L"value 0x%02X"), (unsigned)v);
            ::SendMessageW(s.combo, CB_ADDSTRING, 0, (LPARAM)name.c_str());
            s.values->push_back(v);
            if ((int)v == found->current) select = (int)i;
        }
        ::SendMessageW(s.combo, CB_SETCURSEL, (WPARAM)select, 0);
        ::EnableWindow(s.combo, TRUE);
        ++offered;
    }

    SetText(monFeaturesLabel_, offered > 0
        ? T(L"Monitor commands — they act at once, outside the profile")
        : T(L"Monitor commands — this one exposed none of them"));

    loadingUi_ = wasLoading;
}

void App::LoadAdjustments() {
    const bool wasLoading = loadingUi_;
    loadingUi_ = true;

    Profile* p = EditingProfile();
    const std::wstring key = SelectedMonitorKey();
    Adjustments* a = CurrentAdjustments();
    if (!a) { loadingUi_ = wasLoading; return; }

    ::EnableWindow(perMonitorCheck_, !key.empty());
    SetChecked(perMonitorCheck_, !key.empty() && p && p->Find(key) != nullptr);

    for (int i = 0; i < F_COUNT; ++i) {
        double v = *FieldPtr(*a, (AdjField)i);
        if (i == F_HWBRIGHT && v < 0) v = 70;
        if (i == F_HWCONTRAST && v < 0) v = 50;
        sliders_[i].Set(v);
    }
    SetChecked(invertCheck_, a->invert);

    SetChecked(manageHwBright_, a->hwBrightness >= 0);
    SetChecked(manageHwContrast_, a->hwContrast >= 0);
    sliders_[F_HWBRIGHT].Enable(a->hwBrightness >= 0);
    sliders_[F_HWCONTRAST].Enable(a->hwContrast >= 0);

    DdcMonitorMode ddcMode = DdcMonitorMode::Auto;
    auto modeIt = config_.ddcMonitorModes.find(key);
    if (modeIt != config_.ddcMonitorModes.end()) ddcMode = modeIt->second;
    ::SendMessageW(ddcModeCombo_, CB_SETCURSEL, (WPARAM)(int)ddcMode, 0);
    ::EnableWindow(ddcModeCombo_, !key.empty());

    LoadMonitorFeatures();

    // Capability is per monitor and per feature. A global check enables the
    // contrast slider on a panel without VCP 0x12 merely because some other
    // monitor on the machine answers DDC/CI.
    bool hwBrightAvailable = false, hwContrastAvailable = false;
    if (!key.empty()) {
        const MonitorTarget* selected = monitors::ByKey(key);
        hwBrightAvailable = engine_->Ddc()->SupportsBrightness(key) ||
            (selected && selected->isInternal && engine_->Backlight()->Available());
        hwContrastAvailable = engine_->Ddc()->SupportsContrast(key);
    } else {
        for (const auto& m : monitors::All()) {
            hwBrightAvailable = hwBrightAvailable || engine_->Ddc()->SupportsBrightness(m.key) ||
                (m.isInternal && engine_->Backlight()->Available());
            hwContrastAvailable = hwContrastAvailable || engine_->Ddc()->SupportsContrast(m.key);
        }
    }
    ::EnableWindow(manageHwBright_, hwBrightAvailable);
    ::EnableWindow(manageHwContrast_, hwContrastAvailable);
    SetText(manageHwBright_, T(L"Control the physical brightness"));
    SetText(manageHwContrast_, T(L"Control the physical contrast"));
    if (!hwBrightAvailable)
        SetText(manageHwBright_, T(L"Control the physical brightness — no compatible monitor"));
    if (!hwContrastAvailable)
        SetText(manageHwContrast_, T(L"Control the physical contrast — unavailable"));

    SetText(pauseButton_, engine_->Enabled() ? T(L"Pause") : T(L"Resume"));

    loadingUi_ = wasLoading;
}

Profile* App::SelectedProfileInList() {
    if (!profileList_) return nullptr;
    const int i = (int)::SendMessageW(profileList_, LB_GETCURSEL, 0, 0);
    if (i < 0 || (size_t)i >= config_.profiles.size()) return nullptr;
    return &config_.profiles[(size_t)i];
}

void App::LoadProfileEditor() {
    const bool wasLoading = loadingUi_;
    loadingUi_ = true;

    Profile* p = SelectedProfileInList();
    if (p) {
        SetText(profileNameEdit_, p->name);
        SetText(profileHotkeyEdit_, p->hotkey);
        SetText(transitionEdit_, std::to_wstring(p->transitionMs));
        ::SendMessageW(satEngineCombo_, CB_SETCURSEL, (WPARAM)(int)p->satEngine, 0);
        SetText(defaultLabel_, IEquals(p->name, config_.defaultProfile)
                                   ? std::wstring(T(L"This is the default profile."))
                                   : Format(T(L"Current default profile: %s"),
                                            config_.defaultProfile.c_str()));
    }

    loadingUi_ = wasLoading;
}

void App::LoadRuleLists() {
    if (appListView_) {
        ListView_DeleteAllItems(appListView_);
        for (size_t i = 0; i < config_.appRules.size(); ++i) {
            const auto& r = config_.appRules[i];
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = (int)i;
            item.pszText = const_cast<wchar_t*>(r.process.c_str());
            ListView_InsertItem(appListView_, &item);
            ListView_SetItemText(appListView_, (int)i, 1, const_cast<wchar_t*>(r.profile.c_str()));
            const std::wstring prio = std::to_wstring(r.priority);
            ListView_SetItemText(appListView_, (int)i, 2, const_cast<wchar_t*>(prio.c_str()));
            ListView_SetCheckState(appListView_, (int)i, r.enabled);
        }
    }

    if (schedList_) {
        ListView_DeleteAllItems(schedList_);
        for (size_t i = 0; i < config_.scheduleRules.size(); ++i) {
            const auto& r = config_.scheduleRules[i];
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = (int)i;
            item.pszText = const_cast<wchar_t*>(r.start.c_str());
            ListView_InsertItem(schedList_, &item);
            ListView_SetItemText(schedList_, (int)i, 1, const_cast<wchar_t*>(r.end.c_str()));
            ListView_SetItemText(schedList_, (int)i, 2, const_cast<wchar_t*>(r.profile.c_str()));
            const std::wstring prio = std::to_wstring(r.priority);
            ListView_SetItemText(schedList_, (int)i, 3, const_cast<wchar_t*>(prio.c_str()));
            ListView_SetCheckState(schedList_, (int)i, r.enabled);
        }
    }
}

void App::LoadDiagnostics() {
    if (!diagEdit_) return;

    // This text is deliberately not translated. It exists to be pasted into a
    // problem report, where it is read by whoever maintains the program rather
    // than by the person running it, so it stays in one fixed language.
    std::wstring t;
    t += L"BACKENDS DETECTED\r\n";
    t += L"------------------------------------------------------------------\r\n";
    t += engine_->DescribeBackends();
    t += L"\r\nMONITORS\r\n";
    t += L"------------------------------------------------------------------\r\n";
    for (const auto& m : monitors::All()) {
        t += L"  " + m.friendlyName + (m.isPrimary ? L"  [primary]" : L"") +
             (m.isInternal ? L"  [built-in]" : L"") + L"\r\n";
        t += Format(L"     GDI: %s   %ldx%ld\r\n", m.deviceName.c_str(),
                    m.bounds.right - m.bounds.left, m.bounds.bottom - m.bounds.top);
        t += L"     key: " + m.key + L"\r\n";
        if (!m.adapterName.empty() || m.gpuVendorId) {
            t += L"     adapter: " + m.adapterName;
            const wchar_t* v = GpuVendorName(m.gpuVendorId);
            if (v[0]) t += Format(L"  [%s, PCI %04X]", v, m.gpuVendorId);
            t += L"\r\n";
            // On a machine with two GPUs, the vendor path applies only to the
            // display its own adapter actually drives.
            if (m.gpuVendorId == kVendorIntel)
                t += L"     saturation: through the universal color matrix (Intel does\r\n"
                     L"                 not expose driver vibrance as NVIDIA and AMD do)\r\n";
        }
        if (m.edid.valid) {
            t += Format(L"     EDID: %s %04X", m.edid.manufacturer.c_str(), m.edid.product);
            if (m.edid.year) t += Format(L"  %d", m.edid.year);
            if (!m.edid.serialText.empty()) t += L"  s/n " + m.edid.serialText;
            else if (m.edid.serial) t += Format(L"  s/n %08X", m.edid.serial);
            t += Format(L"  %s\r\n", m.edid.digital ? L"digital" : L"analog");
            t += Format(L"     gamut: area %.3f (sRGB %.3f)%s\r\n", m.edid.gamutArea,
                        kSrgbGamutArea,
                        m.edid.wideGamut ? L"  - wide gamut: colors come out stronger than sRGB"
                                         : L"");
        } else {
            t += L"     EDID: not published or invalid - the key falls back to the "
                 L"device path\r\n";
        }
        if (m.isHdr)
            t += L"     HDR: ON - Windows accepts the gamma ramp and ignores it\r\n";
        else if (m.hdrCapable)
            t += L"     HDR: supported, off\r\n";

        // What actually controls THIS monitor: the global backend count above
        // does not explain why a slider has no effect on a given display.
        for (const auto& line : engine_->MonitorCoverage(m))
            t += L"     " + line + L"\r\n";
    }

    const auto ddcLines = engine_->Ddc()->Diagnose();
    if (!ddcLines.empty()) {
        t += L"\r\nDDC/CI PER MONITOR\r\n";
        t += L"------------------------------------------------------------------\r\n";
        for (const auto& line : ddcLines) t += L"  " + line + L"\r\n";
    }

    Profile* p = engine_->Active();
    t += L"\r\nSTATE\r\n";
    t += L"------------------------------------------------------------------\r\n";
    // Version comes first: this tab exists to be pasted into a problem report,
    // which is unusable without knowing the build it came from.
    t += L"  Version .............. " + std::wstring(ZDISPLAY_VERSION_WSTR) + L"\r\n";
    t += L"  Active profile ....... " + (p ? p->name : std::wstring(L"-")) + L"\r\n";
    t += L"  Mode ................. " +
         std::wstring(engine_->ManualProfile().empty() ? L"automatic" : L"manual") + L"\r\n";
    t += L"  Zdisplay ............. " +
         std::wstring(engine_->Enabled() ? L"active" : L"paused") + L"\r\n";
    t += L"  Program in focus ..... " + engine_->ForegroundProcess() + L"\r\n";
    t += L"  Windows Night light .. " +
         std::wstring(GammaBackend::NightLightActive() ? L"ON (may conflict)" : L"off") + L"\r\n";
    t += L"  Gamma range .......... " +
         std::wstring(GammaBackend::RangeUnlocked() ? L"widened" : L"Windows default") + L"\r\n";
    if (engine_->Gamma()->Limited()) {
        t += Format(L"  WARNING .............. Windows accepted only %.0f%% of the requested effect.\r\n"
                    L"                         Use 'Unlock the full gamma range' on the System\r\n"
                    L"                         tab (needs admin and a sign-out).\r\n",
                    engine_->Gamma()->AcceptedFraction() * 100.0);
    }
    t += L"  Configuration ........ " + ConfigPath() + L"\r\n";

    t += L"\r\nRECENT LOG\r\n";
    t += L"------------------------------------------------------------------\r\n";
    const auto recent = LogRecent();
    const size_t start = recent.size() > 80 ? recent.size() - 80 : 0;
    for (size_t i = recent.size(); i > start; --i) t += L"  " + recent[i - 1] + L"\r\n";

    SetText(diagEdit_, t);
}

void App::LoadVision() {
    if (!visEnable_) return;
    const Vision& v = config_.vision;

    loadingUi_ = true;
    SetChecked(visEnable_, v.enabled);
    SetText(visDayTemp_,     FormatDouble(v.dayTemperature, 0));
    SetText(visNightTemp_,   FormatDouble(v.nightTemperature, 0));
    SetText(visNightBright_, FormatDouble(v.nightBrightness, 0));
    SetText(visTransition_,  std::to_wstring(v.transitionMinutes));
    SetText(visNightStart_,  v.nightStart);
    SetText(visDayStart_,    v.dayStart);
    SetText(visBreak_,       std::to_wstring(v.breakMinutes));
    loadingUi_ = false;

    // Status line: states what is in effect right now and when it changes.
    std::wstring s;
    const bool preview = engine_->VisionPreviewActive();
    if (!v.enabled && !preview) {
        s = T(L"Off — the display follows the active profile exactly.");
    } else {
        const double n = engine_->VisionShownNight();
        SYSTEMTIME now;
        ::GetLocalTime(&now);
        const SolarContext solar = config_.Solar();
        int nightAt = 0, dayAt = 0;
        bool fixedFallback = false;

        if (!ResolveVisionTimes(now, v, solar, &dayAt, &nightAt, &fixedFallback)) {
            s = T(L"The times are not valid. Use, for example, 20:00 and 07:00.");
        } else {
            Vision clean = v;
            clean.Sanitize();
            const double dayMired = 1e6 / clean.dayTemperature;
            const double nightMired = 1e6 / clean.nightTemperature;
            const double kelvin = 1e6 / (dayMired + (nightMired - dayMired) * n);
            const double brightness = 100.0 + (clean.nightBrightness - 100.0) * n;

            if (preview) {
                s = n > 0.5 ? T(L"Temporary preview: night.")
                            : T(L"Temporary preview: day.");
            } else if (n < 0.01) {
                s = T(L"Now: day.");
            } else if (n > 0.99) {
                s = T(L"Now: night.");
            } else {
                // Two whole sentences rather than one with the destination
                // substituted in: an article agreeing with a noun cannot be
                // translated a fragment at a time.
                const double next = NightFraction(MinutesLater(now, 1), v, solar);
                s = next >= n ? Format(T(L"Now: crossing into night (%.0f%%)."), n * 100.0)
                              : Format(T(L"Now: crossing into day (%.0f%%)."), n * 100.0);
            }
            s += Format(T(L"  Target: %.0f K and %.0f%% of the profile's brightness."),
                        kelvin, brightness);
            s += Format(T(L"\r\nNight starts at %02d:%02d and day at %02d:%02d."),
                        nightAt / 60, nightAt % 60, dayAt / 60, dayAt % 60);
            if (fixedFallback)
                s += T(L"  Fixed times while the location is left empty.");
        }
    }
    SetText(visStatus_, s);
}

void App::UpdateStatusBar() {
    if (!statusBar_ || !::IsWindow(statusBar_)) return;

    std::wstring s;
    if (!engine_->Enabled()) {
        s = T(L"Zdisplay paused — the display is in its original state.");
    } else if (engine_->Gamma()->Limited()) {
        // Windows clamped the effect; the status bar reports it instead of
        // implying the adjustment applied in full.
        s = Format(T(L"Windows limited the effect to %.0f%% — see 'Unlock the full gamma "
                     L"range' on the System tab."),
                   engine_->Gamma()->AcceptedFraction() * 100.0);
    } else {
        Profile* p = engine_->Active();
        s = Format(T(L"Profile '%s'   ·   %d backend(s) active   ·   %d monitor(s)   ·   %s"),
                   p ? p->name.c_str() : L"-",
                   engine_->AvailableBackendCount(),
                   (int)monitors::All().size(),
                   engine_->ManualProfile().empty() ? T(L"automatic mode") : T(L"profile pinned"));
    }
    SetText(statusBar_, s);
}

void App::ApplyLive(bool dragging) {
    if (loadingUi_) return;
    // While dragging, only the path that responds immediately and without
    // flicker runs; the rest is applied once the movement settles (see
    // Engine::ApplyInteractive).
    if (dragging) engine_->ApplyInteractive();
    else          engine_->ApplyNow();
    MarkDirty();
    // The value beside the slider is already updated by OnSlider. The status bar
    // and the tray tip do not change during the gesture, and repainting them per
    // pixel only competes with the slider, so both wait for the final settle.
    if (!dragging) {
        UpdateStatusBar();
        UpdateTrayTip();
    }
}

}  // namespace zdisplay
