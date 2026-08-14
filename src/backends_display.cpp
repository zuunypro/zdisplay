// Backends that work on any GPU: gamma ramp, global color matrix and dimming
// overlay.
#include "backends.h"

namespace zdisplay {

// Gamma ramp

static const wchar_t* kIcmKey =
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ICM";
static const wchar_t* kRangeValue = L"GdiIcmGammaRange";

bool GammaBackend::Init() {
    const MonitorTarget* primary = monitors::Primary();
    if (!primary) {
        details_ = L"no monitor detected";
        return false;
    }

    DeviceDC dc(primary->deviceName);
    if (!dc.Ok()) {
        details_ = L"CreateDC failed on the primary monitor";
        return false;
    }

    WORD probe[768];
    const bool canRead = ::GetDeviceGammaRamp(dc.Get(), probe) != FALSE;
    const int caps = ::GetDeviceCaps(dc.Get(), COLORMGMTCAPS);

    if (!canRead && (caps & CM_GAMMA_RAMP) == 0) {
        details_ = L"the video driver exposes no gamma ramp";
        return false;
    }

    details_ = RangeUnlocked() ? L"faixa ampliada liberada no registro"
                               : L"standard Windows range (limited)";
    available_ = true;
    return true;
}

/// Interval between retries of the full gamma ramp after Windows has refused it.
/// Each retry costs a rejection plus a binary search that repaints the screen
/// seven times, and unlocking GdiIcmGammaRange only takes effect after a new
/// Windows session, so retries are deliberately infrequent.
constexpr double kFullRangeProbeMs = 60000.0;

bool GammaBackend::TryWrite(HDC dc, const WORD ramp[768]) {
    // SetDeviceGammaRamp requires a non-const pointer.
    return ::SetDeviceGammaRamp(dc, const_cast<WORD*>(ramp)) != FALSE;
}

/// Interpolates between the monitor's baseline ramp and the target.
/// `base` is the ramp the monitor had before any adjustment — the ICC
/// calibration when one exists — so t = 0 means the untouched screen, not linear.
void GammaBackend::BlendRamp(const WORD target[768], const WORD base[768],
                             double t, WORD out[768]) {
    if (t >= 0.999) { memcpy(out, target, 768 * sizeof(WORD)); return; }
    if (t <= 0.001) { memcpy(out, base, 768 * sizeof(WORD)); return; }
    for (int i = 0; i < 256; ++i) {
        for (int c = 0; c < 3; ++c) {
            const int idx = c * 256 + i;
            const double from = base[idx];
            out[idx] = (WORD)Clamp((long)llround(from + (target[idx] - from) * t), 0L, 65535L);
        }
    }
}

/// Returns the baseline ramp captured for a monitor, falling back to the linear
/// ramp when no baseline is known.
void GammaBackend::BaseRampFor(const std::wstring& monitorKey, WORD out[768]) const {
    auto it = baseline_.find(monitorKey);
    if (it != baseline_.end() && it->second.size() == 768)
        memcpy(out, it->second.data(), 768 * sizeof(WORD));
    else
        IdentityRamp(out);
}

void GammaBackend::WriteAdaptive(const MonitorTarget& m, const WORD target[768], MonState* st) {
    DeviceDC dc(m.deviceName);
    if (!dc.Ok()) {
        KLOG_D(L"CreateDC failed for %s", m.deviceName.c_str());
        return;
    }

    // Interpolation starts from the untouched screen, so an ICC calibration is
    // what remains when the effect has to be reduced.
    WORD base[768];
    BaseRampFor(m.key, base);

    const double now = NowMs();
    // After a reduction the full ramp is retried only rarely: each retry runs
    // the binary search below, which repaints the screen at seven different
    // intensities and reads as a jump, and the registry unlock it probes for
    // only takes effect after a new Windows session.
    double factor = (st->blend < 1.0 && now < st->nextProbeMs) ? st->blend : 1.0;

    WORD attempt[768];
    BlendRamp(target, base, factor, attempt);

    if (!TryWrite(dc.Get(), attempt)) {
        // Windows silently rejects ramps that deviate too far from linear, so
        // this binary-searches for the largest accepted fraction of the effect.
        // Rejection is monotonic: the closer to the baseline, the more is taken.
        double lo = 0.0, hi = factor;
        WORD best[768];
        memcpy(best, base, sizeof(best));
        bool found = false;

        for (int step = 0; step < 7; ++step) {
            const double mid = (lo + hi) / 2.0;
            BlendRamp(target, base, mid, attempt);
            if (TryWrite(dc.Get(), attempt)) {
                lo = mid;
                memcpy(best, attempt, sizeof(best));
                found = true;
            } else {
                hi = mid;
            }
        }

        factor = found ? lo : 0.0;
        // On either branch `best` is the ramp actually in effect: when nothing
        // is accepted it is the baseline, which preserves the ICC calibration
        // and keeps lastWritten from recording a ramp Windows rejected.
        if (!found) TryWrite(dc.Get(), best);
        memcpy(attempt, best, sizeof(best));

        st->nextProbeMs = now + kFullRangeProbeMs;

        if (!warned_) {
            warned_ = true;
            KLOG_W(L"O Windows limitou a rampa de gamma a %.0f%% do efeito pedido. "
                   L"Use 'Unlock the full gamma range' on the System tab for the full effect.",
                   factor * 100.0);
        }
    } else if (factor >= 0.999) {
        st->nextProbeMs = 0;
    }

    st->blend = factor;
    st->lastTarget.assign(target, target + 768);
    st->lastWritten.assign(attempt, attempt + 768);

    // The accepted fraction is per monitor: a secondary screen can be limited
    // while the primary is not.
    st->accepted = factor;
    if (m.isPrimary) acceptedFraction_ = factor;
    limited_ = false;
    for (const auto& kv : state_)
        if (kv.second.accepted < 0.999) { limited_ = true; break; }
}

void GammaBackend::WriteInteractive(const MonitorTarget& m, const WORD target[768], MonState* st) {
    DeviceDC dc(m.deviceName);
    if (!dc.Ok()) return;

    // Once Windows has limited a monitor, dragging stays on the last fraction
    // known to be accepted: running the binary search on every mouse move would
    // flicker between the effect, fractions of it and the baseline ramp.
    const double factor = st->lastWritten.size() == 768
                        ? Clamp(st->blend, 0.0, 1.0) : 1.0;
    if (factor <= 0.001) return;

    WORD base[768], attempt[768];
    BaseRampFor(m.key, base);
    BlendRamp(target, base, factor, attempt);

    // At most one attempt per UI frame. If the driver refuses, it keeps the last
    // valid LUT and the normal apply path searches for the best fraction once,
    // when the gesture ends.
    if (!TryWrite(dc.Get(), attempt)) return;

    st->lastWritten.assign(attempt, attempt + 768);
    st->accepted = factor;
    if (factor >= 0.999) {
        st->lastTarget.assign(target, target + 768);
    } else {
        // Forces SettleInteractive to complete the search for this exact target.
        st->lastTarget.clear();
    }
}

static bool RampIsIdentity(const WORD ramp[768]) {
    for (int i = 0; i < 256; ++i) {
        const WORD linear = (WORD)(i * 257);
        // One-step tolerance: drivers round slightly differently, which does not
        // indicate a calibration.
        if (abs((int)ramp[i] - linear) > 257 ||
            abs((int)ramp[256 + i] - linear) > 257 ||
            abs((int)ramp[512 + i] - linear) > 257)
            return false;
    }
    return true;
}

void GammaBackend::CaptureBaseline(const MonitorTarget& m) {
    if (baseline_.count(m.key)) return;   // already captured

    DeviceDC dc(m.deviceName);
    if (!dc.Ok()) return;

    std::vector<WORD> ramp(768);
    if (!::GetDeviceGammaRamp(dc.Get(), ramp.data())) {
        // With no readable ramp, linear is the best guess.
        IdentityRamp(ramp.data());
    }
    const bool custom = !RampIsIdentity(ramp.data());
    baseline_[m.key] = std::move(ramp);
    baselineCustom_[m.key] = custom;

    KLOG_I(L"Estado original de '%s' guardado (%s).", m.friendlyName.c_str(),
           custom ? L"has its own calibration - it will be preserved" : L"rampa linear");
}

bool GammaBackend::HasCustomBaseline(const std::wstring& monitorKey) const {
    auto it = baselineCustom_.find(monitorKey);
    return it != baselineCustom_.end() && it->second;
}

void GammaBackend::AdoptBaseline(const Baseline& b) {
    for (const auto& kv : b.ramps) {
        if (kv.second.size() != 768) continue;
        baseline_[kv.first] = kv.second;
        baselineCustom_[kv.first] = !RampIsIdentity(kv.second.data());
    }
}

void GammaBackend::ExportBaseline(Baseline* b) const {
    for (const auto& kv : baseline_) b->ramps[kv.first] = kv.second;
}

/// Chains the computed ramp with the baseline: the computed ramp maps input to
/// output, and the baseline carries that output to the panel. Baseline entries
/// are interpolated to avoid precision loss and color banding.
void GammaBackend::ComposeWithBaseline(const WORD ours[768], const WORD baseline[768], WORD out[768]) {
    for (int c = 0; c < 3; ++c) {
        const WORD* base = baseline + c * 256;
        for (int i = 0; i < 256; ++i) {
            const double x = ours[c * 256 + i] / 65535.0 * 255.0;
            const int lo = Clamp((int)x, 0, 255);
            const int hi = (std::min)(lo + 1, 255);
            const double f = x - lo;
            out[c * 256 + i] = (WORD)Clamp(
                (long)llround(base[lo] + (base[hi] - base[lo]) * f), 0L, 65535L);
        }
    }
}

void GammaBackend::BuildTarget(const MonitorTarget& m, const Adjustments& a,
                               WORD target[768]) {
    CaptureBaseline(m);
    if (a.GammaNeutral()) {
        // Neutral means the untouched screen, not the linear ramp.
        auto it = baseline_.find(m.key);
        if (it != baseline_.end() && it->second.size() == 768)
            memcpy(target, it->second.data(), 768 * sizeof(WORD));
        else
            IdentityRamp(target);
    } else {
        BuildRamp(a, target);
        // A monitor with its own calibration gets the effect composed on top of
        // it. The answer comes from the cached map because recomputing
        // RampIsIdentity here costs 768 comparisons per transition frame.
        auto custom = baselineCustom_.find(m.key);
        auto it = baseline_.find(m.key);
        if (it != baseline_.end() && it->second.size() == 768 &&
            custom != baselineCustom_.end() && custom->second) {
            WORD composed[768];
            ComposeWithBaseline(target, it->second.data(), composed);
            memcpy(target, composed, 768 * sizeof(WORD));
        }
    }
}

void GammaBackend::Apply(const MonitorTarget& m, const Adjustments& a) {
    if (!available_) return;

    WORD target[768];
    BuildTarget(m, a, target);

    MonState& st = state_[m.key];

    // Nothing changed and it is not yet time to retry the full effect.
    if (st.lastTarget.size() == 768 &&
        memcmp(st.lastTarget.data(), target, sizeof(target)) == 0 &&
        (st.blend >= 0.999 || NowMs() < st.nextProbeMs))
        return;

    WriteAdaptive(m, target, &st);
}

void GammaBackend::ApplyInteractive(const MonitorTarget& m, const Adjustments& a) {
    if (!available_) return;

    WORD target[768];
    BuildTarget(m, a, target);

    MonState& st = state_[m.key];
    if (st.lastTarget.size() == 768 &&
        memcmp(st.lastTarget.data(), target, sizeof(target)) == 0)
        return;

    WriteInteractive(m, target, &st);
}

void GammaBackend::Reset(const MonitorTarget& m) {
    if (!available_) return;

    // Restores exactly what the screen was, including any ICC calibration.
    // Writing the linear ramp here would discard that calibration.
    WORD ramp[768];
    auto it = baseline_.find(m.key);
    if (it != baseline_.end() && it->second.size() == 768)
        memcpy(ramp, it->second.data(), sizeof(ramp));
    else
        IdentityRamp(ramp);

    DeviceDC dc(m.deviceName);
    if (dc.Ok() && TryWrite(dc.Get(), ramp)) state_.erase(m.key);
    if (m.isPrimary) { limited_ = false; acceptedFraction_ = 1.0; }
}

void GammaBackend::Reassert(const MonitorTarget& m) {
    if (!available_) return;
    auto it = state_.find(m.key);
    if (it == state_.end() || it->second.lastWritten.size() != 768) return;

    DeviceDC dc(m.deviceName);
    if (!dc.Ok()) return;

    // Reads before writing: on most ticks nothing changed, and rewriting the
    // driver LUT every ten seconds is wasted work.
    WORD current[768];
    if (::GetDeviceGammaRamp(dc.Get(), current) &&
        memcmp(current, it->second.lastWritten.data(), sizeof(current)) == 0)
        return;

    if (!TryWrite(dc.Get(), it->second.lastWritten.data())) {
        // The ramp in effect stopped being accepted (HDR enabled, driver
        // changed). Forces a fresh fraction search on the next apply instead of
        // retrying the same rejected ramp forever.
        it->second.blend = 0.0;
        it->second.nextProbeMs = 0;
        it->second.lastTarget.clear();
    }
}

bool GammaBackend::RangeUnlocked() {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kIcmKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    DWORD value = 0, size = sizeof(value), type = 0;
    const LONG r = ::RegQueryValueExW(key, kRangeValue, nullptr, &type,
                                      reinterpret_cast<BYTE*>(&value), &size);
    ::RegCloseKey(key);
    return r == ERROR_SUCCESS && type == REG_DWORD && value >= 256;
}

bool GammaBackend::TryUnlockRange(bool unlock) {
    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_LOCAL_MACHINE, kIcmKey, 0, nullptr, 0,
                          KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        KLOG_W(L"Could not open the ICM key (administrator required).");
        return false;
    }

    LONG r;
    if (unlock) {
        DWORD value = 256;
        r = ::RegSetValueExW(key, kRangeValue, 0, REG_DWORD,
                             reinterpret_cast<const BYTE*>(&value), sizeof(value));
    } else {
        r = ::RegDeleteValueW(key, kRangeValue);
        if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
    }
    ::RegCloseKey(key);

    if (r == ERROR_SUCCESS)
        KLOG_I(L"GdiIcmGammaRange %s.", unlock ? L"liberado" : L"restaurado");
    else
        KLOG_W(L"Failed to write GdiIcmGammaRange (error %ld).", r);
    return r == ERROR_SUCCESS;
}

bool GammaBackend::NightLightActive() {
    // Night light state lives in an undocumented CloudStore blob: a 24-byte
    // header followed by tagged fields, with the pair 0x10 0x00 right after the
    // header while the mode is on. Only that window is examined, because the
    // blob also carries timestamps that can contain the pair by chance.
    static const wchar_t* kPath =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\CloudStore\\Store\\DefaultAccount\\Current\\"
        L"default$windows.data.bluelightreduction.bluelightreductionstate\\"
        L"windows.data.bluelightreduction.bluelightreductionstate";

    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;

    BYTE data[256] = {};
    DWORD size = sizeof(data), type = 0;
    const LONG r = ::RegQueryValueExW(key, L"Data", nullptr, &type, data, &size);
    ::RegCloseKey(key);
    if (r != ERROR_SUCCESS || size < 25) return false;

    // Narrow window just after the header, not the whole blob.
    for (DWORD i = 22; i + 1 < size && i < 26; ++i)
        if (data[i] == 0x10 && data[i + 1] == 0x00) return true;
    return false;
}

// HDR (SDR white level)

bool HdrBackend::Init() {
    Probe();
    // Having no HDR monitor is not an error, only nothing to do yet: the backend
    // stays loaded because HDR can be enabled without restarting the program and
    // the probe in OnDisplayChanged picks that up.
    return available_;
}

void HdrBackend::Probe() {
    int covered = 0, hdrScreens = 0;
    for (const auto& m : monitors::All()) {
        if (!m.isHdr) continue;
        ++hdrScreens;
        if (!m.hasPathInfo) continue;
        if (hdr::ReadWhiteNits(m) > 0) ++covered;
    }

    available_ = covered > 0;
    if (available_)
        details_ = Format(L"%d display(s) with HDR under SDR white level control", covered);
    else if (hdrScreens > 0)
        details_ = L"Windows did not expose the SDR white level on this machine";
    else
        details_ = L"no display with HDR on";
}

bool HdrBackend::Supports(const MonitorTarget& m) const {
    return m.isHdr && m.hasPathInfo;
}

void HdrBackend::CaptureBaseline(const MonitorTarget& m) {
    if (!Supports(m)) return;
    const int nits = hdr::ReadWhiteNits(m);
    if (nits <= 0) return;

    MonState& st = state_[m.key];
    // Does not overwrite a known baseline: from the second capture on, the value
    // read back may be one this backend wrote.
    if (!st.everChanged && st.origNits == 0) {
        st.origNits = nits;
        KLOG_I(L"HDR: %s started at %d nits of SDR white.", m.friendlyName.c_str(), nits);
    }
}

void HdrBackend::AdoptBaseline(const Baseline& b) {
    for (const auto& kv : b.hdrWhite) {
        if (kv.second <= 0) continue;
        MonState& st = state_[kv.first];
        st.origNits = kv.second;
    }
}

void HdrBackend::ExportBaseline(Baseline* b) const {
    if (!b) return;
    for (const auto& kv : state_)
        if (kv.second.origNits > 0) b->hdrWhite[kv.first] = kv.second.origNits;
}

int HdrBackend::CurrentNits(const std::wstring& monitorKey) const {
    auto it = state_.find(monitorKey);
    if (it == state_.end()) return 0;
    return it->second.lastWritten > 0 ? it->second.lastWritten : it->second.origNits;
}

void HdrBackend::Apply(const MonitorTarget& m, const Adjustments& a) {
    if (!Supports(m)) return;

    CaptureBaseline(m);
    MonState& st = state_[m.key];
    // Without a baseline there is no anchor: 100 must mean the level the screen
    // already had, never the maximum.
    const int base = st.origNits > 0 ? st.origNits : hdr::kMinWhiteNits;

    const int want = (int)llround(base * Clamp(a.brightness, 0.0, 150.0) / 100.0);
    const int clamped = (int)Clamp((double)want, (double)hdr::kMinWhiteNits,
                                   (double)hdr::kMaxWhiteNits);

    if (st.lastWritten == clamped) return;
    if (!hdr::WriteWhiteNits(m, clamped)) {
        KLOG_D(L"HDR: o Windows recusou %d nits em %s.", clamped, m.friendlyName.c_str());
        return;
    }
    st.lastWritten = clamped;
    st.everChanged = true;
}

void HdrBackend::Reset(const MonitorTarget& m) {
    auto it = state_.find(m.key);
    if (it == state_.end() || it->second.origNits <= 0) return;
    // Only restores values this backend actually changed.
    if (!it->second.everChanged) return;
    if (!m.hasPathInfo) return;

    if (hdr::WriteWhiteNits(m, it->second.origNits)) {
        it->second.lastWritten = 0;
        it->second.everChanged = false;
    }
}

void HdrBackend::ForceRestore() {
    for (auto& kv : state_) {
        if (kv.second.origNits <= 0) continue;
        const MonitorTarget* m = monitors::ByKey(kv.first);
        if (!m || !m->hasPathInfo) continue;
        if (hdr::WriteWhiteNits(*m, kv.second.origNits)) {
            kv.second.lastWritten = 0;
            kv.second.everChanged = false;
        }
    }
}

void HdrBackend::Reassert(const MonitorTarget& m) {
    if (!Supports(m)) return;
    auto it = state_.find(m.key);
    if (it == state_.end() || it->second.lastWritten <= 0) return;

    // Reads before writing: on most ticks nothing changed. Resume from suspend
    // and video mode changes are the cases where it does.
    const int now = hdr::ReadWhiteNits(m);
    if (now == it->second.lastWritten) return;
    hdr::WriteWhiteNits(m, it->second.lastWritten);
}

// Global color matrix

// Magnification API structure, declared here because the DLL is loaded at run
// time so the program still starts on machines where it is missing.
struct MagColorEffect { float transform[5][5]; };

static MagColorEffect ToMagEffect(const Mat5& m) {
    MagColorEffect e{};
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 5; ++j)
            e.transform[i][j] = m.m[i * 5 + j];
    return e;
}

bool MagnifyBackend::Init() {
    if (!lib_.Load(L"magnification.dll")) {
        details_ = L"magnification.dll not found";
        return false;
    }

    pMagInit_   = lib_.Get<PfnMagInitialize>("MagInitialize");
    pMagUninit_ = lib_.Get<PfnMagUninitialize>("MagUninitialize");
    pMagSet_    = lib_.Get<PfnMagSetFullscreenColorEffect>("MagSetFullscreenColorEffect");
    pMagGet_    = lib_.Get<PfnMagGetFullscreenColorEffect>("MagGetFullscreenColorEffect");

    if (!pMagInit_ || !pMagSet_) {
        details_ = L"the DLL exposes no fullscreen color effect";
        return false;
    }

    if (!pMagInit_()) {
        details_ = L"MagInitialize failed";
        return false;
    }
    initialized_ = true;

    // Reads before writing: MagSetFullscreenColorEffect is the same mechanism as
    // the Windows accessibility color filters and overwrites them instead of
    // failing, so an active filter has to be detected and preserved.
    if (pMagGet_) {
        MagColorEffect cur{};
        if (pMagGet_(&cur)) {
            for (int i = 0; i < 5; ++i)
                for (int j = 0; j < 5; ++j) original_.m[i * 5 + j] = cur.transform[i][j];
            hasOriginal_ = true;
        }
    }

    if (hasOriginal_ && !original_.NearlyEquals(Mat5::Identity())) {
        details_ = L"the Windows Color filters are active - universal "
                   L"saturation stays off so as not to undo them";
        if (pMagUninit_) pMagUninit_();
        initialized_ = false;
        return false;
    }

    // The backend is only reported available once the effect is accepted.
    const MagColorEffect identity = ToMagEffect(Mat5::Identity());
    if (!pMagSet_(&identity)) {
        details_ = L"effect refused (is another magnifier program active?)";
        if (pMagUninit_) pMagUninit_();
        initialized_ = false;
        return false;
    }

    last_ = Mat5::Identity();
    hasLast_ = true;
    details_ = L"global effect active (applies to every monitor)";
    available_ = true;
    return true;
}

Mat5 MagnifyBackend::BuildMatrix(const Adjustments& a, bool includeSaturation,
                                 bool includeLevels) {
    Mat5 m = Mat5::Identity();
    if (includeSaturation && std::fabs(a.saturation - 100.0) > 0.01)
        m = m * Mat5::Saturation(a.saturation / 100.0);
    if (std::fabs(a.hue) > 0.01)
        m = m * Mat5::Hue(a.hue);
    // Brightness, contrast and temperature can also go through the color matrix,
    // but only when the gamma ramp does not apply (HDR enabled); otherwise the
    // same adjustment would be applied twice.
    if (includeLevels)
        m = m * Mat5::FromAdjustments(a);
    if (a.invert)
        m = m * Mat5::Invert();
    return m;
}

void MagnifyBackend::SetMatrix(const Mat5& m) {
    if (hasLast_ && last_.NearlyEquals(m)) return;
    const MagColorEffect e = ToMagEffect(m);
    if (pMagSet_(&e)) { last_ = m; hasLast_ = true; }
    else KLOG_D(L"MagSetFullscreenColorEffect recusou a matriz.");
}

void MagnifyBackend::Apply(const MonitorTarget&, const Adjustments& a) {
    if (!available_) return;
    SetMatrix(BuildMatrix(a, true, compensateGamma_));
}

void MagnifyBackend::Reset(const MonitorTarget&) {
    if (!available_) return;
    // Restores the effect that was in place, not the identity matrix.
    SetMatrix(hasOriginal_ ? original_ : Mat5::Identity());
}

void MagnifyBackend::Reassert() {
    if (!available_ || !hasLast_) return;

    // With no effect of its own to defend, nothing is reasserted: forcing the
    // identity matrix on every tick would block the Windows color filters.
    if (last_.NearlyEquals(Mat5::Identity())) return;

    // Another process may have taken over the fullscreen effect; the matrix is
    // only rewritten when the one in effect is not the expected one.
    if (pMagGet_) {
        MagColorEffect current{};
        if (pMagGet_(&current)) {
            Mat5 cur{};
            for (int i = 0; i < 5; ++i)
                for (int j = 0; j < 5; ++j) cur.m[i * 5 + j] = current.transform[i][j];
            if (cur.NearlyEquals(last_)) return;
        }
    }
    const MagColorEffect e = ToMagEffect(last_);
    pMagSet_(&e);
}

void MagnifyBackend::Shutdown() {
    if (!initialized_) return;
    // Restores the original effect, not the identity matrix.
    const MagColorEffect restore = ToMagEffect(hasOriginal_ ? original_ : Mat5::Identity());
    if (pMagSet_) pMagSet_(&restore);
    if (pMagUninit_) pMagUninit_();
    initialized_ = false;
    available_ = false;
    hasLast_ = false;
}

// Dimming overlay

static const wchar_t* kOverlayClass = L"ZdisplayDimOverlay";

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND: {
            RECT rc;
            ::GetClientRect(hwnd, &rc);
            HBRUSH b = ::CreateSolidBrush(RGB(0, 0, 0));
            ::FillRect(reinterpret_cast<HDC>(wp), &rc, b);
            ::DeleteObject(b);
            return 1;
        }
        // The window must never receive clicks or focus.
        case WM_NCHITTEST:   return HTTRANSPARENT;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        default: break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

bool OverlayBackend::Init() {
    if (!classRegistered_) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = OverlayWndProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = kOverlayClass;
        if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            details_ = L"could not register the overlay window class";
            return false;
        }
        classRegistered_ = true;
    }
    details_ = L"ready (only comes into play with dimming above zero)";
    available_ = true;
    return true;
}

HWND OverlayBackend::GetOrCreate(const MonitorTarget& m) {
    auto it = windows_.find(m.key);
    if (it != windows_.end() && ::IsWindow(it->second)) return it->second;

    const int w = m.bounds.right - m.bounds.left;
    const int h = m.bounds.bottom - m.bounds.top;

    HWND hwnd = ::CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
        WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        kOverlayClass, L"", WS_POPUP,
        m.bounds.left, m.bounds.top, w, h,
        nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);

    if (!hwnd) {
        KLOG_W(L"Could not create the dimming layer (error %lu).", ::GetLastError());
        return nullptr;
    }
    windows_[m.key] = hwnd;
    return hwnd;
}

void OverlayBackend::Apply(const MonitorTarget& m, const Adjustments& a) {
    if (!available_ || suspended_) return;

    const double dim = Clamp(a.dim, 0.0, 90.0) / 100.0;

    if (dim <= 0.001) {
        auto it = windows_.find(m.key);
        if (it != windows_.end() && ::IsWindow(it->second) && ::IsWindowVisible(it->second))
            ::ShowWindow(it->second, SW_HIDE);
        return;
    }

    HWND hwnd = GetOrCreate(m);
    if (!hwnd) return;

    const BYTE alpha = (BYTE)llround(dim * 255.0);
    ::SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA);

    RECT cur{};
    ::GetWindowRect(hwnd, &cur);
    const bool moved = memcmp(&cur, &m.bounds, sizeof(RECT)) != 0;

    if (!::IsWindowVisible(hwnd) || moved) {
        ::SetWindowPos(hwnd, HWND_TOPMOST,
                       m.bounds.left, m.bounds.top,
                       m.bounds.right - m.bounds.left,
                       m.bounds.bottom - m.bounds.top,
                       SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

void OverlayBackend::Reset(const MonitorTarget& m) {
    auto it = windows_.find(m.key);
    if (it != windows_.end() && ::IsWindow(it->second)) ::ShowWindow(it->second, SW_HIDE);
}

void OverlayBackend::SyncMonitors(const std::vector<MonitorTarget>& alive) {
    for (auto it = windows_.begin(); it != windows_.end(); ) {
        bool stillThere = false;
        for (const auto& m : alive)
            if (m.key == it->first) { stillThere = true; break; }

        if (stillThere) { ++it; continue; }

        if (::IsWindow(it->second)) ::DestroyWindow(it->second);
        hiddenBySuspend_.erase(it->first);
        it = windows_.erase(it);
    }
}

void OverlayBackend::Reassert() {
    if (!available_ || suspended_) return;
    // Games and Explorer can move above the overlay, so topmost is reasserted.
    for (auto& kv : windows_) {
        if (!::IsWindow(kv.second) || !::IsWindowVisible(kv.second)) continue;
        ::SetWindowPos(kv.second, HWND_TOPMOST, 0, 0, 0, 0,
                       SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
    }
}

void OverlayBackend::Suspend(bool on) {
    if (suspended_ == on) return;
    suspended_ = on;
    // While the dark-screen confirmation is up the overlay is hidden: it is
    // topmost and the watchdog would push it over the very window the user has
    // to read. On resume only the windows hidden here reappear, so overlays that
    // Apply hid for dim = 0 stay hidden.
    if (on) {
        hiddenBySuspend_.clear();
        for (auto& kv : windows_) {
            if (!::IsWindow(kv.second) || !::IsWindowVisible(kv.second)) continue;
            hiddenBySuspend_.insert(kv.first);
            ::ShowWindow(kv.second, SW_HIDE);
        }
    } else {
        for (auto& kv : windows_) {
            if (!::IsWindow(kv.second)) continue;
            if (hiddenBySuspend_.count(kv.first) == 0) continue;
            ::SetWindowPos(kv.second, HWND_TOPMOST, 0, 0, 0, 0,
                           SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        }
        hiddenBySuspend_.clear();
    }
}

void OverlayBackend::Shutdown() {
    for (auto& kv : windows_)
        if (::IsWindow(kv.second)) ::DestroyWindow(kv.second);
    windows_.clear();
    available_ = false;
}

}  // namespace zdisplay
