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
inline void DetectDpi(HWND hwnd) { dpi::DetectFor(hwnd); }

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
            KLOG_E(L"Não consegui registrar a janela de configuração.");
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
        0, kSettingsClass, L"Zdisplay — configurações",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
        nullptr, nullptr, inst_, nullptr);

    if (!settings_) {
        KLOG_E(L"Não consegui criar a janela de configuração (erro %lu).", ::GetLastError());
        return;
    }

    // Placed on the monitor holding the cursor, at that monitor's DPI and inside
    // its work area. Centering by SM_CXSCREEN would always target the primary
    // monitor and would ignore the taskbar.
    dpi::Current() = dpi::ForCursor();
    RecreateUiFonts();
    theme::ApplyWindowBackdrop(settings_);

    RECT want{0, 0, S(880), S(620)};
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
    ::CopyFileW(path.c_str(), (path + L".antes-do-reset").c_str(), FALSE);

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
    ::DeleteFileW((path + L".invalido").c_str());

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

    KLOG_I(L"Padrão de fábrica restaurado: %d perfil(is), nenhuma regra. "
           L"Cópia do anterior em %s.antes-do-reset",
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

    const wchar_t* tabNames[] = { L"Ajustes", L"Visão", L"Perfis", L"Automação",
                                  L"Sistema", L"Diagnóstico" };
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
        NameField(IDC_PROFILE_COMBO, L"Perfil", ax, y + S(4), S(46));
        profileCombo_ = mk.Combo(IDC_PROFILE_COMBO, ax + S(50), y, S(210));
        mk.Button(L"Automático", IDC_AUTO_BTN, ax + S(268), y, S(94), S(24));
        pauseButton_ = mk.Button(L"Pausar", IDC_PAUSE_BTN, ax + S(368), y, S(80), S(24));

        y += S(32);
        NameField(IDC_MONITOR_COMBO, L"Monitor", ax, y + S(4), S(50));
        monitorCombo_ = mk.Combo(IDC_MONITOR_COMBO, ax + S(50), y, S(300));
        perMonitorCheck_ = mk.Check(L"Ajuste próprio deste monitor", IDC_PER_MONITOR,
                                    ax + S(362), y + S(2), S(240));

        const int colW = (aw - S(24)) / 2;
        const int lx = ax;
        const int rx = ax + colW + S(24);
        int ly = ay + S(70);
        int ry = ly;

        // Left column: gamma ramp.
        Section(L"Luz e tom", lx, ly, colW,
              L"Tudo aqui vai pela rampa de gamma da placa de vídeo: funciona em "
              L"qualquer GPU, em qualquer monitor, e vale inclusive dentro de jogos "
              L"em tela cheia. Não reduz a luz do painel — só interpreta a cor.");
        ly += S(24);
        sliders_[F_BRIGHT].Create(hwnd, L"Brilho", F_BRIGHT, lx, ly, colW,
                                  10, 150, 100, 1, L"%", 0, IDC_SLIDER_RESET_BASE + F_BRIGHT);
        ly += S(32);
        sliders_[F_CONTRAST].Create(hwnd, L"Contraste", F_CONTRAST, lx, ly, colW,
                                    0, 200, 100, 1, L"%", 0, IDC_SLIDER_RESET_BASE + F_CONTRAST);
        ly += S(32);
        sliders_[F_GAMMA].Create(hwnd, L"Gamma", F_GAMMA, lx, ly, colW,
                                 0.3, 3.0, 1.0, 100, L"", 2, IDC_SLIDER_RESET_BASE + F_GAMMA);
        ly += S(32);
        sliders_[F_TEMP].Create(hwnd, L"Temperatura", F_TEMP, lx, ly, colW,
                                1500, 10000, 6500, 0.02, L" K", 0, IDC_SLIDER_RESET_BASE + F_TEMP);
        ly += S(32);
        sliders_[F_BLUEBLOCK].Create(hwnd, L"Bloqueio de luz azul", F_BLUEBLOCK, lx, ly, colW,
                                     0, 100, 0, 1, L"%", 0, IDC_SLIDER_RESET_BASE + F_BLUEBLOCK);
        ly += S(40);

        Section(L"Visão nas sombras", lx, ly, colW,
              L"Clareia o escuro sem lavar o resto da imagem. Sombras levanta o piso "
              L"do preto; Definição separa os tons quase pretos para o levante não "
              L"achatá-los. As duas juntas é que mostram o que estava escondido.");
        ly += S(24);
        sliders_[F_SHADOWS].Create(hwnd, L"Sombras", F_SHADOWS, lx, ly, colW,
                                   0, 100, 0, 1, L"%", 0, IDC_SLIDER_RESET_BASE + F_SHADOWS);
        ly += S(32);
        sliders_[F_CLARITY].Create(hwnd, L"Definição", F_CLARITY, lx, ly, colW,
                                   0, 100, 0, 1, L"%", 0, IDC_SLIDER_RESET_BASE + F_CLARITY);
        ly += S(36);

        Section(L"Balanço de branco", lx, ly, colW,
              L"Teto de cada canal, para casar duas telas lado a lado. Para esquentar "
              L"ou esfriar a imagem inteira, use Temperatura — é mais previsível.");
        ly += S(24);
        sliders_[F_RGAIN].Create(hwnd, L"Ganho vermelho", F_RGAIN, lx, ly, colW,
                                 50, 100, 100, 1, L"%", 0, IDC_SLIDER_RESET_BASE + F_RGAIN);
        ly += S(32);
        sliders_[F_GGAIN].Create(hwnd, L"Ganho verde", F_GGAIN, lx, ly, colW,
                                 50, 100, 100, 1, L"%", 0, IDC_SLIDER_RESET_BASE + F_GGAIN);
        ly += S(32);
        sliders_[F_BGAIN].Create(hwnd, L"Ganho azul", F_BGAIN, lx, ly, colW,
                                 50, 100, 100, 1, L"%", 0, IDC_SLIDER_RESET_BASE + F_BGAIN);
        ly += S(44);

        // Two distinct actions kept apart: one edits the profile, the other
        // restores the screen.
        mk.Button(L"Zerar ajustes do perfil", IDC_RESET_ALL, lx, ly, S(170), S(26));
        mk.Button(L"Restaurar a tela agora", IDC_RESTORE_SCREEN,
                  lx + S(176), ly, S(170), S(26));
        ly += S(32);
        compareButton_ = mk.Add(L"BUTTON", L"Comparar (segure)",
                                WS_TABSTOP | BS_PUSHBUTTON,
                                lx, ly, S(346), S(26), IDC_COMPARE);
        ::SetWindowSubclass(compareButton_, CompareProc, 1, 0);

        // Right column: color, dimming and hardware.
        Section(L"Cor", rx, ry, colW,
              L"Saturação pela matriz universal, que dá o mesmo resultado em qualquer "
              L"PC, e vibrance pela GPU, que realça as cores fracas sem estourar as "
              L"fortes.");
        ry += S(24);
        sliders_[F_SAT].Create(hwnd, L"Saturação", F_SAT, rx, ry, colW,
                               0, 200, 100, 1, L"%", 0, IDC_SLIDER_RESET_BASE + F_SAT);
        ry += S(32);
        sliders_[F_VIB].Create(hwnd, L"Vibrance (GPU)", F_VIB, rx, ry, colW,
                               0, 100, 0, 1, L"%", 0, IDC_SLIDER_RESET_BASE + F_VIB);
        ry += S(32);
        sliders_[F_HUE].Create(hwnd, L"Matiz", F_HUE, rx, ry, colW,
                               -180, 180, 0, 1, L"°", 0, IDC_SLIDER_RESET_BASE + F_HUE);
        ry += S(30);
        invertCheck_ = mk.Check(L"Inverter cores", IDC_INVERT, rx, ry, S(200));
        ry += S(34);

        Section(L"Escurecimento extra", rx, ry, colW,
              L"Para quando o monitor no mínimo ainda está claro demais. É uma camada "
              L"preta por cima da tela: resolve, mas lava o contraste e aparece em "
              L"capturas de tela.");
        ry += S(24);
        sliders_[F_DIM].Create(hwnd, L"Escurecer", F_DIM, rx, ry, colW,
                               0, 90, 0, 1, L"%", 0, IDC_SLIDER_RESET_BASE + F_DIM);
        ry += S(40);

        Section(L"Hardware do monitor", rx, ry, colW,
              L"Fala com o painel pelo cabo de vídeo e reduz a luz de verdade, sem "
              L"lavar o contraste. É o caminho certo para baixar o brilho — a rampa "
              L"de gamma só escurece a imagem.");
        ry += S(24);
        manageHwBright_ = mk.Check(L"Controlar o brilho físico", IDC_MANAGE_HWBRIGHT,
                                   rx, ry, S(260));
        ry += S(24);
        sliders_[F_HWBRIGHT].Create(hwnd, L"Brilho físico", F_HWBRIGHT, rx, ry, colW,
                                    0, 100, 70, 1, L"%", 0, IDC_SLIDER_RESET_BASE + F_HWBRIGHT);
        ry += S(32);
        manageHwContrast_ = mk.Check(L"Controlar o contraste físico", IDC_MANAGE_HWCONTRAST,
                                     rx, ry, S(260));
        ry += S(24);
        sliders_[F_HWCONTRAST].Create(hwnd, L"Contraste físico", F_HWCONTRAST, rx, ry, colW,
                                      0, 100, 50, 1, L"%", 0, IDC_SLIDER_RESET_BASE + F_HWCONTRAST);
        ry += S(40);
        NameField(IDC_DDC_MONITOR_MODE, L"Modo DDC (requer reinício)", rx, ry + S(4), S(190));
        ddcModeCombo_ = mk.Combo(IDC_DDC_MONITOR_MODE, rx + S(196), ry, S(180), 120);
        ry += S(34);

        // Monitor commands sit outside the profile on purpose: switching input
        // or powering the panel off is a one-shot action, not a value worth
        // storing and reapplying on every profile change.
        //
        // Laid out as three columns rather than three label+list rows, because
        // the right column ends a few pixels above the status bar. The section
        // header doubles as the status line, stating why the lists are empty.
        monFeaturesLabel_ = mk.Section(L"Comandos do monitor",
                                       rx, ry, colW);
        ry += S(22);

        const int cmdW  = (colW - S(16)) / 3;
        const int cmdX2 = rx + cmdW + S(8);
        const int cmdX3 = rx + (cmdW + S(8)) * 2;
        NameField(IDC_MON_INPUT, L"Entrada", rx, ry, cmdW);
        NameField(IDC_MON_PRESET, L"Predefinição", cmdX2, ry, cmdW);
        NameField(IDC_MON_POWER, L"Energia", cmdX3, ry, cmdW);
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
        visEnable_ = mk.Check(L"Ajustar a tela automaticamente conforme o horário",
                              IDC_VIS_ENABLE, ax, y, S(400));
        ::SendMessageW(visEnable_, WM_SETFONT, (WPARAM)fontBold_, TRUE);
        y += S(26);

        mk.Hint(L"A tela vai esquentando sozinha conforme o sol se põe e volta ao "
                L"normal de manhã. Funciona por cima de qualquer perfil — você não "
                L"precisa criar perfil nem regra de horário para isso.",
                ax, y, aw, S(34));
        y += S(40);

        visStatus_ = mk.Add(L"STATIC", L"", WS_CHILD | SS_LEFT,
                            ax, y, aw, S(36), IDC_VIS_STATUS);
        ::SendMessageW(visStatus_, WM_SETFONT, (WPARAM)fontBold_, TRUE);
        y += S(44);

        const int fx = ax + S(230);
        const int hintX = ax + S(330);
        const int hintW = aw - S(330);

        Section(L"Cor da tela", ax, y, S(300),
                L"O alvo de cor em cada ponta do dia. O Zdisplay caminha entre os dois "
                L"conforme o horário, em vez de trocar de repente."); y += S(26);

        NameField(IDC_VIS_DAY_TEMP, L"Temperatura de dia", ax, y + S(4), S(220));
        visDayTemp_ = mk.Edit(IDC_VIS_DAY_TEMP, fx, y, S(80), ES_NUMBER);
        mk.Hint(L"6500 K é o branco neutro. Deixe assim se de dia está bom.",
                hintX, y + S(2), hintW, S(20));
        y += S(30);

        NameField(IDC_VIS_NIGHT_TEMP, L"Temperatura de noite", ax, y + S(4), S(220));
        visNightTemp_ = mk.Edit(IDC_VIS_NIGHT_TEMP, fx, y, S(80), ES_NUMBER);
        mk.Hint(L"3400 K é a cor de lâmpada incandescente. Quanto menor, mais "
                L"alaranjado — e menos azul chegando aos olhos à noite.",
                hintX, y + S(2), hintW, S(34));
        y += S(38);

        NameField(IDC_VIS_NIGHT_BRIGHT, L"Brilho à noite (% do perfil)", ax, y + S(4), S(220));
        visNightBright_ = mk.Edit(IDC_VIS_NIGHT_BRIGHT, fx, y, S(80), ES_NUMBER);
        mk.Hint(L"100 não mexe no brilho. Vale tanto quanto a cor: o que cansa a "
                L"vista é a tela estar muito mais clara que o ambiente em volta.",
                hintX, y + S(2), hintW, S(34));
        y += S(42);

        Section(L"Quando", ax, y, S(300),
                L"Os horários que separam dia e noite, e quanto tempo a passagem leva."); y += S(26);

        NameField(IDC_VIS_NIGHT_START, L"Início da noite", ax, y + S(4), S(220));
        visNightStart_ = mk.Edit(IDC_VIS_NIGHT_START, fx, y, S(80));
        NameField(IDC_VIS_DAY_START, L"Início do dia", ax + S(330), y + S(4), S(110));
        visDayStart_ = mk.Edit(IDC_VIS_DAY_START, ax + S(444), y, S(80));
        y += S(30);

        NameField(IDC_VIS_TRANSITION, L"Duração da transição (min)", ax, y + S(4), S(220));
        visTransition_ = mk.Edit(IDC_VIS_TRANSITION, fx, y, S(80), ES_NUMBER);
        mk.Hint(L"A mudança acontece aos poucos em torno do horário, metade antes "
                L"e metade depois. Uma hora é o suficiente para não dar para "
                L"perceber a tela mudando.",
                hintX, y + S(2), hintW, S(34));
        y += S(40);

        mk.Hint(L"Aceita relógio (22:00) ou o sol: 'por', 'nascer', 'por-30'. "
                L"Sem localização, o Zdisplay usa 20:00 e 07:00; ao preencher "
                L"Latitude e Longitude na aba Automação, passa a seguir o sol.",
                ax, y, aw, S(34));
        y += S(42);

        Section(L"Pausa para os olhos", ax, y, S(300),
                L"Independente do ajuste de cor: funciona mesmo com a chave desta aba "
                L"desligada."); y += S(26);

        NameField(IDC_VIS_BREAK, L"Intervalo do lembrete (min)", ax, y + S(4), S(220));
        visBreak_ = mk.Edit(IDC_VIS_BREAK, fx, y, S(80), ES_NUMBER);
        mk.Hint(L"0 desliga. Em 20 minutos o Zdisplay avisa para você olhar 20 "
                L"segundos para algo longe, uns 6 metros. É a única recomendação "
                L"contra cansaço visual de tela com apoio clínico de verdade — "
                L"mais do que qualquer ajuste de cor tem.",
                hintX, y + S(2), hintW, S(48));
        y += S(50);

        mk.Button(L"Testar lembrete", IDC_VIS_TEST_BREAK,
                  ax, y, S(180), S(26));
        mk.Hint(L"O lembrete funciona mesmo com o ajuste de cor desligado.",
                ax + S(190), y + S(5), aw - S(190), S(20));
        y += S(34);

        mk.Button(L"Prévia do dia (5 s)", IDC_VIS_PREVIEW_DAY,
                  ax, y, S(170), S(26));
        mk.Button(L"Prévia da noite (5 s)", IDC_VIS_PREVIEW_NIGHT,
                  ax + S(178), y, S(170), S(26));
        mk.Hint(L"Clique uma vez; a tela volta sozinha após cinco segundos.",
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

        NameField(IDC_PROFILE_NAME, L"Nome", fx, fy + S(4), S(126));
        profileNameEdit_ = mk.Edit(IDC_PROFILE_NAME, fieldX, fy, S(240));
        fy += S(32);

        NameField(IDC_PROFILE_HOTKEY, L"Atalho global", fx, fy + S(4), S(126));
        profileHotkeyEdit_ = mk.Edit(IDC_PROFILE_HOTKEY, fieldX, fy, S(240));
        fy += S(32);

        NameField(IDC_PROFILE_TRANSITION, L"Transição (ms)", fx, fy + S(4), S(126));
        transitionEdit_ = mk.Edit(IDC_PROFILE_TRANSITION, fieldX, fy, S(90), ES_NUMBER);
        fy += S(32);

        NameField(IDC_PROFILE_SATENGINE, L"Motor de saturação", fx, fy + S(4), S(126));
        satEngineCombo_ = mk.Combo(IDC_PROFILE_SATENGINE, fieldX, fy, S(240), 160);
        fy += S(34);

        defaultLabel_ = mk.Label(L"", fx, fy, S(380));
        fy += S(26);

        mk.Hint(L"Automático: a GPU cuida do vibrance e a matriz universal cuida da "
                L"saturação, o que mantem o resultado igual em qualquer máquina.",
                fx, fy, S(380), S(50));

        int by = y + S(312);
        mk.Button(L"Novo", IDC_PROFILE_NEW, ax, by, S(76), S(26));
        mk.Button(L"Duplicar", IDC_PROFILE_DUP, ax + S(82), by, S(82), S(26));
        mk.Button(L"Excluir", IDC_PROFILE_DELETE, ax + S(170), by, S(76), S(26));
        mk.Button(L"Tornar padrão", IDC_PROFILE_DEFAULT, ax + S(252), by, S(110), S(26));
        by += S(32);
        mk.Button(L"Exportar perfis...", IDC_PROFILE_EXPORT, ax, by, S(130), S(26));
        mk.Button(L"Importar perfis...", IDC_PROFILE_IMPORT, ax + S(136), by, S(130), S(26));
    }

    // Automation tab
    mk.sink = &tabControls_[3];
    {
        int y = ay;
        AddTip(mk.Label(L"Regras por aplicativo", ax, y, aw),
               L"Trocam de perfil quando o programa vai para o primeiro plano. Têm "
               L"prioridade sobre as regras por horário: enquanto uma delas bate, "
               L"o horário não é consultado.");
        y += S(22);
        appListView_ = mk.ListView(IDC_APP_LIST, ax, y, aw, S(150));
        AddColumn(appListView_, 0, L"Processo", S(260));
        AddColumn(appListView_, 1, L"Perfil", S(200));
        AddColumn(appListView_, 2, L"Prioridade", S(90));
        y += S(158);

        NameField(IDC_APP_PROCESS, L"Processo", ax, y + S(4), S(70));
        // Drop-down listing the programs running now, still typable for
        // wildcards ('cs*') and for programs that are not running.
        appProcessEdit_ = mk.ComboEdit(IDC_APP_PROCESS, ax + S(72), y, S(180));
        NameField(IDC_APP_PROFILE, L"Perfil", ax + S(262), y + S(4), S(40));
        appProfileCombo_ = mk.Combo(IDC_APP_PROFILE, ax + S(304), y, S(160));
        NameField(IDC_APP_PRIORITY, L"Prior.", ax + S(474), y + S(4), S(38));
        appPriorityEdit_ = mk.Edit(IDC_APP_PRIORITY, ax + S(514), y, S(50), ES_NUMBER);
        appEnabledCheck_ = mk.Check(L"Ativa", IDC_APP_ENABLED, ax + S(576), y + S(2), S(70));
        y += S(30);

        mk.Button(L"Adicionar", IDC_APP_ADD, ax, y, S(90), S(26));
        mk.Button(L"Atualizar", IDC_APP_UPDATE, ax + S(96), y, S(90), S(26));
        mk.Button(L"Remover", IDC_APP_DELETE, ax + S(192), y, S(90), S(26));
        mk.Button(L"Usar o programa em foco", IDC_APP_PICK, ax + S(288), y, S(190), S(26));
        y += S(30);

        mk.Hint(L"A lista de Processo mostra os programas abertos agora. Da para "
                L"digitar também, inclusive com '*' (ex.: cs* pega cs2 e csgo).",
                ax, y, aw, S(18));
        y += S(24);

        AddTip(mk.Label(L"Regras por horário", ax, y, aw),
               L"Valem quando nenhuma regra de aplicativo bate. Se nenhuma faixa "
               L"pegar o horário atual, entra o perfil padrão.");
        y += S(22);
        schedList_ = mk.ListView(IDC_SCHED_LIST, ax, y, aw, S(110));
        AddColumn(schedList_, 0, L"Início", S(120));
        AddColumn(schedList_, 1, L"Fim", S(120));
        AddColumn(schedList_, 2, L"Perfil", S(220));
        // Priority is listed because it decides between two overlapping ranges.
        AddColumn(schedList_, 3, L"Prioridade", S(90));
        y += S(118);

        NameField(IDC_SCHED_START, L"Início", ax, y + S(4), S(46));
        schedStartEdit_ = mk.Edit(IDC_SCHED_START, ax + S(48), y, S(70));
        NameField(IDC_SCHED_END, L"Fim", ax + S(128), y + S(4), S(34));
        schedEndEdit_ = mk.Edit(IDC_SCHED_END, ax + S(164), y, S(70));
        NameField(IDC_SCHED_PROFILE, L"Perfil", ax + S(244), y + S(4), S(40));
        schedProfileCombo_ = mk.Combo(IDC_SCHED_PROFILE, ax + S(286), y, S(160));
        // Updating rebuilds the whole rule, so priority needs a field of its own
        // here to survive a round trip through the dialog.
        NameField(IDC_SCHED_PRIORITY, L"Prior.", ax + S(452), y + S(4), S(40));
        schedPriorityEdit_ = mk.Edit(IDC_SCHED_PRIORITY, ax + S(494), y, S(50));
        schedEnabledCheck_ = mk.Check(L"Ativa", IDC_SCHED_ENABLED, ax + S(556), y + S(2), S(70));
        y += S(30);

        mk.Button(L"Adicionar", IDC_SCHED_ADD, ax, y, S(90), S(26));
        mk.Button(L"Atualizar", IDC_SCHED_UPDATE, ax + S(96), y, S(90), S(26));
        mk.Button(L"Remover", IDC_SCHED_DELETE, ax + S(192), y, S(90), S(26));
        y += S(36);

        mk.Hint(L"Início e Fim aceitam relógio (22:00) ou o próprio sol: 'por', "
                L"'nascer', e com deslocamento como 'por-30' ou 'nascer+45'. O "
                L"horário do por do sol anda mais de duas horas ao longo do ano, "
                L"então uma faixa fixa fica errada em metade dos meses.",
                ax, y, aw, S(34));
        y += S(38);

        AddTip(mk.Label(L"Localização", ax, y + S(4), S(80)),
               L"Só é usada para calcular o nascer e o pôr do sol. Fica no seu PC, no "
               L"zdisplay.ini — o Zdisplay não acessa a rede.");
        NameField(IDC_LATITUDE, L"Latitude", ax + S(84), y + S(4), S(56));
        latitudeEdit_ = mk.Edit(IDC_LATITUDE, ax + S(142), y, S(90));
        NameField(IDC_LONGITUDE, L"Longitude", ax + S(242), y + S(4), S(66));
        longitudeEdit_ = mk.Edit(IDC_LONGITUDE, ax + S(310), y, S(90));
    }

    // System tab
    mk.sink = &tabControls_[4];
    {
        const int colW = (aw - S(30)) / 2;
        const int lx = ax;
        const int rx = ax + colW + S(30);
        int ly = ay, ry = ay;

        Section(L"Comportamento", lx, ly, colW,
              L"Como o Zdisplay se comporta ao ligar, ao sair e enquanto está aberto."); ly += S(26);
        checkStartup_   = mk.Check(L"Iniciar com o Windows", IDC_CHK_STARTUP, lx, ly, colW); ly += S(24);
        checkMinimized_ = mk.Check(L"Iniciar minimizado na bandeja", IDC_CHK_MINIMIZED, lx, ly, colW); ly += S(24);
        checkAppRules_  = mk.Check(L"Trocar de perfil conforme o programa em foco", IDC_CHK_APPRULES, lx, ly, colW); ly += S(24);
        checkSchedule_  = mk.Check(L"Trocar de perfil conforme o horário", IDC_CHK_SCHEDULE, lx, ly, colW); ly += S(24);
        checkRestore_   = mk.Check(L"Restaurar a tela ao sair", IDC_CHK_RESTORE, lx, ly, colW); ly += S(24);
        checkConfirmDark_ = mk.Check(L"Confirmar quando os ajustes escurecerem demais",
                                     IDC_CHK_CONFIRM_DARK, lx, ly, colW); ly += S(24);
        checkMirrorKeys_ = mk.Check(L"Teclas de brilho também valem nos monitores externos",
                                    IDC_CHK_MIRROR_KEYS, lx, ly, colW);
        ly += S(24);
        mk.Hint(L"Aplica o ajuste, mas desfaz sozinho em 15 s se você não confirmar. "
                L"É o que impede alguém de se trancar numa tela preta.",
                lx + S(4), ly, colW - S(4), S(34));
        ly += S(40);

        NameField(IDC_WATCHDOG, L"Reaplicar ajustes a cada (s)", lx, ly + S(4), S(170));
        watchdogEdit_ = mk.Edit(IDC_WATCHDOG, lx + S(176), ly, S(60), ES_NUMBER);
        ly += S(26);
        mk.Hint(L"0 desliga. Protege contra a Luz noturna, drivers e jogos que "
                L"sobrescrevem a rampa de gamma.", lx + S(4), ly, colW - S(4), S(34));
        ly += S(40);

        Section(L"Backends", lx, ly, colW,
              L"Os caminhos pelos quais o Zdisplay mexe na tela. Desligar um só é útil "
              L"para isolar um problema. Mudanças valem no próximo início."); ly += S(26);
        checkVendor_    = mk.Check(L"APIs do fabricante (NVIDIA NVAPI / AMD ADL)", IDC_CHK_VENDOR, lx, ly, colW); ly += S(24);
        checkMagnify_   = mk.Check(L"Matriz de cor universal (Magnification API)", IDC_CHK_MAGNIFY, lx, ly, colW); ly += S(24);
        checkDdc_       = mk.Check(L"Hardware do monitor por DDC/CI", IDC_CHK_DDC, lx, ly, colW); ly += S(24);
        checkBacklight_ = mk.Check(L"Backlight do notebook (WMI)", IDC_CHK_BACKLIGHT, lx, ly, colW); ly += S(24);
        checkOverlay_   = mk.Check(L"Camada de escurecimento", IDC_CHK_OVERLAY, lx, ly, colW); ly += S(32);

        unlockButton_ = mk.Button(L"Liberar faixa completa de gamma (admin)", IDC_UNLOCK_GAMMA,
                                  lx, ly, S(280), S(28));
        ly += S(34);
        mk.Button(L"Abrir pasta de configuração", IDC_OPEN_FOLDER, lx, ly, S(220), S(26));
        mk.Button(L"Padrão de fábrica...", IDC_FACTORY_RESET,
                  lx + S(226), ly, S(174), S(26));

        Section(L"Atalhos globais", rx, ry, colW,
              L"Valem de qualquer lugar do Windows, mesmo com esta janela fechada. "
              L"Formato: Ctrl+Alt+K, Ctrl+Shift+F5, Win+Alt+Up. Vazio desliga."); ry += S(26);
        const wchar_t* hkLabels[7] = {
            L"Aumentar brilho", L"Diminuir brilho",
            L"Aumentar saturação", L"Diminuir saturação",
            L"Pausar / retomar", L"Abrir esta janela",
            L"EMERGÊNCIA: devolver tela",
        };
        for (int i = 0; i < 7; ++i) {
            NameField(IDC_HK_BASE + i, hkLabels[i], rx, ry + S(4), S(150));
            hkEdits_[i] = mk.Edit(IDC_HK_BASE + i, rx + S(156), ry, S(180));
            ry += S(28);
        }
        ry += S(6);
        NameField(IDC_HK_STEP, L"Passo dos atalhos", rx, ry + S(4), S(150));
        stepEdit_ = mk.Edit(IDC_HK_STEP, rx + S(156), ry, S(60), ES_NUMBER);
        ry += S(34);
        mk.Hint(L"Formato: Ctrl+Alt+K, Ctrl+Shift+F5, Win+Alt+Up. Deixe vazio para "
                L"desativar um atalho.", rx, ry, colW, S(40));
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
        mk.Button(L"Atualizar", IDC_DIAG_REFRESH, ax, by, S(100), S(26));
        mk.Button(L"Copiar", IDC_DIAG_COPY, ax + S(106), by, S(90), S(26));
        mk.Button(L"Abrir o log", IDC_DIAG_OPENLOG, ax + S(202), by, S(110), S(26));
        mk.Button(L"Ler capacidades...", IDC_DIAG_CAPS,
                              ax + S(318), by, S(150), S(26));
        mk.Button(L"Testar o monitor", IDC_DIAG_ROUNDTRIP,
                                   ax + S(474), by, S(140), S(26));
        mk.Button(L"Liberar quarentena DDC", IDC_DIAG_DDC_RESET,
                                  ax + S(620), by, S(180), S(26));
    }

    // Tooltips. Every control has one; the label names the field in a couple of
    // words, the tooltip states what changing it does and what it costs. They
    // are grouped here instead of being spread through the layout above so the
    // whole interface can be read as text in one place.
    {
        // Same tooltip on the field and on its label.
        auto Dica = [&](int id, const wchar_t* text) {
            AddTip(id, text);
            for (const auto& r : labels)
                if (r.first == id) AddTip(r.second, text);
        };

        // Sliders: the same tooltip on the label, the bar and the value.
        const struct { AdjField campo; const wchar_t* texto; } dicasDasBarras[] = {
        { F_BRIGHT,
          L"Brilho por software, pela rampa de gamma. Funciona em qualquer GPU e "
          L"também dentro de jogos em tela cheia. Não apaga a luz de fundo do "
          L"painel — para isso use o brilho físico." },
        { F_CONTRAST,
          L"Contraste em torno do cinza médio. Valores altos estouram branco e "
          L"preto, perdendo detalhe nas duas pontas." },
        { F_GAMMA,
          L"Curva de resposta. Abaixo de 1 escurece os tons médios, acima de 1 "
          L"clareia. Não mexe no preto nem no branco." },
        { F_TEMP,
          L"Temperatura de cor. 6500 K é o neutro; abaixo disso a imagem esquenta, "
          L"como na Luz noturna do Windows." },
        { F_BLUEBLOCK,
          L"Corta a faixa azul mantendo o resto. Diferente da temperatura, que "
          L"reequilibra as três cores — aqui a imagem amarela mais rápido, em "
          L"troca de bloquear mais azul." },
        { F_SHADOWS,
          L"Levanta só a parte baixa da curva: o preto ganha um piso e o efeito "
          L"morre antes dos tons médios, então claros, cores fortes e contraste "
          L"geral ficam intactos. É o que os monitores de jogo chamam de Black "
          L"eQualizer. Vale também dentro de jogos em tela cheia." },
        { F_CLARITY,
          L"Afasta os tons quase pretos uns dos outros antes do levante, "
          L"devolvendo o detalhe que ele achataria. Sozinha não clareia nada: "
          L"suba junto com Sombras. Não é nitidez de contorno — é contraste nos "
          L"tons baixos, que é o que faz o detalhe escuro reaparecer." },
        { F_SAT,
          L"Saturação pela matriz de cor universal, que dá o mesmo resultado em "
          L"qualquer PC — inclusive sem GPU dedicada." },
        { F_VIB,
          L"Vibrance da GPU: realça cores fracas sem estourar as fortes. Usa "
          L"NVAPI na NVIDIA e ADL na AMD." },
        { F_HUE,
          L"Gira todas as cores no círculo cromático. Serve para corrigir um "
          L"painel puxado para um lado; em imagem normal, desconfigura." },
        { F_DIM,
          L"Escurece com uma camada preta por cima da tela. Vai além do mínimo do "
          L"painel, mas lava o contraste e aparece em capturas de tela." },
        { F_RGAIN,
          L"Teto do canal vermelho. Serve para casar duas telas lado a lado; para "
          L"esquentar ou esfriar a imagem, use Temperatura." },
        { F_GGAIN, L"Teto do canal verde. Ver Ganho vermelho." },
        { F_BGAIN, L"Teto do canal azul. Ver Ganho vermelho." },
        { F_HWBRIGHT,
          L"Brilho físico do monitor, por DDC/CI ou pela luz de fundo do notebook. "
          L"Reduz a luz de verdade, sem lavar o contraste." },
        { F_HWCONTRAST,
          L"Contraste do próprio painel, por DDC/CI. Mexe no hardware, então vale "
          L"inclusive com HDR ligado." },
        };
        for (const auto& d : dicasDasBarras) {
            AddTip(sliders_[d.campo].bar,   d.texto);
            AddTip(sliders_[d.campo].label, d.texto);
            AddTip(sliders_[d.campo].value, d.texto);
        }

        // Adjustments tab.
        Dica(IDC_PROFILE_COMBO,
               L"Conjunto de ajustes com nome. Escolher aqui fixa o perfil à mão e "
               L"desliga a troca automática até você clicar em Automático.");
        Dica(IDC_AUTO_BTN,
               L"Devolve o comando às regras: volta a valer o programa em foco, o "
               L"horário ou o perfil padrão, nessa ordem.");
        Dica(IDC_PAUSE_BTN,
               L"Devolve a tela ao estado original e para de reaplicar. Nada é "
               L"perdido — ao retomar, tudo volta como estava.");
        Dica(IDC_MONITOR_COMBO,
               L"Escolhe a qual tela os ajustes abaixo se referem. Em 'Todos', o "
               L"que você mexer vale para o conjunto.");
        Dica(IDC_INVERT,
               L"Troca cada cor pela sua oposta. Serve para leitura em tela clara e "
               L"como recurso de acessibilidade.");
        Dica(IDC_MANAGE_HWBRIGHT,
               L"Deixa o perfil mandar no brilho do próprio monitor. Enquanto "
               L"desmarcado, o Zdisplay não toca no que está no painel.");
        Dica(IDC_MANAGE_HWCONTRAST,
               L"Mesma coisa para o contraste do painel. Poucos monitores aceitam, "
               L"e o valor de fábrica costuma ser o melhor.");
        Dica(IDC_RESET_ALL,
               L"Devolve TODAS as barras deste perfil ao neutro. Mexe no perfil, "
               L"não só na tela — e é gravado.");
        Dica(IDC_RESTORE_SCREEN,
               L"Devolve a tela ao estado original sem mexer no perfil. Use quando "
               L"outro programa bagunçar a cor.");
        for (int i = 0; i < F_COUNT; ++i)
            Dica(IDC_SLIDER_RESET_BASE + i, L"Volta esta barra ao valor neutro.");

        Dica(IDC_PER_MONITOR,
             L"Marque para este monitor ter valores próprios, independentes dos "
             L"demais. Desmarcado, ele segue o ajuste comum do perfil.");
        Dica(IDC_COMPARE,
             L"Mantenha pressionado para ver a tela como ela era antes dos "
             L"ajustes. Ao soltar, o ajuste volta.");
        Dica(IDC_DDC_MONITOR_MODE,
             L"Automático usa o intervalo normal entre comandos. Lento espera "
             L"mais, útil em docks e adaptadores instáveis. Nunca usar exclui "
             L"este monitor de qualquer sondagem DDC/CI. Vale no próximo início.");
        Dica(IDC_MON_INPUT,
             L"Troca a entrada de vídeo do monitor (HDMI, DisplayPort, USB-C). É "
             L"o mesmo que fazer pelos botões do painel — útil quando duas "
             L"máquinas dividem a mesma tela.");
        Dica(IDC_MON_PRESET,
             L"Predefinição de cor do próprio monitor. Age no hardware, então "
             L"continua valendo com HDR ligado, quando a rampa de gamma não vale.");
        Dica(IDC_MON_POWER,
             L"Desliga ou suspende o monitor. Para religá-lo pode ser necessário "
             L"o botão do painel: nem todo monitor responde a DDC/CI enquanto "
             L"está desligado.");


        // Vision tab.
        Dica(IDC_VIS_ENABLE,
               L"Uma camada que age por cima do perfil ativo, seja ele qual for. "
               L"Não substitui perfis nem regras de horário: soma-se a eles.");
        Dica(IDC_VIS_DAY_TEMP,
               L"Cor da tela durante o dia, em kelvin. 6500 K é o branco neutro do "
               L"sRGB — deixe assim se de dia já está bom.");
        Dica(IDC_VIS_NIGHT_TEMP,
               L"Cor da tela durante a noite. 3400 K é a cor de lâmpada "
               L"incandescente; quanto menor, menos azul chega aos olhos.");
        Dica(IDC_VIS_NIGHT_BRIGHT,
               L"Percentual do brilho do perfil aplicado à noite. 100 não mexe. "
               L"Cansa mais a vista a tela estar clara demais para o ambiente do "
               L"que a cor dela.");
        Dica(IDC_VIS_NIGHT_START,
               L"Relógio (22:00) ou o sol: 'por', 'por-30', 'por+45'. Sem latitude "
               L"e longitude preenchidas na aba Automação, vale 20:00.");
        Dica(IDC_VIS_DAY_START,
               L"Relógio (07:00) ou o sol: 'nascer', 'nascer+45'. Sem localização, "
               L"vale 07:00.");
        Dica(IDC_VIS_TRANSITION,
               L"Quantos minutos a mudança leva, metade antes e metade depois do "
               L"horário. Uma hora é o bastante para não se perceber acontecendo.");
        Dica(IDC_VIS_BREAK,
               L"0 desliga. A cada tantos minutos aparece um aviso para olhar 20 "
               L"segundos para algo a uns 6 metros — a única recomendação contra "
               L"cansaço visual com apoio clínico de verdade.");
        Dica(IDC_VIS_TEST_BREAK,
               L"Mostra o aviso agora, sem esperar o intervalo. Se as notificações "
               L"do Windows estiverem desligadas, ele avisa em vez de sumir calado.");
        Dica(IDC_VIS_PREVIEW_DAY,
               L"Aplica a cor de dia por cinco segundos e volta sozinho. Com o dia "
               L"em 6500 K não há o que ver: 6500 K já é o neutro.");
        Dica(IDC_VIS_PREVIEW_NIGHT,
               L"Aplica a cor de noite por cinco segundos e volta sozinho.");

        // Profiles tab.
        Dica(IDC_PROFILE_LIST,
               L"Todos os perfis salvos. O selecionado é o que os campos ao lado "
               L"editam — selecionar aqui não aplica nada à tela.");
        Dica(IDC_PROFILE_NAME,
               L"Renomear atualiza sozinho as regras de aplicativo e de horário que "
               L"apontam para este perfil.");
        Dica(IDC_PROFILE_HOTKEY,
               L"Combinação que ativa este perfil de qualquer lugar do Windows. "
               L"Ex.: Ctrl+Alt+1. Vazio desliga.");
        Dica(IDC_PROFILE_TRANSITION,
               L"Quanto tempo a tela leva para chegar neste perfil. 0 troca de "
               L"imediato; algumas centenas de ms disfarçam o salto.");
        Dica(IDC_PROFILE_SATENGINE,
               L"Quem faz a saturação. Automático usa a matriz universal, que dá o "
               L"mesmo resultado em qualquer máquina, e deixa o vibrance com a GPU.");
        Dica(IDC_PROFILE_NEW,      L"Cria um perfil no neutro.");
        Dica(IDC_PROFILE_DUP,      L"Copia o perfil selecionado, com os mesmos valores.");
        Dica(IDC_PROFILE_DELETE,
               L"Apaga o perfil selecionado. As regras que apontavam para ele ficam "
               L"sem efeito até você corrigi-las.");
        Dica(IDC_PROFILE_DEFAULT,
               L"O perfil usado quando nenhuma regra de aplicativo ou de horário "
               L"bate.");
        Dica(IDC_PROFILE_EXPORT,
               L"Grava os perfis num arquivo de texto, para levar a outro PC ou "
               L"guardar antes de experimentar.");
        Dica(IDC_PROFILE_IMPORT,
               L"Lê perfis de um arquivo exportado. Nome repetido entra como cópia; "
               L"nada é sobrescrito.");

        // Automation tab.
        Dica(IDC_APP_LIST,
               L"Trocam de perfil quando o programa vai para o primeiro plano. A "
               L"caixa de cada linha liga e desliga a regra sem apagá-la.");
        Dica(IDC_APP_PROCESS,
               L"Nome do executável, sem .exe. A lista mostra o que está aberto "
               L"agora, e dá para digitar com '*' — 'cs*' pega cs2 e csgo.");
        Dica(IDC_APP_PROFILE,   L"Perfil aplicado enquanto esse programa estiver em foco.");
        Dica(IDC_APP_PRIORITY,
               L"Desempata quando dois padrões pegam o mesmo programa. Maior vence.");
        Dica(IDC_APP_ENABLED,   L"Desliga a regra sem apagá-la.");
        Dica(IDC_APP_ADD,       L"Cria uma regra com o que está nos campos acima.");
        Dica(IDC_APP_UPDATE,    L"Grava as alterações na regra selecionada na lista.");
        Dica(IDC_APP_DELETE,    L"Apaga a regra selecionada.");
        Dica(IDC_APP_PICK,
               L"Preenche o campo Processo com o programa que está em primeiro "
               L"plano agora — poupa descobrir o nome do executável.");
        Dica(IDC_SCHED_LIST,
               L"Valem quando nenhuma regra de aplicativo bate. A caixa de cada "
               L"linha liga e desliga a faixa sem apagá-la.");
        Dica(IDC_SCHED_START,
               L"Relógio (22:00) ou o sol: 'por', 'nascer', 'por-30', 'nascer+45'. "
               L"O pôr do sol anda mais de duas horas ao longo do ano, então faixa "
               L"fixa fica errada em metade dos meses.");
        Dica(IDC_SCHED_END,      L"Mesmo formato do Início. A faixa pode cruzar a meia-noite.");
        Dica(IDC_SCHED_PROFILE,  L"Perfil aplicado dentro da faixa.");
        Dica(IDC_SCHED_PRIORITY, L"Desempata faixas que se sobrepõem. Maior vence.");
        Dica(IDC_SCHED_ENABLED,  L"Desliga a faixa sem apagá-la.");
        Dica(IDC_SCHED_ADD,      L"Cria uma faixa com o que está nos campos acima.");
        Dica(IDC_SCHED_UPDATE,   L"Grava as alterações na faixa selecionada na lista.");
        Dica(IDC_SCHED_DELETE,   L"Apaga a faixa selecionada.");

        Dica(IDC_LATITUDE,
             L"Em graus decimais, positivo ao norte. São Paulo: -23,55. Sem "
             L"preencher, as regras com 'nascer' e 'por' não entram — trocar o "
             L"perfil no horário de um lugar onde você não está seria pior que "
             L"não trocar.");
        Dica(IDC_LONGITUDE,
             L"Em graus decimais, positivo a leste. São Paulo: -46,63.");

        // System tab.
        Dica(IDC_CHK_STARTUP,
               L"Registra o Zdisplay para abrir no login. Vale só para esta conta "
               L"do Windows e não pede administrador.");
        Dica(IDC_CHK_MINIMIZED,
               L"Abre direto na bandeja, sem esta janela. Os ajustes são aplicados "
               L"do mesmo jeito.");
        Dica(IDC_CHK_APPRULES,
               L"Chave geral das regras por aplicativo. Desligada, o Zdisplay para "
               L"de acompanhar qual programa está em foco.");
        Dica(IDC_CHK_SCHEDULE, L"Chave geral das regras por horário.");
        Dica(IDC_CHK_RESTORE,
               L"Ao sair, devolve a tela ao estado original. Desmarcado, o último "
               L"ajuste continua na tela depois de fechar.");
        Dica(IDC_CHK_CONFIRM_DARK,
               L"Aplica o ajuste e desfaz sozinho em 15 s se você não confirmar. É "
               L"o que impede alguém de se trancar numa tela preta.");
        Dica(IDC_WATCHDOG,
               L"De quantos em quantos segundos o ajuste é reaplicado. 0 desliga. "
               L"Protege contra a Luz noturna, drivers e jogos que sobrescrevem a "
               L"rampa de gamma.");
        Dica(IDC_CHK_VENDOR,
               L"NVAPI na NVIDIA, ADL na AMD. É o único caminho para o vibrance de "
               L"verdade da placa.");
        Dica(IDC_CHK_MAGNIFY,
               L"Matriz de cor aplicada pelo compositor do Windows. Dá saturação e "
               L"matiz iguais em qualquer máquina, com ou sem placa dedicada.");
        Dica(IDC_CHK_DDC,
               L"Fala com o monitor pelo cabo de vídeo. É o que permite mexer no "
               L"brilho físico, no contraste e na entrada.");
        Dica(IDC_CHK_BACKLIGHT,
               L"Luz de fundo do painel interno de notebooks, via WMI.");
        Dica(IDC_CHK_OVERLAY,
               L"Janela preta translúcida por cima de tudo. Escurece além do mínimo "
               L"do painel, mas lava o contraste e aparece em capturas de tela.");
        Dica(IDC_UNLOCK_GAMMA,
               L"O Windows corta pela metade a força da rampa de gamma. Liberar a "
               L"faixa completa exige administrador e escreve uma chave do sistema; "
               L"vale para todos os programas, não só o Zdisplay.");
        Dica(IDC_RELOCK_GAMMA,
               L"Devolve o limite padrão do Windows à rampa de gamma. Exige "
               L"administrador e vale para todos os programas.");
        Dica(IDC_FACTORY_RESET,
             L"Apaga tudo o que você configurou — perfis, regras, atalhos, "
             L"localização e as opções desta aba — e devolve o Zdisplay ao "
             L"estado de recém-instalado. Pede confirmação e guarda uma cópia "
             L"do arquivo atual antes de apagar.");
        Dica(IDC_OPEN_FOLDER,
               L"Abre a pasta com zdisplay.ini, o log e a cópia do estado original "
               L"da tela.");
        Dica(IDC_HK_BASE + 0, L"Sobe o brilho do perfil ativo, de qualquer lugar do Windows.");
        Dica(IDC_HK_BASE + 1, L"Desce o brilho do perfil ativo.");
        Dica(IDC_HK_BASE + 2, L"Sobe a saturação do perfil ativo.");
        Dica(IDC_HK_BASE + 3, L"Desce a saturação do perfil ativo.");
        Dica(IDC_HK_BASE + 4, L"Pausa e retoma sem abrir esta janela.");
        Dica(IDC_HK_BASE + 5, L"Traz esta janela para a frente.");
        Dica(IDC_HK_STEP,
               L"De quanto andam os atalhos de brilho e saturação a cada toque, em "
               L"pontos percentuais.");

        Dica(IDC_CHK_MIRROR_KEYS,
             L"Num notebook acoplado, as teclas de brilho só mexem no painel de "
             L"dentro. Com isto ligado, o Zdisplay leva a mesma mudança aos "
             L"monitores externos por DDC/CI. Perfil que já gerencia o brilho "
             L"físico continua mandando, para os dois não brigarem.");
        Dica(IDC_HK_BASE + 6,
             L"Devolve a tela ao estado original e pausa o Zdisplay, de qualquer "
             L"lugar do Windows. Se apagar este campo, o padrão volta sozinho — "
             L"é a saída de emergência.");

        // Diagnostics tab.
        Dica(IDC_DIAG_TEXT,
               L"Retrato do que o Zdisplay enxerga: monitores, placa, EDID e quais "
               L"backends subiram. É o que vale anexar num relato de problema.");
        Dica(IDC_DIAG_REFRESH, L"Relê tudo agora, sem reiniciar o programa.");

        // Labels held in members, which have no control ID.
        AddTip(monFeaturesLabel_,
               L"Agem na hora e não entram no perfil: trocar a entrada ou desligar "
               L"a tela é uma ação pontual, não um ajuste que faça sentido "
               L"reaplicar a cada troca de perfil.");
        AddTip(statusBar_,
               L"Perfil ativo, quantos caminhos de ajuste subiram e quantos "
               L"monitores o Zdisplay enxerga agora.");
        AddTip(visStatus_,
               L"O que está valendo neste instante e o que vem a seguir. Atualiza "
               L"sozinho conforme o dia passa.");
        AddTip(defaultLabel_,
               L"O perfil padrão é o usado quando nenhuma regra de aplicativo ou de "
               L"horário bate.");
        AddTip(hotkeyWarning_,
               L"Aparece quando o Windows recusa um atalho porque outro programa já "
               L"o tomou. Vazio significa que todos foram aceitos.");
        Dica(IDC_DIAG_COPY,    L"Copia este texto para a área de transferência.");
        Dica(IDC_DIAG_CAPS,
             L"Pergunta a cada monitor quais recursos DDC/CI ele declara. Não é "
             L"feito sozinho: existe um defeito do Windows em que uma resposta "
             L"malformada — justo a de monitor genérico — derruba o sistema. Só "
             L"serve para diagnóstico; o Zdisplay não precisa dela.");
        Dica(IDC_DIAG_ROUNDTRIP,
             L"Prova que o monitor obedece de verdade: muda o brilho um passo, lê "
             L"de volta, confere e devolve o valor que estava. Há painel que "
             L"aceita o comando, responde sucesso e não muda nada — só este teste "
             L"separa esse caso de um ajuste que funcionou.");
        Dica(IDC_DIAG_DDC_RESET,
             L"Libera monitores cuja leitura de capacidades ficou bloqueada "
             L"depois de uma queda. Não inicia leitura nenhuma sozinho.");
        Dica(IDC_DIAG_OPENLOG,
               L"Abre o zdisplay.log no editor padrão. É lá que ficam os erros e o "
               L"histórico das últimas execuções.");
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
        L"Automático (recomendado)",
        L"Forçar GPU do fabricante",
        L"Forçar universal (qualquer PC)",
        L"Não mexer em saturação",
    };
    for (const wchar_t* e : engines)
        ::SendMessageW(satEngineCombo_, CB_ADDSTRING, 0, (LPARAM)e);

    ::SendMessageW(ddcModeCombo_, CB_RESETCONTENT, 0, 0);
    const wchar_t* ddcModes[] = {
        L"Automático (recomendado)", L"DDC lento (dock/adaptador)", L"Nunca usar DDC neste monitor"
    };
    for (const wchar_t* mode : ddcModes)
        ::SendMessageW(ddcModeCombo_, CB_ADDSTRING, 0, (LPARAM)mode);

    // System tab
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
        SetText(unlockButton_, L"Restaurar a faixa padrão do Windows (admin)");
        ::SetWindowLongPtrW(unlockButton_, GWLP_ID, IDC_RELOCK_GAMMA);
    }

    // Controls this machine cannot honor are dimmed, rather than left responsive
    // with no effect.
    const bool vendor = engine_->Nvidia()->Available() || engine_->Amd()->Available();
    if (!vendor) {
        SetText(sliders_[F_VIB].label, L"Vibrance (sem GPU)");
    }

    SetText(pauseButton_, engine_->Enabled() ? L"Pausar" : L"Retomar");

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
    ::SendMessageW(monitorCombo_, CB_ADDSTRING, 0, (LPARAM)L"Todos os monitores");
    for (const auto& m : monitors::All()) {
        const std::wstring label = m.isPrimary ? m.friendlyName + L"  (principal)" : m.friendlyName;
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
        SetText(monFeaturesLabel_, L"Comandos do monitor — escolha um monitor acima");
        loadingUi_ = wasLoading;
        return;
    }

    if (!engine_->Ddc()->Supports(key)) {
        SetText(monFeaturesLabel_, L"Comandos do monitor — este não responde a DDC/CI");
        loadingUi_ = wasLoading;
        return;
    }

    if (!engine_->Ddc()->FeaturesProbed(key)) {
        // The probe costs several slow commands and completes asynchronously, so
        // the header states that the empty lists are pending rather than
        // unsupported.
        SetText(monFeaturesLabel_, L"Comandos do monitor — perguntando ao monitor...");
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
            if (name.empty()) name = Format(L"valor 0x%02X", (unsigned)v);
            ::SendMessageW(s.combo, CB_ADDSTRING, 0, (LPARAM)name.c_str());
            s.values->push_back(v);
            if ((int)v == found->current) select = (int)i;
        }
        ::SendMessageW(s.combo, CB_SETCURSEL, (WPARAM)select, 0);
        ::EnableWindow(s.combo, TRUE);
        ++offered;
    }

    SetText(monFeaturesLabel_, offered > 0
        ? L"Comandos do monitor — agem na hora, fora do perfil"
        : L"Comandos do monitor — este não expôs nenhum deles");

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
    SetText(manageHwBright_, L"Controlar o brilho físico");
    SetText(manageHwContrast_, L"Controlar o contraste físico");
    if (!hwBrightAvailable)
        SetText(manageHwBright_, L"Controlar o brilho físico — sem monitor compatível");
    if (!hwContrastAvailable)
        SetText(manageHwContrast_, L"Controlar o contraste físico — indisponível");

    SetText(pauseButton_, engine_->Enabled() ? L"Pausar" : L"Retomar");

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
                                   ? L"Este é o perfil padrão."
                                   : L"Perfil padrão atual: " + config_.defaultProfile);
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

    std::wstring t;
    t += L"BACKENDS DETECTADOS\r\n";
    t += L"------------------------------------------------------------------\r\n";
    t += engine_->DescribeBackends();
    t += L"\r\nMONITORES\r\n";
    t += L"------------------------------------------------------------------\r\n";
    for (const auto& m : monitors::All()) {
        t += L"  " + m.friendlyName + (m.isPrimary ? L"  [principal]" : L"") +
             (m.isInternal ? L"  [embutido]" : L"") + L"\r\n";
        t += Format(L"     GDI: %s   %ldx%ld\r\n", m.deviceName.c_str(),
                    m.bounds.right - m.bounds.left, m.bounds.bottom - m.bounds.top);
        t += L"     chave: " + m.key + L"\r\n";
        if (!m.adapterName.empty() || m.gpuVendorId) {
            t += L"     placa: " + m.adapterName;
            const wchar_t* v = GpuVendorName(m.gpuVendorId);
            if (v[0]) t += Format(L"  [%s, PCI %04X]", v, m.gpuVendorId);
            t += L"\r\n";
            // On a machine with two GPUs, the vendor path applies only to the
            // display its own adapter actually drives.
            if (m.gpuVendorId == kVendorIntel)
                t += L"     saturação: pela matriz de cor universal (a Intel não\r\n"
                     L"                expõe vibrance por driver como NVIDIA e AMD)\r\n";
        }
        if (m.edid.valid) {
            t += Format(L"     EDID: %s %04X", m.edid.manufacturer.c_str(), m.edid.product);
            if (m.edid.year) t += Format(L"  %d", m.edid.year);
            if (!m.edid.serialText.empty()) t += L"  s/n " + m.edid.serialText;
            else if (m.edid.serial) t += Format(L"  s/n %08X", m.edid.serial);
            t += Format(L"  %s\r\n", m.edid.digital ? L"digital" : L"analogico");
            t += Format(L"     gamut: área %.3f (sRGB %.3f)%s\r\n", m.edid.gamutArea,
                        kSrgbGamutArea,
                        m.edid.wideGamut ? L"  — gamut largo: cores saem mais fortes que o sRGB"
                                         : L"");
        } else {
            t += L"     EDID: não publicado ou inválido — a chave cai para o "
                 L"caminho do dispositivo\r\n";
        }
        if (m.isHdr)
            t += L"     HDR: LIGADO — o Windows aceita a rampa de gamma e a ignora\r\n";
        else if (m.hdrCapable)
            t += L"     HDR: suportado, desligado\r\n";

        // What actually controls THIS monitor: the global backend count above
        // does not explain why a slider has no effect on a given display.
        for (const auto& line : engine_->MonitorCoverage(m))
            t += L"     " + line + L"\r\n";
    }

    const auto ddcLines = engine_->Ddc()->Diagnose();
    if (!ddcLines.empty()) {
        t += L"\r\nDDC/CI POR MONITOR\r\n";
        t += L"------------------------------------------------------------------\r\n";
        for (const auto& line : ddcLines) t += L"  " + line + L"\r\n";
    }

    Profile* p = engine_->Active();
    t += L"\r\nESTADO\r\n";
    t += L"------------------------------------------------------------------\r\n";
    // Version comes first: this tab exists to be pasted into a problem report,
    // which is unusable without knowing the build it came from.
    t += L"  Versão ............... " + std::wstring(ZDISPLAY_VERSION_WSTR) + L"\r\n";
    t += L"  Perfil ativo ......... " + (p ? p->name : std::wstring(L"-")) + L"\r\n";
    t += L"  Modo ................. " +
         std::wstring(engine_->ManualProfile().empty() ? L"automático" : L"manual") + L"\r\n";
    t += L"  Zdisplay ............. " +
         std::wstring(engine_->Enabled() ? L"ativo" : L"pausado") + L"\r\n";
    t += L"  Programa em foco ..... " + engine_->ForegroundProcess() + L"\r\n";
    t += L"  Luz noturna Windows .. " +
         std::wstring(GammaBackend::NightLightActive() ? L"LIGADA (pode conflitar)" : L"desligada") + L"\r\n";
    t += L"  Faixa de gamma ....... " +
         std::wstring(GammaBackend::RangeUnlocked() ? L"ampliada" : L"padrão do Windows") + L"\r\n";
    if (engine_->Gamma()->Limited()) {
        t += Format(L"  ATENÇÃO .............. o Windows só aceitou %.0f%% do efeito pedido.\r\n"
                    L"                         Use 'Liberar faixa completa de gamma' na aba\r\n"
                    L"                         Sistema (precisa de admin e de reiniciar a sessão).\r\n",
                    engine_->Gamma()->AcceptedFraction() * 100.0);
    }
    t += L"  Configuração ......... " + ConfigPath() + L"\r\n";

    t += L"\r\nLOG RECENTE\r\n";
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
        s = L"Desligado — a tela segue exatamente o perfil ativo.";
    } else {
        const double n = engine_->VisionShownNight();
        SYSTEMTIME now;
        ::GetLocalTime(&now);
        const SolarContext solar = config_.Solar();
        int nightAt = 0, dayAt = 0;
        bool fixedFallback = false;

        if (!ResolveVisionTimes(now, v, solar, &dayAt, &nightAt, &fixedFallback)) {
            s = L"Os horários não são válidos. Use, por exemplo, 20:00 e 07:00.";
        } else {
            Vision clean = v;
            clean.Sanitize();
            const double dayMired = 1e6 / clean.dayTemperature;
            const double nightMired = 1e6 / clean.nightTemperature;
            const double kelvin = 1e6 / (dayMired + (nightMired - dayMired) * n);
            const double brightness = 100.0 + (clean.nightBrightness - 100.0) * n;

            if (preview) {
                s = n > 0.5 ? L"Prévia temporária: noite."
                            : L"Prévia temporária: dia.";
            } else if (n < 0.01) {
                s = L"Agora: dia.";
            } else if (n > 0.99) {
                s = L"Agora: noite.";
            } else {
                const double next = NightFraction(MinutesLater(now, 1), v, solar);
                s = Format(L"Agora: passando para %s (%.0f%%).",
                           next >= n ? L"a noite" : L"o dia", n * 100.0);
            }
            s += Format(L"  Alvo: %.0f K e %.0f%% do brilho do perfil.",
                        kelvin, brightness);
            s += Format(L"\r\nA noite começa às %02d:%02d e o dia às %02d:%02d.",
                        nightAt / 60, nightAt % 60, dayAt / 60, dayAt % 60);
            if (fixedFallback)
                s += L"  Horário fixo enquanto a localização não estiver preenchida.";
        }
    }
    SetText(visStatus_, s);
}

void App::UpdateStatusBar() {
    if (!statusBar_ || !::IsWindow(statusBar_)) return;

    std::wstring s;
    if (!engine_->Enabled()) {
        s = L"Zdisplay pausado — a tela está no estado original.";
    } else if (engine_->Gamma()->Limited()) {
        // Windows clamped the effect; the status bar reports it instead of
        // implying the adjustment applied in full.
        s = Format(L"O Windows limitou o efeito a %.0f%% — veja 'Liberar faixa completa "
                   L"de gamma' na aba Sistema.",
                   engine_->Gamma()->AcceptedFraction() * 100.0);
    } else {
        Profile* p = engine_->Active();
        s = Format(L"Perfil '%s'   ·   %d backend(s) ativo(s)   ·   %d monitor(es)   ·   %s",
                   p ? p->name.c_str() : L"-",
                   engine_->AvailableBackendCount(),
                   (int)monitors::All().size(),
                   engine_->ManualProfile().empty() ? L"modo automático" : L"perfil fixado");
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
