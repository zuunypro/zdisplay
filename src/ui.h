#pragma once
#include "engine.h"
#include "services.h"

// Device arrival and removal broadcasts. Not reached through windows.h, which
// leaves dbt.h out under WIN32_LEAN_AND_MEAN.
#include <dbt.h>

namespace zdisplay {

// The drop-down index mappings live in core.h, where the test suite can reach
// them: a mapping that drifts from the order the list is filled in stores a
// different value than the one the user picked, and does it silently.

inline const wchar_t* PerformanceHintFor(PerformanceMode m) {
    switch (m) {
        case PerformanceMode::Quality:
            return T(L"Reasserts every 5 s and searches for the maximum effect "
                     L"even while a slider is moving.");
        case PerformanceMode::Light:
            return T(L"Lowest cost: reasserts every 30 s and switches profile "
                     L"without animation. No feature is turned off.");
        default:
            return T(L"The default: reasserts every 10 s and completes the "
                     L"search once the slider stops.");
    }
}

/// Label of a System tab hotkey field, in field order. One source for the field
/// the tab builds and for the message naming it in a conflict.
const wchar_t* HotkeyFieldLabel(int index);

/// Balloon anchored to an edit control, stating why the value it was just given
/// was refused.
///
/// The alternative to a field that ignores a keystroke and says nothing, which
/// reads as a broken control. Windows dismisses it on the next keystroke or
/// when the field loses focus, so it never has to be taken back.
void ShowFieldTip(HWND edit, const wchar_t* title, const std::wstring& text);

// Defined in icon.cpp.
HICON CreateAppIcon(int size);
bool  WriteIcoFile(const std::wstring& path, int size);

/// True when notifications are disabled by the global Windows switch. In that
/// state Shell_NotifyIcon still reports success and the balloon is discarded
/// afterwards with no signal, so the setting has to be queried directly.
bool ToastsGloballyOff();

/// Adjustments fields driven by a slider. The index keeps slider handling
/// generic instead of one branch per field.
///
/// The last four are built on the monitor colour window rather than on the
/// Adjustments tab, and are otherwise ordinary rows: one array of rows means
/// one place that reads a slider and writes the profile.
enum AdjField {
    F_BRIGHT = 0, F_CONTRAST, F_GAMMA, F_TEMP,
    F_SHADOWS, F_CLARITY,
    F_SAT, F_VIB, F_HUE, F_DIM,
    F_RGAIN, F_GGAIN, F_BGAIN, F_BLUEBLOCK,
    F_HWBRIGHT, F_HWCONTRAST,
    F_HWRGAIN, F_HWGGAIN, F_HWBGAIN, F_HWSAT,
    F_COUNT,
    /// First field that belongs to the monitor colour window.
    F_HWCOLOR_FIRST = F_HWRGAIN
};

double* FieldPtr(Adjustments& a, AdjField f);

/// One adjustment row: label, slider, value and reset button.
struct SliderRow {
    HWND label = nullptr, bar = nullptr, value = nullptr, reset = nullptr;
    double scale = 1.0;      ///< the slider is integral; the real value is pos/scale
    double defValue = 0.0;
    const wchar_t* suffix = L"%";
    int decimals = 0;
    AdjField field = F_BRIGHT;

    void Create(HWND parent, const wchar_t* caption, AdjField f,
                int x, int y, int width,
                double minV, double maxV, double defV,
                double scaleFactor, const wchar_t* suffixText, int decimalPlaces,
                int resetId);
    double Get() const;
    void Set(double v);
    void UpdateValueLabel();
    void Enable(bool on);
    void Show(bool on);
};

/// The application: hidden host window, tray icon, on-demand settings window,
/// hotkeys, automation and the command channel.
class App {
public:
    static App* Get() { return instance_; }

    bool Init(HINSTANCE inst, const std::vector<std::wstring>& args);
    int  Run();
    void RequestExit();

    Engine& GetEngine() { return *engine_; }
    Config& GetConfig() { return config_; }

    void ShowSettings();

    /// The panel's own colour registers, in a window of their own.
    ///
    /// Not on the Adjustments tab: that tab already runs to the bottom of a
    /// window sized to fit a 1366x768 laptop, and four more rows would not fit
    /// on one. These controls are also the only ones that depend on the monitor
    /// being in its user colour preset, which needs a sentence of its own.
    void ShowMonitorColor();

    /// Fast user switching notifications. While the session is in the
    /// background the engine stops reasserting; see Engine::SetSessionActive.
    void RegisterSessionNotifications();
    void UnregisterSessionNotifications();
    void TogglePause(const wchar_t* source = L"?");
    /// Marks the config as modified and schedules a single save 3 s later, so an
    /// idle process is never woken just to poll for pending changes.
    void MarkDirty();
    /// Returns Zdisplay to its freshly installed state: factory profiles, no
    /// rules, default hotkeys, original screen. Backs up the config file first
    /// and touches nothing outside the user's own data.
    void FactoryReset();
    void RegisterHotkeys();
    /// The action that already answers to this combination, or an empty string.
    ///
    /// Windows refuses the second registration of a combination even inside one
    /// process, and reports it exactly like a clash with another program. The
    /// clash is found here first, so the field can say which of Zdisplay's own
    /// actions is holding it instead of blaming a program that is not involved.
    ///
    /// `skipField` is the System tab field being edited (-1 for none) and
    /// `skipProfile` the profile whose own hotkey is being edited; without them
    /// every field would collide with itself.
    std::wstring HotkeyOwner(const std::wstring& combo, int skipField,
                             const std::wstring& skipProfile) const;
    void RefreshUi();

    /// Restores the screen and pauses. Bound to the emergency hotkey.
    void EmergencyRestore();

    /// Checks whether the current adjustments leave the screen too dark and, if
    /// so, asks for confirmation with an automatic revert.
    void GuardDarkScreen();

    /// Schedules the check above, restarting the delay on every call. A slider
    /// drag produces dozens of notifications per second, and checking on each
    /// one would raise the confirmation window mid-drag with its countdown
    /// already running.
    void ScheduleDarkGuard();

    /// Interprets a command line arriving over the named pipe or from the
    /// process arguments.
    std::wstring HandleCommand(const std::wstring& line);

private:
    // --- host window ---
    static LRESULT CALLBACK HostProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnHostMessage(HWND, UINT, WPARAM, LPARAM);
    void BuildTrayMenu();
    void UpdateTrayTip();
    static void OnForegroundChanged(const std::wstring& process, void* ctx);
    static void OnEngineStateChanged(void* ctx);

    // --- settings window (ui_settings.cpp) ---
    static LRESULT CALLBACK SettingsProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnSettingsMessage(HWND, UINT, WPARAM, LPARAM);
    void CreateSettingsControls(HWND hwnd);
    /// Rebuilds the settings window at the DPI of the monitor it now sits on.
    /// The layout is computed entirely through S(), so rebuilding is the
    /// relayout and there is no second copy of the logic to diverge. Called on
    /// WM_DPICHANGED, at the end of a drag so the window is not destroyed under
    /// the mouse.
    void RebuildSettingsForDpi();
    /// Recreates font_/fontBold_ for the current DPI; a system-DPI font
    /// overflows the scaled controls on a mixed-DPI setup.
    void RecreateUiFonts();
    void ShowTab(int index);
    void ReloadAll();
    void ReloadProfileCombos();
    /// Fills the process list with the programs open right now. Rebuilt each
    /// time the list is opened so closed programs are not offered.
    void ReloadRunningApps();
    void ReloadMonitorCombo();
    void LoadAdjustments();
    void LoadProfileEditor();
    void LoadRuleLists();
    void LoadDiagnostics();
    /// Fills the Vision tab and writes its status line.
    void LoadVision();
    /// Reads the Vision tab fields into the config and reapplies them.
    void CommitVision();
    /// (Re)arms the break reminder. Called at startup and when the interval
    /// changes.
    void ScheduleBreakReminder();
    /// Discreet tray notification that neither steals focus nor covers the
    /// user's work.
    void ShowTrayBalloon(const wchar_t* title, const wchar_t* text);
    void UpdateStatusBar();
    /// `dragging` means the slider is still held by the cursor; only the
    /// immediate, flicker-free path runs (see Engine::ApplyInteractive).
    void ApplyLive(bool dragging = false);
    void CommitProfileEditor();

    Adjustments* CurrentAdjustments();
    Profile*     EditingProfile();
    std::wstring SelectedMonitorKey();
    Profile*     SelectedProfileInList();

    void OnCommand(int id, HWND control, int code);
    void OnSlider(HWND bar, bool dragging);

    // --- state ---
    static App* instance_;

    HINSTANCE inst_ = nullptr;
    HWND host_ = nullptr;
    HWND settings_ = nullptr;
    HICON icon_ = nullptr;
    /// Icon drawn at the size the tray actually asks for (SM_CXSMICON,
    /// typically 16 px). Letting Windows downscale a 32 px icon pushes the thin
    /// ring in the artwork below a pixel and blurs it; drawing at the target
    /// size keeps it sharp.
    HICON iconSmall_ = nullptr;
    HFONT font_ = nullptr;
    HFONT fontBold_ = nullptr;
    /// Monospaced font for the diagnostics tab. Lives with the settings window
    /// and is destroyed in its WM_DESTROY.
    HFONT fontMono_ = nullptr;
    NOTIFYICONDATAW tray_{};
    bool trayAdded_ = false;
    bool sessionNotifyOk_ = false;
    HPOWERNOTIFY displayNotify_ = nullptr;  ///< display on/off notification
    HDEVNOTIFY   deviceNotify_ = nullptr;   ///< monitor arrival and removal
    UINT taskbarCreatedMsg_ = 0;

    Config config_;
    std::unique_ptr<Engine> engine_;
    std::unique_ptr<Hotkeys> hotkeys_;
    ForegroundWatcher foreground_;
    PipeServer pipe_;

    /// Maps a registered hotkey id to its action.
    struct HotkeyAction { int id; int kind; std::wstring profile; };
    std::vector<HotkeyAction> hotkeyActions_;
    /// Combinations Windows refused because another program already holds them;
    /// the settings window reports these to the user.
    std::vector<std::wstring> failedHotkeys_;
    /// The emergency hotkey warning is shown once per run, not on every
    /// re-registration (which happens on resume from sleep, for example).
    bool panicWarned_ = false;

    bool dirty_ = false;
    bool exiting_ = false;
    bool loadingUi_ = false;

    // --- dark screen protection ---
    static LRESULT CALLBACK ConfirmProc(HWND, UINT, WPARAM, LPARAM);
    void ShowDarkConfirm();
    void CloseDarkConfirm(bool keep);

    /// Last state of the active profile that was still readable.
    Profile safeSnapshot_;
    bool    hasSafeSnapshot_ = false;
    /// Luminance the user has explicitly accepted, and the profile it was
    /// accepted in. Confirmation is asked again only when the screen goes darker
    /// than that accepted point, not on every adjustment. -1 means nothing has
    /// been accepted yet.
    double       acceptedDarkLuminance_ = -1.0;
    std::wstring acceptedDarkProfile_;
    /// Tick of the last confirmation shown; at most one every 30 s.
    ULONGLONG    lastDarkAskTick_ = 0;
    HWND    confirmWnd_ = nullptr;
    HWND    confirmText_ = nullptr;
    /// Font at the DPI of the monitor the confirmation opened on; font_ follows
    /// the system DPI and overflows the controls on a mixed-DPI setup.
    HFONT   confirmFont_ = nullptr;
    int     confirmSecondsLeft_ = 0;

    // --- WM_DPICHANGED for the settings window ---
    /// Whether the window has finished being built. The SetWindowPos inside
    /// ShowSettings already fires a synchronous WM_DPICHANGED, and rebuilding at
    /// that moment would destroy the window in the middle of its own creation.
    bool settingsBuilt_ = false;
    bool inSizeMove_ = false;
    bool pendingDpiChange_ = false;
    RECT pendingDpiRect_{};
    bool    reverting_ = false;

    // --- settings window controls ---
    HWND tabs_ = nullptr;
    HWND statusBar_ = nullptr;
    std::vector<HWND> tabControls_[6];
    int activeTab_ = 0;

    HWND profileCombo_ = nullptr, monitorCombo_ = nullptr, perMonitorCheck_ = nullptr;
    HWND pauseButton_ = nullptr, invertCheck_ = nullptr;
    HWND manageHwBright_ = nullptr, manageHwContrast_ = nullptr;
    HWND ddcModeCombo_ = nullptr;
    /// Monitor commands: input, color preset and power. Not part of a profile;
    /// they act on the panel immediately.
    HWND monInputCombo_ = nullptr;
    HWND monPresetCombo_ = nullptr;
    HWND monPowerCombo_ = nullptr;
    HWND monFeaturesLabel_ = nullptr;
    /// Per-position values for each combo, translating a selection back into the
    /// number the monitor expects.
    std::vector<unsigned char> monInputValues_, monPresetValues_, monPowerValues_;
    /// Fills the monitor command combos with what the capability probe found.
    void LoadMonitorFeatures();
    SliderRow sliders_[F_COUNT];

    HWND profileList_ = nullptr, profileNameEdit_ = nullptr, profileHotkeyEdit_ = nullptr;
    HWND transitionEdit_ = nullptr, satEngineCombo_ = nullptr, defaultLabel_ = nullptr;

    HWND appListView_ = nullptr, appProcessEdit_ = nullptr, appProfileCombo_ = nullptr,
         appPriorityEdit_ = nullptr, appEnabledCheck_ = nullptr;
    HWND schedList_ = nullptr, schedStartEdit_ = nullptr, schedEndEdit_ = nullptr,
         schedProfileCombo_ = nullptr, schedPriorityEdit_ = nullptr,
         schedEnabledCheck_ = nullptr;
    /// Location for the rules that follow the sun (sunrise, sunset).
    HWND latitudeEdit_ = nullptr, longitudeEdit_ = nullptr;

    HWND checkStartup_ = nullptr, checkMinimized_ = nullptr, checkAppRules_ = nullptr,
         checkSchedule_ = nullptr, checkRestore_ = nullptr, watchdogEdit_ = nullptr,
         checkVendor_ = nullptr, checkMagnify_ = nullptr, checkDdc_ = nullptr,
         checkBacklight_ = nullptr, checkOverlay_ = nullptr, unlockButton_ = nullptr,
         checkConfirmDark_ = nullptr, checkMirrorKeys_ = nullptr,
         languageCombo_ = nullptr, performanceCombo_ = nullptr,
         performanceHint_ = nullptr;
    HWND hkEdits_[7] = {};
    HWND stepEdit_ = nullptr;
    /// Warning line for the hotkeys Windows refused. Empty when every
    /// combination is registered.
    HWND hotkeyWarning_ = nullptr;
    /// Writes that line from failedHotkeys_.
    void UpdateHotkeyWarning();
    HWND compareButton_ = nullptr;
    HWND tooltip_ = nullptr;

    /// Vision tab.
    HWND visEnable_ = nullptr, visDayTemp_ = nullptr, visNightTemp_ = nullptr,
         visNightBright_ = nullptr, visTransition_ = nullptr,
         visNightStart_ = nullptr, visDayStart_ = nullptr,
         visBreak_ = nullptr, visStatus_ = nullptr;

    HWND diagEdit_ = nullptr;

    // --- monitor colour window (ui_color.cpp) ---
    static LRESULT CALLBACK ColorProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnColorMessage(HWND, UINT, WPARAM, LPARAM);
    /// Fills the window from the profile being edited and enables only what the
    /// selected panel actually answers.
    void LoadMonitorColor();
    /// Turns one of the two groups on or off, seeding the sliders with the
    /// panel's own values so switching it on does not move the image.
    void ToggleMonitorColorGroup(bool gain);
    void CloseMonitorColor();
    HWND  colorWnd_ = nullptr;
    HFONT colorFont_ = nullptr;
    HWND  colorGainCheck_ = nullptr, colorSatCheck_ = nullptr;
    HWND  colorStatus_ = nullptr, colorPresetNote_ = nullptr;
    /// The monitor whose registers the open window is editing. The window is
    /// modal, so the selection cannot change underneath it.
    std::wstring colorMonitorKey_;

    /// Adds a tooltip to a control.
    void AddTip(HWND control, const wchar_t* text);
    /// Same, locating the control by control ID. Most controls are created
    /// without keeping a handle, so the single tooltip block at the end of
    /// CreateSettingsControls looks them up by ID instead.
    void AddTip(int id, const wchar_t* text);
};

}  // namespace zdisplay
