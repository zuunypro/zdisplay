#pragma once
#include "core.h"

#include <set>

namespace zdisplay {

/// Internal executable mode used to isolate the most dangerous DDC/CI query in
/// a separate process. Not part of the public CLI.
int RunDdcCapabilitiesProbe(const std::wstring& monitorKey,
                            const std::wstring& outputPath);

/// Capability report of a backend: what it is able to do. The engine queries it
/// to decide which backend handles what and to degrade gracefully when the
/// machine lacks the required GPU or monitor.
enum Caps : unsigned {
    CAP_NONE        = 0,
    CAP_BRIGHTNESS  = 1u << 0,
    CAP_CONTRAST    = 1u << 1,
    CAP_GAMMA       = 1u << 2,
    CAP_TEMPERATURE = 1u << 3,
    CAP_SATURATION  = 1u << 4,
    CAP_VIBRANCE    = 1u << 5,
    CAP_HUE         = 1u << 6,
    CAP_INVERT      = 1u << 7,
    CAP_DIM         = 1u << 8,
    CAP_HW_BRIGHT   = 1u << 9,
    CAP_HW_CONTRAST = 1u << 10,
    CAP_PER_MONITOR = 1u << 11,
};

/// One path for applying adjustments. No method may throw or bring the program
/// down: a backend that fails simply disables itself.
class Backend {
public:
    Backend() = default;
    virtual ~Backend() = default;

    /// Every backend owns system resources: a loaded DLL, a thread, a physical
    /// monitor handle, a COM connection. Copying would duplicate the owner and
    /// the second destructor would release what the first already released —
    /// DestroyPhysicalMonitors twice on one handle, CloseHandle on one thread,
    /// `delete impl_` on one pointer.
    ///
    /// Declared here so that the rule is deliberate and uniform for every
    /// backend, instead of following from which members a class happens to hold.
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    virtual const wchar_t* Name() const = 0;
    virtual unsigned Capabilities() const = 0;

    /// Attempts to initialize. Returns false when the resource does not exist on
    /// this machine.
    virtual bool Init() = 0;
    virtual void Shutdown() {}

    virtual void Apply(const MonitorTarget& m, const Adjustments& a) = 0;
    virtual void Reset(const MonitorTarget& m) = 0;

    bool Available() const { return available_; }
    const std::wstring& Details() const { return details_; }

protected:
    bool available_ = false;
    std::wstring details_;
};

/// Gamma ramp through GDI. Universal path: any GPU, per monitor, and the only
/// one that still applies in exclusive fullscreen games.
/// Does no saturation: a ramp cannot mix channels.
class GammaBackend : public Backend {
public:
    const wchar_t* Name() const override { return L"Gamma ramp (GDI)"; }
    unsigned Capabilities() const override {
        return CAP_BRIGHTNESS | CAP_CONTRAST | CAP_GAMMA | CAP_TEMPERATURE | CAP_PER_MONITOR;
    }
    bool Init() override;
    void Apply(const MonitorTarget& m, const Adjustments& a) override;
    /// Low-latency path for sliders in motion: performs at most one LUT write
    /// and never runs the visible adaptive search mid-gesture. The normal Apply
    /// completes the search once the drag stops.
    void ApplyInteractive(const MonitorTarget& m, const Adjustments& a);
    void Reset(const MonitorTarget& m) override;

    /// Rewrites the last known ramp (watchdog against other software).
    void Reassert(const MonitorTarget& m);

    /// The extended range requires the GdiIcmGammaRange registry key.
    static bool RangeUnlocked();
    static bool TryUnlockRange(bool unlock);
    /// The system night light feature contends for the same gamma ramp.
    static bool NightLightActive();

    /// True when Windows refused the full ramp and the effect had to be reduced.
    /// The interface reports this instead of implying that it worked.
    bool Limited() const { return limited_; }
    /// Fraction of the effect Windows accepted (0..1) on the primary monitor.
    double AcceptedFraction() const { return acceptedFraction_; }

    /// Reads and stores the ramp that was in effect before the display was
    /// touched. With an ICC calibration in place, that is what a reset restores.
    void CaptureBaseline(const MonitorTarget& m);
    /// Injects a baseline loaded from disk (recovery after a crash).
    void AdoptBaseline(const Baseline& b);
    /// Exports what was captured, for writing to disk.
    void ExportBaseline(Baseline* b) const;
    /// True when the monitor's baseline ramp is not the linear one.
    bool HasCustomBaseline(const std::wstring& monitorKey) const;

private:
    struct MonState {
        std::vector<WORD> lastTarget;   ///< what was requested
        std::vector<WORD> lastWritten;  ///< what Windows accepted
        double blend = 1.0;             ///< applied fraction
        double nextProbeMs = 0;         ///< when to try the full ramp again
        double accepted = 1.0;          ///< fraction Windows accepted on this monitor
    };

    /// Writes the ramp; if Windows refuses, searches for the largest accepted fraction.
    void WriteAdaptive(const MonitorTarget& m, const WORD target[768], MonState* st);
    /// Tries a single ramp during a drag. On refusal, keeps the last valid ramp
    /// instead of oscillating between intensities.
    void WriteInteractive(const MonitorTarget& m, const WORD target[768], MonState* st);
    static bool TryWrite(HDC dc, const WORD ramp[768]);
    /// Interpolates from the monitor's baseline (`base`) to the target.
    static void BlendRamp(const WORD target[768], const WORD base[768],
                          double t, WORD out[768]);
    /// The ramp the monitor had before Zdisplay; linear when nothing was recorded.
    void BaseRampFor(const std::wstring& monitorKey, WORD out[768]) const;
    /// Composes the baseline ramp with the computed one, preserving the calibration.
    static void ComposeWithBaseline(const WORD ours[768], const WORD baseline[768], WORD out[768]);
    /// Builds the final LUT, including the calibration already present on the monitor.
    void BuildTarget(const MonitorTarget& m, const Adjustments& a, WORD target[768]);

    std::map<std::wstring, std::vector<WORD>> baseline_;

    /// `!RampIsIdentity(baseline_[key])`, computed once at capture time.
    ///
    /// Whether a monitor carries its own calibration is constant, since the
    /// baseline does not change after capture; recomputing it inside BuildTarget
    /// would cost 768 comparisons per monitor on every transition frame.
    ///
    /// Invariant: written together with `baseline_`, and only in the two
    /// functions that fill it (CaptureBaseline and AdoptBaseline).
    std::map<std::wstring, bool> baselineCustom_;
    std::map<std::wstring, MonState> state_;
    bool   limited_ = false;
    double acceptedFraction_ = 1.0;
    bool   warned_ = false;
};

/// Brightness on a display with HDR enabled, through the SDR white level.
///
/// With HDR on, the gamma ramp is dead: Windows accepts SetDeviceGammaRamp,
/// reports success and ignores the result. The remaining fallbacks are poor —
/// the color matrix is global, so a single HDR display would spoil the others,
/// and it does not reach exclusive fullscreen; physical DDC/CI brightness writes
/// to the EEPROM and is not available on every monitor.
///
/// The SDR white level has none of those drawbacks: it acts per display, is a
/// plain API call with no I2C traffic and no wear, and applies in exclusive
/// fullscreen.
///
/// Its honest limit: it governs SDR content only, that is the desktop, windows
/// and ordinary video. Genuine HDR content draws in the HDR range and keeps its
/// own brightness.
class HdrBackend : public Backend {
public:
    const wchar_t* Name() const override { return L"HDR (SDR white level)"; }
    unsigned Capabilities() const override { return CAP_BRIGHTNESS | CAP_PER_MONITOR; }
    bool Init() override;
    void Apply(const MonitorTarget& m, const Adjustments& a) override;
    void Reset(const MonitorTarget& m) override;
    /// Rewrites the last value (watchdog): resuming from sleep and video mode
    /// changes return the white level to the system default.
    void Reassert(const MonitorTarget& m);

    /// Re-evaluates which displays this path covers. HDR is turned on and off at
    /// run time, so availability cannot be frozen at Init as it is for the other
    /// backends.
    void Probe();

    /// Reports whether this display can take brightness through this path.
    bool Supports(const MonitorTarget& m) const;

    void CaptureBaseline(const MonitorTarget& m);
    void AdoptBaseline(const Baseline& b);
    void ExportBaseline(Baseline* b) const;
    /// Restores the original white level even when this session never changed it
    /// (recovery after an abnormal shutdown).
    void ForceRestore();

    /// Current level in nits, for diagnostics. 0 when unknown.
    int CurrentNits(const std::wstring& monitorKey) const;

private:
    struct MonState {
        int  origNits = 0;      ///< what the display had before it was touched
        int  lastWritten = 0;   ///< never rewrites the same value
        bool everChanged = false;
    };
    std::map<std::wstring, MonState> state_;
};

/// 5x5 color matrix applied to the whole desktop. The only saturation path that
/// works on any machine, including one without a discrete GPU.
/// The effect is global: saturation cannot differ per monitor, and it does not
/// reach exclusive fullscreen games (borderless does work).
class MagnifyBackend : public Backend {
public:
    const wchar_t* Name() const override { return L"Color matrix (Magnification API)"; }
    unsigned Capabilities() const override {
        return CAP_SATURATION | CAP_HUE | CAP_INVERT;
    }
    bool Init() override;
    void Shutdown() override;
    void Apply(const MonitorTarget& m, const Adjustments& a) override;
    void Reset(const MonitorTarget& m) override;
    void Reassert();

    static Mat5 BuildMatrix(const Adjustments& a, bool includeSaturation = true,
                            bool includeLevels = false);

    /// Routes brightness, contrast and temperature through the matrix. The
    /// engine enables this only when ALL displays are in HDR, where the gamma
    /// ramp does not apply. It cannot be enabled with only some displays in HDR,
    /// because the matrix affects the entire desktop and the SDR displays would
    /// receive the adjustment twice.
    void SetCompensateGamma(bool on) { compensateGamma_ = on; }
    bool CompensatingGamma() const { return compensateGamma_; }

private:
    void SetMatrix(const Mat5& m);

    DynLib lib_;
    bool   initialized_ = false;
    Mat5   last_ = Mat5::Identity();
    bool   hasLast_ = false;
    /// Effect already in force before this backend touched it.
    ///
    /// MagSetFullscreenColorEffect is the same mechanism as the Windows
    /// accessibility color filters: a write never fails, it overwrites. Storing
    /// the previous effect is what allows a color-blindness or greyscale filter
    /// to be restored instead of being replaced and re-imposed by the watchdog.
    Mat5   original_ = Mat5::Identity();
    bool   hasOriginal_ = false;
    /// Brightness/contrast/temperature through the matrix too (see SetCompensateGamma).
    bool   compensateGamma_ = false;

    typedef BOOL (WINAPI *PfnMagInitialize)(void);
    typedef BOOL (WINAPI *PfnMagUninitialize)(void);
    typedef BOOL (WINAPI *PfnMagSetFullscreenColorEffect)(const void*);
    typedef BOOL (WINAPI *PfnMagGetFullscreenColorEffect)(void*);

    PfnMagInitialize               pMagInit_ = nullptr;
    PfnMagUninitialize             pMagUninit_ = nullptr;
    PfnMagSetFullscreenColorEffect pMagSet_ = nullptr;
    PfnMagGetFullscreenColorEffect pMagGet_ = nullptr;
};

/// Vibrance and hue on NVIDIA GPUs, through NVAPI loaded at run time.
/// Without an NVIDIA GPU the backend simply does not become available.
class NvapiBackend : public Backend {
public:
    const wchar_t* Name() const override { return L"NVIDIA (NVAPI)"; }
    unsigned Capabilities() const override { return CAP_VIBRANCE | CAP_HUE | CAP_PER_MONITOR; }
    bool Init() override;
    void Shutdown() override;
    void Apply(const MonitorTarget& m, const Adjustments& a) override;
    void Reset(const MonitorTarget& m) override;

    /// Re-enumerates after a monitor change, so that a hot-plug cannot leave
    /// Resolve addressing a stale display.
    void Rediscover();

    /// Reports whether the driver exposes hue FOR THIS monitor. Available() is
    /// not enough on its own: hue can be absent on a particular display even
    /// when the vendor API is present.
    bool HasHue(const MonitorTarget& m) const;
    /// The engine turns this off when hue is routed through the universal
    /// matrix, so the same adjustment is not applied twice on one monitor.
    void SetHandleHue(bool on) { handleHue_ = on; }

    void AdoptBaseline(const Baseline& b);
    void ExportBaseline(Baseline* b) const;

private:
    struct Display {
        void*    handle = nullptr;
        unsigned outputId = 0;
        // Range and original values are per display: with a different vibrance
        // on each monitor, backend-wide scalars would overwrite one another and
        // lose the second monitor's original value.
        int minLevel = 0, maxLevel = 63, defaultLevel = 0;
        int origLevel = -1;
        int origHue = -1;
        bool hasHue = false;
    };
    Display* Resolve(const MonitorTarget& m);
    bool SetVibrance(const Display& d, int level);
    void Enumerate();

    DynLib lib_;
    std::map<std::wstring, Display> displays_;  // GDI name -> display
    std::map<std::wstring, int> lastLevel_, lastHue_;
    bool handleHue_ = true;
    bool apiInitialized_ = false;
    void* fns_[16] = {};
};

/// Saturation and hue on AMD GPUs through ADL (atiadlxx.dll), loaded at run time.
class AdlBackend : public Backend {
public:
    const wchar_t* Name() const override { return L"AMD (ADL)"; }
    unsigned Capabilities() const override {
        return CAP_VIBRANCE | CAP_SATURATION | CAP_HUE | CAP_PER_MONITOR;
    }
    bool Init() override;
    void Shutdown() override;
    void Apply(const MonitorTarget& m, const Adjustments& a) override;
    void Reset(const MonitorTarget& m) override;

    /// The engine enables this when the GPU is to handle absolute saturation.
    void SetHandleSaturation(bool on) { handleSaturation_ = on; }

    /// Reports whether the driver exposes hue for this monitor (see NvapiBackend).
    bool HasHue(const MonitorTarget& m) const;
    /// Turned off when hue is routed through the universal matrix.
    void SetHandleHue(bool on) { handleHue_ = on; }

    /// Re-enumerates after a monitor change (see NvapiBackend).
    void Rediscover();

    void AdoptBaseline(const Baseline& b);
    void ExportBaseline(Baseline* b) const;

private:
    struct Disp {
        std::wstring gdiName;
        int adapter = 0, display = 0;
        int satMin = 0, satMax = 0, satDefault = 0;
        int hueMin = 0, hueMax = 0, hueDefault = 0;
        /// Values the user already had in the vendor control panel.
        int origSat = -1, origHue = -1;
        bool hasSat = false, hasHue = false;
    };
    const Disp* Resolve(const MonitorTarget& m) const;
    void Enumerate();

    DynLib lib_;
    std::vector<Disp> displays_;
    std::map<std::wstring, std::pair<int, int>> lastApplied_;
    bool handleSaturation_ = false;
    bool handleHue_ = true;
    bool apiInitialized_ = false;
    void* fns_[8] = {};
};

/// DDC/CI: brightness and contrast in the external monitor's own hardware.
/// Reduces light for real, without washing out contrast the way software
/// dimming does. Commands are slow and write to the monitor's EEPROM, so they
/// pass through a queue with coalescing and a minimum interval.
class DdcciBackend : public Backend {
public:
    ~DdcciBackend() override { Shutdown(); }
    const wchar_t* Name() const override { return L"Monitor hardware (DDC/CI)"; }
    unsigned Capabilities() const override {
        unsigned c = CAP_HW_BRIGHT | CAP_HW_CONTRAST | CAP_PER_MONITOR;
        // Monitor RGB gain counts as a color temperature path only when some
        // panel actually exposes it (VCP 0x16/0x18/0x1A).
        if (anyGain_ != 0) c |= CAP_TEMPERATURE;
        return c;
    }
    bool Init() override;
    void Shutdown() override;
    void Apply(const MonitorTarget& m, const Adjustments& a) override;
    void Reset(const MonitorTarget& m) override;

    /// Re-enumerates when the display arrangement changes.
    void Discover();

    /// Holds commands for a while, keeping whatever is pending.
    ///
    /// Right after a resume from sleep the I2C bus and the display drivers are
    /// not answering yet, and writing at that moment produces a run of failures
    /// that spends the retry budget, counts against the per-minute limit and can
    /// disable the monitor for consecutive failures.
    ///
    /// The queue already coalesces by key, so holding loses nothing: what is
    /// sent when the hold expires is the LAST requested value, applied once.
    void HoldCommands(int milliseconds);
    void SetMonitorModes(const std::map<std::wstring, DdcMonitorMode>& modes) {
        Guard g(lock_);
        monitorModes_ = modes;
    }
    bool Supports(const std::wstring& monitorKey) const;
    bool SupportsBrightness(const std::wstring& monitorKey) const;
    /// Physical brightness the monitor had before Zdisplay touched it, in
    /// percent; -1 when the monitor does not expose brightness. It is the
    /// reference point for the HDR fallback: on a display with HDR enabled the
    /// gamma ramp does not apply, so software brightness becomes a FRACTION of
    /// this value.
    int OriginalBrightness(const std::wstring& monitorKey) const;
    bool SupportsContrast(const std::wstring& monitorKey) const;
    /// Whether the panel answered the RGB gain registers, and the saturation one.
    bool SupportsGain(const std::wstring& monitorKey) const;
    bool SupportsSaturation(const std::wstring& monitorKey) const;
    /// The panel's own value for a color register before Zdisplay wrote to it,
    /// in percent; -1 when the register is not available. Offered as the
    /// starting point for the sliders, so switching the controls on does not
    /// move the image.
    int OriginalGain(const std::wstring& monitorKey, int channel) const;
    int OriginalSaturation(const std::wstring& monitorKey) const;
    /// Reads the current physical brightness (0..100), or -1.
    double ReadBrightness(const std::wstring& monitorKey);

    /// One diagnostic line per monitor: path used, ranges, and what the
    /// capability string declared.
    std::vector<std::wstring> Diagnose() const;

    /// Requests a read of the capability string. Not automatic, by design.
    ///
    /// Reading capabilities is the only DDC/CI command here that can bring the
    /// machine down: a Windows kernel defect turns a malformed string — exactly
    /// what a generic monitor tends to report — into a bug check. The string is
    /// used for diagnostics only, since RGB gain is detected by reading the
    /// register directly, so it sits behind an explicit user action, with a
    /// warning and an on-disk marker that blocks a second attempt when the first
    /// one never returned.
    void RequestCapabilities();

    /// Requests a round-trip test: writes a value close to the current one,
    /// reads it back, checks it and restores the original value.
    ///
    /// It separates "brightness was detected on this monitor" from "this monitor
    /// obeys": panels exist that answer reads, accept writes and report success
    /// without changing a pixel, and without the round trip the diagnostics
    /// would claim something that is not happening on screen.
    ///
    /// Not applied automatically: each call is a real EEPROM write on the panel,
    /// so it is issued only on explicit user action.
    void RequestRoundTrip();

    /// An extra feature discovered on a monitor (input source, volume, power
    /// mode, color preset).
    struct Feature {
        unsigned char code = 0;
        int current = -1;
        int maximum = 0;
        /// Accepted values, when the monitor declared them. Empty = continuous.
        std::vector<unsigned char> values;
    };

    /// Probes the extra features by reading the registers directly.
    ///
    /// Deliberately kept out of discovery: it costs four more slow commands per
    /// monitor and the vast majority of sessions never open those controls. It
    /// does not depend on the capability string either, since reading a register
    /// answers immediately and avoids the kernel defect that makes the string
    /// dangerous.
    void RequestFeatureProbe();
    /// What the probe found on this monitor.
    std::vector<Feature> Features(const std::wstring& monitorKey) const;
    /// Reports whether the probe has already run, even if it found nothing.
    bool FeaturesProbed(const std::wstring& monitorKey) const;
    /// Sends a value to an extra feature. Enters the same queue, with the same
    /// minimum interval and per-minute ceiling as every other command.
    void SetFeature(const std::wstring& monitorKey, unsigned char code, int value);

    /// Clears persistent quarantines created after a crash during a dangerous
    /// probe. Does not run the probe again: that still requires explicit user
    /// confirmation.
    int ClearSafetyBlocks();
    bool RestoreIncomplete() const { return restoreIncomplete_; }

    void AdoptBaseline(const Baseline& b);
    void ExportBaseline(Baseline* b) const;
    /// Restores the original values even when this session never changed them.
    /// Used to recover after an abnormal shutdown, where the previous run made
    /// the changes.
    void ForceRestore();

    /// Gives one group of colour registers back to the monitor: the RGB gains,
    /// or the saturation. An empty key means every panel.
    ///
    /// Needed because a profile that stops managing a register simply says
    /// nothing about it, and silence leaves the panel on the last value written.
    /// Only registers this session actually changed are written.
    void RestoreColor(const std::wstring& monitorKey, bool gain);

private:
    struct FeatureState {
        bool liveProven = false;
        bool advertised = false;
        bool blocked = false;
        int failures = 0;
        DWORD lastError = 0;
        DdcErrorKind lastKind = DdcErrorKind::None;
        double retryAfterMs = 0;
        DWORD codeType = 0;
        DWORD rawCurrent = 0;
        DWORD rawMaximum = 0;
    };

    struct Phys {
        HANDLE handle = nullptr;
        std::wstring description;
        bool hasBrightness = false, hasContrast = false;
        DWORD bMin = 0, bMax = 100, cMin = 0, cMax = 100;
        /// The high-level API (GetMonitorBrightness/SetMonitorBrightness)
        /// refused this monitor, so raw VCP is used to talk to it.
        ///
        /// Common on generic monitors: the high-level API validates the
        /// capability string before talking to the panel, and low-end panels
        /// often report that string truncated, malformed or absent.
        bool viaVcp = false;
        /// Hardware RGB gain (VCP 0x16/0x18/0x1A), in DDC units.
        bool hasGain = false;
        DWORD gMax = 100;
        int origGain[3] = {-1, -1, -1};
        int lastWrittenGain[3] = {-1, -1, -1};
        FeatureState gainState[3];
        /// Hardware color saturation (VCP 0x8A), in DDC units. Read on the same
        /// pass as the gains, and absent from most panels.
        bool hasSat = false;
        DWORD satMin = 0, satMax = 100;
        int origSat = -1;
        int lastWrittenSat = -1;
        FeatureState satState;
        /// Raw capability string, read on demand by the queue thread.
        std::wstring caps;
        /// Result of the last round-trip test, ready for the diagnostics.
        /// Empty = never tested.
        std::wstring roundTrip;
        /// Extra features found by the on-demand probe.
        std::vector<Feature> features;
        bool featuresProbed = false;
        FeatureState brightnessState, contrastState;
        bool handleUnavailable = false;
        bool capsUnsafe = false;
        /// Brightness register of THIS panel. Almost always the standard 0x10,
        /// but some models answer on another one and, on 0x10, accept the
        /// command without changing anything, which is worse than refusing it.
        BYTE brightnessCode = 0x10;
        /// Further panels behind the SAME HMONITOR, as keys into `monitors_`.
        ///
        /// Windows presents mirrored displays as one desktop area, so a clone
        /// pair arrives as a single monitor key with two physical panels behind
        /// it. Each panel keeps its own entry, because the ranges, the original
        /// values and the EEPROM write budget belong to the panel and not to the
        /// desktop area. Filled on the primary entry only.
        std::vector<std::wstring> clones;
        /// This entry is one of those extra panels. The rest of the program
        /// addresses monitors by their own key and never sees these.
        bool isClone = false;
        DdcMonitorMode mode = DdcMonitorMode::Auto;
        double lastCommandMs = 0;
        /// Values the monitor had before it was touched, in percent.
        int origBrightness = -1, origContrast = -1;
        /// Only what was actually changed is restored.
        bool everChanged = false;
        bool changedBrightness = false, changedContrast = false;
        bool changedGain[3] = {false, false, false};
        bool changedSat = false;
        /// Hard per-minute command limit, to spare the EEPROM.
        int commandsThisMinute = 0;
        double minuteStartMs = 0;
        /// Last value actually written, in DDC units. A command is never sent
        /// when its value equals this one, so moving any slider cannot rewrite
        /// an unchanged brightness to the EEPROM.
        int lastWrittenB = -1, lastWrittenC = -1;
    };
    struct Want {
        double brightness = -1;
        double contrast = -1;
        /// Factor 0..1 applied to the monitor's original RGB gain. -1 = do not
        /// manage. Filled in only when the gamma ramp does not apply to the display.
        double gainFactor[3] = {-1, -1, -1};
        /// Gain asked for by the profile, in percent of the range the monitor
        /// reports. -1 = the profile does not manage this channel.
        ///
        /// Takes precedence over the factor above: a value the user set is an
        /// instruction, while the factor is a fallback for a display whose gamma
        /// ramp does nothing.
        double gainPercent[3] = {-1, -1, -1};
        double satPercent = -1;    ///< the same, for VCP 0x8A
        bool dirty = false;
        bool restoring = false;
        unsigned long long generation = 0;
    };

    static DWORD WINAPI WorkerThunk(LPVOID self);
    void WorkerLoop();
    /// The enumeration itself. May run only on the queue thread, or before that
    /// thread exists: it destroys the physical handles the queue uses.
    void DiscoverNow();
    void ReleaseHandles();
    /// Runs whatever is left in the queue on the calling thread. Used on exit,
    /// so that the physical brightness restore does not die with the thread.
    bool DrainPending();
    /// Reads the capability string of the monitors that do not have it yet.
    ///
    /// Deliberately kept out of Init: the query takes from hundreds of
    /// milliseconds to several seconds PER MONITOR, and running it during
    /// discovery would delay startup by several seconds.
    void FetchCapabilities();
    /// Runs the round-trip test. Queue thread only: these are slow commands to
    /// the monitor.
    void RunRoundTrip();
    /// Probes the extra features. Queue thread only.
    void ProbeFeatures();
    void LoadSafetyState();
    void SaveSafetyState() const;
    void MarkCapsUnsafe(const std::wstring& monitorKey, const std::wstring& stage);

    /// What a panel was carrying, kept across discovery passes.
    ///
    /// A monitor that answers nothing during one pass — routine right after a
    /// resume — is not inserted into `monitors_`. Rebuilding this from
    /// `monitors_` would therefore drop its original values, and the next pass
    /// that does find it would record the value Zdisplay itself wrote as if it
    /// were the user's own, and clear the flags that make the restore happen.
    ///
    /// Capabilities are held the same way and for the same reason: a register
    /// proven by a read once does not stop existing because a later read failed,
    /// and a register proven absent is not probed again on every pass.
    struct KnownState {
        int origBrightness = -1, origContrast = -1;
        int origGain[3] = {-1, -1, -1};
        int origSat = -1;
        bool changedBrightness = false, changedContrast = false;
        bool changedGain[3] = {false, false, false};
        bool changedSat = false;
        FeatureState brightnessState, contrastState, gainState[3], satState;
        std::wstring caps;
        bool gainProven = false;
        DWORD gMax = 100;
        /// Tri-state by the pair: neither flag set means "never established".
        bool satProven = false, satAbsent = false;
        DWORD satMin = 0, satMax = 100;
    };

    DynLib lib_;
    std::map<std::wstring, Phys> monitors_;
    std::map<std::wstring, KnownState> lastKnown_;
    std::vector<std::pair<HANDLE, size_t>> owned_;  // blocks returned by the API
    std::map<std::wstring, Want> pending_;
    std::map<std::wstring, std::wstring> capsUnsafe_;
    /// Panels that were enumerated but answered no register at all, by monitor
    /// key -> display name.
    ///
    /// Kept so the diagnostics can state that the panel was found and stayed
    /// mute. Dropping it from the list instead would read as "no such monitor",
    /// which is the one thing the user cannot tell apart from a bug.
    std::map<std::wstring, std::wstring> unresponsive_;
    std::map<std::wstring, DdcMonitorMode> monitorModes_;
    Lock   lock_;
    HANDLE thread_ = nullptr;
    HANDLE wake_ = nullptr;
    volatile LONG running_ = 0;
    volatile LONG rediscover_ = 0;
    volatile LONG needCaps_ = 0;
    volatile LONG needRoundTrip_ = 0;
    volatile LONG needFeatures_ = 0;
    /// Pending writes to extra features. A queue separate from the adjustments
    /// because the semantics differ: these are discrete actions that accumulate,
    /// not a state in which the last value replaces the previous one.
    std::vector<std::pair<std::wstring, std::pair<unsigned char, int>>> featureQueue_;
    unsigned long long nextGeneration_ = 0;
    /// Instant until which no command is sent (see HoldCommands). Under `lock_`.
    double holdUntilMs_ = 0;
    bool restoreIncomplete_ = false;
    volatile LONG anyGain_ = 0;

    void* fns_[16] = {};
};

/// Backlight of a laptop's internal panel, through WMI. The equivalent of
/// DDC/CI for panels that do not speak DDC/CI.
class BacklightBackend : public Backend {
public:
    ~BacklightBackend() override { Shutdown(); }
    const wchar_t* Name() const override { return L"Laptop backlight (WMI)"; }
    unsigned Capabilities() const override { return CAP_HW_BRIGHT; }
    bool Init() override;
    void Shutdown() override;
    void Apply(const MonitorTarget& m, const Adjustments& a) override;
    void Reset(const MonitorTarget& m) override;

    double Read();
    void AdoptBaseline(const Baseline& b);
    void ExportBaseline(Baseline* b) const;
    void ForceRestore();

    /// Requests a re-read of the panel's real brightness on the queue's next pass.
    void RequestRefresh();

    /// Rebuilds the WMI connection.
    ///
    /// Required after a resume from sleep: the stored COM pointers can be dead,
    /// and from then on every read and write fails silently, leaving the
    /// internal panel unresponsive.
    void Reconnect();

    /// Completes an Init that did not wait for WMI to answer. Returns true ONCE,
    /// at the moment the backend becomes available, which is when the baseline
    /// must be completed and the profile reapplied.
    bool PollReady();
    /// Reports whether an Init is still waiting for a WMI answer.
    bool InitPending() const { return pendingInit_; }

    /// Reports once a brightness change that Zdisplay did not request, in
    /// practice the user pressing the keyboard brightness keys.
    ///
    /// This is what extends the Fn keys to the external monitors as well. It
    /// rides on the watchdog instead of owning a thread: a WMI query every few
    /// seconds costs almost nothing, and the delay helps, since it keeps every
    /// intermediate value of a held key from reaching the monitor's EEPROM.
    bool TakeExternalChange(int* percent);

private:
    bool SetBrightness(int percent);

    struct Impl;
    Impl* impl_ = nullptr;
    double lastWriteMs_ = 0;
    /// Brightness the internal panel had before Zdisplay changed it.
    int original_ = -1;
    bool everChanged_ = false;
    /// Init returned without waiting for WMI; PollReady() completes it later.
    bool pendingInit_ = false;
};

/// Dimming layer built from overlay windows. The only way to darken beyond the
/// panel's own minimum, and it works on any machine. In exchange it washes out
/// contrast slightly and appears in screen captures, so it defaults to zero.
class OverlayBackend : public Backend {
public:
    ~OverlayBackend() override { Shutdown(); }
    const wchar_t* Name() const override { return L"Dimming overlay"; }
    unsigned Capabilities() const override { return CAP_DIM | CAP_PER_MONITOR; }
    bool Init() override;
    void Shutdown() override;
    void Apply(const MonitorTarget& m, const Adjustments& a) override;
    void Reset(const MonitorTarget& m) override;
    void Reassert();

    /// Destroys the windows of monitors that left the arrangement.
    ///
    /// Without it, disconnecting a monitor orphans its window: no loop visits it
    /// again, since they all iterate monitors::All(), Reassert keeps asserting
    /// HWND_TOPMOST, and being WS_EX_TRANSPARENT it cannot even be clicked to
    /// close, leaving a permanent dark rectangle.
    void SyncMonitors(const std::vector<MonitorTarget>& alive);

    /// Hides the layer while the dark-screen confirmation is open.
    void Suspend(bool on);
    bool Suspended() const { return suspended_; }

private:
    HWND GetOrCreate(const MonitorTarget& m);
    std::map<std::wstring, HWND> windows_;
    /// Which windows Suspend(true) hid. Suspend(false) may show only these:
    /// showing every invisible window would resurrect, with its old alpha, a
    /// window that Apply had hidden because dim = 0.
    std::set<std::wstring> hiddenBySuspend_;
    bool classRegistered_ = false;
    bool suspended_ = false;
};

}  // namespace zdisplay
