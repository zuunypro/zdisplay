// Hidden host window, tray icon, global hotkeys, and the command interpreter
// shared by the command line and the named pipe.
#include "ui.h"
#include "ui_dpi.h"
#include "ui_ids.h"
#include "ui_theme.h"

namespace zdisplay {

App* App::instance_ = nullptr;

static const wchar_t* kHostClass = L"ZdisplayHostWindow";

bool ToastsGloballyOff() {
    // Master notification switch. The registry value only exists once it has
    // been changed, so a missing value means notifications are enabled.
    DWORD value = 1, size = sizeof(value), type = REG_DWORD;
    if (::RegGetValueW(HKEY_CURRENT_USER,
                       L"Software\\Microsoft\\Windows\\CurrentVersion\\PushNotifications",
                       L"ToastEnabled", RRF_RT_REG_DWORD, &type, &value, &size) != ERROR_SUCCESS)
        return false;
    return value == 0;
}

void EnableModernTrayBehavior(HWND owner, UINT id) {
    NOTIFYICONDATAW version{};
    version.cbSize = sizeof(version);
    version.hWnd = owner;
    version.uID = id;
    version.uVersion = NOTIFYICON_VERSION_4;
    ::Shell_NotifyIconW(NIM_SETVERSION, &version);
}

// Startup

bool App::Init(HINSTANCE inst, const std::vector<std::wstring>& args) {
    instance_ = this;
    inst_ = inst;

    // Must run before InitCommonControls and the host window: Windows caches the
    // native menu theme on first use, so dark mode has to be set up here for the
    // tray menu to be drawn in the right theme from the first frame.
    theme::InitializeProcess();

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES |
                ICC_LISTVIEW_CLASSES | ICC_UPDOWN_CLASS;
    ::InitCommonControlsEx(&icc);

    // UI font: the same one Windows uses in its dialogs.
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        font_ = ::CreateFontIndirectW(&ncm.lfMessageFont);
        LOGFONTW bold = ncm.lfMessageFont;
        bold.lfWeight = FW_SEMIBOLD;
        fontBold_ = ::CreateFontIndirectW(&bold);
    }
    if (!font_) font_ = (HFONT)::GetStockObject(DEFAULT_GUI_FONT);
    if (!fontBold_) fontBold_ = font_;

    icon_ = CreateAppIcon(32);
    iconSmall_ = CreateAppIcon((std::max)(16, ::GetSystemMetrics(SM_CXSMICON)));

    // Ordinary window that is never shown. It must be top-level rather than
    // message-only to receive broadcasts such as WM_DISPLAYCHANGE.
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostProc;
    wc.hInstance = inst_;
    wc.lpszClassName = kHostClass;
    wc.hIcon = icon_;
    if (!::RegisterClassExW(&wc)) {
        KLOG_E(L"Could not register the host window class (error %lu).", ::GetLastError());
        return false;
    }

    host_ = ::CreateWindowExW(WS_EX_TOOLWINDOW, kHostClass, L"Zdisplay",
                              WS_OVERLAPPED, 0, 0, 0, 0,
                              nullptr, nullptr, inst_, nullptr);
    if (!host_) {
        KLOG_E(L"Could not create the host window (error %lu).", ::GetLastError());
        return false;
    }
    RegisterSessionNotifications();

    LoadConfig(&config_);

    // Before any window exists: controls read their captions as they are
    // created, so a language installed later would leave a window half
    // translated.
    SetLanguage(config_.language);

    // Keeps the run-at-startup registry entry in sync with the config file.
    if (config_.startWithWindows != startup::IsEnabled())
        startup::Set(config_.startWithWindows);

    engine_.reset(new Engine(&config_));
    engine_->stateContext = this;
    engine_->onStateChanged = OnEngineStateChanged;

    // The tray icon is added before hardware initialization because DDC/CI and
    // WMI probing can take seconds. Clicks that arrive early are only processed
    // once the message loop starts, by which time the engine is ready.
    taskbarCreatedMsg_ = ::RegisterWindowMessageW(L"TaskbarCreated");
    tray_.cbSize = sizeof(tray_);
    tray_.hWnd = host_;
    tray_.uID = 1;
    tray_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    tray_.uCallbackMessage = WM_ZDISPLAY_TRAY;
    tray_.hIcon = iconSmall_ ? iconSmall_ : icon_;
    wcscpy_s(tray_.szTip, L"Zdisplay");
    trayAdded_ = ::Shell_NotifyIconW(NIM_ADD, &tray_) != FALSE;
    if (trayAdded_) EnableModernTrayBehavior(host_, tray_.uID);

    engine_->Initialize(host_);

    hotkeys_.reset(new Hotkeys(host_));
    RegisterHotkeys();
    ScheduleBreakReminder();

    if (config_.enableAppRules) {
        foreground_.Start(OnForegroundChanged, this);
        ::SetTimer(host_, TIMER_FOREGROUND_POLL, kForegroundPollMs, nullptr);
    }

    pipe_.Start(host_);

    UpdateTrayTip();

    // --tray forces a tray-only start regardless of the saved configuration.
    bool forceTray = false;
    for (const auto& a : args)
        if (a == L"--tray" || a == L"--minimized" || a == L"--background") forceTray = true;

    if (!forceTray && !config_.startMinimized) ShowSettings();

    // Checked at startup too: a saved profile that is too dark also needs
    // confirmation, otherwise the session begins on an unreadable screen.
    GuardDarkScreen();

    KLOG_I(L"Zdisplay ready. %d backend(s) active, %d monitor(s).",
           engine_->AvailableBackendCount(), (int)monitors::All().size());
    return true;
}

int App::Run() {
    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // IsDialogMessage supplies Tab navigation, mnemonics and default-button
        // handling for the settings window and for the dark-screen confirmation,
        // which has to stay keyboard-operable when the screen is unreadable.
        if (confirmWnd_ && ::IsWindow(confirmWnd_) && ::IsDialogMessageW(confirmWnd_, &msg))
            continue;
        if (settings_ && ::IsWindow(settings_) && ::IsDialogMessageW(settings_, &msg))
            continue;
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

void App::RequestExit() {
    if (exiting_) return;
    exiting_ = true;
    KLOG_I(L"Shutting Zdisplay down.");

    SaveConfig(config_);

    if (trayAdded_) { ::Shell_NotifyIconW(NIM_DELETE, &tray_); trayAdded_ = false; }
    UnregisterSessionNotifications();
    ::KillTimer(host_, TIMER_AUTOSAVE);
    ::KillTimer(host_, TIMER_DARKGUARD);
    ::KillTimer(host_, TIMER_FOREGROUND_POLL);
    ::KillTimer(host_, TIMER_VISION_PREVIEW);

    // The pipe thread may be blocked in a SendMessage to this thread. Stop()
    // pumps messages while waiting, and the command handler refuses requests
    // once exiting_ is set, so an in-flight command is answered rather than
    // reaching an engine that is being torn down.
    pipe_.Stop();
    foreground_.Stop();
    hotkeys_.reset();

    if (settings_ && ::IsWindow(settings_)) ::DestroyWindow(settings_);
    settings_ = nullptr;

    engine_.reset();

    if (font_ && font_ != (HFONT)::GetStockObject(DEFAULT_GUI_FONT)) ::DeleteObject(font_);
    if (fontBold_ && fontBold_ != font_) ::DeleteObject(fontBold_);
    if (icon_) ::DestroyIcon(icon_);
    if (iconSmall_) ::DestroyIcon(iconSmall_);
    theme::Shutdown();

    ::PostQuitMessage(0);
}

// Callbacks

void App::OnForegroundChanged(const std::wstring& process, void* ctx) {
    auto* self = static_cast<App*>(ctx);
    if (!self || !self->engine_) return;
    self->engine_->OnForegroundProcess(process);
    self->UpdateTrayTip();
}

void App::OnEngineStateChanged(void* ctx) {
    auto* self = static_cast<App*>(ctx);
    if (!self) return;
    self->MarkDirty();
    self->UpdateTrayTip();
    self->RefreshUi();
    self->ScheduleDarkGuard();
}

void App::ScheduleDarkGuard() {
    if (!host_ || exiting_) return;
    ::SetTimer(host_, TIMER_DARKGUARD, 700, nullptr);
}

void App::MarkDirty() {
    dirty_ = true;
    // One-shot timer; further changes before it fires push the deadline out.
    if (host_ && !exiting_) ::SetTimer(host_, TIMER_AUTOSAVE, 3000, nullptr);
}

void App::UpdateHotkeyWarning() {
    if (!hotkeyWarning_ || !::IsWindow(hotkeyWarning_)) return;

    if (failedHotkeys_.empty()) { ::SetWindowTextW(hotkeyWarning_, L""); return; }

    std::wstring list;
    for (size_t i = 0; i < failedHotkeys_.size(); ++i) {
        if (i) list += L", ";
        list += failedHotkeys_[i];
    }
    const std::wstring msg = Format(
        T(L"Not registered (another program already uses the combination): %s. "
          L"Choose a different combination for those."), list.c_str());
    ::SetWindowTextW(hotkeyWarning_, msg.c_str());
}

void App::RecreateUiFonts() {
    HFONT f = dpi::MessageFontFor(dpi::Current());
    if (!f) return;

    HFONT oldFont = font_, oldBold = fontBold_;
    font_ = f;

    LOGFONTW bold{};
    ::GetObjectW(font_, sizeof(bold), &bold);
    bold.lfWeight = FW_SEMIBOLD;
    fontBold_ = ::CreateFontIndirectW(&bold);
    if (!fontBold_) fontBold_ = font_;

    if (oldBold && oldBold != oldFont) ::DeleteObject(oldBold);
    if (oldFont && oldFont != (HFONT)::GetStockObject(DEFAULT_GUI_FONT))
        ::DeleteObject(oldFont);
}

void App::RefreshUi() {
    if (!settings_ || !::IsWindow(settings_)) return;
    if (loadingUi_) return;
    LoadAdjustments();
    if (activeTab_ == 1) LoadVision();
    UpdateStatusBar();
}

// Tray

// Fast user switching

namespace {
// Constants from wtsapi32.h, repeated here to avoid pulling in the header and
// its import library for a handful of values.
constexpr DWORD kNotifyForThisSession = 0;
constexpr WPARAM kWtsConsoleConnect    = 0x1;
constexpr WPARAM kWtsConsoleDisconnect = 0x2;
constexpr WPARAM kWtsRemoteConnect     = 0x3;
constexpr WPARAM kWtsRemoteDisconnect  = 0x4;

/// GUID_CONSOLE_DISPLAY_STATE, written out by hand because MinGW does not
/// export it in every version.
const GUID kGuidConsoleDisplayState =
    {0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};

/// GUID_DEVINTERFACE_MONITOR, written out by hand for the same reason.
const GUID kGuidDevInterfaceMonitor =
    {0xe6f07b5f, 0xee97, 0x4a90, {0xb0, 0x76, 0x33, 0xf5, 0x7b, 0xf4, 0xea, 0xa7}};

DynLib& WtsLib() {
    static DynLib lib(L"wtsapi32.dll");
    return lib;
}
}  // namespace

void App::RegisterSessionNotifications() {
    // Loaded at run time so it adds no link-time dependency; if the DLL is
    // missing, the program only loses this notification.
    using Fn = BOOL(WINAPI*)(HWND, DWORD);
    auto reg = WtsLib().Get<Fn>("WTSRegisterSessionNotification");
    sessionNotifyOk_ = reg && reg(host_, kNotifyForThisSession);
    if (!sessionNotifyOk_)
        KLOG_W(L"No user-switch notification; reassertion stays on even while the "
               L"session is in the background.");

    // Display on/off notification. Most external monitors reset brightness to
    // the factory default whenever they lose power, and the watchdog does not
    // poll DDC/CI because sending I2C every few seconds is slow and wears the
    // bus; the display-on signal fires once, exactly when reassertion is needed.
    displayNotify_ = ::RegisterPowerSettingNotification(
        host_, &kGuidConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (!displayNotify_)
        KLOG_W(L"No display-on notification; DDC/CI brightness may not come back "
               L"after the monitor is switched off and on again.");

    // Monitor arrival and removal. WM_DISPLAYCHANGE covers the plug that moves
    // the desktop layout, which is most of them, but not the panel swapped on a
    // KVM into the same resolution, nor the dock that publishes the monitor
    // after the topology has already settled. This notification catches those.
    DEV_BROADCAST_DEVICEINTERFACE_W filter{};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = kGuidDevInterfaceMonitor;
    deviceNotify_ = ::RegisterDeviceNotificationW(host_, &filter,
                                                  DEVICE_NOTIFY_WINDOW_HANDLE);
    if (!deviceNotify_)
        KLOG_W(L"No monitor-arrival notification; a panel swapped on a KVM may only "
               L"be noticed at the next video event.");
}

void App::UnregisterSessionNotifications() {
    if (deviceNotify_) {
        ::UnregisterDeviceNotification(deviceNotify_);
        deviceNotify_ = nullptr;
    }
    if (displayNotify_) {
        ::UnregisterPowerSettingNotification(displayNotify_);
        displayNotify_ = nullptr;
    }
    if (!sessionNotifyOk_) return;
    sessionNotifyOk_ = false;
    using Fn = BOOL(WINAPI*)(HWND);
    if (auto un = WtsLib().Get<Fn>("WTSUnRegisterSessionNotification")) un(host_);
}

void App::UpdateTrayTip() {
    if (!trayAdded_) return;

    std::wstring tip;
    if (!engine_ || !engine_->Enabled()) {
        tip = T(L"Zdisplay — paused");
    } else {
        Profile* p = engine_->Active();
        tip = Format(T(L"Zdisplay — %s\nBrightness %.0f%%  Saturation %.0f%%"),
                     p ? p->name.c_str() : L"-",
                     p ? p->global.brightness : 100.0,
                     p ? p->global.saturation : 100.0);
    }
    // The tray tooltip is limited to 127 characters.
    if (tip.size() > 126) tip.resize(126);
    wcscpy_s(tray_.szTip, tip.c_str());
    tray_.uFlags = NIF_TIP;
    ::Shell_NotifyIconW(NIM_MODIFY, &tray_);
}

void App::BuildTrayMenu() {
    HMENU menu = ::CreatePopupMenu();
    if (!menu) return;

    const bool on = engine_->Enabled();
    Profile* active = engine_->Active();

    std::wstring header = on
        ? (T(L"Profile: ") + (active ? active->name : std::wstring(L"-")))
        : T(L"Zdisplay paused");
    ::AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, header.c_str());
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    for (size_t i = 0; i < config_.profiles.size(); ++i) {
        const bool checked = active && IEquals(active->name, config_.profiles[i].name);
        ::AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : 0),
                      IDM_PROFILE_BASE + (UINT_PTR)i, config_.profiles[i].name.c_str());
    }

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING | (engine_->ManualProfile().empty() ? MF_CHECKED : 0),
                  IDM_AUTO, T(L"Automatic (app and schedule rules)"));

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, IDM_PAUSE,
                  on ? T(L"Pause (restores the display)") : T(L"Resume"));
    ::AppendMenuW(menu, MF_STRING, IDM_RESTORE,
                  Format(L"%s\t%s", T(L"Restore the display now"),
                         config_.hkPanic.c_str()).c_str());

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, IDM_SETTINGS, T(L"Settings..."));
    ::AppendMenuW(menu, MF_STRING, IDM_EXIT, T(L"Exit"));

    POINT pt;
    ::GetCursorPos(&pt);
    // Required: without this the menu does not dismiss on an outside click.
    ::SetForegroundWindow(host_);

    // TPM_RETURNCMD returns the item id instead of sending WM_COMMAND from
    // inside the menu's modal loop, where the menu still holds activation and
    // would take focus back from any window the command opens. The id is posted
    // after DestroyMenu, so the command runs with activation already free.
    const int cmd = (int)::TrackPopupMenu(
        menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RETURNCMD | TPM_NONOTIFY,
        pt.x, pt.y, 0, host_, nullptr);
    ::PostMessageW(host_, WM_NULL, 0, 0);
    ::DestroyMenu(menu);

    if (cmd) ::PostMessageW(host_, WM_COMMAND, (WPARAM)(UINT)cmd, 0);
}

void App::TogglePause(const wchar_t* source) {
    KLOG_I(L"[pausa] alternada por: %s", source ? source : L"?");
    engine_->SetEnabled(!engine_->Enabled());
    if (pauseButton_ && ::IsWindow(pauseButton_))
        ::SetWindowTextW(pauseButton_, engine_->Enabled() ? T(L"Pause") : T(L"Resume"));
    UpdateTrayTip();
    UpdateStatusBar();
}

// Global hotkeys

void App::ScheduleBreakReminder() {
    if (!host_) return;
    ::KillTimer(host_, TIMER_BREAK);

    const int minutes = config_.vision.breakMinutes;
    if (minutes <= 0) return;

    // The clamp keeps a corrupt value in the config file from producing a timer
    // that never fires.
    const UINT ms = (UINT)Clamp(minutes, 1, 240) * 60000u;
    ::SetTimer(host_, TIMER_BREAK, ms, nullptr);
}

void App::ShowTrayBalloon(const wchar_t* title, const wchar_t* text) {
    if (!trayAdded_) return;
    NOTIFYICONDATAW n = tray_;
    // NIF_ICON alongside NIF_INFO: without it the balloon carries no application
    // icon, and Windows silently drops a toast it cannot attribute.
    n.uFlags = NIF_INFO | NIF_ICON;
    n.hIcon = iconSmall_ ? iconSmall_ : icon_;
    // hBalloonIcon must stay null: setting it makes the shell return
    // ERROR_INCORRECT_SIZE (1462) and drop the notification even with a correct
    // V4 cbSize. Left null, NIIF_USER falls back to hIcon.
    n.hBalloonIcon = nullptr;
    n.dwInfoFlags = NIIF_USER | NIIF_NOSOUND;   // silent: the reminder is a break cue
    wcsncpy_s(n.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(n.szInfo, text, _TRUNCATE);

    // The return value is checked so a notification dropped by the shell can be
    // told apart from one that was never sent.
    ::SetLastError(0);
    if (::Shell_NotifyIconW(NIM_MODIFY, &n)) {
        // Acceptance by the shell does not mean the notification is displayed:
        // with the master switch off it is discarded without any error, so that
        // case is logged separately.
        if (ToastsGloballyOff())
            KLOG_W(L"Notification '%s' accepted, but notifications are turned off "
                   L"in Settings > System > Notifications - "
                   L"Windows will discard it.", title);
        else
            KLOG_I(L"Notification sent: %s", title);
        return;
    }

    // Second attempt without the custom icon: if the NIIF_USER variant is
    // refused, a notification with the generic Windows icon is better than none.
    const DWORD firstErr = ::GetLastError();
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    ::SetLastError(0);
    if (::Shell_NotifyIconW(NIM_MODIFY, &n))
        KLOG_I(L"Notification sent: %s (without its own icon; error %lu on the first attempt)",
               title, firstErr);
    else
        KLOG_W(L"Could not show the notification '%s' (error %lu, then %lu).",
               title, firstErr, ::GetLastError());
}

void App::RegisterHotkeys() {
    hotkeys_->UnregisterAll();
    hotkeyActions_.clear();
    failedHotkeys_.clear();

    // A combination already claimed by another program makes RegisterHotKey
    // fail without any visible sign, so failures are collected here for the
    // settings window to report.
    const auto add = [this](const std::wstring& combo, int kind, const std::wstring& profile,
                            const wchar_t* what) {
        if (Trim(combo).empty()) return;          // empty = deliberately disabled
        const int id = hotkeys_->Register(combo);
        if (id) { hotkeyActions_.push_back({id, kind, profile}); return; }
        failedHotkeys_.push_back(Format(L"%s (%s)", what, Trim(combo).c_str()));
    };

    add(config_.hkBrightnessUp,   HK_BRIGHT_UP,   L"", T(L"Brightness up"));
    add(config_.hkBrightnessDown, HK_BRIGHT_DOWN, L"", T(L"Brightness down"));
    add(config_.hkSaturationUp,   HK_SAT_UP,      L"", T(L"Saturation up"));
    add(config_.hkSaturationDown, HK_SAT_DOWN,    L"", T(L"Saturation down"));
    add(config_.hkToggle,         HK_TOGGLE,      L"", T(L"Pause / resume"));
    add(config_.hkShow,           HK_SHOW,        L"", T(L"Open this window"));

    // The emergency hotkey is not optional: an empty field falls back to the
    // default, since it is the way out of an unreadable screen.
    if (Trim(config_.hkPanic).empty()) config_.hkPanic = L"Ctrl+Alt+Shift+K";
    const size_t before = failedHotkeys_.size();
    add(config_.hkPanic, HK_PANIC, L"", T(L"EMERGENCY: give the screen back"));
    const bool panicFailed = failedHotkeys_.size() > before;

    for (const auto& p : config_.profiles)
        if (!Trim(p.hotkey).empty())
            add(p.hotkey, HK_PROFILE, p.name, p.name.c_str());

    UpdateHotkeyWarning();

    // A failed emergency hotkey warrants a dialog, since it is the recovery path
    // for an unreadable screen; other failures only appear in the window notice.
    if (panicFailed && !panicWarned_) {
        panicWarned_ = true;
        ::MessageBoxW(settings_,
                      Format(T(L"Could not register the emergency hotkey (%s): another "
                                 L"program is already using that combination.\n\n"
                                 L"Choose a different one on the System tab. In the "
                                 L"meantime the emergency exit stays available from the "
                                 L"tray menu and through \"zdisplay.exe --panic\"."),
                             Trim(config_.hkPanic).c_str()).c_str(),
                      L"Zdisplay", MB_OK | MB_ICONWARNING);
    }
}

// Host window

LRESULT CALLBACK App::HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* self = App::Get();
    if (self) return self->OnHostMessage(hwnd, msg, wp, lp);
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::OnHostMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Explorer can restart, in which case the tray icon has to be added again.
    if (taskbarCreatedMsg_ && msg == taskbarCreatedMsg_) {
        // The small icon size may have changed meanwhile (DPI change, different
        // taskbar), so it is redrawn at the size requested now.
        const int want = (std::max)(16, ::GetSystemMetrics(SM_CXSMICON));
        if (HICON fresh = CreateAppIcon(want)) {
            if (iconSmall_) ::DestroyIcon(iconSmall_);
            iconSmall_ = fresh;
        }
        tray_.hIcon = iconSmall_ ? iconSmall_ : icon_;
        tray_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
        trayAdded_ = ::Shell_NotifyIconW(NIM_ADD, &tray_) != FALSE;
        if (trayAdded_) EnableModernTrayBehavior(host_, tray_.uID);
        UpdateTrayTip();
        return 0;
    }

    // During shutdown GetMessage still delivers anything posted before WM_QUIT:
    // a WM_HOTKEY queued ahead of UnregisterHotKey, a tray click ahead of
    // NIM_DELETE, a menu item. All three reach the engine, which may already
    // have been released.
    if ((exiting_ || !engine_) &&
        (msg == WM_ZDISPLAY_TRAY || msg == WM_COMMAND || msg == WM_HOTKEY))
        return 0;

    switch (msg) {
        case WM_ZDISPLAY_TRAY:
            switch (LOWORD(lp)) {
                case WM_LBUTTONDBLCLK: ShowSettings(); break;
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:   BuildTrayMenu(); break;
                case WM_MBUTTONUP:     TogglePause(L"tray middle click"); break;
                default: break;
            }
            return 0;

        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (id >= IDM_PROFILE_BASE && id < IDM_PROFILE_BASE + 500) {
                const size_t index = (size_t)(id - IDM_PROFILE_BASE);
                if (index < config_.profiles.size()) {
                    engine_->SetManualProfile(config_.profiles[index].name);
                    MarkDirty();
                }
                return 0;
            }
            switch (id) {
                case IDM_AUTO:     engine_->ClearManualProfile(); return 0;
                case IDM_PAUSE:    TogglePause(L"tray menu"); return 0;
                case IDM_SETTINGS: ShowSettings(); return 0;
                case IDM_RESTORE:  EmergencyRestore(); return 0;
                case IDM_BRIGHT_UP:
                    engine_->NudgeBrightness(Clamp(config_.hotkeyStep, 1.0, 25.0));
                    return 0;
                case IDM_BRIGHT_DOWN:
                    engine_->NudgeBrightness(-Clamp(config_.hotkeyStep, 1.0, 25.0));
                    return 0;
                case IDM_EXIT:     RequestExit(); return 0;
                default: break;
            }
            return 0;
        }

        case WM_HOTKEY: {
            const int id = (int)wp;
            for (const auto& a : hotkeyActions_) {
                if (a.id != id) continue;
                const double step = Clamp(config_.hotkeyStep, 1.0, 25.0);
                switch (a.kind) {
                    case HK_BRIGHT_UP:   engine_->NudgeBrightness(step); break;
                    case HK_BRIGHT_DOWN: engine_->NudgeBrightness(-step); break;
                    case HK_SAT_UP:      engine_->NudgeSaturation(step); break;
                    case HK_SAT_DOWN:    engine_->NudgeSaturation(-step); break;
                    case HK_TOGGLE:      TogglePause(L"global hotkey"); break;
                    case HK_SHOW:        ShowSettings(); break;
                    case HK_PANIC:       EmergencyRestore(); break;
                    case HK_PROFILE:     engine_->SetManualProfile(a.profile); break;
                    default: break;
                }
                break;
            }
            return 0;
        }

        // Everything below touches the engine and needs a guard: the queue keeps
        // delivering messages during shutdown, and the engine may already have
        // been released.
        case WM_TIMER:
            if (wp == TIMER_AUTOSAVE) {
                // One-shot timer armed by MarkDirty(); nothing pending, nothing runs.
                ::KillTimer(host_, TIMER_AUTOSAVE);
                if (dirty_) { dirty_ = false; SaveConfig(config_); }
            } else if (wp == TIMER_BREAK) {
                // Killed before the balloon is shown and rearmed afterwards:
                // Shell_NotifyIcon blocks while Windows pumps messages into this
                // window, so a live periodic timer would reenter this handler.
                // Rearming also restarts the interval from when the reminder
                // appeared, which is what the 20-20-20 rule means.
                ::KillTimer(host_, TIMER_BREAK);
                ShowTrayBalloon(T(L"Zdisplay — eye break"),
                                T(L"Look at something about 6 metres away for 20 seconds. "
                                  L"That relaxes the muscle holding your near focus."));
                ScheduleBreakReminder();
            } else if (wp == TIMER_DARKGUARD) {
                // One-shot: runs only after the user stops making changes.
                ::KillTimer(host_, TIMER_DARKGUARD);
                if (!exiting_ && engine_) GuardDarkScreen();
            } else if (wp == TIMER_FOREGROUND_POLL) {
                if (!exiting_ && config_.enableAppRules) foreground_.Poll();
            } else if (wp == TIMER_VISION_PREVIEW) {
                ::KillTimer(host_, TIMER_VISION_PREVIEW);
                if (!exiting_ && engine_) engine_->EndPreviewVision();
            } else if (engine_) {
                engine_->OnTimer(wp);
            }
            return 0;

        case WM_DISPLAYCHANGE:
            if (exiting_ || !engine_) return 0;
            engine_->OnDisplayChanged();
            if (settings_ && ::IsWindow(settings_)) { ReloadMonitorCombo(); LoadAdjustments(); }
            return 0;

        case WM_DEVICECHANGE:
            // Only the staged rediscovery is armed, never an immediate one: a
            // single plug raises this several times, and the engine re-arms its
            // timer, so the burst settles into one pass. The settings window is
            // refreshed by the WM_DISPLAYCHANGE that normally accompanies it,
            // and by the staged pass when it does not.
            if (!exiting_ && engine_ &&
                (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE))
                engine_->OnDeviceChanged();
            return TRUE;

        case WM_WTSSESSION_CHANGE:
            // Fast user switching. Two sessions running Zdisplay would fight
            // over the same physical monitor, each watchdog undoing the other's
            // adjustment, so only the foreground session applies changes.
            if (!exiting_ && engine_) {
                if (wp == kWtsConsoleDisconnect || wp == kWtsRemoteDisconnect)
                    engine_->SetSessionActive(false);
                else if (wp == kWtsConsoleConnect || wp == kWtsRemoteConnect)
                    engine_->SetSessionActive(true);
            }
            return 0;

        case WM_POWERBROADCAST:
            // The display turned off or on. Only the 0 -> 1 edge matters: a
            // monitor that has just regained power forgot the brightness sent
            // over DDC/CI and reverted to its own default.
            if (wp == PBT_POWERSETTINGCHANGE && lp && !exiting_ && engine_) {
                auto* s = reinterpret_cast<const POWERBROADCAST_SETTING*>(lp);
                if (::IsEqualGUID(s->PowerSetting, kGuidConsoleDisplayState) &&
                    s->DataLength >= 1) {
                    // 0 off, 1 on, 2 dimmed. Dimmed does not count: the panel
                    // stays powered and keeps the values it was given.
                    if (s->Data[0] == 0)      engine_->SetDisplayOn(false);
                    else if (s->Data[0] == 1) engine_->SetDisplayOn(true);
                }
                return TRUE;
            }
            // The driver clears the gamma ramp across suspend, so it is reapplied.
            if ((wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) &&
                !exiting_ && engine_) {
                // Forces hardware rediscovery: PHYSICAL_MONITOR handles and the
                // WMI COM connection do not survive suspend, and OnDisplayChanged
                // alone returns early when the layout comes back identical. The
                // engine retries at 1, 5, 15 and 30 s for slow docks and drivers.
                engine_->OnResume();

                // Global hotkeys may have been claimed by another program while
                // the machine slept; RegisterHotkeys releases the old ones first,
                // so re-registering is cheap and safe.
                RegisterHotkeys();

                // Recompute rather than only ApplyNow: after a long sleep the
                // schedule may select a different profile, and reapplying the
                // old one first would produce a visible brightness jump.
                engine_->Recompute(false);
                engine_->ApplyNow();
            }
            return TRUE;

        case WM_ZDISPLAY_COMMAND: {
            // The LPARAM is a cookie, not a pointer: it only resolves while the
            // pipe thread has a request published. The window class name is
            // fixed and any process on the desktop can find it, so an unknown
            // cookie resolves to nothing and is logged.
            CommandRequest* req = command_channel::Resolve((UINT_PTR)lp);
            if (!req) {
                KLOG_W(L"WM_ZDISPLAY_COMMAND with an unknown identifier (%llu): "
                       L"message ignored.", (unsigned long long)lp);
                return 0;
            }
            if (exiting_ || !engine_) { req->reply = L"error: Zdisplay is shutting down"; return 1; }
            req->reply = HandleCommand(req->command);
            return 1;
        }

        case WM_ENDSESSION:
            if (wp) {
                SaveConfig(config_);
                if (engine_ && config_.restoreOnExit) engine_->ResetAll();
            }
            return 0;

        case WM_CLOSE:
            RequestExit();
            return 0;

        case WM_DESTROY:
            if (!exiting_) RequestExit();
            return 0;

        default: break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// External commands

namespace {

std::vector<std::wstring> SplitArgs(const std::wstring& line) {
    std::vector<std::wstring> out;
    std::wstring cur;
    bool inQuotes = false;
    for (wchar_t c : line) {
        if (c == L'"') { inQuotes = !inQuotes; continue; }
        if (!inQuotes && iswspace(c)) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            continue;
        }
        cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::wstring Normalize(const std::wstring& arg) {
    std::wstring s = arg;
    while (!s.empty() && (s.front() == L'-' || s.front() == L'/')) s.erase(s.begin());
    return ToLower(s);
}

}  // namespace

std::wstring App::HandleCommand(const std::wstring& line) {
    const auto args = SplitArgs(line);
    if (args.empty()) { ShowSettings(); return L"ok"; }

    std::wstring reply = L"ok";

    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring a = Normalize(args[i]);
        const auto next = [&]() -> const std::wstring* {
            return (i + 1 < args.size()) ? &args[++i] : nullptr;
        };

        // Sets a numeric field of the active profile.
        const auto setField = [&](AdjField f, double lo, double hi) -> std::wstring {
            const std::wstring* raw = next();
            if (!raw) return L"error: the value is missing";
            double v = 0;
            if (!ParseDouble(*raw, &v)) return L"error: invalid value '" + *raw + L"'";
            v = Clamp(v, lo, hi);

            Profile* p = engine_->Active();
            if (!p) return L"error: no active profile";

            // Only the global value is written. Per-monitor overrides are an
            // explicit choice and are left untouched, so a single command cannot
            // irreversibly flatten the differences configured between screens.
            *FieldPtr(p->global, f) = v;
            p->global.Sanitize();

            engine_->ApplyNow();
            MarkDirty();
            RefreshUi();

            if (!p->perMonitor.empty()) {
                return L"ok (" + FormatDouble(v) + L"; " +
                       std::to_wstring(p->perMonitor.size()) +
                       L" monitor(s) with their own settings were left unchanged)";
            }
            return L"ok (" + FormatDouble(v) + L")";
        };

        if (a == L"tray" || a == L"minimized" || a == L"background" ||
            a == L"verbose" || a == L"v" || a.empty()) {
            continue;
        }
        else if (a == L"show" || a == L"config") { ShowSettings(); }
        else if (a == L"tab" || a == L"aba") {
            // Tab indices: 0 adjustments, 1 vision, 2 profiles, 3 automation,
            // 4 system, 5 diagnostics.
            const std::wstring* n = next();
            double v = 0;
            if (!n || !ParseDouble(*n, &v)) return L"error: give the tab number (0..5)";
            ShowSettings();
            const int index = Clamp((int)v, 0, 5);
            if (tabs_ && ::IsWindow(tabs_)) {
                ::SendMessageW(tabs_, TCM_SETCURSEL, (WPARAM)index, 0);
                ShowTab(index);
            }
            reply = L"tab " + std::to_wstring(index);
        }
        else if (a == L"profile" || a == L"perfil") {
            const std::wstring* n = next();
            if (!n) return L"error: the profile name is missing";
            if (!config_.Find(*n)) return L"error: profile '" + *n + L"' does not exist";
            engine_->SetManualProfile(*n);
            reply = L"profile '" + *n + L"' activated";
        }
        else if (a == L"auto")          { engine_->ClearManualProfile(); reply = L"automatic mode"; }
        else if (a == L"brightness" || a == L"brilho")      reply = setField(F_BRIGHT, 10, 150);
        else if (a == L"contrast"   || a == L"contraste")   reply = setField(F_CONTRAST, 0, 200);
        else if (a == L"saturation" || a == L"saturacao")   reply = setField(F_SAT, 0, 200);
        else if (a == L"vibrance")                          reply = setField(F_VIB, 0, 100);
        else if (a == L"temperature"|| a == L"temperatura") reply = setField(F_TEMP, 1500, 10000);
        else if (a == L"gamma")                             reply = setField(F_GAMMA, 0.3, 3.0);
        else if (a == L"shadows"    || a == L"sombras")     reply = setField(F_SHADOWS, 0, 100);
        else if (a == L"clarity"    || a == L"definicao")   reply = setField(F_CLARITY, 0, 100);
        else if (a == L"hue"        || a == L"matiz")       reply = setField(F_HUE, -180, 180);
        else if (a == L"dim"        || a == L"escurecer")   reply = setField(F_DIM, 0, 90);
        else if (a == L"hwbrightness")                      reply = setField(F_HWBRIGHT, 0, 100);
        else if (a == L"toggle" || a == L"pausar") {
            TogglePause();
            reply = engine_->Enabled() ? L"active" : L"paused";
        }
        else if (a == L"on"  || a == L"ligar")    { engine_->SetEnabled(true);  reply = L"active"; }
        else if (a == L"off" || a == L"desligar") { engine_->SetEnabled(false); reply = L"paused"; }
        else if (a == L"reset" || a == L"resetar"){ engine_->ResetAll(); reply = L"display restored"; }
        else if (a == L"panic" || a == L"emergencia") {
            EmergencyRestore();
            reply = L"display returned to its original state and Zdisplay paused";
        }
        else if (a == L"status") {
            // Reports effective values, with the vision layer already applied,
            // so the numbers match what is on screen rather than the raw
            // profile settings.
            Profile* p = engine_->Active();
            const MonitorTarget* prim = monitors::Primary();
            Adjustments eff;
            if (p && prim) eff = engine_->Effective(*p, *prim);

            reply = Format(L"profile=%s; active=%s; manual=%s; focus=%s; "
                           L"brightness=%.0f; saturation=%.0f; temperature=%.0fK",
                           p ? p->name.c_str() : L"-",
                           engine_->Enabled() ? L"yes" : L"no",
                           engine_->ManualProfile().empty() ? L"-" : engine_->ManualProfile().c_str(),
                           engine_->ForegroundProcess().c_str(),
                           eff.brightness, eff.saturation, eff.temperature);
            if (config_.vision.enabled)
                reply += Format(L"; vision=%.0f%% night", engine_->NightNow() * 100.0);
        }
        else if (a == L"list" || a == L"perfis") {
            reply.clear();
            for (const auto& p : config_.profiles) {
                if (!reply.empty()) reply += L", ";
                reply += p.name;
            }
        }
        else if (a == L"diag" || a == L"diagnostico") {
            reply = L"BACKENDS\r\n" + engine_->DescribeBackends();
            reply += Format(L"\r\nMonitors: %d   Active backends: %d\r\n",
                            (int)monitors::All().size(), engine_->AvailableBackendCount());
            for (const auto& m : monitors::All()) {
                reply += L"  " + m.friendlyName + L"  [" + m.deviceName + L"]  key=" + m.key;
                if (m.edid.valid && m.edid.wideGamut) reply += L"  wide-gamut";
                if (m.isHdr) reply += L"  HDR-ON(ramp-ignored)";
                if (!m.edid.valid) reply += L"  no-EDID";
                reply += L"\r\n";
                // Same per-monitor coverage as the diagnostics tab, because
                // --diag output is what gets pasted into problem reports.
                for (const auto& line : engine_->MonitorCoverage(m))
                    reply += L"      " + line + L"\r\n";
            }
            for (const auto& line : engine_->Ddc()->Diagnose())
                reply += L"  DDC/CI: " + line + L"\r\n";
            if (engine_->Gamma()->Limited())
                reply += Format(L"\r\nWARNING: Windows accepted only %.0f%% of the gamma effect.\r\n",
                                engine_->Gamma()->AcceptedFraction() * 100.0);
        }
        else if (a == L"quit" || a == L"sair") {
            ::PostMessageW(host_, WM_COMMAND, IDM_EXIT, 0);
            return L"shutting down";
        }
        else {
            return L"error: unknown command '" + args[i] + L"'";
        }

        if (reply.rfind(kCommandErrorPrefix, 0) == 0) return reply;
    }

    MarkDirty();
    return reply;
}

}  // namespace zdisplay
