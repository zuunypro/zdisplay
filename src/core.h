#pragma once
#include "common.h"

namespace zdisplay {

// Color adjustments

/// All-neutral values leave the display untouched: a freshly created profile
/// changes nothing.
struct Adjustments {
    // Gamma ramp path: works on any GPU and still applies inside exclusive
    // fullscreen games.
    double brightness  = 100;    // 10..150, 100 = neutral
    double contrast    = 100;    // 0..200
    double gamma       = 1.0;    // 0.3..3.0
    double temperature = 6500;   // Kelvin, 6500 = neutral
    double redGain     = 100;    // 50..100 (white balance)
    double greenGain   = 100;
    double blueGain    = 100;

    /// Blue light block, 0..100. 0 = neutral.
    ///
    /// Deliberately separate from color temperature: temperature shifts the
    /// whole white point, while this value cuts blue only and its number states
    /// directly how much blue is being removed.
    double blueBlock   = 0;

    // Shadow visibility. Same ramp, but only the low end of the curve: these two
    // act on dark tones and leave midtones and highlights where they are.
    double shadows     = 0;      // 0..100, 0 = neutral (shadow lift)
    double clarity     = 0;      // 0..100, 0 = neutral (separates what the lift flattens)

    // Color matrix / vendor GPU path.
    double saturation  = 100;    // 0..200, 100 = neutral
    double vibrance    = 0;      // 0..100, 0 = neutral (vendor GPU vibrance)
    double hue         = 0;      // -180..180 degrees
    bool   invert      = false;

    // Overlay.
    double dim         = 0;      // 0..90 %

    // Monitor hardware. -1 means "do not manage".
    double hwBrightness = -1;    // 0..100 via DDC/CI or backlight
    double hwContrast   = -1;    // 0..100 via DDC/CI

    /// Final per-channel multipliers: the white balance gains already combined
    /// with the blue light block.
    ///
    /// Single source for both the ramp sent to the display and the luminance
    /// estimate that enforces the light floor, so the floor always guards the
    /// image actually on screen.
    void ChannelGains(double* r, double* g, double* b) const;

    bool GammaNeutral() const;
    bool MatrixNeutral() const;
    bool Neutral() const;

    /// Clamps every value to its usable range. No input path (hand-edited file,
    /// command line, imported profile) can produce a state that leaves the
    /// display unusable.
    void Sanitize();

    /// Estimated light remaining on screen, 0..1, combining software brightness
    /// with overlay dimming. Used to keep the user from locking themselves into
    /// a black screen.
    double EffectiveLuminance() const;

    static Adjustments Blend(const Adjustments& a, const Adjustments& b, double t);
};

/// Below this luminance the screen becomes hard to read; Zdisplay asks for
/// confirmation with automatic revert.
constexpr double kRiskyLuminance = 0.20;
/// How far luminance must fall below the level the user already accepted before
/// the dark-screen confirmation is asked again.
constexpr double kDarkReaskMargin = 0.03;
/// Minimum interval between two dark-screen confirmation prompts, so that
/// dragging a slider through many dark states yields at most one prompt.
constexpr unsigned kDarkAskCooldownMs = 30000;
/// Absolute light floor: not even the command line can go below it.
constexpr double kFloorLuminance = 0.08;

// Profiles

enum class SatEngine {
    Auto = 0,       ///< GPU handles vibrance; the universal matrix handles saturation.
    Gpu = 1,        ///< Forces the vendor GPU path (only where it exists).
    Universal = 2,  ///< Forces the color matrix; identical result on any machine.
    Off = 3,        ///< Leaves saturation untouched.
};

struct Profile {
    std::wstring name = L"Novo perfil";
    Adjustments  global;
    /// Per-monitor overrides, indexed by the stable monitor key.
    std::map<std::wstring, Adjustments> perMonitor;

    std::wstring hotkey;             ///< e.g. "Ctrl+Alt+1"
    SatEngine    satEngine = SatEngine::Auto;
    int          transitionMs = 400;

    const Adjustments& For(const std::wstring& monitorKey) const;
    Adjustments*       Find(const std::wstring& monitorKey);
    Adjustments&       Ensure(const std::wstring& monitorKey);
};

/// Clamps every field of a profile to its usable range: global adjustments,
/// per-monitor overrides, transition duration and saturation engine.
///
/// Every input path must go through this function, so that a profile brought
/// from another machine can neither breach the light floor nor let NaN reach the
/// gamma ramp.
void SanitizeProfile(Profile* p);

/// Strips from a profile name the text that would break the file format.
///
/// "|monitor:" is the section header separator, so a name containing it would be
/// written and read back as another profile's per-monitor override.
std::wstring SanitizeProfileName(const std::wstring& name);

struct AppRule {
    bool         enabled = true;
    std::wstring process;      ///< executable name without .exe; '*' accepted
    std::wstring profile;
    int          priority = 0;
    bool Matches(const std::wstring& processName) const;
    /// Tie-breaker between rules of equal priority: exact beats longest wildcard.
    int Specificity() const;
};

/// User location, for the rules that follow the sun.
struct SolarContext {
    double latitude = 0;    ///< degrees, positive north
    double longitude = 0;   ///< degrees, positive east
    double tzHours = 0;     ///< local offset from UTC, in hours
    bool   valid = false;   ///< false = no location; solar times do not resolve
};

/// Sunrise and sunset, in minutes since local midnight.
///
/// Serves the schedule rules: "night" means from sunset onwards, and that moves
/// by more than two hours over the year, so a fixed clock range is wrong for
/// half of them.
///
/// Returns false on days when the sun neither rises nor sets at the given
/// latitude, which happens above the polar circles.
bool SunTimes(int year, int month, int day, double latDeg, double lonDeg,
              double tzOffsetHours, int* sunriseMin, int* sunsetMin);

/// Converts a rule's time text into minutes since midnight.
///
/// Accepts clock times ("22:00") and solar times with an optional offset:
/// "nascer", "por", "por-30" (half an hour before sunset), "nascer+45".
/// Returns -1 when the text is invalid or asks for a solar time with no location.
int ResolveRuleTime(const std::wstring& text, const SYSTEMTIME& now,
                    const SolarContext& solar);

/// Reports whether a rule's time text is well formed.
///
/// Syntax only: independent of location and of the current date. Accepts both
/// clock times and solar times such as "por" and "nascer+45", so the interface
/// can validate exactly what the engine understands.
bool IsValidRuleTime(const std::wstring& text);

/// Current Windows time zone offset, daylight saving included, in hours.
double LocalTimeZoneHours();

// Vision care

/// Visual comfort layer, applied on top of the active profile.
///
/// Deliberately not another profile: warming the display as the sun sets is a
/// background behaviour that holds while any profile is active, so it is driven
/// by a single switch and needs no profile or rule.
struct Vision {
    bool   enabled = false;
    /// Target color temperature for day and night, in Kelvin.
    double dayTemperature   = 6500;   // neutral
    double nightTemperature = 3400;   // incandescent lamp light
    /// Night brightness as a percentage of what the profile requests.
    ///
    /// Matters as much as color: eye strain comes from the difference between
    /// the screen and the surrounding room, not from blue light alone.
    double nightBrightness  = 85;     // 100 = brightness untouched
    /// Length of the day-to-night crossfade, in minutes, centred on the event.
    /// A hard cut at the minute of sunset is more noticeable than the color shift.
    int    transitionMinutes = 60;
    /// When day and night begin. Accepts clock times and solar times
    /// ("nascer", "por", "por-30"), the same text as the schedule rules.
    std::wstring dayStart   = L"nascer";
    std::wstring nightStart = L"por";

    /// Break reminder interval, in minutes. 0 disables it.
    ///
    /// Follows the 20-20-20 rule: every 20 minutes, look at something about 6
    /// metres away for 20 seconds.
    int    breakMinutes = 0;

    void Sanitize();
};

/// Resolves the times used by the vision layer.
///
/// When solar times are selected but no location has been entered, falls back
/// explicitly to 07:00/20:00; real solar times take over automatically as soon
/// as a location is provided.
bool ResolveVisionTimes(const SYSTEMTIME& now, const Vision& v,
                        const SolarContext& solar,
                        int* dayStartMin, int* nightStartMin,
                        bool* usedFixedFallback = nullptr);

/// How much "night" applies right now: 0 = full day, 1 = full night.
///
/// The transition is smooth at both ends, centred on the event and
/// `transitionMinutes` wide.
double NightFraction(const SYSTEMTIME& now, const Vision& v, const SolarContext& solar);

/// Applies the vision layer on top of a profile's adjustments.
///
/// Never cools or brightens past what the profile requested: if the profile is
/// already warmer or darker than the night target, the profile wins. The layer
/// pulls towards comfort only, never against the user's intent.
Adjustments ApplyVision(const Adjustments& base, double nightFraction, const Vision& v);

struct ScheduleRule {
    bool         enabled = true;
    /// Clock time ("22:00") or solar time ("por", "nascer+30").
    std::wstring start = L"22:00";
    std::wstring end   = L"06:00";
    std::wstring profile;
    /// Tie-breaker between overlapping ranges; the highest priority wins, so
    /// line order in the file never decides the outcome.
    int          priority = 0;
    bool Matches(const SYSTEMTIME& now, const SolarContext& solar = SolarContext{}) const;
};

// Configuration

enum class DdcMonitorMode { Auto = 0, Slow = 1, Disabled = 2 };
const wchar_t* DdcMonitorModeName(DdcMonitorMode mode);
DdcMonitorMode ParseDdcMonitorMode(const std::wstring& text);

/// Known quirk of a monitor MODEL.
///
/// DDC/CI is implemented inconsistently across vendors: some panels use a
/// different brightness register, some hang when queried for their capability
/// string, and some have firmware that brings down the display driver.
/// Discovering that on the user's own hardware is expensive, so what is already
/// known is kept in a table.
///
/// Keyed by EDID identifier: three manufacturer letters plus the product code in
/// hexadecimal, e.g. "FUS087C". It identifies the model, not the individual unit.
struct MonitorQuirk {
    std::wstring edidId;
    /// Never attempt DDC/CI at all. Reserved for models that hang or crash the
    /// driver: hardware control is lost, the machine is not.
    bool block = false;
    /// Brightness register when it is not the standard 0x10. 0 = use the standard.
    int brightnessVcp = 0;
    /// Never read this model's capability string: it is the only command here
    /// that can trigger a bug check through a Windows kernel defect.
    bool unsafeCaps = false;
    std::wstring note;
};

/// Looks up the quirk for a model. Returns nullptr when there is none.
/// User rules take precedence over the built-in table.
const MonitorQuirk* FindMonitorQuirk(const std::wstring& edidId);

/// Installs the user rules read from the configuration file.
void SetUserMonitorQuirks(const std::vector<MonitorQuirk>& quirks);

/// Parses a rule in the file format: `bloquear`, `sem-capacidades` and
/// `brilho-vcp:6B`, separated by commas or spaces.
/// Returns false when nothing was recognised, so that a mistyped line does not
/// silently become an empty rule.
bool ParseMonitorQuirk(const std::wstring& edidId, const std::wstring& text,
                       MonitorQuirk* out);

/// Formats a quirk in the same format parsed above (round trip).
std::wstring FormatMonitorQuirk(const MonitorQuirk& q);

struct Config {
    std::vector<Profile>      profiles;
    std::vector<AppRule>      appRules;
    std::vector<ScheduleRule> scheduleRules;
    std::wstring              defaultProfile = L"Padrão";

    bool startWithWindows = false;
    bool startMinimized   = true;
    bool enableAppRules   = true;
    bool enableSchedule   = true;
    bool restoreOnExit    = true;

    /// Reasserts the adjustments every N seconds, guarding against other
    /// software that overwrites the gamma ramp. 0 disables it.
    int  watchdogSeconds  = 10;

    bool enableVendorApis   = true;   // NVAPI / ADL
    bool enableMagnification = true;  // universal color matrix
    bool enableDdcCi        = true;   // monitor hardware
    bool enableBacklight    = true;   // WMI, laptop panel
    bool enableOverlay      = true;   // overlay dimming
    /// Per-connection/panel exceptions. Absent = Auto.
    std::map<std::wstring, DdcMonitorMode> ddcMonitorModes;
    /// Per-MODEL monitor rules; they add to the built-in table and take
    /// precedence over it, so an unusual panel can be fixed locally without
    /// waiting for a new release.
    std::vector<MonitorQuirk> monitorQuirks;

    std::wstring hkBrightnessUp   = L"Ctrl+Alt+Up";
    std::wstring hkBrightnessDown = L"Ctrl+Alt+Down";
    std::wstring hkSaturationUp   = L"Ctrl+Alt+Right";
    std::wstring hkSaturationDown = L"Ctrl+Alt+Left";
    std::wstring hkToggle         = L"Ctrl+Alt+K";
    std::wstring hkShow           = L"Ctrl+Alt+P";
    /// Emergency exit: always restores the display and pauses.
    std::wstring hkPanic          = L"Ctrl+Alt+Shift+K";
    double       hotkeyStep       = 5;

    /// Ask for confirmation, with automatic revert, when the adjustments would
    /// leave the screen too dark. Turning this off is at the user's own risk.
    bool confirmDarkSettings = true;

    /// Mirror brightness changes made with the keyboard brightness keys to the
    /// external monitors, so that a docked laptop keeps every panel in step.
    bool mirrorInternalBrightness = true;

    /// Visual comfort layer, on top of any profile.
    Vision vision;

    /// User location, for the schedule rules that follow the sun. A value
    /// outside the valid range (the default) means "no location": rules using
    /// "nascer" or "por" simply do not match instead of guessing a place.
    double latitude  = 999;
    double longitude = 999;
    bool HasLocation() const {
        return latitude >= -90 && latitude <= 90 && longitude >= -180 && longitude <= 180;
    }
    /// Solar context built from the location and the Windows time zone.
    SolarContext Solar() const;

    Profile* Find(const std::wstring& name);
    const Profile* Find(const std::wstring& name) const;
    Profile* Default();
    std::wstring UniqueName(const std::wstring& base) const;

    void SeedDefaults();
};

/// Hand-readable and hand-editable INI file, written atomically.
bool LoadConfig(Config* cfg);
bool SaveConfig(const Config& cfg);

/// Profile exchange between machines, in the same INI format.
bool ExportProfiles(const std::wstring& path, const std::vector<Profile>& profiles);
bool ImportProfiles(const std::wstring& path, std::vector<Profile>* out);

// Display baseline

/// Everything the display was before Zdisplay touched it.
///
/// Restoring theoretical neutral values does not return a display to its
/// original state: an ICC calibration profile, or brightness set with the
/// monitor buttons, is a different starting point. The real values are captured
/// and restored exactly.
struct Baseline {
    std::map<std::wstring, std::vector<WORD>> ramps;   ///< monitor key -> 768 ramp entries
    std::map<std::wstring, std::pair<int, int>> hardware;  ///< brightness, contrast (-1 = unknown)
    int backlight = -1;                                 ///< internal panel (-1 = unknown)

    /// Vibrance/saturation and hue as found in the vendor control panel, by
    /// display name. Persisting them keeps a later Init from reading back a
    /// value Zdisplay itself wrote and treating it as the original.
    std::map<std::wstring, std::pair<int, int>> vendor;

    /// SDR white level in nits, by monitor key. It is the brightness control
    /// that governs SDR content on a display with HDR enabled, and the only
    /// value here that survives a restart without being read back from the
    /// system, so restoring it is what returns the display to its original point.
    std::map<std::wstring, int> hdrWhite;

    bool Empty() const {
        return ramps.empty() && hardware.empty() && backlight < 0 &&
               vendor.empty() && hdrWhite.empty();
    }
};

/// Persists the baseline to disk, so that not even a crash can leave the display
/// stuck in an adjusted state.
bool SaveBaseline(const Baseline& b);
bool LoadBaseline(Baseline* b);
void ClearBaseline();

/// Session marker: present while Zdisplay is running. If it already exists at
/// startup, the previous run did not shut down cleanly.
bool SessionWasDirty();
void SessionBegin();
void SessionEnd();

// Color math

/// RGB multipliers (0..1) for a color temperature in Kelvin. 6500 K = white.
void TemperatureToRgb(double kelvin, double* r, double* g, double* b);

/// Shadow curve: takes and returns a tone in 0..1.
///
/// `shadows` lifts the black floor with a weight that fades out at the end of
/// the window; `clarity` first expands the difference between near-black tones.
/// Both only add light, and the curve is monotonic for any combination, so no
/// setting can invert tones or erase detail.
double ShadowCurve(double v, double shadows, double clarity);

/// Soft-knee contrast: takes and returns a tone in 0..1.
///
/// `contrast` is the interface slider (0..200; 100 = neutral). The shape is a
/// power S-curve: `0.5*(2v)^c` in the lower half and its mirror in the upper
/// half. It keeps the same midtone slope, passes exactly through 0, 0.5 and 1,
/// and compresses the ends instead of clipping them, so no tone collapses and
/// the shadow curve still has detail to lift.
double ApplyContrast(double v, double contrast);

/// Builds the GDI gamma ramp: 256 red, 256 green, 256 blue entries.
void BuildRamp(const Adjustments& a, WORD ramp[768]);
void IdentityRamp(WORD ramp[768]);

/// 5x5 matrices in Magnification API form (row vector [r g b a 1] * M).
struct Mat5 {
    float m[25];
    static Mat5 Identity();
    static Mat5 Saturation(double s);
    static Mat5 Hue(double degrees);
    static Mat5 Invert();

    /// Per-channel gain plus a common offset: out = in * gain + offset.
    ///
    /// Provides brightness, contrast and white balance through the color matrix
    /// when the gamma ramp does not apply, as on displays with HDR enabled where
    /// SetDeviceGammaRamp reports success without changing a pixel.
    static Mat5 Levels(double rGain, double gGain, double bGain, double offset);

    /// Brightness/contrast/temperature matrix equivalent to what the gamma ramp
    /// would produce for these adjustments. Reproduces neither gamma nor the
    /// shadow curve: both are curves, and a 5x5 matrix is a linear transform.
    static Mat5 FromAdjustments(const Adjustments& a);

    Mat5 operator*(const Mat5& o) const;
    bool NearlyEquals(const Mat5& o, float tol = 0.0005f) const;
};

// Monitors

/// Physical panel identity, read from the EDID.
///
/// The EDID is the only description the monitor publishes about itself. The
/// name Windows shows identifies nothing, and the device path carries the
/// instance path, which changes when the cable moves to another port.
struct EdidInfo {
    bool         valid = false;
    std::wstring manufacturer;  ///< 3-letter PnP code, e.g. "DEL", "AUO"
    unsigned     product = 0;   ///< model code
    unsigned     serial = 0;    ///< numeric serial (0 when the panel publishes none)
    std::wstring serialText;    ///< serial as text, from descriptor 0xFF
    std::wstring modelName;     ///< model name, from descriptor 0xFC
    int          year = 0;      ///< year of manufacture
    bool         digital = false;
    /// Area of the primaries triangle in the xy diagram. sRGB is about 0.112.
    double       gamutArea = 0;
    /// Gamut clearly wider than sRGB, which explains the exaggerated saturation
    /// of panels that advertise wide coverage without offering an sRGB mode.
    bool         wideGamut = false;
};

/// Area of the sRGB primaries triangle, for comparison with the panel's.
constexpr double kSrgbGamutArea = 0.11205;

/// Parses an EDID block. Accepts 128 bytes (base block) or more.
///
/// Returns false when the header or the checksum does not match; a generic
/// monitor often reports a truncated or zeroed block, and falling back to the
/// legacy identity is preferable to inventing a serial.
bool ParseEdid(const unsigned char* data, size_t size, EdidInfo* out);

/// Extracts the VCP codes from an MCCS capability string.
///
/// The monitor answers in this form:
///   (prot(monitor)type(lcd)model(X)cmds(01 02 03)vcp(02 10 12 14(01 05 08) 16 18 1A))
/// Numbers inside a nested parenthesis are the VALUES that code accepts, not
/// codes, so nesting depth is what separates the two. Returns empty when there
/// is no vcp(...) section, which is common on low-end panels.
std::vector<unsigned char> ParseVcpCodes(const std::string& caps);

/// A VCP feature together with the values the monitor declares it accepts.
///
/// The list matters: a monitor with two inputs must not be offered five in the
/// interface. Empty means a continuous feature, such as volume, where any value
/// in range is valid.
struct VcpFeature {
    unsigned char code = 0;
    std::vector<unsigned char> values;
};

/// Same scan as ParseVcpCodes, but keeping what follows each code in
/// parentheses: `60(01 03 0F)` means input 0x60 accepts the values 01, 03 and 0F.
std::vector<VcpFeature> ParseVcpFeatures(const std::string& caps);

/// Human-readable name of a VCP feature VALUE ("HDMI 1", "Ligado").
/// Empty when no name is known, in which case the interface shows the number.
std::wstring VcpValueName(unsigned char code, unsigned char value);

/// Extra features the interface exposes per monitor, beyond brightness and
/// contrast. These exist on common panels and warrant a control of their own.
constexpr unsigned char kVcpColorPreset = 0x14;
constexpr unsigned char kVcpInputSource = 0x60;
constexpr unsigned char kVcpVolume      = 0x62;
constexpr unsigned char kVcpPowerMode   = 0xD6;

/// Semantic classification of a failure returned by the Windows DDC/CI APIs.
/// The retry decision must not depend on the localized GetLastError text.
enum class DdcErrorKind {
    None,
    Unsupported,   ///< the monitor or bus reported the request as not implemented
    Transient,     ///< framing, checksum, arbitration or timeout; worth retrying
    Unavailable,   ///< the handle died or the monitor no longer exists
    Permanent,     ///< unknown or permanent failure; retrying only makes it worse
};

DdcErrorKind ClassifyDdcError(unsigned long error);
bool DdcErrorCanRetry(unsigned long error);

/// Checks the ceiling before reserving a batch: keeps 39 + 5 from crossing a
/// 40-command limit and guards against overflow and negative inputs.
bool DdcWriteBatchFits(int alreadyUsed, int planned, int limit);

/// Converts a raw DDC/CI value, in the range the monitor declared, to 0..100.
///
/// The value is clamped to the range deliberately: a panel's reply is untrusted
/// input. Monitors exist that report "current 255, maximum 100", that always
/// return zero, or that answer below their own minimum, and an unclamped
/// subtraction of two DWORDs would wrap and be stored as the user's original value.
///
/// Returns -1 when the range itself is meaningless (maximum <= minimum).
int DdcRawToPercent(unsigned long raw, unsigned long lo, unsigned long hi);

/// Manufacturer name from the 3-letter EDID PnP code ("SAM", "DEL").
///
/// The codes are assigned by the UEFI PNP registry; only the handful that
/// appears on real monitors is covered here, since the full registry holds
/// thousands of mostly non-display devices. An unknown code is returned
/// unchanged, which is still a useful identification.
std::wstring MonitorVendorName(const std::wstring& pnpId);

/// Name of a standard MCCS VCP code ("0x10" -> "Brilho").
/// Returns empty for non-standard codes, which are mostly vendor specific and
/// better left unnamed than guessed.
std::wstring VcpFeatureName(unsigned char code);

struct MonitorTarget {
    std::wstring key;           ///< key that stays stable across restarts
    /// Legacy-format identity (PnP#instance-path). Kept only to migrate settings
    /// saved before the key started being derived from the EDID.
    std::wstring legacyKey;
    /// Physical instance/port reported by PnP. The operational identity: two
    /// identical panels without a serial remain distinct.
    std::wstring connectionKey;
    /// Manufacturer + product + serial from the EDID. Recognises the same panel
    /// on another port, but is unique only when a reliable serial exists.
    std::wstring modelKey;
    std::wstring deviceName;    ///< \\.\DISPLAY1 — used by the gamma ramp
    std::wstring friendlyName;  ///< human-readable name
    HMONITOR     handle = nullptr;
    RECT         bounds{};
    bool         isPrimary = false;
    /// Built-in panel (laptop, all-in-one). This, and not `isPrimary`, is the
    /// target of the WMI backlight path: on a docked laptop the external monitor
    /// can be the primary one.
    bool         isInternal = false;
    /// Advanced color (HDR) enabled on this display.
    ///
    /// Matters because with HDR enabled SetDeviceGammaRamp RETURNS SUCCESS and
    /// does nothing.
    ///
    /// True only for actual HDR. Automatic Color Management sets the same
    /// `advancedColorEnabled` bit of the legacy query while the display is in
    /// SDR, where the gamma ramp still applies, so the query that reports the
    /// active color mode is the one used.
    bool         isHdr = false;
    bool         hdrCapable = false;
    /// Canonical device path as QueryDisplayConfig returns it, without the
    /// #{guid} suffix: \\?\DISPLAY#DEL4093#5&ab12&0&UID4353.
    ///
    /// This is the identity Windows uses for the SAME panel across its modern
    /// APIs, and the only meeting point with WMI, whose InstanceName carries the
    /// same fields with different punctuation (see DevicePathFromWmiInstance).
    std::wstring devicePath;
    /// Address of this display in the video topology. Advanced color is
    /// addressed by (adapter, target), so the pair must be stored to query or
    /// set the SDR white level of a single display.
    LUID         pathAdapterId{};
    UINT32       pathTargetId = 0;
    bool         hasPathInfo = false;
    EdidInfo     edid;
    /// GPU that actually drives this display, and its human-readable name.
    ///
    /// Matters on hybrid laptops, where the panel is usually attached to the
    /// integrated GPU even when a discrete one is present: the vendor API would
    /// otherwise accept a vibrance command and report success with no change on
    /// screen, a silent failure.
    unsigned     gpuVendorId = 0;   ///< PCI ID: 0x8086 Intel, 0x10DE NVIDIA, 0x1002 AMD
    std::wstring adapterName;
};

/// PCI IDs of the GPU vendors of interest.
constexpr unsigned kVendorIntel  = 0x8086;
constexpr unsigned kVendorNvidia = 0x10DE;
constexpr unsigned kVendorAmd    = 0x1002;

/// Short vendor name from the PCI ID ("Intel", "NVIDIA", "AMD").
const wchar_t* GpuVendorName(unsigned vendorId);

/// Converts a WMI InstanceName into the same canonical form QueryDisplayConfig
/// returns in `MonitorTarget::devicePath`.
///
///   WMI:    DISPLAY\BOE0900\4&1a2b3c&0&UID111_0
///   output: \\?\DISPLAY#BOE0900#4&1a2b3c&0&UID111
///
/// Both carry the same three fields — enumerator, EDID ID and PnP instance — in
/// different punctuation, so normalising one reduces matching the two APIs to a
/// string comparison. Required on laptops with TWO built-in panels, where
/// `isInternal` alone does not identify which panel WMI is driving. Returns an
/// empty string when the input lacks the three fields.
std::wstring DevicePathFromWmiInstance(const std::wstring& instanceName);

/// Advanced color (HDR) per display.
///
/// On a display with HDR enabled the gamma ramp does not apply: Windows accepts
/// SetDeviceGammaRamp, reports success and ignores the result. The SDR white
/// level is the only brightness control that still works there. Unlike the color
/// matrix it acts PER DISPLAY and reaches exclusive fullscreen games.
namespace hdr {

/// Range Windows accepts for the SDR white level.
constexpr int kMinWhiteNits = 80;    ///< 80 nits = the default, "100%" on the slider
constexpr int kMaxWhiteNits = 480;

/// Reads this display's SDR white level, in nits. 0 when unavailable.
int ReadWhiteNits(const MonitorTarget& m);
/// Sets this display's SDR white level. `nits` is clamped to the usable range.
bool WriteWhiteNits(const MonitorTarget& m, int nits);

}  // namespace hdr

namespace monitors {
/// Re-enumerates. Returns true when the list changed.
bool Refresh();
const std::vector<MonitorTarget>& All();
const MonitorTarget* Primary();
const MonitorTarget* ByKey(const std::wstring& key);

/// Disambiguates duplicate keys by appending each display's PnP instance.
///
/// Some panels ship with the SAME serial in the EDID, so two units of one model
/// would produce the same key and one unit's adjustments would leak into the
/// other. Exposed here because it is testable without hardware.
void DisambiguateDuplicateKeys(std::vector<MonitorTarget>* list);

/// Rewrites per-monitor keys stored in the legacy format to the current key.
///
/// The key is now derived from the EDID; without this migration a stored
/// per-monitor override would silently stop matching any live monitor after an
/// update. Returns how many entries were renamed.
int MigrateKeys(Config* c);
}  // namespace monitors

}  // namespace zdisplay
