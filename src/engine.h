#pragma once
#include "backends.h"

namespace zdisplay {

/// Timer IDs created on the host window.
enum : UINT_PTR {
    TIMER_TRANSITION = 1,
    TIMER_WATCHDOG   = 2,
    TIMER_SCHEDULE   = 3,
    TIMER_AUTOSAVE   = 4,
    /// Defers the dark-screen check until the user stops moving the slider.
    TIMER_DARKGUARD  = 5,
    /// Closes a slider drag by applying what the fast path left out.
    TIMER_INTERACTIVE = 6,
    /// Eye-break reminder (20-20-20 rule).
    TIMER_BREAK = 7,
    /// Staggered retries after hotplug or resume.
    TIMER_REDISCOVER = 8,
    /// Waits for the first WMI answer without blocking the UI thread during
    /// startup, which the backlight Init would otherwise do for several seconds.
    TIMER_BACKLIGHT_POLL = 9,
    /// Safety net for the foreground hook: covers events that are lost or
    /// blocked.
    TIMER_FOREGROUND_POLL = 10,
    /// Ends the day/night preview automatically.
    TIMER_VISION_PREVIEW = 11,
    /// Waits for the display power state to settle before acting on it.
    TIMER_DISPLAYSTATE = 12,
};

/// The hook delivers the switch immediately; this tick only covers the cases
/// where Windows did not publish the event. 750 ms stays imperceptible and
/// costs one foreground-window query when nothing changed.
constexpr UINT kForegroundPollMs = 750;

/// The preview lasts long enough to compare, but never leaves a test
/// adjustment stuck if the window is closed right after the click.
constexpr UINT kVisionPreviewMs = 5000;

/// How long without slider movement counts as the end of a drag.
///
/// 140 ms is below the perceptible-delay threshold and well above the interval
/// between two WM_HSCROLL messages of a continuous drag, so the slow path runs
/// once per gesture.
constexpr UINT kInteractiveSettleMs = 140;

/// How long after wake commands to the display are held instead of sent.
///
/// Waking fires a burst: the power event, one or more WM_DISPLAYCHANGE
/// messages and the staggered rediscoveries, each of which reapplies the
/// profile. Without the hold that becomes several EEPROM writes on the display
/// carrying the same final value; holding briefly lets the queue, which
/// already coalesces by key, deliver a single write.
///
/// Deliberately short. Waiting for the bus is pointless: while it does not
/// answer, discovery does not include the display and nothing is queued, and a
/// longer hold would only delay a brightness change made right after wake.
constexpr int kResumeHoldMs = 2500;

/// The engine: detects what the machine supports, decides which backend
/// handles what, resolves which profile should be active and runs the smooth
/// transitions.
///
/// Everything runs on the UI thread. The slow backends (DDC/CI, WMI) have
/// their own queue and thread, so nothing here blocks.
class Engine {
public:
    explicit Engine(Config* config) : cfg_(config) {}
    ~Engine() { Shutdown(); }

    /// `all_` holds pointers to backends that are members of this instance
    /// (`&gamma_`, `&ddc_`, ...). A copy would carry an `all_` pointing at the
    /// original's backends: both objects would drive the same hardware and both
    /// destructors would call Shutdown() on it.
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /// hostWindow receives the engine's WM_TIMER messages.
    void Initialize(HWND hostWindow);
    void Shutdown();

    // --- state ---
    Config* GetConfig() const { return cfg_; }

    /// Resolves the active profile by name on every call.
    ///
    /// `Config::profiles` is a std::vector: creating, duplicating or importing
    /// a profile can reallocate it and deleting one shifts its elements, so a
    /// cached `Profile*` would dangle. Resolving by name costs one search over
    /// a handful of items and cannot go stale.
    Profile* Active() const { return cfg_ ? cfg_->Find(activeName_) : nullptr; }
    const std::wstring& ActiveName() const { return activeName_; }
    const std::wstring& ManualProfile() const { return manualProfile_; }
    bool Enabled() const { return enabled_; }
    const std::wstring& ForegroundProcess() const { return foreground_; }

    // --- control ---
    /// Determines which profile should be active. Priority: manual override,
    /// then per-application match, then schedule, then the default profile.
    void Recompute(bool animate = true);
    void SetManualProfile(const std::wstring& name);
    void ClearManualProfile();
    void SetEnabled(bool on);
    /// Applies the active profile immediately, without a transition.
    void ApplyNow();

    /// Applies while a slider is being dragged.
    ///
    /// Only the paths that respond instantly and without flicker run here: the
    /// gamma ramp and the overlay. The color matrix, the vendor APIs and the
    /// display hardware wait until the slider is released.
    ///
    /// MagSetFullscreenColorEffect rebuilds the compositor's fullscreen effect
    /// on every call, so one call per scroll event shows up as the whole screen
    /// flickering. The vendor APIs are deferred because their driver calls take
    /// tens of milliseconds and stall the slider movement itself.
    void ApplyInteractive();
    /// Runs what the drag deferred. Called from TIMER_INTERACTIVE.
    void SettleInteractive();

    /// Mirrors brightness changed with the keyboard keys on the internal panel
    /// out to the external displays. Called by the watchdog.
    void MirrorInternalBrightness();

    /// A profile's adjustments for one display, with the vision layer already
    /// applied. Every path that changes the screen goes through here.
    Adjustments Effective(const Profile& p, const MonitorTarget& m) const;

    /// Recomputes how much "night" applies now and reapplies if it moved
    /// enough. Called at startup, on the schedule tick and on vision changes.
    void UpdateVision();
    /// 0 = full day, 1 = full night. Read by the UI to show the state.
    double NightNow() const { return visionNight_; }
    bool VisionPreviewActive() const { return visionPreview_ >= 0.0; }
    double VisionShownNight() const {
        return visionPreview_ >= 0.0 ? visionPreview_ : visionNight_;
    }

    /// Shows the screen as it would look with the vision layer at the given
    /// point, without changing the configuration.
    void PreviewVision(double nightFraction);
    void EndPreviewVision();

    void ResetAll();

    /// Shows the original screen while the compare button is held, without
    /// changing the enabled/paused state.
    void PreviewOriginal(bool on);
    bool Previewing() const { return previewing_; }

    /// A profile was renamed in the UI; updates the references held by name.
    void OnProfileRenamed(const std::wstring& oldName, const std::wstring& newName);
    /// The profile list changed in the UI (created, duplicated, imported,
    /// deleted).
    void OnProfilesChanged();

    void OnForegroundProcess(const std::wstring& processName);
    void OnDisplayChanged();
    void OnResume();

    /// A monitor device arrived or left, which is not always a layout change.
    ///
    /// Swapping the panel on a KVM, or a dock that publishes the monitor after
    /// the video topology has already settled, produces no WM_DISPLAYCHANGE.
    /// Only the staged rediscovery is armed here, never an immediate one: the
    /// notification fires several times per plug, and the staged path re-arms
    /// its timer, so a burst collapses into a single pass.
    void OnDeviceChanged();

    /// The user session moved to the background (fast user switching) or came
    /// back. While it is behind, the screen belongs to another session, so
    /// reasserting there is wasted work and fights the other session's instance
    /// over the same physical display.
    void SetSessionActive(bool active);

    /// The display turned off or on (GUID_CONSOLE_DISPLAY_STATE).
    ///
    /// While off, reasserting is worse than useless: every SetDeviceGammaRamp
    /// on a sleeping display wakes the compositor for nothing and dwm.exe burns
    /// CPU with nothing on screen. On the way back, an external display has
    /// typically forgotten what was sent over DDC/CI and returned to its
    /// factory settings, so everything is reapplied.
    ///
    /// The state is not obeyed on arrival: talking DDC/CI to the display makes
    /// the link blink and Windows reports that as off-then-on, so obeying
    /// directly would let a reapply trigger the next notification in a loop.
    /// Only the settled state counts.
    void SetDisplayOn(bool on);
    void OnTimer(UINT_PTR id);
    void UpdateWatchdogInterval();
    /// Creates or kills the clock tick depending on whether the scheduler or
    /// the vision layer is on: the day/night ramp depends on it as much as
    /// time-based profile switching. Call whenever either one changes.
    void UpdateScheduleTimer();

    /// Returns the screen to its original state and pauses. This is the
    /// emergency exit: no adjustment may leave the user unable to see.
    void EmergencyRestore();

    /// Reads and stores on disk everything the screen was before it was
    /// touched.
    ///
    /// `screenIsTrustworthy` false means the previous session crashed and the
    /// baseline on disk cannot be read, so the current screen still carries
    /// that session's adjustments. Nothing is written in that state, since
    /// writing would make the loss of the original, ICC calibration included,
    /// permanent.
    void CaptureBaseline(bool screenIsTrustworthy = true);
    /// If the previous run crashed, returns the screen to the stored state.
    bool RecoverFromCrash();

    /// How much light is left on the screen under the current settings (0..1).
    double CurrentLuminance() const;

    /// Increments applied by the global hotkeys.
    void NudgeBrightness(double delta);
    void NudgeSaturation(double delta);
    void NudgeTemperature(double delta);

    /// Applies a change to the active profile (global plus per-display
    /// overrides).
    void MutateActive(void (*fn)(Adjustments&, double), double value);

    // --- diagnostics ---
    std::wstring DescribeBackends() const;
    int AvailableBackendCount() const;

    GammaBackend*     Gamma()     { return &gamma_; }
    HdrBackend*       Hdr()       { return &hdr_; }
    MagnifyBackend*   Magnify()   { return &magnify_; }
    NvapiBackend*     Nvidia()    { return &nvidia_; }
    AdlBackend*       Amd()       { return &amd_; }
    DdcciBackend*     Ddc()       { return &ddc_; }
    BacklightBackend* Backlight() { return &backlight_; }
    OverlayBackend*   Overlay()   { return &overlay_; }

    /// Called whenever the state changes so the UI can refresh.
    void (*onStateChanged)(void* ctx) = nullptr;
    void* stateContext = nullptr;

private:
    Profile* Resolve();
    void BeginTransition(Profile* p);
    void SnapTo(Profile* p);
    /// Reapplies the active profile after the UI changes the profile list.
    void SnapToActive();
    void ApplyToMonitor(const MonitorTarget& m, const Adjustments& a, bool duringTransition);
    void ApplyHardware(const MonitorTarget& m, const Adjustments& a);
    void ApplyGlobalMatrix();
    bool UseVendorSaturation() const;
    bool VendorVibranceAvailable() const;
    bool VendorHueAvailable() const;
    /// Whether every display has HDR on. The color matrix may only take over
    /// brightness, contrast and temperature in that case, since its effect is
    /// global.
    bool AllMonitorsHdr() const;

public:
    /// What actually controls this display, in user-facing language.
    ///
    /// A global backend count does not answer the practical question, "why does
    /// this slider do nothing on this display?". Each line names what handles
    /// what, and when nothing does, the reason why.
    std::vector<std::wstring> MonitorCoverage(const MonitorTarget& m) const;

private:

    /// On an HDR display in a mixed layout, converts software brightness into
    /// physical brightness over DDC/CI: the gamma ramp does not apply there and
    /// the matrix cannot be used without affecting the SDR displays. Returns
    /// `a` untouched when the fallback does not apply, including when the SDR
    /// white level already handles this display's brightness, since that path
    /// is per display, needs no I2C and causes no EEPROM wear, and applying
    /// both would darken twice.
    Adjustments HdrBrightnessFallback(const MonitorTarget& m, const Adjustments& a) const;
    void RediscoverHardware();
    void ScheduleRediscovery();
    void NotifyChanged();

    Config* cfg_;
    HWND host_ = nullptr;

    GammaBackend     gamma_;
    HdrBackend       hdr_;
    MagnifyBackend   magnify_;
    NvapiBackend     nvidia_;
    AdlBackend       amd_;
    DdcciBackend     ddc_;
    BacklightBackend backlight_;
    OverlayBackend   overlay_;
    std::vector<Backend*> all_;

    std::wstring activeName_;
    /// How much "night" applies now, recomputed on the schedule tick.
    double visionNight_ = 0.0;
    /// Value forced while a preview button is held; -1 = none.
    double visionPreview_ = -1.0;
    std::wstring manualProfile_;
    std::wstring foreground_;
    bool         enabled_ = true;
    // Two independent reasons for the engine to stop reasserting. `working_` is
    // the conjunction of both: keeping them separate stops one of them
    // returning from re-enabling the ticks while the other still requires
    // silence.
    bool         sessionActive_ = true;
    bool         displayOn_ = true;       ///< settled state
    bool         displayPending_ = true;  ///< last notification received
    bool         working_ = true;
    void         UpdateSuspension();
    void         SettleDisplayState();    ///< called from TIMER_DISPLAYSTATE

    // transition
    std::map<std::wstring, Adjustments> shown_, from_, to_;
    double transitionStartMs_ = 0;
    int    transitionMs_ = 0;
    bool   transitioning_ = false;
    bool   previewing_ = false;
    int    rediscoveryStep_ = -1;
};

}  // namespace zdisplay
