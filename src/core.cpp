#include "core.h"

#include <fstream>
#include <limits>
#include <sstream>

namespace zdisplay {

// Color adjustments

static bool Near(double a, double b) { return std::fabs(a - b) < 0.01; }

void Adjustments::ChannelGains(double* r, double* g, double* b) const {
    double gr = Clamp(redGain,   0.0, 100.0) / 100.0;
    double gg = Clamp(greenGain, 0.0, 100.0) / 100.0;
    double gb = Clamp(blueGain,  0.0, 100.0) / 100.0;

    // Blue block cuts blue hard and green slightly: cutting blue alone turns the
    // image visibly purple at high values, while pulling some green along with it
    // produces amber. At most 15% of blue survives, since zeroing the channel
    // would erase every distinction between blue tones and force the light floor
    // to undo the whole adjustment.
    const double k = Clamp(blueBlock, 0.0, 100.0) / 100.0;
    if (k > 0.0) {
        gb *= 1.0 - 0.85 * k;
        gg *= 1.0 - 0.15 * k;
    }

    *r = gr; *g = gg; *b = gb;
}

bool Adjustments::GammaNeutral() const {
    return Near(brightness, 100) && Near(contrast, 100) && Near(gamma, 1.0) &&
           Near(temperature, 6500) && Near(redGain, 100) &&
           Near(greenGain, 100) && Near(blueGain, 100) &&
           Near(blueBlock, 0) && Near(shadows, 0) && Near(clarity, 0);
}

bool Adjustments::MatrixNeutral() const {
    return Near(saturation, 100) && Near(hue, 0) && !invert;
}

bool Adjustments::Neutral() const {
    return GammaNeutral() && MatrixNeutral() && Near(vibrance, 0) &&
           Near(dim, 0) && hwBrightness < 0 && hwContrast < 0;
}

void Adjustments::Sanitize() {
    // Anything that becomes NaN or infinite reverts to neutral: a single
    // malformed line in the file must not be able to blank the display.
    const auto fix = [](double& v, double neutral, double lo, double hi) {
        if (!(v == v) || v > 1e12 || v < -1e12) v = neutral;   // NaN/infinite
        v = Clamp(v, lo, hi);
    };

    fix(brightness,  100,  10,  150);
    fix(contrast,    100,   0,  200);
    fix(gamma,       1.0, 0.3,  3.0);
    fix(temperature, 6500, 1500, 10000);
    fix(redGain,     100,  50,  100);
    fix(greenGain,   100,  50,  100);
    fix(blueGain,    100,  50,  100);
    fix(blueBlock,     0,   0,  100);
    fix(shadows,       0,   0,  100);
    fix(clarity,       0,   0,  100);
    fix(saturation,  100,   0,  200);
    fix(vibrance,      0,   0,  100);
    fix(hue,           0, -180, 180);
    fix(dim,           0,   0,   90);

    // -1 means "not managed"; any other value is clamped to 0..100.
    if (hwBrightness >= 0) fix(hwBrightness, -1, 0, 100); else hwBrightness = -1;
    if (hwContrast   >= 0) fix(hwContrast,   -1, 0, 100); else hwContrast = -1;

    // Absolute light floor. Relaxes, in order, whatever darkens most and costs
    // least in user intent: the overlay veil, hardware brightness, software
    // brightness, the white gains, temperature, gamma and finally contrast.
    //
    // Every field EffectiveLuminance measures must appear here, otherwise a
    // profile can sit below the floor with nothing left to relax.
    for (int guard = 0; guard < 256 && EffectiveLuminance() < kFloorLuminance; ++guard) {
        if (dim > 0)                                      dim = (std::max)(0.0, dim - 5.0);
        else if (hwBrightness >= 0 && hwBrightness < 100) hwBrightness = (std::min)(100.0, hwBrightness + 5.0);
        else if (brightness < 100)                        brightness = (std::min)(100.0, brightness + 5.0);
        else if (redGain < 100 || greenGain < 100 || blueGain < 100) {
            redGain   = (std::min)(100.0, redGain + 5.0);
            greenGain = (std::min)(100.0, greenGain + 5.0);
            blueGain  = (std::min)(100.0, blueGain + 5.0);
        }
        else if (blueBlock > 0)                           blueBlock = (std::max)(0.0, blueBlock - 5.0);
        else if (temperature < 6500)                      temperature = (std::min)(6500.0, temperature + 250.0);
        else if (gamma < 1.0)                             gamma = (std::min)(1.0, gamma + 0.05);
        else if (contrast < 100)                          contrast = (std::min)(100.0, contrast + 5.0);
        else break;
    }

    // Final net: if anything is still below the floor, every light-affecting
    // field returns to neutral. The guarantee that no input path can produce
    // less than 8% light must not depend on the relaxation above having
    // anticipated every possible combination.
    if (EffectiveLuminance() < kFloorLuminance) {
        brightness = 100; contrast = 100; gamma = 1.0; temperature = 6500;
        redGain = greenGain = blueGain = 100;
        blueBlock = 0;
        shadows = 0; clarity = 0; dim = 0;
        if (hwBrightness >= 0) hwBrightness = 100;
    }
}

double Adjustments::EffectiveLuminance() const {
    // Measured over the SAME curve BuildRamp produces rather than a parallel
    // formula, so every one of the nine factors that affect light is accounted
    // for. 33 samples give ample resolution for an average and stay cheap enough
    // to run inside the relaxation loop in Sanitize.
    double tr, tg, tb;
    TemperatureToRgb(temperature, &tr, &tg, &tb);

    const double br = Clamp(brightness, 0.0, 150.0) / 100.0;
    const double gm = Clamp(gamma,      0.30, 3.00);
    double gr = 1, gg = 1, gb = 1;
    ChannelGains(&gr, &gg, &gb);

    constexpr int kSteps = 32;
    double sum = 0.0;
    for (int i = 0; i <= kSteps; ++i) {
        // Same order as BuildRamp: gamma, contrast, shadows, brightness.
        double v = (double)i / (double)kSteps;
        v = std::pow(v, 1.0 / gm);
        v = ApplyContrast(v, contrast);
        v = Clamp(v, 0.0, 1.0);
        v = ShadowCurve(v, shadows, clarity);
        v = Clamp(v * br, 0.0, 1.0);
        const double r = Clamp(v * tr * gr, 0.0, 1.0);
        const double g = Clamp(v * tg * gg, 0.0, 1.0);
        const double b = Clamp(v * tb * gb, 0.0, 1.0);
        // Rec.709 weights, the correct ones for sRGB content.
        sum += 0.2126 * r + 0.7152 * g + 0.0722 * b;
    }

    // The neutral ramp averages 0.5, so dividing by 0.5 makes an untouched
    // display measure exactly 1.0.
    const double curve = Clamp((sum / (kSteps + 1)) / 0.5, 0.0, 2.0);

    const double veil = 1.0 - Clamp(dim, 0.0, 90.0) / 100.0;
    // Hardware brightness counts only while it is managed.
    const double hw = hwBrightness >= 0 ? Clamp(hwBrightness, 0.0, 100.0) / 100.0 : 1.0;
    // Hardware alone never blanks the display completely, so it weighs less.
    return curve * veil * (0.35 + 0.65 * hw);
}

Adjustments Adjustments::Blend(const Adjustments& a, const Adjustments& b, double t) {
    t = Clamp(t, 0.0, 1.0);
    Adjustments r;
    r.brightness  = Lerp(a.brightness,  b.brightness,  t);
    r.contrast    = Lerp(a.contrast,    b.contrast,    t);
    r.gamma       = Lerp(a.gamma,       b.gamma,       t);
    // Temperature is interpolated in mired (10^6/K), not Kelvin: mired is the
    // scale on which equal steps look like equal changes, so a 6500 -> 3400
    // transition moves evenly instead of racing through its sensitive part.
    {
        const double ma = 1e6 / Clamp(a.temperature, 1000.0, 20000.0);
        const double mb = 1e6 / Clamp(b.temperature, 1000.0, 20000.0);
        const double m  = Lerp(ma, mb, t);
        r.temperature = m > 1e-6 ? 1e6 / m : b.temperature;
    }
    r.redGain     = Lerp(a.redGain,     b.redGain,     t);
    r.greenGain   = Lerp(a.greenGain,   b.greenGain,   t);
    r.blueGain    = Lerp(a.blueGain,    b.blueGain,    t);
    r.blueBlock   = Lerp(a.blueBlock,   b.blueBlock,   t);
    r.shadows     = Lerp(a.shadows,     b.shadows,     t);
    r.clarity     = Lerp(a.clarity,     b.clarity,     t);
    r.saturation  = Lerp(a.saturation,  b.saturation,  t);
    r.vibrance    = Lerp(a.vibrance,    b.vibrance,    t);
    r.hue         = Lerp(a.hue,         b.hue,         t);
    r.dim         = Lerp(a.dim,         b.dim,         t);
    // Non-interpolable values snap straight to the destination.
    r.invert       = b.invert;
    r.hwBrightness = b.hwBrightness;
    r.hwContrast   = b.hwContrast;
    return r;
}

// Profiles

const Adjustments& Profile::For(const std::wstring& monitorKey) const {
    auto it = perMonitor.find(monitorKey);
    return it != perMonitor.end() ? it->second : global;
}

Adjustments* Profile::Find(const std::wstring& monitorKey) {
    auto it = perMonitor.find(monitorKey);
    return it != perMonitor.end() ? &it->second : nullptr;
}

Adjustments& Profile::Ensure(const std::wstring& monitorKey) {
    auto it = perMonitor.find(monitorKey);
    if (it == perMonitor.end()) it = perMonitor.emplace(monitorKey, global).first;
    return it->second;
}

void SanitizeProfile(Profile* p) {
    if (!p) return;
    p->global.Sanitize();
    for (auto& kv : p->perMonitor) kv.second.Sanitize();
    p->transitionMs = Clamp(p->transitionMs, 0, 10000);
    if ((int)p->satEngine < 0 || (int)p->satEngine > 3) p->satEngine = SatEngine::Auto;
}

bool AppRule::Matches(const std::wstring& processName) const {
    if (!enabled || process.empty() || processName.empty()) return false;
    std::wstring pat = Trim(process);
    if (pat.size() > 4 && IEquals(pat.substr(pat.size() - 4), L".exe"))
        pat = pat.substr(0, pat.size() - 4);
    if (pat.find(L'*') != std::wstring::npos || pat.find(L'?') != std::wstring::npos)
        return WildcardMatch(pat, processName);
    return IEquals(pat, processName);
}

int AppRule::Specificity() const {
    std::wstring pat = Trim(process);
    if (pat.size() > 4 && IEquals(pat.substr(pat.size() - 4), L".exe"))
        pat.resize(pat.size() - 4);

    int literal = 0;
    bool wildcard = false;
    for (wchar_t c : pat) {
        if (c == L'*' || c == L'?') wildcard = true;
        else ++literal;
    }
    // An exact rule is always more specific than any wildcard; among wildcards,
    // more literal characters means a narrower target.
    return (wildcard ? 0 : 10000) + literal;
}

static bool ParseHm(const std::wstring& s, int* minutes) {
    int h = 0, m = 0;
    if (swscanf_s(s.c_str(), L"%d:%d", &h, &m) != 2) return false;
    if (h < 0 || h > 23 || m < 0 || m > 59) return false;
    *minutes = h * 60 + m;
    return true;
}

// Solar times

namespace {

constexpr double kPi = 3.14159265358979323846;
double Rad(double deg) { return deg * kPi / 180.0; }
double Deg(double rad) { return rad * 180.0 / kPi; }

/// Julian day at 0h UT for the given civil date (Gregorian calendar).
double JulianDay(int y, int m, int d) {
    if (m <= 2) { y -= 1; m += 12; }
    const int a = y / 100;
    const int b = 2 - a + a / 4;
    return std::floor(365.25 * (y + 4716)) + std::floor(30.6001 * (m + 1)) + d + b - 1524.5;
}

}  // namespace

bool SunTimes(int year, int month, int day, double latDeg, double lonDeg,
              double tzOffsetHours, int* sunriseMin, int* sunsetMin) {
    if (!sunriseMin || !sunsetMin) return false;
    if (!(latDeg >= -90.0 && latDeg <= 90.0)) return false;     // also rejects NaN
    if (!(lonDeg >= -180.0 && lonDeg <= 180.0)) return false;
    if (!(tzOffsetHours >= -14.0 && tzOffsetHours <= 14.0)) return false;
    if (month < 1 || month > 12 || day < 1 || day > 31) return false;

    // NOAA algorithm, referenced to solar noon. Using the Julian day at 0h UT
    // instead of local noon costs under a minute of error, well below the useful
    // resolution for switching a display profile.
    const double t = (JulianDay(year, month, day) - 2451545.0) / 36525.0;

    const double meanLong = std::fmod(280.46646 + t * (36000.76983 + t * 0.0003032), 360.0);
    const double meanAnom = 357.52911 + t * (35999.05029 - 0.0001537 * t);
    const double eccent   = 0.016708634 - t * (0.000042037 + 0.0000001267 * t);

    // Equation of the center: Earth's orbit is an ellipse, not a circle.
    const double center = std::sin(Rad(meanAnom))     * (1.914602 - t * (0.004817 + 0.000014 * t)) +
                          std::sin(Rad(2 * meanAnom)) * (0.019993 - 0.000101 * t) +
                          std::sin(Rad(3 * meanAnom)) * 0.000289;

    const double omega   = 125.04 - 1934.136 * t;
    const double appLong = meanLong + center - 0.00569 - 0.00478 * std::sin(Rad(omega));

    const double eps0 = 23.0 + (26.0 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60.0) / 60.0;
    const double eps  = eps0 + 0.00256 * std::cos(Rad(omega));

    const double declin = Deg(std::asin(std::sin(Rad(eps)) * std::sin(Rad(appLong))));

    // Equation of time, in minutes: the difference between the true Sun and a
    // mean Sun.
    const double vy = std::tan(Rad(eps / 2)) * std::tan(Rad(eps / 2));
    const double eqTime = 4.0 * Deg(
        vy * std::sin(2 * Rad(meanLong)) - 2 * eccent * std::sin(Rad(meanAnom)) +
        4 * eccent * vy * std::sin(Rad(meanAnom)) * std::cos(2 * Rad(meanLong)) -
        0.5 * vy * vy * std::sin(4 * Rad(meanLong)) -
        1.25 * eccent * eccent * std::sin(2 * Rad(meanAnom)));

    // 90.833 degrees is the zenith of the visible horizon: 90 geometric plus
    // atmospheric refraction and the apparent radius of the solar disc.
    const double cosH = (std::cos(Rad(90.833)) - std::sin(Rad(latDeg)) * std::sin(Rad(declin))) /
                        (std::cos(Rad(latDeg)) * std::cos(Rad(declin)));
    if (!(cosH >= -1.0 && cosH <= 1.0)) return false;   // midnight sun or polar night

    const double hourAngle = Deg(std::acos(cosH));
    const double solarNoon = 720.0 - 4.0 * lonDeg - eqTime + tzOffsetHours * 60.0;

    *sunriseMin = (int)llround(solarNoon - 4.0 * hourAngle);
    *sunsetMin  = (int)llround(solarNoon + 4.0 * hourAngle);
    return true;
}

double LocalTimeZoneHours() {
    TIME_ZONE_INFORMATION tz{};
    const DWORD mode = ::GetTimeZoneInformation(&tz);
    if (mode == TIME_ZONE_ID_INVALID) return 0.0;
    // Bias is defined as UTC = local + bias, in minutes, so the sign flips here.
    LONG bias = tz.Bias;
    if (mode == TIME_ZONE_ID_DAYLIGHT) bias += tz.DaylightBias;
    else if (mode == TIME_ZONE_ID_STANDARD) bias += tz.StandardBias;
    return -(double)bias / 60.0;
}

namespace {

bool ParseSolarRule(const std::wstring& text, std::wstring* word, int* offset) {
    const std::wstring s = Trim(text);
    if (s.empty()) return false;

    std::wstring name = s;
    int delta = 0;
    const size_t sign = s.find_first_of(L"+-");
    if (sign != std::wstring::npos) {
        if (sign == 0) return false;
        name = Trim(s.substr(0, sign));
        const std::wstring digits = Trim(s.substr(sign + 1));
        if (digits.empty()) return false;

        int value = 0;
        for (wchar_t c : digits) {
            if (c < L'0' || c > L'9') return false;
            value = value * 10 + (c - L'0');
            if (value > 720) return false;
        }
        delta = s[sign] == L'-' ? -value : value;
    }

    const bool solarName = IEquals(name, L"nascer") || IEquals(name, L"sunrise") ||
                           IEquals(name, L"por") || IEquals(name, L"pôr") ||
                           IEquals(name, L"sunset");
    if (!solarName) return false;
    if (word) *word = name;
    if (offset) *offset = delta;
    return true;
}

}  // namespace

bool IsValidRuleTime(const std::wstring& text) {
    const std::wstring s = Trim(text);
    if (s.empty()) return false;

    int hm = 0;
    if (ParseHm(s, &hm)) return true;
    return ParseSolarRule(s, nullptr, nullptr);
}

int ResolveRuleTime(const std::wstring& text, const SYSTEMTIME& now,
                    const SolarContext& solar) {
    const std::wstring s = Trim(text);
    if (s.empty()) return -1;

    // Plain clock time.
    int hm = 0;
    if (ParseHm(s, &hm)) return hm;

    // Solar time, with optional offset: "por", "nascer+30", "por-45".
    std::wstring word;
    int offset = 0;
    if (!ParseSolarRule(s, &word, &offset)) return -1;

    const bool sunrise = IEquals(word, L"nascer") || IEquals(word, L"sunrise");
    if (!solar.valid) return -1;

    int rise = 0, set = 0;
    if (!SunTimes(now.wYear, now.wMonth, now.wDay,
                  solar.latitude, solar.longitude, solar.tzHours, &rise, &set))
        return -1;

    // The offset can push outside the day, so wrap back to 0..1439 to keep range
    // comparisons valid.
    int minutes = (sunrise ? rise : set) + offset;
    minutes %= 1440;
    if (minutes < 0) minutes += 1440;
    return minutes;
}

// Vision care

void Vision::Sanitize() {
    // The comfort layer never cools the display, so values above 6500 K are
    // ignored. Night must not be cooler than day either, or the transition would
    // run in the opposite direction.
    dayTemperature   = Clamp(dayTemperature, 1000.0, 6500.0);
    nightTemperature = Clamp(nightTemperature, 1000.0, dayTemperature);
    nightBrightness  = Clamp(nightBrightness, 20.0, 100.0);
    transitionMinutes = Clamp(transitionMinutes, 0, 240);
    breakMinutes      = Clamp(breakMinutes, 0, 240);
    if (!IsValidRuleTime(dayStart))   dayStart = L"nascer";
    if (!IsValidRuleTime(nightStart)) nightStart = L"por";
}

namespace {

/// Minutes moving forward around the 24 h circle, from `from` to `to`.
double MinutesForward(int from, int to) {
    int d = to - from;
    while (d < 0) d += 1440;
    return (double)d;
}

/// Smooth step: no corners at the ends, so the transition has no visible snap
/// at its start or finish.
double SmoothStep(double x) {
    x = Clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

}  // namespace

bool ResolveVisionTimes(const SYSTEMTIME& now, const Vision& v,
                        const SolarContext& solar,
                        int* dayStartMin, int* nightStartMin,
                        bool* usedFixedFallback) {
    if (!dayStartMin || !nightStartMin) return false;

    Vision cfg = v;
    cfg.Sanitize();
    int day = ResolveRuleTime(cfg.dayStart, now, solar);
    int night = ResolveRuleTime(cfg.nightStart, now, solar);
    bool fallback = false;

    // Only solar expressions get the fallback. An invalid clock time is never
    // silently turned into a different time; Sanitize already replaces it with
    // the solar default beforehand.
    if (day < 0 && ParseSolarRule(cfg.dayStart, nullptr, nullptr)) {
        day = 7 * 60;
        fallback = true;
    }
    if (night < 0 && ParseSolarRule(cfg.nightStart, nullptr, nullptr)) {
        night = 20 * 60;
        fallback = true;
    }

    if (day < 0 || night < 0 || day == night) return false;
    *dayStartMin = day;
    *nightStartMin = night;
    if (usedFixedFallback) *usedFixedFallback = fallback;
    return true;
}

double NightFraction(const SYSTEMTIME& now, const Vision& v, const SolarContext& solar) {
    if (!v.enabled) return 0.0;

    int dayAt = 0, nightAt = 0;
    if (!ResolveVisionTimes(now, v, solar, &dayAt, &nightAt)) return 0.0;

    const int t = now.wHour * 60 + now.wMinute;
    const double intoNight = MinutesForward(nightAt, t);
    const double nightLen  = MinutesForward(nightAt, dayAt);

    // Zero means a genuinely immediate switch, with no residual blend at the
    // exact instant of the boundary.
    if (v.transitionMinutes <= 0)
        return intoNight < nightLen ? 1.0 : 0.0;

    const double w = (double)Clamp(v.transitionMinutes, 1, 240);
    const double half = w / 2.0;

    if (intoNight <= nightLen) {
        // Inside night: rises on entry, falls on exit, and the smaller of the two
        // wins, which keeps the curve correct even when night is shorter than the
        // transition itself.
        const double up   = SmoothStep((intoNight + half) / w);
        const double down = SmoothStep((nightLen - intoNight + half) / w);
        return up < down ? up : down;
    }

    const double intoDay = intoNight - nightLen;
    const double dayLen  = 1440.0 - nightLen;
    const double up   = SmoothStep((intoDay + half) / w);
    const double down = SmoothStep((dayLen - intoDay + half) / w);
    return 1.0 - (up < down ? up : down);
}

Adjustments ApplyVision(const Adjustments& base, double nightFraction, const Vision& v) {
    if (!v.enabled) return base;
    const double n = Clamp(nightFraction, 0.0, 1.0);

    Vision cfg = v;
    cfg.Sanitize();

    Adjustments out = base;

    // Temperature is interpolated in mired, not Kelvin: in Kelvin the midpoint of
    // the transition falls far from the visually intermediate point.
    const double dayMired   = 1e6 / cfg.dayTemperature;
    const double nightMired = 1e6 / cfg.nightTemperature;
    const double target = 1e6 / (dayMired + (nightMired - dayMired) * n);

    // If the profile already asks for something warmer it wins: this layer only
    // pulls toward comfort, never against the user's choice.
    out.temperature = base.temperature < target ? base.temperature : target;

    const double factor = 1.0 + (cfg.nightBrightness / 100.0 - 1.0) * n;
    out.brightness = base.brightness * factor;

    out.Sanitize();
    return out;
}

bool ScheduleRule::Matches(const SYSTEMTIME& now, const SolarContext& solar) const {
    if (!enabled) return false;
    const int s = ResolveRuleTime(start, now, solar);
    const int e = ResolveRuleTime(end, now, solar);
    if (s < 0 || e < 0) return false;
    const int t = now.wHour * 60 + now.wMinute;
    // Range that crosses midnight.
    return s <= e ? (t >= s && t < e) : (t >= s || t < e);
}

// Configuration

Profile* Config::Find(const std::wstring& name) {
    for (auto& p : profiles) if (IEquals(p.name, name)) return &p;
    return nullptr;
}

const Profile* Config::Find(const std::wstring& name) const {
    for (auto& p : profiles) if (IEquals(p.name, name)) return &p;
    return nullptr;
}

SolarContext Config::Solar() const {
    SolarContext s;
    if (!HasLocation()) return s;    // valid stays false: solar rules never match
    s.latitude = latitude;
    s.longitude = longitude;
    s.tzHours = LocalTimeZoneHours();
    s.valid = true;
    return s;
}

Profile* Config::Default() {
    if (Profile* p = Find(defaultProfile)) return p;
    return profiles.empty() ? nullptr : &profiles[0];
}

std::wstring SanitizeProfileName(const std::wstring& name) {
    // "|monitor:" separates the INI section header, so a profile whose name
    // contains it would be written as [perfil:Jogo|monitor:X] and read back as
    // another profile's per-monitor override. Brackets break the header the same
    // way.
    std::wstring s = Trim(name);
    for (const wchar_t* bad : {L"|monitor:", L"|", L"[", L"]"}) {
        size_t pos;
        while ((pos = s.find(bad)) != std::wstring::npos)
            s.erase(pos, wcslen(bad));
    }
    s = Trim(s);
    return s;
}

std::wstring Config::UniqueName(const std::wstring& base) const {
    const std::wstring clean = SanitizeProfileName(base);
    const std::wstring root = clean.empty() ? std::wstring(L"Perfil") : clean;
    if (!Find(root)) return root;
    for (int i = 2; i < 1000; ++i) {
        std::wstring candidate = root + L" " + std::to_wstring(i);
        if (!Find(candidate)) return candidate;
    }
    return root + L" novo";
}

void Config::SeedDefaults() {
    profiles.clear();

    Profile padrao;
    padrao.name = L"Padrão";
    padrao.transitionMs = 300;
    profiles.push_back(padrao);

    Profile jogo;
    jogo.name = L"Jogo";
    jogo.transitionMs = 200;
    jogo.global.brightness = 105;
    jogo.global.contrast   = 108;
    jogo.global.saturation = 130;
    jogo.global.vibrance   = 55;
    jogo.global.gamma      = 1.05;
    jogo.global.shadows    = 40;
    jogo.global.clarity    = 30;
    profiles.push_back(jogo);

    // For dark maps: shadow lift near maximum, with high clarity so dark corners
    // gain light without turning into a grey blur.
    Profile competitivo;
    competitivo.name = L"Competitivo";
    competitivo.transitionMs = 150;
    competitivo.global.shadows    = 78;
    competitivo.global.clarity    = 65;
    competitivo.global.contrast   = 104;
    competitivo.global.saturation = 118;
    competitivo.global.vibrance   = 45;
    profiles.push_back(competitivo);

    Profile noite;
    noite.name = L"Noite";
    noite.transitionMs = 1500;
    noite.global.brightness  = 75;
    noite.global.temperature = 3400;
    noite.global.gamma       = 0.95;
    noite.global.saturation  = 95;
    profiles.push_back(noite);

    Profile filme;
    filme.name = L"Filme";
    filme.transitionMs = 600;
    filme.global.brightness = 95;
    filme.global.contrast   = 112;
    filme.global.saturation = 112;
    filme.global.gamma      = 1.08;
    profiles.push_back(filme);

    Profile leitura;
    leitura.name = L"Leitura";
    leitura.transitionMs = 800;
    leitura.global.brightness  = 82;
    leitura.global.temperature = 4800;
    leitura.global.saturation  = 90;
    leitura.global.contrast    = 96;
    profiles.push_back(leitura);

    defaultProfile = L"Padrão";

    ScheduleRule noturno;
    noturno.enabled = false;
    noturno.start = L"21:00";
    noturno.end = L"07:00";
    noturno.profile = L"Noite";
    scheduleRules.push_back(noturno);
}

// INI

namespace {

/// One INI section: name plus key/value pairs in read order.
struct IniSection {
    std::wstring name;
    std::vector<std::pair<std::wstring, std::wstring>> values;

    const std::wstring* Get(const wchar_t* key) const {
        for (auto& kv : values) if (IEquals(kv.first, key)) return &kv.second;
        return nullptr;
    }
    std::wstring Str(const wchar_t* key, const std::wstring& def = L"") const {
        const std::wstring* v = Get(key);
        return v ? *v : def;
    }
    double Num(const wchar_t* key, double def) const {
        const std::wstring* v = Get(key);
        double out = def;
        return (v && ParseDouble(*v, &out)) ? out : def;
    }
    int Int(const wchar_t* key, int def) const {
        const double value = Num(key, def);
        if (!std::isfinite(value)) return def;
        const double safe = Clamp(value,
                                  (double)(std::numeric_limits<int>::min)(),
                                  (double)(std::numeric_limits<int>::max)());
        return (int)llround(safe);
    }
    bool Bool(const wchar_t* key, bool def) const {
        const std::wstring* v = Get(key);
        if (!v) return def;
        std::wstring s = ToLower(Trim(*v));
        if (s == L"1" || s == L"true" || s == L"sim" || s == L"yes") return true;
        if (s == L"0" || s == L"false" || s == L"nao" || s == L"não" || s == L"no") return false;
        return def;
    }
};

std::vector<IniSection> ParseIni(const std::wstring& text) {
    std::vector<IniSection> sections;
    IniSection current;
    current.name = L"";

    std::wistringstream in(text);
    std::wstring line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        std::wstring t = Trim(line);
        if (t.empty() || t[0] == L';' || t[0] == L'#') continue;

        if (t.front() == L'[' && t.back() == L']') {
            if (!current.name.empty() || !current.values.empty())
                sections.push_back(current);
            current = IniSection{};
            current.name = Trim(t.substr(1, t.size() - 2));
            continue;
        }

        size_t eq = t.find(L'=');
        if (eq == std::wstring::npos) continue;
        current.values.emplace_back(Trim(t.substr(0, eq)), Trim(t.substr(eq + 1)));
    }
    if (!current.name.empty() || !current.values.empty()) sections.push_back(current);
    return sections;
}

void WriteAdjustments(std::wostringstream& out, const Adjustments& a) {
    out << L"brilho="       << FormatDouble(a.brightness)  << L"\r\n";
    out << L"contraste="    << FormatDouble(a.contrast)    << L"\r\n";
    out << L"gamma="        << FormatDouble(a.gamma, 3)    << L"\r\n";
    out << L"temperatura="  << FormatDouble(a.temperature, 0) << L"\r\n";
    out << L"ganhoR="       << FormatDouble(a.redGain)     << L"\r\n";
    out << L"ganhoG="       << FormatDouble(a.greenGain)   << L"\r\n";
    out << L"ganhoB="       << FormatDouble(a.blueGain)    << L"\r\n";
    out << L"bloqueioAzul=" << FormatDouble(a.blueBlock)   << L"\r\n";
    out << L"sombras="      << FormatDouble(a.shadows)     << L"\r\n";
    out << L"definicao="    << FormatDouble(a.clarity)     << L"\r\n";
    out << L"saturacao="    << FormatDouble(a.saturation)  << L"\r\n";
    out << L"vibrance="     << FormatDouble(a.vibrance)    << L"\r\n";
    out << L"matiz="        << FormatDouble(a.hue)         << L"\r\n";
    out << L"inverter="     << (a.invert ? L"1" : L"0")    << L"\r\n";
    out << L"escurecer="    << FormatDouble(a.dim)         << L"\r\n";
    out << L"brilhoHw="     << FormatDouble(a.hwBrightness) << L"\r\n";
    out << L"contrasteHw="  << FormatDouble(a.hwContrast)  << L"\r\n";
}

void ReadAdjustments(const IniSection& s, Adjustments* a) {
    a->brightness   = s.Num(L"brilho",      a->brightness);
    a->contrast     = s.Num(L"contraste",   a->contrast);
    a->gamma        = s.Num(L"gamma",       a->gamma);
    a->temperature  = s.Num(L"temperatura", a->temperature);
    a->redGain      = s.Num(L"ganhoR",      a->redGain);
    a->greenGain    = s.Num(L"ganhoG",      a->greenGain);
    a->blueGain     = s.Num(L"ganhoB",      a->blueGain);
    a->blueBlock    = s.Num(L"bloqueioAzul", a->blueBlock);
    a->shadows      = s.Num(L"sombras",     a->shadows);
    a->clarity      = s.Num(L"definicao",   a->clarity);
    a->saturation   = s.Num(L"saturacao",   a->saturation);
    a->vibrance     = s.Num(L"vibrance",    a->vibrance);
    a->hue          = s.Num(L"matiz",       a->hue);
    a->invert       = s.Bool(L"inverter",   a->invert);
    a->dim          = s.Num(L"escurecer",   a->dim);
    a->hwBrightness = s.Num(L"brilhoHw",    a->hwBrightness);
    a->hwContrast   = s.Num(L"contrasteHw", a->hwContrast);
}

bool ReadWholeFile(const std::wstring& path, std::string* out) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size;
    if (!::GetFileSizeEx(h, &size) || size.QuadPart > 8 * 1024 * 1024) {
        ::CloseHandle(h);
        return false;
    }
    out->resize((size_t)size.QuadPart);
    DWORD read = 0;
    bool ok = out->empty() ||
              (::ReadFile(h, &(*out)[0], (DWORD)out->size(), &read, nullptr) && read == out->size());
    ::CloseHandle(h);
    return ok;
}

bool WriteWholeFile(const std::wstring& path, const std::string& data) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = ::WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr) &&
              written == data.size();
    ::FlushFileBuffers(h);
    ::CloseHandle(h);
    return ok;
}

/// Replaces a file in a single step.
///
/// Writes to a temporary, flushes the bytes to disk and only then swaps, so a
/// power loss can never leave a truncated file in place. With `makeBak`, the
/// previous version is kept before the swap. Returns false if any step fails.
bool WriteWholeFileAtomic(const std::wstring& path, const std::string& data, bool makeBak) {
    const std::wstring tmp = path + L".tmp";
    if (!WriteWholeFile(tmp, data)) return false;
    if (makeBak) ::CopyFileW(path.c_str(), (path + L".bak").c_str(), FALSE);
    if (!::MoveFileExW(tmp.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ::DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

SatEngine ParseSatEngine(const std::wstring& s) {
    std::wstring v = ToLower(Trim(s));
    if (v == L"gpu") return SatEngine::Gpu;
    if (v == L"universal") return SatEngine::Universal;
    if (v == L"off" || v == L"desligado") return SatEngine::Off;
    return SatEngine::Auto;
}

const wchar_t* SatEngineName(SatEngine e) {
    switch (e) {
        case SatEngine::Gpu: return L"gpu";
        case SatEngine::Universal: return L"universal";
        case SatEngine::Off: return L"desligado";
        default: return L"auto";
    }
}

/// Decodes file bytes to text, recognizing the UTF-8 BOM and both UTF-16 BOMs.
///
/// Notepad's "Unicode" option writes UTF-16, which Utf8ToWide alone would turn
/// into garbage.
std::wstring DecodeText(const std::string& raw) {
    if (raw.size() >= 2) {
        const unsigned char b0 = (unsigned char)raw[0];
        const unsigned char b1 = (unsigned char)raw[1];

        // Both orders assemble the code units byte by byte rather than casting
        // the buffer to wchar_t*. The cast would read a char array through an
        // unrelated pointer type, which is undefined behaviour even where the
        // alignment happens to work out, as it does on Windows.
        if (b0 == 0xFF && b1 == 0xFE) {  // UTF-16 LE
            const size_t chars = (raw.size() - 2) / 2;
            std::wstring out(chars, L'\0');
            for (size_t i = 0; i < chars; ++i) {
                const unsigned char lo = (unsigned char)raw[2 + i * 2];
                const unsigned char hi = (unsigned char)raw[2 + i * 2 + 1];
                out[i] = (wchar_t)((hi << 8) | lo);
            }
            return out;
        }
        if (b0 == 0xFE && b1 == 0xFF) {  // UTF-16 BE: swap the bytes
            const size_t chars = (raw.size() - 2) / 2;
            std::wstring out(chars, L'\0');
            for (size_t i = 0; i < chars; ++i) {
                const unsigned char hi = (unsigned char)raw[2 + i * 2];
                const unsigned char lo = (unsigned char)raw[2 + i * 2 + 1];
                out[i] = (wchar_t)((hi << 8) | lo);
            }
            return out;
        }
    }

    if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF &&
        (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF)
        return Utf8ToWide(raw.substr(3));

    return Utf8ToWide(raw);
}

/// Fills the configuration from text. Returns false when the text yielded no
/// profile, which is the signal that the backup copy is worth trying.
bool ParseConfigText(const std::wstring& text, Config* cfg) {
    const auto sections = ParseIni(text);

    cfg->profiles.clear();
    cfg->appRules.clear();
    cfg->scheduleRules.clear();
    cfg->ddcMonitorModes.clear();

    for (const auto& s : sections) {
        const std::wstring lower = ToLower(s.name);

        if (lower == L"geral") {
            cfg->defaultProfile     = s.Str(L"perfilPadrao", cfg->defaultProfile);
            cfg->startWithWindows   = s.Bool(L"iniciarComWindows", cfg->startWithWindows);
            cfg->startMinimized     = s.Bool(L"iniciarMinimizado", cfg->startMinimized);
            cfg->enableAppRules     = s.Bool(L"regrasPorApp", cfg->enableAppRules);
            cfg->enableSchedule     = s.Bool(L"regrasPorHorario", cfg->enableSchedule);
            cfg->restoreOnExit      = s.Bool(L"restaurarAoSair", cfg->restoreOnExit);
            cfg->watchdogSeconds    = s.Int(L"reforcarSegundos", cfg->watchdogSeconds);
            cfg->enableVendorApis   = s.Bool(L"apisDoFabricante", cfg->enableVendorApis);
            cfg->enableMagnification= s.Bool(L"matrizUniversal", cfg->enableMagnification);
            cfg->enableDdcCi        = s.Bool(L"ddcci", cfg->enableDdcCi);
            cfg->enableBacklight    = s.Bool(L"backlight", cfg->enableBacklight);
            cfg->enableOverlay      = s.Bool(L"sobreposicao", cfg->enableOverlay);
            cfg->hkBrightnessUp     = s.Str(L"atalhoBrilhoMais", cfg->hkBrightnessUp);
            cfg->hkBrightnessDown   = s.Str(L"atalhoBrilhoMenos", cfg->hkBrightnessDown);
            cfg->hkSaturationUp     = s.Str(L"atalhoSaturacaoMais", cfg->hkSaturationUp);
            cfg->hkSaturationDown   = s.Str(L"atalhoSaturacaoMenos", cfg->hkSaturationDown);
            cfg->hkToggle           = s.Str(L"atalhoPausar", cfg->hkToggle);
            cfg->hkShow             = s.Str(L"atalhoJanela", cfg->hkShow);
            cfg->hkPanic            = s.Str(L"atalhoEmergencia", cfg->hkPanic);
            cfg->hotkeyStep         = s.Num(L"passoAtalho", cfg->hotkeyStep);
            cfg->confirmDarkSettings= s.Bool(L"confirmarTelaEscura", cfg->confirmDarkSettings);
            cfg->latitude           = s.Num(L"latitude", cfg->latitude);
            cfg->longitude          = s.Num(L"longitude", cfg->longitude);
            cfg->mirrorInternalBrightness =
                s.Bool(L"espelharBrilhoDasTeclas", cfg->mirrorInternalBrightness);
            cfg->vision.enabled          = s.Bool(L"visao", cfg->vision.enabled);
            cfg->vision.dayTemperature   = s.Num(L"visaoTempDia", cfg->vision.dayTemperature);
            cfg->vision.nightTemperature = s.Num(L"visaoTempNoite", cfg->vision.nightTemperature);
            cfg->vision.nightBrightness  = s.Num(L"visaoBrilhoNoite", cfg->vision.nightBrightness);
            cfg->vision.transitionMinutes= s.Int(L"visaoTransicao", cfg->vision.transitionMinutes);
            cfg->vision.dayStart         = s.Str(L"visaoInicioDia", cfg->vision.dayStart);
            cfg->vision.nightStart       = s.Str(L"visaoInicioNoite", cfg->vision.nightStart);
            cfg->vision.breakMinutes     = s.Int(L"visaoPausaMinutos", cfg->vision.breakMinutes);
        }
        else if (lower.rfind(L"ddc:", 0) == 0) {
            const std::wstring key = Trim(s.name.substr(4));
            if (!key.empty()) cfg->ddcMonitorModes[key] = ParseDdcMonitorMode(s.Str(L"modo", L"auto"));
        }
        else if (lower.rfind(L"modelo:", 0) == 0) {
            // [modelo:FUS087C] with regra=brilho-vcp:6B describes a quirk of a
            // monitor MODEL, not of one unit. [ddc:CHAVE] is per connected panel.
            const std::wstring edidId = Trim(s.name.substr(7));
            MonitorQuirk q;
            if (ParseMonitorQuirk(edidId, s.Str(L"regra"), &q))
                cfg->monitorQuirks.push_back(q);
            else if (!edidId.empty())
                KLOG_W(L"Regra de modelo '%s' não foi reconhecida e sera ignorada: '%s'",
                       edidId.c_str(), s.Str(L"regra").c_str());
        }
        else if (lower.rfind(L"perfil:", 0) == 0) {
            // [perfil:Nome] or [perfil:Nome|monitor:CHAVE]
            std::wstring rest = s.name.substr(7);
            size_t bar = rest.find(L"|monitor:");
            if (bar == std::wstring::npos) {
                Profile p;
                p.name = Trim(rest);
                if (p.name.empty()) continue;
                p.hotkey       = s.Str(L"atalho");
                p.transitionMs = s.Int(L"transicao", p.transitionMs);
                p.satEngine    = ParseSatEngine(s.Str(L"motorSaturacao", L"auto"));
                ReadAdjustments(s, &p.global);
                cfg->profiles.push_back(p);
            } else {
                std::wstring pname = Trim(rest.substr(0, bar));
                std::wstring mkey  = Trim(rest.substr(bar + 9));
                if (Profile* p = cfg->Find(pname)) {
                    Adjustments a = p->global;
                    ReadAdjustments(s, &a);
                    p->perMonitor[mkey] = a;
                }
            }
        }
        else if (lower.rfind(L"app:", 0) == 0) {
            AppRule r;
            r.process  = Trim(s.name.substr(4));
            r.enabled  = s.Bool(L"ativa", true);
            r.profile  = s.Str(L"perfil");
            r.priority = s.Int(L"prioridade", 0);
            if (!r.process.empty()) cfg->appRules.push_back(r);
        }
        else if (lower.rfind(L"horario", 0) == 0) {
            ScheduleRule r;
            r.enabled  = s.Bool(L"ativa", true);
            r.start    = s.Str(L"inicio", r.start);
            r.end      = s.Str(L"fim", r.end);
            r.profile  = s.Str(L"perfil");
            r.priority = s.Int(L"prioridade", 0);
            if (!r.profile.empty()) cfg->scheduleRules.push_back(r);
        }
    }

    return !cfg->profiles.empty();
}

}  // namespace

bool LoadConfig(Config* cfg) {
    const std::wstring path = ConfigPath();
    const std::wstring bakPath = path + L".bak";

    std::string raw;
    const bool haveMain = ReadWholeFile(path, &raw);

    // The fallback criterion is "the parse yielded a profile", not "the file
    // could be read": ReadWholeFile returns true for an empty file, so a present
    // but empty or damaged zdisplay.ini would otherwise skip straight to the
    // defaults and let the next SaveConfig overwrite a still-good .bak.
    bool parsed = haveMain && ParseConfigText(DecodeText(raw), cfg);

    bool recoveredFromBak = false;
    if (!parsed) {
        std::string bak;
        if (ReadWholeFile(bakPath, &bak) && !bak.empty() &&
            ParseConfigText(DecodeText(bak), cfg)) {
            KLOG_W(L"Configuracao principal sem nada aproveitavel; usando a copia de seguranca.");
            parsed = true;
            recoveredFromBak = true;
            // Keeps the bad file for inspection. The .bak may only be deleted
            // AFTER the recovered version is written back to the main file;
            // deleting earlier would leave the good copy nowhere if the machine
            // died in between.
            if (haveMain) ::CopyFileW(path.c_str(), (path + L".invalido").c_str(), FALSE);
        }
    }

    if (!parsed) {
        if (!haveMain) {
            cfg->SeedDefaults();
            SaveConfig(*cfg);
            KLOG_I(L"Configuracao nova criada em %s", path.c_str());
            return false;
        }
        KLOG_W(L"Nenhum arquivo valido encontrado; recriando os perfis padrao.");
        cfg->SeedDefaults();
        // The file existed but yielded nothing usable: keep a copy for
        // inspection instead of overwriting it silently.
        ::CopyFileW(path.c_str(), (path + L".invalido").c_str(), FALSE);
    }

    // No value from disk is accepted without being clamped to its valid range.
    for (auto& p : cfg->profiles) SanitizeProfile(&p);
    cfg->vision.Sanitize();
    cfg->watchdogSeconds = Clamp(cfg->watchdogSeconds, 0, 300);
    cfg->hotkeyStep = Clamp(cfg->hotkeyStep, 1.0, 25.0);

    // The default profile must be fixed BEFORE the rules: rules fall back to it
    // when they point at a profile that is gone, so the fallback name must
    // itself exist.
    if (!cfg->Find(cfg->defaultProfile) && !cfg->profiles.empty())
        cfg->defaultProfile = cfg->profiles[0].name;

    for (auto& r : cfg->appRules)
        if (!cfg->Find(r.profile)) r.profile = cfg->defaultProfile;
    for (auto& r : cfg->scheduleRules)
        if (!cfg->Find(r.profile)) r.profile = cfg->defaultProfile;

    // After recovering from the backup, rewrite the main file immediately and
    // only then discard the .bak. Deferring this to autosave would open a window
    // in which the main file is corrupt and the backup is already gone.
    if (recoveredFromBak) {
        SaveConfig(*cfg);
        ::DeleteFileW(bakPath.c_str());
    }

    KLOG_I(L"Configuracao carregada: %d perfil(is), %d regra(s) de app, %d horario(s).",
           (int)cfg->profiles.size(), (int)cfg->appRules.size(), (int)cfg->scheduleRules.size());
    return true;
}

bool SaveConfig(const Config& cfg) {
    std::wostringstream out;
    out << L"; Configuracao do Zdisplay — pode ser editada a mao.\r\n";
    out << L"; Valores neutros: brilho/contraste/saturacao 100, gamma 1, temperatura 6500,\r\n";
    out << L"; vibrance 0, matiz 0, sombras 0, definicao 0. Use -1 em\r\n";
    out << L"; brilhoHw/contrasteHw para nao gerenciar.\r\n\r\n";

    out << L"[geral]\r\n";
    out << L"perfilPadrao="          << cfg.defaultProfile << L"\r\n";
    out << L"iniciarComWindows="     << (cfg.startWithWindows ? L"1" : L"0") << L"\r\n";
    out << L"iniciarMinimizado="     << (cfg.startMinimized ? L"1" : L"0") << L"\r\n";
    out << L"regrasPorApp="          << (cfg.enableAppRules ? L"1" : L"0") << L"\r\n";
    out << L"regrasPorHorario="      << (cfg.enableSchedule ? L"1" : L"0") << L"\r\n";
    out << L"restaurarAoSair="       << (cfg.restoreOnExit ? L"1" : L"0") << L"\r\n";
    out << L"reforcarSegundos="      << cfg.watchdogSeconds << L"\r\n";
    out << L"apisDoFabricante="      << (cfg.enableVendorApis ? L"1" : L"0") << L"\r\n";
    out << L"matrizUniversal="       << (cfg.enableMagnification ? L"1" : L"0") << L"\r\n";
    out << L"ddcci="                 << (cfg.enableDdcCi ? L"1" : L"0") << L"\r\n";
    out << L"backlight="             << (cfg.enableBacklight ? L"1" : L"0") << L"\r\n";
    out << L"sobreposicao="          << (cfg.enableOverlay ? L"1" : L"0") << L"\r\n";
    out << L"atalhoBrilhoMais="      << cfg.hkBrightnessUp << L"\r\n";
    out << L"atalhoBrilhoMenos="     << cfg.hkBrightnessDown << L"\r\n";
    out << L"atalhoSaturacaoMais="   << cfg.hkSaturationUp << L"\r\n";
    out << L"atalhoSaturacaoMenos="  << cfg.hkSaturationDown << L"\r\n";
    out << L"atalhoPausar="          << cfg.hkToggle << L"\r\n";
    out << L"atalhoJanela="          << cfg.hkShow << L"\r\n";
    out << L"atalhoEmergencia="      << cfg.hkPanic << L"\r\n";
    out << L"passoAtalho="           << FormatDouble(cfg.hotkeyStep) << L"\r\n";
    out << L"confirmarTelaEscura="   << (cfg.confirmDarkSettings ? L"1" : L"0") << L"\r\n";
    out << L"espelharBrilhoDasTeclas=" << (cfg.mirrorInternalBrightness ? L"1" : L"0") << L"\r\n";
    out << L"visao="                 << (cfg.vision.enabled ? L"1" : L"0") << L"\r\n";
    out << L"visaoTempDia="          << FormatDouble(cfg.vision.dayTemperature, 0) << L"\r\n";
    out << L"visaoTempNoite="        << FormatDouble(cfg.vision.nightTemperature, 0) << L"\r\n";
    out << L"visaoBrilhoNoite="      << FormatDouble(cfg.vision.nightBrightness, 0) << L"\r\n";
    out << L"visaoTransicao="        << cfg.vision.transitionMinutes << L"\r\n";
    out << L"visaoInicioDia="        << cfg.vision.dayStart << L"\r\n";
    out << L"visaoInicioNoite="      << cfg.vision.nightStart << L"\r\n";
    out << L"visaoPausaMinutos="     << cfg.vision.breakMinutes << L"\r\n";
    // Written only when valid, so the file never carries a meaningless placeholder.
    if (cfg.HasLocation()) {
        out << L"latitude="          << FormatDouble(cfg.latitude, 4) << L"\r\n";
        out << L"longitude="         << FormatDouble(cfg.longitude, 4) << L"\r\n";
    }
    out << L"\r\n";

    for (const auto& item : cfg.ddcMonitorModes) {
        if (item.second == DdcMonitorMode::Auto) continue;
        out << L"[ddc:" << item.first << L"]\r\n";
        out << L"modo=" << DdcMonitorModeName(item.second) << L"\r\n\r\n";
    }

    for (const auto& q : cfg.monitorQuirks) {
        const std::wstring rule = FormatMonitorQuirk(q);
        if (q.edidId.empty() || rule.empty()) continue;
        out << L"[modelo:" << q.edidId << L"]\r\n";
        out << L"regra=" << rule << L"\r\n\r\n";
    }

    for (const auto& p : cfg.profiles) {
        out << L"[perfil:" << p.name << L"]\r\n";
        out << L"atalho="         << p.hotkey << L"\r\n";
        out << L"transicao="      << p.transitionMs << L"\r\n";
        out << L"motorSaturacao=" << SatEngineName(p.satEngine) << L"\r\n";
        WriteAdjustments(out, p.global);
        out << L"\r\n";

        for (const auto& kv : p.perMonitor) {
            out << L"[perfil:" << p.name << L"|monitor:" << kv.first << L"]\r\n";
            WriteAdjustments(out, kv.second);
            out << L"\r\n";
        }
    }

    for (const auto& r : cfg.appRules) {
        out << L"[app:" << r.process << L"]\r\n";
        out << L"ativa="      << (r.enabled ? L"1" : L"0") << L"\r\n";
        out << L"perfil="     << r.profile << L"\r\n";
        out << L"prioridade=" << r.priority << L"\r\n\r\n";
    }

    for (size_t i = 0; i < cfg.scheduleRules.size(); ++i) {
        const auto& r = cfg.scheduleRules[i];
        out << L"[horario" << (int)(i + 1) << L"]\r\n";
        out << L"ativa="  << (r.enabled ? L"1" : L"0") << L"\r\n";
        out << L"inicio=" << r.start << L"\r\n";
        out << L"fim="        << r.end << L"\r\n";
        out << L"perfil="     << r.profile << L"\r\n";
        out << L"prioridade=" << r.priority << L"\r\n\r\n";
    }

    EnsureDir(ConfigDir());

    // Atomic write: write to a temporary and replace it, so a power loss never
    // leaves a half-written file.
    const std::wstring finalPath = ConfigPath();
    const std::wstring tmpPath = finalPath + L".tmp";

    std::string utf8 = "\xEF\xBB\xBF" + WideToUtf8(out.str());
    if (!WriteWholeFile(tmpPath, utf8)) {
        KLOG_E(L"Nao consegui gravar a configuracao.");
        return false;
    }

    // Keeps the previous version before replacing it. An empty file is not a
    // usable backup, so a corrupt zdisplay.ini must not be allowed to overwrite
    // the last good copy.
    {
        std::string previous;
        if (ReadWholeFile(finalPath, &previous) && previous.size() > 16)
            ::CopyFileW(finalPath.c_str(), (finalPath + L".bak").c_str(), FALSE);
    }

    if (!::MoveFileExW(tmpPath.c_str(), finalPath.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        KLOG_E(L"Nao consegui substituir a configuracao (erro %lu).", ::GetLastError());
        return false;
    }
    return true;
}

bool ExportProfiles(const std::wstring& path, const std::vector<Profile>& profiles) {
    std::wostringstream out;
    out << L"; Perfis do Zdisplay exportados. Importe pela aba Perfis.\r\n\r\n";
    for (const auto& p : profiles) {
        out << L"[perfil:" << p.name << L"]\r\n";
        out << L"atalho="         << p.hotkey << L"\r\n";
        out << L"transicao="      << p.transitionMs << L"\r\n";
        out << L"motorSaturacao=" << SatEngineName(p.satEngine) << L"\r\n";
        WriteAdjustments(out, p.global);
        out << L"\r\n";
        for (const auto& kv : p.perMonitor) {
            out << L"[perfil:" << p.name << L"|monitor:" << kv.first << L"]\r\n";
            WriteAdjustments(out, kv.second);
            out << L"\r\n";
        }
    }
    return WriteWholeFile(path, "\xEF\xBB\xBF" + WideToUtf8(out.str()));
}

bool ImportProfiles(const std::wstring& path, std::vector<Profile>* out) {
    std::string raw;
    if (!ReadWholeFile(path, &raw)) return false;

    // Same decoder as LoadConfig, so a profile file saved as UTF-16 is still
    // recognized.
    const auto sections = ParseIni(DecodeText(raw));
    out->clear();

    for (const auto& s : sections) {
        if (ToLower(s.name).rfind(L"perfil:", 0) != 0) continue;
        const std::wstring rest = s.name.substr(7);
        const size_t bar = rest.find(L"|monitor:");

        if (bar == std::wstring::npos) {
            Profile p;
            p.name = Trim(rest);
            if (p.name.empty()) continue;
            p.hotkey       = s.Str(L"atalho");
            p.transitionMs = s.Int(L"transicao", p.transitionMs);
            p.satEngine    = ParseSatEngine(s.Str(L"motorSaturacao", L"auto"));
            ReadAdjustments(s, &p.global);
            out->push_back(p);
        } else {
            const std::wstring pname = Trim(rest.substr(0, bar));
            const std::wstring mkey  = Trim(rest.substr(bar + 9));
            for (auto& p : *out) {
                if (!IEquals(p.name, pname)) continue;
                Adjustments a = p.global;
                ReadAdjustments(s, &a);
                p.perMonitor[mkey] = a;
                break;
            }
        }
    }

    // An imported profile is untrusted input just like zdisplay.ini, so it is
    // sanitized before it can ever reach a gamma ramp.
    for (auto& p : *out) SanitizeProfile(&p);

    return !out->empty();
}

// Display baseline

namespace {

std::wstring BaselinePath() { return ConfigDir() + L"\\baseline.dat"; }
std::wstring SessionPath()  { return ConfigDir() + L"\\session.lock"; }

const uint32_t kBaselineMagic = 0x4C42524B;  // "KRBL"
/// v2 added the vendor block (vendor-panel vibrance/hue); v3 added the SDR white
/// level of HDR displays. Older files still load: the newer blocks are simply
/// absent from them.
const uint32_t kBaselineVersion = 3;

void PushBytes(std::string& out, const void* data, size_t n) {
    const char* p = static_cast<const char*>(data);
    out.append(p, n);
}

bool PullBytes(const std::string& in, size_t* pos, void* out, size_t n) {
    if (*pos + n > in.size()) return false;
    memcpy(out, in.data() + *pos, n);
    *pos += n;
    return true;
}

}  // namespace

bool SaveBaseline(const Baseline& b) {
    std::string out;
    PushBytes(out, &kBaselineMagic, 4);
    PushBytes(out, &kBaselineVersion, 4);

    const uint32_t rampCount = (uint32_t)b.ramps.size();
    PushBytes(out, &rampCount, 4);
    for (const auto& kv : b.ramps) {
        if (kv.second.size() != 768) continue;
        const std::string key = WideToUtf8(kv.first);
        const uint32_t keyLen = (uint32_t)key.size();
        PushBytes(out, &keyLen, 4);
        PushBytes(out, key.data(), key.size());
        PushBytes(out, kv.second.data(), 768 * sizeof(WORD));
    }

    const uint32_t hwCount = (uint32_t)b.hardware.size();
    PushBytes(out, &hwCount, 4);
    for (const auto& kv : b.hardware) {
        const std::string key = WideToUtf8(kv.first);
        const uint32_t keyLen = (uint32_t)key.size();
        PushBytes(out, &keyLen, 4);
        PushBytes(out, key.data(), key.size());
        const int32_t vb = kv.second.first, vc = kv.second.second;
        PushBytes(out, &vb, 4);
        PushBytes(out, &vc, 4);
    }

    const int32_t backlight = b.backlight;
    PushBytes(out, &backlight, 4);

    const uint32_t vendorCount = (uint32_t)b.vendor.size();
    PushBytes(out, &vendorCount, 4);
    for (const auto& kv : b.vendor) {
        const std::string key = WideToUtf8(kv.first);
        const uint32_t keyLen = (uint32_t)key.size();
        PushBytes(out, &keyLen, 4);
        PushBytes(out, key.data(), key.size());
        const int32_t vv = kv.second.first, vh = kv.second.second;
        PushBytes(out, &vv, 4);
        PushBytes(out, &vh, 4);
    }

    const uint32_t hdrCount = (uint32_t)b.hdrWhite.size();
    PushBytes(out, &hdrCount, 4);
    for (const auto& kv : b.hdrWhite) {
        const std::string key = WideToUtf8(kv.first);
        const uint32_t keyLen = (uint32_t)key.size();
        PushBytes(out, &keyLen, 4);
        PushBytes(out, key.data(), key.size());
        const int32_t nits = kv.second;
        PushBytes(out, &nits, 4);
    }

    EnsureDir(ConfigDir());
    // This file is the only thing between a crash and a display left stuck in an
    // adjustment, so it is written atomically and with a backup.
    return WriteWholeFileAtomic(BaselinePath(), out, true);
}

namespace {
/// Decodes a baseline already read from disk. Returns false for any truncated
/// file, wrong magic number or future version.
bool ParseBaseline(const std::string& raw, Baseline* b);
}  // namespace

bool LoadBaseline(Baseline* b) {
    std::string raw;
    if (ReadWholeFile(BaselinePath(), &raw) && ParseBaseline(raw, b)) return true;

    // The fallback criterion is "decoded", not "opened": a present but truncated
    // baseline must fall through to the .bak as well, otherwise the current
    // adjustment ends up saved over the original display state.
    if (!ReadWholeFile(BaselinePath() + L".bak", &raw)) return false;
    KLOG_W(L"baseline.dat ilegivel ou incompleto; usando a copia de seguranca.");
    return ParseBaseline(raw, b);
}

namespace {
bool ParseBaseline(const std::string& raw, Baseline* b) {

    // Builds into a local object and only commits at the end: filling *b during
    // the read would leave half-populated state with Empty() == false on a file
    // that ends mid-record.
    Baseline tmp;
    size_t pos = 0;
    uint32_t magic = 0, version = 0;
    if (!PullBytes(raw, &pos, &magic, 4) || magic != kBaselineMagic) return false;
    if (!PullBytes(raw, &pos, &version, 4)) return false;
    if (version < 1 || version > kBaselineVersion) return false;

    uint32_t rampCount = 0;
    if (!PullBytes(raw, &pos, &rampCount, 4) || rampCount > 64) return false;
    for (uint32_t i = 0; i < rampCount; ++i) {
        uint32_t keyLen = 0;
        if (!PullBytes(raw, &pos, &keyLen, 4) || keyLen > 4096) return false;
        std::string key(keyLen, '\0');
        if (keyLen && !PullBytes(raw, &pos, &key[0], keyLen)) return false;
        std::vector<WORD> ramp(768);
        if (!PullBytes(raw, &pos, ramp.data(), 768 * sizeof(WORD))) return false;
        tmp.ramps[Utf8ToWide(key)] = std::move(ramp);
    }

    uint32_t hwCount = 0;
    if (!PullBytes(raw, &pos, &hwCount, 4) || hwCount > 64) return false;
    for (uint32_t i = 0; i < hwCount; ++i) {
        uint32_t keyLen = 0;
        if (!PullBytes(raw, &pos, &keyLen, 4) || keyLen > 4096) return false;
        std::string key(keyLen, '\0');
        if (keyLen && !PullBytes(raw, &pos, &key[0], keyLen)) return false;
        int32_t vb = -1, vc = -1;
        if (!PullBytes(raw, &pos, &vb, 4) || !PullBytes(raw, &pos, &vc, 4)) return false;
        tmp.hardware[Utf8ToWide(key)] = std::make_pair((int)vb, (int)vc);
    }

    int32_t backlight = -1;
    if (!PullBytes(raw, &pos, &backlight, 4)) return false;
    tmp.backlight = (int)backlight;

    // Vendor block: present only from v2 on.
    if (version >= 2) {
        uint32_t vendorCount = 0;
        if (!PullBytes(raw, &pos, &vendorCount, 4) || vendorCount > 64) return false;
        for (uint32_t i = 0; i < vendorCount; ++i) {
            uint32_t keyLen = 0;
            if (!PullBytes(raw, &pos, &keyLen, 4) || keyLen > 4096) return false;
            std::string key(keyLen, '\0');
            if (keyLen && !PullBytes(raw, &pos, &key[0], keyLen)) return false;
            int32_t vv = -1, vh = -1;
            if (!PullBytes(raw, &pos, &vv, 4) || !PullBytes(raw, &pos, &vh, 4)) return false;
            tmp.vendor[Utf8ToWide(key)] = std::make_pair((int)vv, (int)vh);
        }
    }

    // SDR white level block: present only from v3 on.
    if (version >= 3) {
        uint32_t hdrCount = 0;
        if (!PullBytes(raw, &pos, &hdrCount, 4) || hdrCount > 64) return false;
        for (uint32_t i = 0; i < hdrCount; ++i) {
            uint32_t keyLen = 0;
            if (!PullBytes(raw, &pos, &keyLen, 4) || keyLen > 4096) return false;
            std::string key(keyLen, '\0');
            if (keyLen && !PullBytes(raw, &pos, &key[0], keyLen)) return false;
            int32_t nits = 0;
            if (!PullBytes(raw, &pos, &nits, 4)) return false;
            if (nits > 0) tmp.hdrWhite[Utf8ToWide(key)] = (int)nits;
        }
    }

    *b = std::move(tmp);
    return true;
}
}  // namespace

void ClearBaseline() {
    ::DeleteFileW(BaselinePath().c_str());
    // The .bak goes too: LoadBaseline deliberately falls back to it when the main
    // file fails to decode, so leaving it would let a cleared baseline return as
    // the original display state.
    ::DeleteFileW((BaselinePath() + L".bak").c_str());
}

bool SessionWasDirty() {
    return ::GetFileAttributesW(SessionPath().c_str()) != INVALID_FILE_ATTRIBUTES;
}

void SessionBegin() {
    EnsureDir(ConfigDir());
    WriteWholeFile(SessionPath(), "zdisplay");
}

void SessionEnd() {
    ::DeleteFileW(SessionPath().c_str());
}

// Color math

namespace {

/// Tanner Helland approximation, in 0..1, unnormalized.
///
/// The two branches (t <= 66 and t > 66) do not meet: at 6600 K, only 100 K from
/// neutral, green steps from 1.000000 to 0.986779. The branches are blended with
/// a smoothstep over a narrow window around 66, which removes the discontinuity
/// without disturbing the rest of the curve.
void RawTemperatureRgb(double kelvin, double* r, double* g, double* b) {
    const double t = Clamp(kelvin, 1000.0, 12000.0) / 100.0;

    const double lowT  = (std::max)(t, 1.0001);
    const double highT = (std::max)(t - 60.0, 0.0001);

    const double rLow  = 255.0;
    const double gLow  = 99.4708025861 * std::log(lowT) - 161.1195681661;
    const double rHigh = 329.698727446 * std::pow(highT, -0.1332047592);
    const double gHigh = 288.1221695283 * std::pow(highT, -0.0755148492);

    double w = Clamp((t - 64.0) / 4.0, 0.0, 1.0);
    w = w * w * (3.0 - 2.0 * w);   // smoothstep

    double rr = Lerp(rLow, rHigh, w);
    double gg = Lerp(gLow, gHigh, w);

    double bb;
    if (t <= 19.0) {
        bb = 0.0;
    } else {
        const double bLow = 138.5177312231 * std::log((std::max)(t - 10.0, 0.0001)) - 305.0447927307;
        bb = Lerp(bLow, 255.0, w);
    }

    *r = Clamp(rr, 0.0, 255.0) / 255.0;
    *g = Clamp(gg, 0.0, 255.0) / 255.0;
    *b = Clamp(bb, 0.0, 255.0) / 255.0;
}

}  // namespace

void TemperatureToRgb(double kelvin, double* r, double* g, double* b) {
    double rr, gg, bb;
    RawTemperatureRgb(kelvin, &rr, &gg, &bb);

    // Anchors on the declared neutral. The approximation returns
    // (1, 0.9965, 0.9806) at 6500 K, the point the rest of the program treats as
    // unchanged; without this division 6500 K is 1.94% short on blue and tints
    // the display yellow.
    struct Ref { double r, g, b; };
    static const Ref ref = [] {
        Ref v{};
        RawTemperatureRgb(6500.0, &v.r, &v.g, &v.b);
        return v;
    }();

    if (ref.r > 1e-6) rr /= ref.r;
    if (ref.g > 1e-6) gg /= ref.g;
    if (ref.b > 1e-6) bb /= ref.b;

    // Normalize by the strongest channel to avoid losing brightness needlessly.
    const double mx = (std::max)(rr, (std::max)(gg, bb));
    if (mx > 0.0001) { rr /= mx; gg /= mx; bb /= mx; }

    *r = Clamp(rr, 0.0, 1.0);
    *g = Clamp(gg, 0.0, 1.0);
    *b = Clamp(bb, 0.0, 1.0);
}

void IdentityRamp(WORD ramp[768]) {
    for (int i = 0; i < 256; ++i) {
        const WORD v = (WORD)(i * 257);  // 0..65535 linear
        ramp[i] = ramp[256 + i] = ramp[512 + i] = v;
    }
}

// Shadow vision
//
// In a dark scene the detail that matters lives between tones 0 and 30. Raising
// brightness or gamma lifts the whole image and washes out the rest, so these
// two controls act only on the low end of the curve.
//
// Raising the floor costs slope near black, and slope is the factor by which two
// neighboring tones separate: below 1 they converge and the shadow becomes a
// grey blur however bright it gets. Hence two controls rather than one — the
// first lifts, the second gives back the slope the first removed. The composed
// slope is the product of both stages, and the constants below are chosen so the
// curve never stops increasing; each block carries its own proof.

namespace {

constexpr double kLiftMax     = 0.16;  ///< maximum floor: tone 0 becomes ~41 of 255
constexpr double kLiftWinLow  = 0.28;  ///< lift reach at the minimum setting
constexpr double kLiftWinHigh = 0.73;  ///< lift reach at the maximum setting
// Clarity works in two stages with different windows: the narrow one covers deep
// black (up to tone 30 of 255), the wide one the shadow region as a whole. Each
// stage costs slope at the end of its own window, and the end of the narrow
// window (tone 20) falls well inside the wide stage's expansion zone, which
// restores there what the other took. Gains at black therefore multiply while
// the worst composed slope barely moves.
constexpr double kClarityDeepWin = 0.12;  ///< range of the narrow stage
constexpr double kClarityDeepMax = 1.20;  ///< its slope gain at black
constexpr double kClarityWideWin = 0.45;  ///< range of the wide stage
constexpr double kClarityWideMax = 1.00;  ///< its slope gain at black

/// Hermite expansion: adds B*v*(1-t)^2 inside the window `win`.
///
/// It is zero at both ends of the range, so tone `win` comes out exactly as it
/// went in and nothing above it moves. The derivative is 1 + B*(1-t)*(1-3t),
/// which is 1+B at black and falls to 1-B/3 at t=2/3 — increasing for every
/// B < 3.
inline double Expand(double v, double win, double b) {
    if (b <= 0.0 || v >= win) return v;
    const double t = v / win;
    const double k = 1.0 - t;
    return v + b * v * k * k;
}

}  // namespace

double ApplyContrast(double v, double contrast) {
    const double c = Clamp(contrast, 0.0, 200.0) / 100.0;
    if (std::fabs(c - 1.0) < 1e-9) return Clamp(v, 0.0, 1.0);
    v = Clamp(v, 0.0, 1.0);

    // Power S-curve, symmetric about the midtone:
    //   v <= 0.5:  0.5 * (2v)^c
    //   v >  0.5:  1 - 0.5 * (2(1-v))^c
    //
    // It passes exactly through (0,0), (0.5,0.5) and (1,1), and its derivative at
    // the midtone is exactly `c`, so the user-facing control keeps its meaning.
    // Unlike a straight line, which leaves the range and gets clipped (collapsing
    // 64 levels into one at contrast 200), this compresses at the ends and stays
    // strictly increasing for c > 0, so no two neighboring tones become one.
    if (v <= 0.5) return 0.5 * std::pow(2.0 * v, c);
    return 1.0 - 0.5 * std::pow(2.0 * (1.0 - v), c);
}

double ShadowCurve(double v, double shadows, double clarity) {
    v = Clamp(v, 0.0, 1.0);

    // 1) Clarity, in two stages: separates near-black tones from each other
    //    before the lift flattens them. The narrow stage acts first, on deep
    //    black; the wide one covers the whole shadow region. Each only adds
    //    light and each is increasing, so the composition is too.
    const double c = Clamp(clarity, 0.0, 100.0) / 100.0;
    if (c > 0.0) {
        v = Expand(v, kClarityDeepWin, kClarityDeepMax * c);
        v = Expand(v, kClarityWideWin, kClarityWideMax * c);
    }

    // 2) Lift: adds a floor whose weight dies out at the end of the window.
    //
    //    The weight is (1-t)^3 * (1+3t). Its derivative is zero at t=0, so the
    //    lift leaves the slope at black untouched and tones 0,1,2,3 rise together
    //    keeping the spacing clarity gave them. The obvious weight, (1-t)^2, has
    //    its MAXIMUM derivative at t=0 and would cancel that slope exactly where
    //    stage 1 created it.
    //
    //    The derivative is 1 - 12*(L/W)*t*(1-t)^2, minimal at t=1/3 with value
    //    1 - 1.78*L/W. The ratio L/W grows with the setting and stops at
    //    0.16/0.73 = 0.219, so the slope never falls below 0.61. Weight and its
    //    derivative reach zero together at the seam, so a dark gradient crosses
    //    the junction without visible banding.
    const double s = Clamp(shadows, 0.0, 100.0) / 100.0;
    if (s > 0.0) {
        const double lift = kLiftMax * s;
        const double win  = kLiftWinLow + (kLiftWinHigh - kLiftWinLow) * s;
        if (v < win) {
            const double t = v / win;
            const double k = 1.0 - t;
            v += lift * k * k * k * (1.0 + 3.0 * t);
        }
    }

    // Both stages only add light, so this curve never darkens the display and
    // cannot push EffectiveLuminance below its safety floor.
    return Clamp(v, 0.0, 1.0);
}

void BuildRamp(const Adjustments& a, WORD ramp[768]) {
    double tr, tg, tb;
    TemperatureToRgb(a.temperature, &tr, &tg, &tb);

    const double brightness = a.brightness / 100.0;
    const double contrast   = a.contrast / 100.0;
    const double gamma      = Clamp(a.gamma, 0.30, 3.00);
    double gr = 1, gg = 1, gb = 1;
    a.ChannelGains(&gr, &gg, &gb);

    const bool applyGamma    = std::fabs(gamma - 1.0) > 0.0001;
    const bool applyContrast = std::fabs(contrast - 1.0) > 0.0001;
    const bool applyShadows  = a.shadows > 0.01 || a.clarity > 0.01;

    for (int i = 0; i < 256; ++i) {
        double v = i / 255.0;
        if (applyGamma)    v = std::pow(v, 1.0 / gamma);
        if (applyContrast) v = ApplyContrast(v, a.contrast);
        v = Clamp(v, 0.0, 1.0);

        // Shadows come after gamma and contrast, so neither can push the shadow
        // back to black, but BEFORE brightness. Brightness is a global
        // multiplier: applied first, it would pull every tone into the lift
        // window (up to 0.73) whenever brightness is below 73%, so tone 255 would
        // count as shadow and break the guarantee that nothing above 180 changes.
        if (applyShadows) v = ShadowCurve(v, a.shadows, a.clarity);

        v = Clamp(v * brightness, 0.0, 1.0);

        const double r = Clamp(v * tr * gr, 0.0, 1.0);
        const double g = Clamp(v * tg * gg, 0.0, 1.0);
        const double b = Clamp(v * tb * gb, 0.0, 1.0);

        ramp[i]       = (WORD)llround(r * 65535.0);
        ramp[256 + i] = (WORD)llround(g * 65535.0);
        ramp[512 + i] = (WORD)llround(b * 65535.0);
    }
}

// Matrices
//
// Magnification API convention: row vector [r g b a 1] multiplied by the matrix,
// that is out[j] = sum_i in[i] * m[i*5 + j].

static const double kLumR = 0.2126, kLumG = 0.7152, kLumB = 0.0722;

Mat5 Mat5::Identity() {
    Mat5 r{};
    for (int i = 0; i < 25; ++i) r.m[i] = 0.0f;
    r.m[0] = r.m[6] = r.m[12] = r.m[18] = r.m[24] = 1.0f;
    return r;
}

Mat5 Mat5::Saturation(double s) {
    const double inv = 1.0 - s;
    const double r = kLumR * inv, g = kLumG * inv, b = kLumB * inv;

    Mat5 out = Identity();
    out.m[0]  = (float)(r + s); out.m[5]  = (float)g;       out.m[10] = (float)b;
    out.m[1]  = (float)r;       out.m[6]  = (float)(g + s); out.m[11] = (float)b;
    out.m[2]  = (float)r;       out.m[7]  = (float)g;       out.m[12] = (float)(b + s);
    return out;
}

Mat5 Mat5::Hue(double degrees) {
    const double rad = degrees * 3.14159265358979323846 / 180.0;
    const double c = std::cos(rad), s = std::sin(rad);

    Mat5 out = Identity();
    out.m[0]  = (float)(kLumR + c * (1 - kLumR) + s * (-kLumR));
    out.m[5]  = (float)(kLumG + c * (-kLumG)    + s * (-kLumG));
    out.m[10] = (float)(kLumB + c * (-kLumB)    + s * (1 - kLumB));

    out.m[1]  = (float)(kLumR + c * (-kLumR)    + s * 0.143);
    out.m[6]  = (float)(kLumG + c * (1 - kLumG) + s * 0.140);
    out.m[11] = (float)(kLumB + c * (-kLumB)    + s * (-0.283));

    out.m[2]  = (float)(kLumR + c * (-kLumR)    + s * (-(1 - kLumR)));
    out.m[7]  = (float)(kLumG + c * (-kLumG)    + s * kLumG);
    out.m[12] = (float)(kLumB + c * (1 - kLumB) + s * kLumB);
    return out;
}

Mat5 Mat5::Invert() {
    Mat5 out = Identity();
    out.m[0] = out.m[6] = out.m[12] = -1.0f;
    out.m[20] = out.m[21] = out.m[22] = 1.0f;
    return out;
}

Mat5 Mat5::Levels(double rGain, double gGain, double bGain, double offset) {
    Mat5 out = Identity();
    out.m[0]  = (float)rGain;
    out.m[6]  = (float)gGain;
    out.m[12] = (float)bGain;
    // Row 4 is the translation in the [r g b a 1] row-vector convention.
    out.m[20] = out.m[21] = out.m[22] = (float)offset;
    return out;
}

Mat5 Mat5::FromAdjustments(const Adjustments& a) {
    double tr = 1, tg = 1, tb = 1;
    TemperatureToRgb(a.temperature, &tr, &tg, &tb);
    double gr = 1, gg = 1, gb = 1;
    a.ChannelGains(&gr, &gg, &gb);

    const double br = Clamp(a.brightness, 0.0, 150.0) / 100.0;
    const double ct = Clamp(a.contrast,   0.0, 200.0) / 100.0;

    // Contrast about mid grey: out = (in - 0.5) * ct + 0.5, that is gain ct with
    // offset 0.5*(1 - ct). Brightness and temperature apply as per-channel gain
    // on top of that.
    return Mat5::Levels(br * ct * tr * gr,
                        br * ct * tg * gg,
                        br * ct * tb * gb,
                        br * 0.5 * (1.0 - ct));
}

Mat5 Mat5::operator*(const Mat5& o) const {
    Mat5 r{};
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 5; ++j) {
            float sum = 0.0f;
            for (int k = 0; k < 5; ++k) sum += m[i * 5 + k] * o.m[k * 5 + j];
            r.m[i * 5 + j] = sum;
        }
    return r;
}

bool Mat5::NearlyEquals(const Mat5& o, float tol) const {
    for (int i = 0; i < 25; ++i)
        if (std::fabs(m[i] - o.m[i]) > tol) return false;
    return true;
}

// EDID

namespace {

/// Reads one EDID chromaticity coordinate: 8 high bits in a dedicated byte and
/// the 2 low bits packed into bytes 25/26.
double EdidChroma(const unsigned char* d, int highByte, int lowByte, int lowShift) {
    const unsigned low = ((unsigned)d[lowByte] >> lowShift) & 0x3u;
    return (double)((((unsigned)d[highByte]) << 2) | low) / 1024.0;
}

/// Text of an EDID descriptor (0xFC name, 0xFF serial): up to 13 ASCII bytes,
/// terminated by 0x0A and space-padded.
std::wstring EdidDescriptorText(const unsigned char* desc) {
    std::wstring s;
    for (int i = 5; i < 18; ++i) {
        const unsigned char c = desc[i];
        if (c == 0x0A) break;
        if (c < 0x20 || c > 0x7E) continue;   // skip junk, common on cheap panels
        s += (wchar_t)c;
    }
    return Trim(s);
}

}  // namespace

bool ParseEdid(const unsigned char* d, size_t size, EdidInfo* out) {
    if (!d || !out || size < 128) return false;
    *out = EdidInfo{};

    static const unsigned char kHeader[8] = {0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00};
    if (memcmp(d, kHeader, 8) != 0) return false;

    // The 128 bytes of the base block must sum to zero. Without this check a
    // truncated or zeroed block would yield an invented serial, and the monitor
    // key would point at the wrong panel.
    unsigned sum = 0;
    for (int i = 0; i < 128; ++i) sum += d[i];
    if ((sum & 0xFFu) != 0) return false;

    // Manufacturer: three 5-bit letters packed big-endian into bytes 8-9.
    wchar_t letters[4] = {0, 0, 0, 0};
    const unsigned mfg = ((unsigned)d[8] << 8) | (unsigned)d[9];
    for (int i = 0; i < 3; ++i) {
        const unsigned v = (mfg >> (10 - 5 * i)) & 0x1Fu;
        if (v < 1 || v > 26) return false;   // outside A-Z: block is not trustworthy
        letters[i] = (wchar_t)(L'A' + (int)v - 1);
    }
    out->manufacturer = letters;

    out->product = (unsigned)d[10] | ((unsigned)d[11] << 8);
    out->serial  = (unsigned)d[12] | ((unsigned)d[13] << 8) |
                   ((unsigned)d[14] << 16) | ((unsigned)d[15] << 24);
    if (d[17] > 0 && d[17] != 0xFF) out->year = 1990 + (int)d[17];
    out->digital = (d[20] & 0x80u) != 0;

    // Primaries, used to tell whether the panel is wide gamut.
    const double rx = EdidChroma(d, 27, 25, 6), ry = EdidChroma(d, 28, 25, 4);
    const double gx = EdidChroma(d, 29, 25, 2), gy = EdidChroma(d, 30, 25, 0);
    const double bx = EdidChroma(d, 31, 26, 6), by = EdidChroma(d, 32, 26, 4);
    out->gamutArea = 0.5 * fabs(rx * (gy - by) + gx * (by - ry) + bx * (ry - gy));
    // 15% above sRGB separates an ordinary panel from a cheap DCI-P3 one; below
    // that the difference fits within the coarse precision of the EDID primaries.
    out->wideGamut = out->gamutArea > kSrgbGamutArea * 1.15;

    for (int i = 0; i < 4; ++i) {
        const unsigned char* desc = d + 54 + (size_t)i * 18;
        // Text descriptor: the first three bytes are zero. Anything else is a
        // timing descriptor, which is not relevant here.
        if (desc[0] != 0 || desc[1] != 0 || desc[2] != 0) continue;
        if (desc[3] == 0xFC)      out->modelName  = EdidDescriptorText(desc);
        else if (desc[3] == 0xFF) out->serialText = EdidDescriptorText(desc);
    }

    out->valid = true;
    return true;
}

std::vector<unsigned char> ParseVcpCodes(const std::string& caps) {
    std::vector<unsigned char> out;

    // Some monitors answer "VCP (" and others strip EVERY space between codes
    // ("vcp(101214(0105)16)"). The reply is untrusted input: the name is matched
    // case-insensitively with optional space before the parenthesis, without
    // letting sections such as cmds(...) leak in.
    size_t body = std::string::npos;
    for (size_t i = 0; i + 3 <= caps.size(); ++i) {
        const auto lower = [](char c) {
            return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        };
        if (lower(caps[i]) != 'v' || lower(caps[i + 1]) != 'c' ||
            lower(caps[i + 2]) != 'p') continue;
        size_t p = i + 3;
        while (p < caps.size() && (caps[p] == ' ' || caps[p] == '\t' ||
                                   caps[p] == '\r' || caps[p] == '\n')) ++p;
        if (p < caps.size() && caps[p] == '(') { body = p + 1; break; }
    }
    if (body == std::string::npos) return out;

    int depth = 0;              // depth INSIDE the vcp(...) section
    std::string token;

    auto flush = [&]() {
        // An MCCS code is one hex pair. A token can hold several pairs when the
        // firmware omits the separators. Values inside nested parentheses belong
        // to the preceding code and never become codes themselves.
        if (depth == 0 && !token.empty() && (token.size() % 2) == 0) {
            for (size_t at = 0; at < token.size(); at += 2) {
                unsigned v = 0;
                bool hex = true;
                for (size_t j = at; j < at + 2; ++j) {
                    const char c = token[j];
                    v <<= 4;
                    if      (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
                    else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
                    else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
                    else { hex = false; break; }
                }
                if (hex && std::find(out.begin(), out.end(), (unsigned char)v) == out.end())
                    out.push_back((unsigned char)v);
            }
        }
        token.clear();
    };

    for (size_t i = body; i < caps.size(); ++i) {
        const char c = caps[i];
        if (c == '(') { flush(); ++depth; continue; }
        if (c == ')') {
            flush();
            if (depth == 0) break;      // closes the vcp section itself
            --depth;
            continue;
        }
        const bool hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
                         (c >= 'a' && c <= 'f');
        if (!hex) { flush(); continue; }
        token += c;
    }
    flush();
    return out;
}

std::vector<VcpFeature> ParseVcpFeatures(const std::string& caps) {
    // Same scan as ParseVcpCodes, except that pairs read INSIDE a nested
    // parenthesis are attributed to the preceding code instead of discarded.
    std::vector<VcpFeature> out;

    size_t body = std::string::npos;
    for (size_t i = 0; i + 3 <= caps.size(); ++i) {
        const auto lower = [](char c) {
            return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        };
        if (lower(caps[i]) != 'v' || lower(caps[i + 1]) != 'c' ||
            lower(caps[i + 2]) != 'p') continue;
        size_t p = i + 3;
        while (p < caps.size() && (caps[p] == ' ' || caps[p] == '\t' ||
                                   caps[p] == '\r' || caps[p] == '\n')) ++p;
        if (p < caps.size() && caps[p] == '(') { body = p + 1; break; }
    }
    if (body == std::string::npos) return out;

    int depth = 0;
    std::string token;

    const auto hexPair = [](const std::string& s, size_t at, unsigned* v) {
        unsigned acc = 0;
        for (size_t j = at; j < at + 2; ++j) {
            const char c = s[j];
            acc <<= 4;
            if      (c >= '0' && c <= '9') acc |= (unsigned)(c - '0');
            else if (c >= 'A' && c <= 'F') acc |= (unsigned)(c - 'A' + 10);
            else if (c >= 'a' && c <= 'f') acc |= (unsigned)(c - 'a' + 10);
            else return false;
        }
        *v = acc;
        return true;
    };

    auto flush = [&]() {
        if (!token.empty() && (token.size() % 2) == 0) {
            for (size_t at = 0; at < token.size(); at += 2) {
                unsigned v = 0;
                if (!hexPair(token, at, &v)) break;
                if (depth == 0) {
                    // New code; a repeat in the firmware is not a second entry.
                    bool seen = false;
                    for (const auto& f : out)
                        if (f.code == (unsigned char)v) { seen = true; break; }
                    if (!seen) {
                        VcpFeature f;
                        f.code = (unsigned char)v;
                        out.push_back(f);
                    }
                } else if (!out.empty()) {
                    // Value belonging to the preceding code.
                    auto& values = out.back().values;
                    if (std::find(values.begin(), values.end(), (unsigned char)v) == values.end())
                        values.push_back((unsigned char)v);
                }
            }
        }
        token.clear();
    };

    for (size_t i = body; i < caps.size(); ++i) {
        const char c = caps[i];
        if (c == '(') { flush(); ++depth; continue; }
        if (c == ')') {
            flush();
            if (depth == 0) break;
            --depth;
            continue;
        }
        const bool hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
                         (c >= 'a' && c <= 'f');
        if (!hex) { flush(); continue; }
        token += c;
    }
    flush();
    return out;
}

std::wstring VcpValueName(unsigned char code, unsigned char value) {
    // Only the features the interface offers. Names come from the MCCS table;
    // anything missing is shown as a number, which is still useful when
    // diagnosing an unusual monitor.
    struct Entry { unsigned char code, value; const wchar_t* name; };
    static const Entry kNames[] = {
        // 0x14 — color preset.
        {0x14, 0x01, L"sRGB"},          {0x14, 0x02, L"nativa do painel"},
        {0x14, 0x03, L"4000 K"},        {0x14, 0x04, L"5000 K"},
        {0x14, 0x05, L"6500 K"},        {0x14, 0x06, L"7500 K"},
        {0x14, 0x07, L"8200 K"},        {0x14, 0x08, L"9300 K"},
        {0x14, 0x09, L"10000 K"},       {0x14, 0x0A, L"11500 K"},
        {0x14, 0x0B, L"usuário 1"},     {0x14, 0x0C, L"usuário 2"},
        {0x14, 0x0D, L"usuário 3"},
        // 0x60 — input source.
        {0x60, 0x01, L"VGA 1"},         {0x60, 0x02, L"VGA 2"},
        {0x60, 0x03, L"DVI 1"},         {0x60, 0x04, L"DVI 2"},
        {0x60, 0x05, L"composto 1"},    {0x60, 0x06, L"composto 2"},
        {0x60, 0x07, L"S-Video 1"},     {0x60, 0x08, L"S-Video 2"},
        {0x60, 0x09, L"tuner 1"},       {0x60, 0x0A, L"tuner 2"},
        {0x60, 0x0B, L"tuner 3"},       {0x60, 0x0C, L"componente 1"},
        {0x60, 0x0D, L"componente 2"},  {0x60, 0x0E, L"componente 3"},
        {0x60, 0x0F, L"DisplayPort 1"}, {0x60, 0x10, L"DisplayPort 2"},
        {0x60, 0x11, L"HDMI 1"},        {0x60, 0x12, L"HDMI 2"},
        // Non-standard but common: several vendors use these for USB-C.
        {0x60, 0x1B, L"USB-C"},         {0x60, 0x1C, L"USB-C 2"},
        {0x60, 0x19, L"HDMI 3"},        {0x60, 0x1A, L"HDMI 4"},
        // 0xD6 — power mode.
        {0xD6, 0x01, L"ligado"},        {0xD6, 0x02, L"espera"},
        {0xD6, 0x03, L"suspenso"},      {0xD6, 0x04, L"desligado (software)"},
        {0xD6, 0x05, L"desligado"},
    };
    for (const auto& e : kNames)
        if (e.code == code && e.value == value) return e.name;
    return std::wstring();
}

const wchar_t* DdcMonitorModeName(DdcMonitorMode mode) {
    switch (mode) {
        case DdcMonitorMode::Slow:     return L"lento";
        case DdcMonitorMode::Disabled: return L"desativado";
        case DdcMonitorMode::Auto:
        default:                       return L"auto";
    }
}

DdcMonitorMode ParseDdcMonitorMode(const std::wstring& text) {
    const std::wstring value = ToLower(Trim(text));
    if (value == L"lento" || value == L"slow") return DdcMonitorMode::Slow;
    if (value == L"desativado" || value == L"disabled" || value == L"off")
        return DdcMonitorMode::Disabled;
    return DdcMonitorMode::Auto;
}

// Monitor model quirks

namespace {

/// Known deviations from the standard, by monitor model.
///
/// Deliberately short: only entries with a known cause and a consequence bad
/// enough that it should not be discovered on a user's machine.
const MonitorQuirk kBuiltinQuirks[] = {
    // Firmware that takes down the video driver on receiving DDC/CI.
    { L"LTM2C02", true,  0,    false, L"o firmware derruba o driver de vídeo" },
    { L"GSM7714", true,  0,    false, L"o firmware derruba o driver de vídeo" },
    // Fujitsu panels that answer brightness on a private register: the standard
    // 0x10 accepts the command and changes nothing, a silent failure.
    { L"FUS087C", false, 0x6B, false, L"usa o registrador 0x6B para brilho" },
    { L"FUS06AB", false, 0x13, false, L"usa o registrador 0x13 para brilho" },
};

std::vector<MonitorQuirk> g_userQuirks;
Lock g_quirkLock;

}  // namespace

void SetUserMonitorQuirks(const std::vector<MonitorQuirk>& quirks) {
    Guard g(g_quirkLock);
    g_userQuirks = quirks;
}

const MonitorQuirk* FindMonitorQuirk(const std::wstring& edidId) {
    if (edidId.empty()) return nullptr;

    // User entries win: they can add a model or neutralize a built-in entry that
    // misbehaves on their hardware.
    {
        Guard g(g_quirkLock);
        for (const auto& q : g_userQuirks)
            if (IEquals(q.edidId, edidId)) return &q;
    }

    for (const auto& q : kBuiltinQuirks)
        if (IEquals(q.edidId, edidId)) return &q;
    return nullptr;
}

bool ParseMonitorQuirk(const std::wstring& edidId, const std::wstring& text,
                       MonitorQuirk* out) {
    if (!out || edidId.empty()) return false;

    MonitorQuirk q;
    q.edidId = Trim(edidId);
    bool any = false;

    size_t start = 0;
    const std::wstring source = text;
    while (start <= source.size()) {
        size_t end = source.find_first_of(L", \t", start);
        if (end == std::wstring::npos) end = source.size();
        const std::wstring token = ToLower(Trim(source.substr(start, end - start)));
        start = end + 1;
        if (token.empty()) continue;

        if (token == L"bloquear" || token == L"block") { q.block = true; any = true; }
        else if (token == L"sem-capacidades" || token == L"no-caps") { q.unsafeCaps = true; any = true; }
        else if (token.compare(0, 11, L"brilho-vcp:") == 0 ||
                 token.compare(0, 15, L"brightness-vcp:") == 0) {
            const size_t colon = token.find(L':');
            const std::wstring value = token.substr(colon + 1);
            // Hexadecimal by default: that is how VCP registers are written
            // throughout the documentation (0x10, 0x6B).
            wchar_t* stop = nullptr;
            const long code = wcstol(value.c_str(), &stop, 16);
            if (stop && stop != value.c_str() && code > 0 && code <= 0xFF) {
                q.brightnessVcp = (int)code;
                any = true;
            }
        }
    }

    if (!any) return false;
    *out = q;
    return true;
}

std::wstring FormatMonitorQuirk(const MonitorQuirk& q) {
    std::wstring out;
    const auto add = [&](const std::wstring& s) {
        if (!out.empty()) out += L",";
        out += s;
    };
    if (q.block) add(L"bloquear");
    if (q.unsafeCaps) add(L"sem-capacidades");
    if (q.brightnessVcp > 0) add(Format(L"brilho-vcp:%02X", q.brightnessVcp));
    return out;
}

DdcErrorKind ClassifyDdcError(unsigned long error) {
    // winerror.h constants, kept numeric because some older MinGW headers do not
    // declare the whole ERROR_GRAPHICS_* family.
    switch (error) {
        case 0:          return DdcErrorKind::None;
        case 0xC0262580: // ERROR_GRAPHICS_I2C_NOT_SUPPORTED
        case 0xC0262581: // ERROR_GRAPHICS_I2C_DEVICE_DOES_NOT_EXIST
        case 0xC0262584: // ERROR_GRAPHICS_DDCCI_VCP_NOT_SUPPORTED
            return DdcErrorKind::Unsupported;

        case 0xC026258C: // ERROR_GRAPHICS_INVALID_PHYSICAL_MONITOR_HANDLE
        case 0xC026258D: // ERROR_GRAPHICS_MONITOR_NO_LONGER_EXISTS
            return DdcErrorKind::Unavailable;

        case 0xC0262582: // ERROR_GRAPHICS_I2C_ERROR_TRANSMITTING_DATA
        case 0xC0262583: // ERROR_GRAPHICS_I2C_ERROR_RECEIVING_DATA
        case 0xC0262585: // ERROR_GRAPHICS_DDCCI_INVALID_DATA
        case 0xC0262588: // ERROR_GRAPHICS_MCA_INTERNAL_ERROR
        case 0xC0262589: // ERROR_GRAPHICS_DDCCI_INVALID_MESSAGE_COMMAND
        case 0xC026258A: // ERROR_GRAPHICS_DDCCI_INVALID_MESSAGE_LENGTH
        case 0xC026258B: // ERROR_GRAPHICS_DDCCI_INVALID_MESSAGE_CHECKSUM
        case 0xC02625D8: // current > maximum (also appears in a corrupt reply)
        case ERROR_TIMEOUT:
            return DdcErrorKind::Transient;

        default:
            return DdcErrorKind::Permanent;
    }
}

bool DdcErrorCanRetry(unsigned long error) {
    return ClassifyDdcError(error) == DdcErrorKind::Transient;
}

bool DdcWriteBatchFits(int alreadyUsed, int planned, int limit) {
    if (alreadyUsed < 0 || planned < 0 || limit < 0) return false;
    if (alreadyUsed > limit) return false;
    return planned <= limit - alreadyUsed;
}

int DdcRawToPercent(unsigned long raw, unsigned long lo, unsigned long hi) {
    if (hi <= lo) return -1;
    if (raw < lo) raw = lo;
    if (raw > hi) raw = hi;
    return (int)llround((double)(raw - lo) * 100.0 / (double)(hi - lo));
}

std::wstring MonitorVendorName(const std::wstring& pnpId) {
    struct Entry { const wchar_t* id; const wchar_t* name; };
    static const Entry kVendors[] = {
        {L"AAC", L"AcerView"},      {L"ACI", L"ASUS"},          {L"ACR", L"Acer"},
        {L"ADI", L"ADI"},           {L"AOC", L"AOC"},           {L"APP", L"Apple"},
        {L"AUO", L"AU Optronics"},  {L"BNQ", L"BenQ"},          {L"BOE", L"BOE"},
        {L"CMN", L"Chi Mei Innolux"}, {L"CMO", L"Chi Mei"},     {L"CPQ", L"Compaq"},
        {L"CPT", L"Chunghwa"},      {L"DEL", L"Dell"},          {L"EIZ", L"EIZO"},
        {L"ENC", L"EIZO"},          {L"EPI", L"Envision"},      {L"FUS", L"Fujitsu"},
        {L"GSM", L"LG"},            {L"GWY", L"Gateway"},       {L"HEI", L"Hyundai"},
        {L"HIT", L"Hitachi"},       {L"HPN", L"HP"},            {L"HSD", L"HannStar"},
        {L"HSL", L"Hansol"},        {L"HWP", L"HP"},            {L"IBM", L"IBM"},
        {L"IVM", L"iiyama"},        {L"LEN", L"Lenovo"},        {L"LGD", L"LG Display"},
        {L"LPL", L"LG Philips"},    {L"MED", L"Medion"},        {L"MEI", L"Panasonic"},
        {L"MEL", L"Mitsubishi"},    {L"MSI", L"MSI"},           {L"NEC", L"NEC"},
        {L"NVD", L"NVIDIA"},        {L"PHL", L"Philips"},       {L"PLA", L"Planar"},
        {L"QDS", L"Quanta"},        {L"SAM", L"Samsung"},       {L"SAN", L"Sanyo"},
        {L"SEC", L"Seiko Epson"},   {L"SHP", L"Sharp"},         {L"SNY", L"Sony"},
        {L"STN", L"Samtron"},       {L"TOS", L"Toshiba"},       {L"TSB", L"Toshiba"},
        {L"VSC", L"ViewSonic"},     {L"VIZ", L"Vizio"},
    };

    const std::wstring id = Trim(pnpId);
    for (const auto& e : kVendors)
        if (IEquals(id, e.id)) return e.name;
    return id;
}

std::wstring VcpFeatureName(unsigned char code) {
    struct Entry { unsigned char code; const wchar_t* name; };
    // Only MCCS standard codes that are certain. The rest are mostly
    // vendor-specific, and inventing names for them would mislead anyone trying
    // to understand an unusual monitor.
    static const Entry kCodes[] = {
        {0x02, L"novo valor de controle"}, {0x04, L"restaurar padrao de fabrica"},
        {0x05, L"restaurar brilho/contraste de fabrica"},
        {0x08, L"restaurar cor de fabrica"},
        {0x0B, L"incremento de temperatura de cor"},
        {0x0C, L"temperatura de cor"},     {0x10, L"brilho"},
        {0x12, L"contraste"},              {0x14, L"predefinicao de cor"},
        {0x16, L"ganho do vermelho"},      {0x18, L"ganho do verde"},
        {0x1A, L"ganho do azul"},          {0x1E, L"ajuste automatico"},
        {0x20, L"posicao horizontal"},     {0x30, L"posicao vertical"},
        {0x52, L"controle ativo"},         {0x60, L"fonte de entrada"},
        {0x62, L"volume"},                 {0x6C, L"nivel de preto do vermelho"},
        {0x6E, L"nivel de preto do verde"},{0x70, L"nivel de preto do azul"},
        {0x86, L"modo de escala"},         {0x8D, L"mudo"},
        {0xAC, L"frequencia horizontal"},  {0xAE, L"frequencia vertical"},
        {0xB2, L"tipo de subpixel"},       {0xB6, L"tecnologia do painel"},
        {0xC0, L"horas de uso"},           {0xC6, L"chave de ativacao"},
        {0xC8, L"tipo de controlador"},    {0xC9, L"versao do firmware"},
        {0xCA, L"controle do menu do monitor"}, {0xCC, L"idioma do menu"},
        {0xD6, L"modo de energia"},        {0xDF, L"versao do VCP"},
    };
    for (const auto& e : kCodes)
        if (e.code == code) return e.name;
    return std::wstring();
}

const wchar_t* GpuVendorName(unsigned vendorId) {
    switch (vendorId) {
        case kVendorIntel:  return L"Intel";
        case kVendorNvidia: return L"NVIDIA";
        case kVendorAmd:    return L"AMD";
        default:            return L"";
    }
}

std::wstring DevicePathFromWmiInstance(const std::wstring& instanceName) {
    // DISPLAY\BOE0900\4&1a2b3c&0&UID111_0  ->  \\?\DISPLAY#BOE0900#4&1a2b3c&0&UID111
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start <= instanceName.size()) {
        const size_t p = instanceName.find(L'\\', start);
        if (p == std::wstring::npos) { parts.push_back(instanceName.substr(start)); break; }
        parts.push_back(instanceName.substr(start, p - start));
        start = p + 1;
    }
    if (parts.size() < 3 || parts[0].empty() || parts[1].empty() || parts[2].empty())
        return std::wstring();

    // WMI appends an instance-index suffix _N that the device path does not
    // carry; without stripping it no comparison matches.
    std::wstring instance = parts[2];
    const size_t underscore = instance.find_last_of(L'_');
    if (underscore != std::wstring::npos && underscore > 0)
        instance.resize(underscore);

    // parts[0] is the enumerator ("DISPLAY"), reused rather than hardcoded in
    // case another one appears.
    return L"\\\\?\\" + parts[0] + L"#" + parts[1] + L"#" + instance;
}

// HDR (SDR white level)

namespace {

/// SDR white level: how many nits Windows delivers to SDR content on a display
/// with HDR enabled. The field is in thousandths of 80 nits, so 1000 means
/// 80 nits, the default and equivalent to no change.
constexpr UINT32 kGetSdrWhiteLevel = 12;
constexpr UINT32 kSetSdrWhiteLevel = 0xFFFFFFEEu;

struct SdrWhiteLevelGet {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    ULONG SDRWhiteLevel;
};

struct SdrWhiteLevelSet {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    UINT32 SDRWhiteLevel;
    UCHAR  finalValue;
};

}  // namespace

namespace hdr {

int ReadWhiteNits(const MonitorTarget& m) {
    if (!m.hasPathInfo) return 0;

    SdrWhiteLevelGet q{};
    q.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)kGetSdrWhiteLevel;
    q.header.size = sizeof(q);
    q.header.adapterId = m.pathAdapterId;
    q.header.id = m.pathTargetId;
    if (::DisplayConfigGetDeviceInfo(&q.header) != ERROR_SUCCESS) return 0;

    const int nits = (int)((q.SDRWhiteLevel * 80u) / 1000u);
    return (nits > 0) ? nits : 0;
}

bool WriteWhiteNits(const MonitorTarget& m, int nits) {
    if (!m.hasPathInfo) return false;

    nits = (int)Clamp((double)nits, (double)kMinWhiteNits, (double)kMaxWhiteNits);
    // The driver only accepts steps of 4 nits; an off-step value is refused or
    // silently rounded, and the read-back then never matches the request.
    if (nits % 4 != 0) nits += 4 - (nits % 4);

    SdrWhiteLevelSet s{};
    s.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)kSetSdrWhiteLevel;
    s.header.size = sizeof(s);
    s.header.adapterId = m.pathAdapterId;
    s.header.id = m.pathTargetId;
    s.SDRWhiteLevel = (UINT32)((unsigned)nits * 1000u / 80u);
    s.finalValue = 1;

    return ::DisplayConfigSetDeviceInfo(&s.header) == ERROR_SUCCESS;
}

}  // namespace hdr

// Monitors

namespace monitors {
namespace {

std::vector<MonitorTarget> g_list;
Lock g_lock;

/// Reads the panel's EDID block from the registry.
///
/// The path derives from the DeviceID returned by EnumDisplayDevices:
/// \\?\DISPLAY#DEL4093#5&ab12&0&UID4353#{guid} becomes
/// ...\Enum\DISPLAY\DEL4093\5&ab12&0&UID4353\Device Parameters.
bool ReadEdidFromRegistry(const std::wstring& pnpId, const std::wstring& instance,
                          std::vector<unsigned char>* out) {
    if (pnpId.empty() || instance.empty() || !out) return false;
    const std::wstring path = L"SYSTEM\\CurrentControlSet\\Enum\\DISPLAY\\" +
                              pnpId + L"\\" + instance + L"\\Device Parameters";

    HKEY k = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS)
        return false;

    bool ok = false;
    DWORD type = 0, size = 0;
    if (::RegQueryValueExW(k, L"EDID", nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
        type == REG_BINARY && size >= 128 && size <= 32768) {
        out->assign((size_t)size, 0);
        ok = ::RegQueryValueExW(k, L"EDID", nullptr, nullptr, out->data(), &size) == ERROR_SUCCESS;
        if (ok) out->resize((size_t)size);
    }
    ::RegCloseKey(k);
    return ok;
}

/// Resolves the friendly name, physical identity and stable key of a GDI
/// adapter (\\.\DISPLAY1).
void Describe(const std::wstring& adapterDevice, MonitorTarget* t) {
    t->friendlyName = adapterDevice;
    t->legacyKey = adapterDevice;
    t->connectionKey = adapterDevice;

    // Name of the adapter driving this output. With device=NULL the function
    // enumerates adapters; with device=name it enumerates that adapter's
    // monitors, which is the call just below.
    for (DWORD i = 0; ; ++i) {
        DISPLAY_DEVICEW ad{};
        ad.cb = sizeof(ad);
        if (!::EnumDisplayDevicesW(nullptr, i, &ad, 0)) break;
        if (IEquals(ad.DeviceName, adapterDevice)) {
            t->adapterName = Trim(ad.DeviceString);
            break;
        }
    }

    DISPLAY_DEVICEW mon{};
    mon.cb = sizeof(mon);
    if (::EnumDisplayDevicesW(adapterDevice.c_str(), 0, &mon, 1 /*EDD_GET_DEVICE_INTERFACE_NAME*/)) {
        if (mon.DeviceString[0]) t->friendlyName = Trim(mon.DeviceString);

        // DeviceID looks like \\?\DISPLAY#DEL4093#5&ab12#{guid}; only
        // manufacturer+model and the instance are kept, the interface GUID is
        // ignored.
        std::wstring id = mon.DeviceID;
        if (!id.empty()) {
            std::vector<std::wstring> parts;
            size_t start = 0;
            while (start <= id.size()) {
                size_t p = id.find(L'#', start);
                if (p == std::wstring::npos) { parts.push_back(id.substr(start)); break; }
                parts.push_back(id.substr(start, p - start));
                start = p + 1;
            }
            if (parts.size() >= 3) {
                t->legacyKey = parts[1] + L"#" + parts[2];
                t->connectionKey = t->legacyKey;
                std::vector<unsigned char> raw;
                if (ReadEdidFromRegistry(parts[1], parts[2], &raw))
                    ParseEdid(raw.data(), raw.size(), &t->edid);
            } else {
                t->legacyKey = id;
                t->connectionKey = id;
            }
        }
    }

    // The key comes from the EDID whenever it is valid. An instance path
    // (5&ab12&0&UID4353) changes when the cable moves to another output on the
    // adapter, which would lose the monitor's own adjustment. Two displays of the
    // same model without a serial still collide; Refresh's duplicate loop breaks
    // the tie.
    t->key = t->connectionKey;
    if (t->edid.valid) {
        std::wstring stable = Format(L"%s%04X", t->edid.manufacturer.c_str(), t->edid.product);
        const bool hasSerialText = !t->edid.serialText.empty();
        const bool hasNumericSerial = t->edid.serial != 0;
        if (hasSerialText)              stable += L"-" + t->edid.serialText;
        else if (hasNumericSerial)      stable += Format(L"-%08X", t->edid.serial);
        t->modelKey = stable;
        // A valid serial follows the panel across ports. Without one the model
        // alone is not an identity, so the instance/port is included from the
        // start, not only when two identical displays are connected at once.
        t->key = (hasSerialText || hasNumericSerial)
               ? stable : stable + L"|" + t->connectionKey;
    }

    // The EDID model name beats the Windows one, which is usually "Generic PnP
    // Monitor" precisely on monitors without a vendor driver.
    if (t->edid.valid && !t->edid.modelName.empty())
        t->friendlyName = t->edid.modelName;

    // If the name is generic, append the adapter label so two identical displays
    // can be told apart.
    const std::wstring label = adapterDevice.substr(adapterDevice.find_last_of(L'\\') + 1);
    if (t->friendlyName == adapterDevice || t->friendlyName.empty())
        t->friendlyName = label;
    else
        t->friendlyName = t->friendlyName + L" (" + label + L")";

    if (t->key.empty()) t->key = adapterDevice;
    if (t->legacyKey.empty()) t->legacyKey = adapterDevice;
    if (t->connectionKey.empty()) t->connectionKey = t->legacyKey;
}

/// DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO. Declared by hand because
/// the MinGW header does not always carry it; the value has been stable since
/// Windows 10 1709 and the query simply fails on versions that do not know the
/// type.
constexpr UINT32 kGetAdvancedColorInfo = 9;

struct AdvancedColorInfo {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    UINT32 value;                ///< bit0 = supported, bit1 = enabled
    UINT32 colorEncoding;
    UINT32 bitsPerColorChannel;
};

/// DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2, from Windows 11 24H2.
///
/// With Automatic Color Management enabled, the older query reports
/// `advancedColorEnabled` true while the display is still SDR, where the gamma
/// ramp remains valid. This query reports the ACTIVE MODE instead, which
/// separates the cases. On older Windows it fails and the old path takes over.
constexpr UINT32 kGetAdvancedColorInfo2 = 15;

enum : UINT32 {
    kAdvancedColorModeSdr = 0,
    kAdvancedColorModeWcg = 1,   ///< wide gamut, still SDR: the ramp applies
    kAdvancedColorModeHdr = 2,
};

struct AdvancedColorInfo2 {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    UINT32 value;                ///< bit4 = HDR supported, bit5 = enabled by the user
    UINT32 colorEncoding;
    UINT32 bitsPerColorChannel;
    UINT32 activeColorMode;      ///< 0 SDR, 1 WCG, 2 HDR
};


/// Extracts the PCI vendor ID from the adapter device path, which has the form
/// \\?\PCI#VEN_8086&DEV_9A49#... — the most direct source, and the only one that
/// does not depend on translating a commercial name.
unsigned VendorIdFromDevicePath(const std::wstring& path) {
    const size_t at = path.find(L"VEN_");
    if (at == std::wstring::npos || at + 8 > path.size()) return 0;
    unsigned v = 0;
    for (size_t i = at + 4; i < at + 8; ++i) {
        const wchar_t c = path[i];
        v <<= 4;
        if      (c >= L'0' && c <= L'9') v |= (unsigned)(c - L'0');
        else if (c >= L'A' && c <= L'F') v |= (unsigned)(c - L'A' + 10);
        else if (c >= L'a' && c <= L'f') v |= (unsigned)(c - L'a' + 10);
        else return 0;
    }
    return v;
}

/// Marks internal panels and displays with advanced color (HDR) enabled from the
/// video topology, the only reliable source for either: the GDI name says
/// nothing and "primary" is a user choice.
void MarkFromDisplayConfig(std::vector<MonitorTarget>* list) {
    UINT32 pathCount = 0, modeCount = 0;
    if (::GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
        return;

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (::QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(),
                             &modeCount, modes.data(), nullptr) != ERROR_SUCCESS)
        return;

    for (UINT32 i = 0; i < pathCount; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (::DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;

        const auto tech = paths[i].targetInfo.outputTechnology;
        const bool internal =
            tech == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL ||
            tech == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED ||
            tech == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED;

        // With advanced color enabled, SetDeviceGammaRamp returns TRUE and does
        // not change a pixel. The 24H2 query comes first because the older one
        // does not distinguish HDR from ACM/wide gamut, and in those two the ramp
        // STILL applies; when it is unavailable the old path takes over.
        bool capable = false, enabled = false;
        AdvancedColorInfo2 adv2{};
        adv2.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)kGetAdvancedColorInfo2;
        adv2.header.size = sizeof(adv2);
        adv2.header.adapterId = paths[i].targetInfo.adapterId;
        adv2.header.id = paths[i].targetInfo.id;
        if (::DisplayConfigGetDeviceInfo(&adv2.header) == ERROR_SUCCESS) {
            capable = (adv2.value & 0x10u) != 0;   // highDynamicRangeSupported
            enabled = (adv2.activeColorMode == kAdvancedColorModeHdr);
        } else {
            AdvancedColorInfo adv{};
            adv.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)kGetAdvancedColorInfo;
            adv.header.size = sizeof(adv);
            adv.header.adapterId = paths[i].targetInfo.adapterId;
            adv.header.id = paths[i].targetInfo.id;
            if (::DisplayConfigGetDeviceInfo(&adv.header) == ERROR_SUCCESS) {
                capable = (adv.value & 0x1u) != 0;
                enabled = (adv.value & 0x2u) != 0;
            }
        }

        // Canonical panel path. WMI carries the same text with different
        // punctuation, so keeping it here is what allows matching the two APIs
        // without guessing by index.
        std::wstring devicePath;
        DISPLAYCONFIG_TARGET_DEVICE_NAME tgt{};
        tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tgt.header.size = sizeof(tgt);
        tgt.header.adapterId = paths[i].targetInfo.adapterId;
        tgt.header.id = paths[i].targetInfo.id;
        if (::DisplayConfigGetDeviceInfo(&tgt.header) == ERROR_SUCCESS) {
            devicePath = tgt.monitorDevicePath;
            const size_t guid = devicePath.find(L"#{");
            if (guid != std::wstring::npos) devicePath.resize(guid);
        }

        // Which adapter drives this display. On a machine with two GPUs, a vendor
        // path only applies to the display its own GPU actually drives.
        unsigned vendor = 0;
        DISPLAYCONFIG_ADAPTER_NAME ada{};
        ada.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME;
        ada.header.size = sizeof(ada);
        ada.header.adapterId = paths[i].sourceInfo.adapterId;
        ada.header.id = paths[i].sourceInfo.id;
        if (::DisplayConfigGetDeviceInfo(&ada.header) == ERROR_SUCCESS)
            vendor = VendorIdFromDevicePath(ada.adapterDevicePath);

        for (auto& m : *list) {
            if (!IEquals(m.deviceName, src.viewGdiDeviceName)) continue;
            if (internal) m.isInternal = true;
            m.hdrCapable = capable;
            m.isHdr = enabled;
            if (vendor) m.gpuVendorId = vendor;
            if (!devicePath.empty()) m.devicePath = devicePath;
            m.pathAdapterId = paths[i].targetInfo.adapterId;
            m.pathTargetId  = paths[i].targetInfo.id;
            m.hasPathInfo   = true;
        }
    }
}

BOOL CALLBACK EnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM lparam) {
    auto* out = reinterpret_cast<std::vector<MonitorTarget>*>(lparam);

    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!::GetMonitorInfoW(hMon, &mi)) return TRUE;

    MonitorTarget t;
    t.handle = hMon;
    t.bounds = mi.rcMonitor;
    t.isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    t.deviceName = mi.szDevice;
    Describe(t.deviceName, &t);
    out->push_back(t);
    return TRUE;
}

}  // namespace

void DisambiguateDuplicateKeys(std::vector<MonitorTarget>* list) {
    if (!list) return;

    // Last resort for cloned EDIDs that share the SAME defective serial. The PnP
    // instance is more stable than DISPLAY1/DISPLAY2, which changes with topology.
    //
    // The comparison must run against a COPY of the original keys: comparing
    // against the live list would erase duplicates before they are found, and
    // which display ends up without a suffix would then depend on
    // EnumDisplayMonitors ordering, which Windows does not guarantee to be
    // stable across runs.
    std::vector<std::wstring> original;
    original.reserve(list->size());
    for (const auto& m : *list) original.push_back(m.key);

    for (size_t i = 0; i < list->size(); ++i) {
        bool dupe = false;
        for (size_t j = 0; j < list->size() && !dupe; ++j)
            if (i != j && original[i] == original[j]) dupe = true;
        if (dupe) (*list)[i].key = original[i] + L"|" + (*list)[i].connectionKey;
    }
}

bool Refresh() {
    std::vector<MonitorTarget> found;
    ::EnumDisplayMonitors(nullptr, nullptr, EnumProc, reinterpret_cast<LPARAM>(&found));

    DisambiguateDuplicateKeys(&found);

    MarkFromDisplayConfig(&found);

    Guard g(g_lock);
    bool changed = found.size() != g_list.size();
    if (!changed) {
        for (size_t i = 0; i < found.size(); ++i) {
            // Geometry counts as much as identity: changing resolution or
            // rotating a display keeps the same key, yet the overlay must be
            // resized. HDR is here for the same reason: toggling it changes
            // nothing else in this list but changes which backends can work.
            if (found[i].key != g_list[i].key ||
                found[i].isPrimary != g_list[i].isPrimary ||
                found[i].isInternal != g_list[i].isInternal ||
                found[i].isHdr != g_list[i].isHdr ||
                memcmp(&found[i].bounds, &g_list[i].bounds, sizeof(RECT)) != 0) {
                changed = true;
                break;
            }
        }
    }

    g_list = found;

    if (changed) {
        KLOG_I(L"Monitores detectados: %d", (int)g_list.size());
        for (const auto& m : g_list) {
            KLOG_I(L"  %s  [%s]  %ldx%ld%s%s", m.friendlyName.c_str(), m.deviceName.c_str(),
                   m.bounds.right - m.bounds.left, m.bounds.bottom - m.bounds.top,
                   m.isPrimary ? L"  (principal)" : L"",
                   m.isInternal ? L"  (embutido)" : L"");
            if (!m.adapterName.empty())
                KLOG_I(L"      placa: %s%s", m.adapterName.c_str(),
                       GpuVendorName(m.gpuVendorId)[0]
                           ? Format(L"  [%s]", GpuVendorName(m.gpuVendorId)).c_str() : L"");
            if (m.edid.valid)
                KLOG_I(L"      EDID: %s %04X%s%s  gamut %.3f%s", m.edid.manufacturer.c_str(),
                       m.edid.product,
                       m.edid.year ? Format(L"  %d", m.edid.year).c_str() : L"",
                       m.edid.serialText.empty() ? L"" : (L"  s/n " + m.edid.serialText).c_str(),
                       m.edid.gamutArea, m.edid.wideGamut ? L" (gamut largo)" : L"");
            else
                KLOG_W(L"      sem EDID valido: identidade cai para o caminho do dispositivo");
            if (m.isHdr)
                KLOG_W(L"      HDR ligado: a rampa de gamma nao vale nesta tela");
        }
    }
    return changed;
}

int MigrateKeys(Config* c) {
    if (!c) return 0;

    std::map<std::wstring, std::wstring> candidate;
    std::map<std::wstring, int> aliasCount;
    {
        Guard g(g_lock);
        const auto add = [&](const std::wstring& alias, const std::wstring& key) {
            if (alias.empty() || alias == key) return;
            candidate[alias] = key;
            ++aliasCount[alias];
        };
        for (const auto& m : g_list) {
            add(m.legacyKey, m.key);
            if (m.connectionKey != m.legacyKey) add(m.connectionKey, m.key);
            add(m.modelKey, m.key); // only used when unambiguous
            if (!m.modelKey.empty()) add(m.modelKey + L"|" + m.deviceName, m.key);
        }
    }
    if (candidate.empty()) return 0;

    int changed = 0;
    for (auto& p : c->profiles) {
        for (const auto& r : candidate) {
            if (aliasCount[r.first] != 1) continue; // never guess between identical monitors
            auto it = p.perMonitor.find(r.first);
            if (it == p.perMonitor.end()) continue;
            // If the new key already exists it wins: it was written by a run that
            // already read the EDID, so it is the more recent one.
            if (p.perMonitor.find(r.second) == p.perMonitor.end())
                p.perMonitor[r.second] = it->second;
            p.perMonitor.erase(it);
            ++changed;
        }
    }
    for (const auto& r : candidate) {
        if (aliasCount[r.first] != 1) continue;
        auto it = c->ddcMonitorModes.find(r.first);
        if (it == c->ddcMonitorModes.end()) continue;
        if (c->ddcMonitorModes.find(r.second) == c->ddcMonitorModes.end())
            c->ddcMonitorModes[r.second] = it->second;
        c->ddcMonitorModes.erase(it);
        ++changed;
    }
    if (changed)
        KLOG_I(L"Migradas %d sobrescritas de monitor para a identidade do EDID.", changed);
    return changed;
}

const std::vector<MonitorTarget>& All() { return g_list; }

const MonitorTarget* Primary() {
    for (const auto& m : g_list) if (m.isPrimary) return &m;
    return g_list.empty() ? nullptr : &g_list[0];
}

const MonitorTarget* ByKey(const std::wstring& key) {
    for (const auto& m : g_list) if (m.key == key) return &m;
    return nullptr;
}

}  // namespace monitors
}  // namespace zdisplay
