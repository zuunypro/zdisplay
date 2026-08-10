#include "engine.h"

namespace zdisplay {

void Engine::CaptureBaseline(bool screenIsTrustworthy) {
    for (const auto& m : monitors::All()) {
        gamma_.CaptureBaseline(m);
        hdr_.CaptureBaseline(m);
    }

    Baseline b;
    gamma_.ExportBaseline(&b);
    hdr_.ExportBaseline(&b);
    ddc_.ExportBaseline(&b);
    backlight_.ExportBaseline(&b);
    nvidia_.ExportBaseline(&b);
    amd_.ExportBaseline(&b);
    if (b.Empty()) return;

    // If the previous session crashed and the on-disk baseline is unreadable, the
    // current screen holds that session's adjustments, not the original state;
    // recording it as the baseline would make the loss permanent, ICC included.
    if (!screenIsTrustworthy) {
        KLOG_W(L"Não vou regravar o estado original: a execucao anterior travou e o "
               L"baseline em disco não pode ser lido, então a tela de agora não é "
               L"confiavel como referência. O reset usara o que já estava guardado.");
        return;
    }

    SaveBaseline(b);
}

bool Engine::RecoverFromCrash() {
    Baseline b;
    if (!LoadBaseline(&b) || b.Empty()) return false;

    KLOG_W(L"A execucao anterior não terminou direito. Devolvendo a tela ao "
           L"estado original guardado antes de qualquer ajuste.");

    gamma_.AdoptBaseline(b);
    hdr_.AdoptBaseline(b);
    ddc_.AdoptBaseline(b);
    backlight_.AdoptBaseline(b);
    nvidia_.AdoptBaseline(b);
    amd_.AdoptBaseline(b);

    for (const auto& m : monitors::All()) {
        gamma_.Reset(m);
        magnify_.Reset(m);
        overlay_.Reset(m);
        nvidia_.Reset(m);
        amd_.Reset(m);
    }
    // The hardware was changed by the previous run, so this session's dirty flags
    // are clear and the restore is forced anyway. The SDR white level is included
    // because it outlives the process and would stay at the crashed session's value.
    hdr_.ForceRestore();
    ddc_.ForceRestore();
    backlight_.ForceRestore();
    return true;
}

void Engine::EmergencyRestore() {
    KLOG_W(L"RESTAURAÇÃO DE EMERGÊNCIA acionada — devolvendo a tela e pausando.");
    if (transitioning_ && host_) { ::KillTimer(host_, TIMER_TRANSITION); transitioning_ = false; }
    enabled_ = false;
    ResetAll();
    NotifyChanged();
}

double Engine::CurrentLuminance() const {
    const Profile* p = Active();
    if (!enabled_ || !p) return 1.0;

    // Uses the target value rather than the mid-transition value, and the darkest
    // monitor wins. The minimum is computed by hand because std::min(x, NaN)
    // returns x, which would let a NaN slip past the black-screen guard.
    double worst = 1.0;
    for (const auto& m : monitors::All()) {
        // The guard judges what actually reaches the screen, after the vision
        // layer, not what the profile asks for before it.
        const double l = Effective(*p, m).EffectiveLuminance();
        if (!(l == l)) return 0.0;      // NaN: treat as a dark screen
        if (l < worst) worst = l;
    }
    return worst;
}

void Engine::Initialize(HWND hostWindow) {
    host_ = hostWindow;
    monitors::Refresh();

    // Migrates monitor keys written before identity came from EDID. It runs on
    // every start without writing anything and only covers monitors connected
    // now, so entries for disconnected monitors stay intact in the file.
    monitors::MigrateKeys(cfg_);
    UpdateVision();
    // Must run before any backend starts: DDC/CI discovery reads this table, and
    // a blocking rule has to be in effect for the first probe, since that probe
    // is what hangs the machine on defective models.
    SetUserMonitorQuirks(cfg_->monitorQuirks);
    ddc_.SetMonitorModes(cfg_->ddcMonitorModes);

    // Registration order is application order.
    struct Entry { Backend* b; bool enabled; };
    const Entry entries[] = {
        { &gamma_,     true },
        // Stands in for the gamma ramp on displays where the ramp has no effect,
        // so it must be ready before anything is applied.
        { &hdr_,       true },
        { &nvidia_,    cfg_->enableVendorApis },
        { &amd_,       cfg_->enableVendorApis },
        { &magnify_,   cfg_->enableMagnification },
        { &ddc_,       cfg_->enableDdcCi },
        { &backlight_, cfg_->enableBacklight },
        { &overlay_,   cfg_->enableOverlay },
    };

    for (const auto& e : entries) {
        if (!e.enabled) {
            KLOG_I(L"Backend '%s' desligado nas configurações.", e.b->Name());
            continue;
        }
        const bool ok = e.b->Init();
        KLOG_I(L"Backend '%s': %s — %s", e.b->Name(),
               ok ? L"disponivel" : L"indisponivel", e.b->Details().c_str());
        all_.push_back(e.b);
    }

    if (GammaBackend::NightLightActive()) {
        KLOG_W(L"A Luz noturna do Windows esta ligada e disputa a mesma rampa de gamma. "
               L"Se as cores oscilarem, desligue-a nas Configurações do Windows.");
    }

    // Order matters: restore the screen if the previous run crashed, then capture
    // this session's baseline.
    const bool wasDirty = SessionWasDirty();
    const bool recovered = wasDirty && RecoverFromCrash();
    // A dirty session whose recovery failed means the current screen is not a
    // valid reference; see CaptureBaseline.
    CaptureBaseline(!wasDirty || recovered);
    SessionBegin();

    UpdateScheduleTimer();
    UpdateWatchdogInterval();

    // Backlight Init does not wait on WMI; when the response arrives, the tick
    // below completes the backend, the baseline and the profile application.
    if (backlight_.InitPending())
        ::SetTimer(host_, TIMER_BACKLIGHT_POLL, 500, nullptr);

    Profile* start = cfg_->Default();
    activeName_ = start ? start->name : std::wstring();
    SnapTo(start);
    KLOG_I(L"Motor iniciado com o perfil '%s'.%s",
           start ? start->name.c_str() : L"(nenhum)",
           recovered ? L" (tela recuperada após encerramento anormal)" : L"");
}

void Engine::Shutdown() {
    if (host_) {
        ::KillTimer(host_, TIMER_TRANSITION);
        ::KillTimer(host_, TIMER_WATCHDOG);
        ::KillTimer(host_, TIMER_SCHEDULE);
        ::KillTimer(host_, TIMER_REDISCOVER);
        ::KillTimer(host_, TIMER_BACKLIGHT_POLL);
        ::KillTimer(host_, TIMER_DISPLAYSTATE);
        host_ = nullptr;
    }
    if (cfg_ && cfg_->restoreOnExit) ResetAll();
    for (Backend* b : all_) b->Shutdown();
    all_.clear();

    // The exit counts as clean only if the hardware also came back. Keeping the
    // marker when a monitor refused the final restore makes the next run retry
    // from the persisted baseline.
    if (!ddc_.RestoreIncomplete()) SessionEnd();
    else KLOG_W(L"Sessão mantida como incompleta porque o restauro DDC/CI falhou.");
}

void Engine::NotifyChanged() {
    if (onStateChanged) onStateChanged(stateContext);
}

// Profile resolution

Profile* Engine::Resolve() {
    // 1) manual selection always wins
    if (!manualProfile_.empty()) {
        if (Profile* p = cfg_->Find(manualProfile_)) return p;
        manualProfile_.clear();
    }

    // 2) foreground application rule
    if (cfg_->enableAppRules && !foreground_.empty()) {
        const AppRule* best = nullptr;
        for (const auto& r : cfg_->appRules) {
            if (!r.Matches(foreground_)) continue;
            // A rule pointing at a missing profile must not mask another rule
            // that matches; the target can be absent after a hand-edited INI or
            // a configuration copied from another machine.
            if (!cfg_->Find(r.profile)) continue;
            if (!best || r.priority > best->priority ||
                (r.priority == best->priority && r.Specificity() > best->Specificity()))
                best = &r;
        }
        if (best) {
            if (Profile* p = cfg_->Find(best->profile)) return p;
        }
    }

    // 3) time rule, highest priority first, as with foreground application rules.
    if (cfg_->enableSchedule) {
        SYSTEMTIME now;
        ::GetLocalTime(&now);
        const SolarContext solar = cfg_->Solar();
        const ScheduleRule* best = nullptr;
        for (const auto& r : cfg_->scheduleRules) {
            if (!r.Matches(now, solar)) continue;
            if (!cfg_->Find(r.profile)) continue;
            if (!best || r.priority > best->priority) best = &r;
        }
        if (best) {
            if (Profile* p = cfg_->Find(best->profile)) return p;
        }
    }

    // 4) default
    return cfg_->Default();
}

void Engine::Recompute(bool animate) {
    Profile* wanted = Resolve();
    if (!wanted) return;
    // Compares by name: after an erase the following element takes over the same
    // address, so pointer comparison cannot tell one profile from another.
    if (IEquals(wanted->name, activeName_)) return;

    KLOG_I(L"Perfil '%s' -> '%s'",
           activeName_.empty() ? L"(nenhum)" : activeName_.c_str(), wanted->name.c_str());
    activeName_ = wanted->name;
    if (animate) BeginTransition(wanted); else SnapTo(wanted);
    NotifyChanged();
}

void Engine::SnapToActive() {
    // Called by the UI after the profile list changes; if the active profile was
    // deleted, resolution falls back to the default instead of leaving no target.
    Profile* p = Active();
    if (!p) {
        p = Resolve();
        activeName_ = p ? p->name : std::wstring();
    }
    if (p) SnapTo(p);
    NotifyChanged();
}

void Engine::OnProfileRenamed(const std::wstring& oldName, const std::wstring& newName) {
    if (IEquals(activeName_, oldName)) activeName_ = newName;
    if (IEquals(manualProfile_, oldName)) manualProfile_ = newName;
}

void Engine::OnProfilesChanged() {
    // Reselect when the active profile is no longer in the list.
    if (!Active()) {
        Profile* p = Resolve();
        activeName_ = p ? p->name : std::wstring();
        if (p) SnapTo(p);
    }
    NotifyChanged();
}

void Engine::OnForegroundProcess(const std::wstring& processName) {
    if (IEquals(processName, foreground_)) return;
    foreground_ = processName;
    KLOG_D(L"Primeiro plano: %s", foreground_.empty() ? L"(nao identificado)"
                                                       : foreground_.c_str());
    Recompute();
}

// Control

void Engine::SetManualProfile(const std::wstring& name) {
    Profile* p = cfg_->Find(name);
    if (!p) return;
    manualProfile_ = p->name;
    activeName_ = p->name;
    BeginTransition(p);
    NotifyChanged();
}

void Engine::ClearManualProfile() {
    manualProfile_.clear();
    Recompute();
    NotifyChanged();
}

void Engine::SetEnabled(bool on) {
    if (enabled_ == on) return;
    enabled_ = on;
    if (on) {
        KLOG_I(L"Zdisplay reativado.");
        BeginTransition(Active());
    } else {
        KLOG_I(L"Zdisplay pausado — restaurando a tela.");
        if (transitioning_) { ::KillTimer(host_, TIMER_TRANSITION); transitioning_ = false; }
        ResetAll();
    }
    NotifyChanged();
}

void Engine::PreviewOriginal(bool on) {
    if (previewing_ == on) return;
    previewing_ = on;

    if (on) {
        if (transitioning_ && host_) { ::KillTimer(host_, TIMER_TRANSITION); transitioning_ = false; }
        // Restores everything to the original state, monitor hardware included, so
        // the comparison shows the untouched screen. Each DDC/CI reset costs a
        // 50-200 ms round trip per monitor and one press performs two of them, on
        // panels that may store brightness in non-volatile memory.
        ResetAll();
    } else {
        previewing_ = false;
        ApplyNow();
    }
}

void Engine::ApplyNow() {
    Profile* act = Active();
    if (!enabled_ || !act || previewing_) return;
    if (transitioning_) { ::KillTimer(host_, TIMER_TRANSITION); transitioning_ = false; }
    // A full apply covers whatever a slider drag deferred, so the pending
    // interactive timer is no longer needed.
    if (host_) ::KillTimer(host_, TIMER_INTERACTIVE);

    for (const auto& m : monitors::All()) {
        const Adjustments a = Effective(*act, m);
        shown_[m.key] = a;
        ApplyToMonitor(m, a, false);
    }
    ApplyGlobalMatrix();

    // Notifies observers (UI, dark-screen guard); without this, adjustments made
    // from the sliders or the command channel would go unnoticed.
    NotifyChanged();
}

void Engine::ApplyInteractive() {
    Profile* act = Active();
    if (!enabled_ || !act || previewing_) return;
    if (transitioning_) { ::KillTimer(host_, TIMER_TRANSITION); transitioning_ = false; }

    for (const auto& m : monitors::All()) {
        const Adjustments a = Effective(*act, m);
        shown_[m.key] = a;
        // The gamma ramp skips a rewrite when the target is unchanged and the
        // overlay only touches its window when the dim value changes, so neither
        // flickers under rapid repeats.
        gamma_.ApplyInteractive(m, a);
        overlay_.Apply(m, a);
    }

    // Rescheduled on every event so the timer only fires once movement stops;
    // SetTimer on the same id restarts the countdown instead of adding a timer.
    if (host_) ::SetTimer(host_, TIMER_INTERACTIVE, kInteractiveSettleMs, nullptr);

    // NotifyChanged is deliberately not called here: the callback reloads every
    // control, including TBM_SETPOS on the slider still under mouse capture.
    // SettleInteractive notifies once, after the drag stops.
}

Adjustments Engine::Effective(const Profile& p, const MonitorTarget& m) const {
    const Adjustments& base = p.For(m.key);

    // During a preview the vision layer applies even when disabled: the preview
    // exists to show what enabling it would do.
    if (visionPreview_ >= 0.0) {
        Vision v = cfg_->vision;
        v.enabled = true;
        return ApplyVision(base, visionPreview_, v);
    }

    if (!cfg_->vision.enabled) return base;
    return ApplyVision(base, visionNight_, cfg_->vision);
}

void Engine::PreviewVision(double nightFraction) {
    visionPreview_ = Clamp(nightFraction, 0.0, 1.0);
    ApplyNow();
}

void Engine::EndPreviewVision() {
    if (visionPreview_ < 0.0) return;
    visionPreview_ = -1.0;
    ApplyNow();
}

void Engine::UpdateVision() {
    const double before = visionNight_;

    if (cfg_->vision.enabled) {
        SYSTEMTIME now;
        ::GetLocalTime(&now);
        visionNight_ = NightFraction(now, cfg_->vision, cfg_->Solar());
    } else {
        visionNight_ = 0.0;
    }

    // Reapplies only when the change is visible: the recompute runs every 20 s and
    // a one-hour transition moves 1.7% in that interval, so without this threshold
    // the gamma ramp would be rewritten constantly for almost nothing.
    if (std::fabs(visionNight_ - before) > 0.005) ApplyNow();
}

void Engine::MirrorInternalBrightness() {
    if (!cfg_->mirrorInternalBrightness) return;
    if (!backlight_.Available() || !ddc_.Available()) return;

    // Consumes what the previous pass found and requests the next read. Both are
    // asynchronous by design: the WMI query runs on the queue thread, never on
    // the UI thread.
    int level = -1;
    const bool changed = backlight_.TakeExternalChange(&level);
    backlight_.RequestRefresh();
    if (!changed || level < 0) return;

    Profile* act = Active();
    if (!act) return;

    int mirrored = 0;
    for (const auto& m : monitors::All()) {
        if (m.isInternal) continue;              // the display the change came from
        if (!ddc_.Supports(m.key)) continue;

        // A profile that already manages physical brightness wins: mirroring on
        // top of it would make the two undo each other every tick. The HDR
        // fallback counts as managing, since it drives brightness over DDC/CI.
        Adjustments a = HdrBrightnessFallback(m, Effective(*act, m));
        if (a.hwBrightness >= 0) continue;

        a.hwBrightness = level;
        ddc_.Apply(m, a);
        ++mirrored;
    }

    if (mirrored > 0)
        KLOG_I(L"Brilho do painel interno mudou para %d%% pelas teclas; espelhado "
               L"em %d monitor(es) externo(s).", level, mirrored);
}

void Engine::SettleInteractive() {
    if (host_) ::KillTimer(host_, TIMER_INTERACTIVE);
    ApplyNow();
}

void Engine::SnapTo(Profile* p) {
    if (!p || previewing_) return;
    if (transitioning_) { ::KillTimer(host_, TIMER_TRANSITION); transitioning_ = false; }

    for (const auto& m : monitors::All()) {
        const Adjustments a = Effective(*p, m);
        shown_[m.key] = a;
        if (enabled_) ApplyToMonitor(m, a, false);
    }
    if (enabled_) ApplyGlobalMatrix();
}

void Engine::BeginTransition(Profile* p) {
    // previewing_ is part of the guard so the foreground hook, the schedule timer
    // or a channel command cannot cancel a comparison that is still in progress.
    if (!p || !enabled_ || previewing_) return;

    transitionMs_ = Clamp(p->transitionMs, 0, 10000);
    if (transitionMs_ <= 20) { SnapTo(p); return; }

    from_.clear();
    to_.clear();
    for (const auto& m : monitors::All()) {
        auto it = shown_.find(m.key);
        from_[m.key] = it != shown_.end() ? it->second : Adjustments{};
        to_[m.key] = Effective(*p, m);
    }

    transitionStartMs_ = NowMs();
    transitioning_ = true;
    // Windows timers round up to multiples of the 15.6 ms tick: asking for 16 ms
    // lands on the next multiple (~31 ms, ~32 fps), while 15 ms stays on the first.
    ::SetTimer(host_, TIMER_TRANSITION, 15, nullptr);   // 1 tick (~64 fps)
}

void Engine::OnTimer(UINT_PTR id) {
    switch (id) {
        // One-shot: the slider drag stopped, so run what it deferred.
        case TIMER_INTERACTIVE:
            SettleInteractive();
            return;

        // One-shot: display notifications stopped arriving, so the state is settled.
        case TIMER_DISPLAYSTATE:
            SettleDisplayState();
            return;

        case TIMER_TRANSITION: {
            if (!enabled_ || !transitioning_) {
                ::KillTimer(host_, TIMER_TRANSITION);
                transitioning_ = false;
                return;
            }
            double t = transitionMs_ <= 0 ? 1.0
                     : Clamp((NowMs() - transitionStartMs_) / transitionMs_, 0.0, 1.0);
            const double eased = Ease(t);

            const bool last = t >= 1.0;

            for (const auto& m : monitors::All()) {
                auto a = from_.find(m.key);
                auto b = to_.find(m.key);
                if (a == from_.end() || b == to_.end()) continue;
                const Adjustments cur = Adjustments::Blend(a->second, b->second, eased);
                shown_[m.key] = cur;
                // On the last frame the hardware is driven by the block below with
                // the exact target value; passing true here avoids sending the same
                // DDC/CI command twice per transition.
                ApplyToMonitor(m, cur, true);
            }
            ApplyGlobalMatrix();

            if (last) {
                ::KillTimer(host_, TIMER_TRANSITION);
                transitioning_ = false;
                // Slow hardware commands are sent only at the final value.
                for (const auto& m : monitors::All()) {
                    auto b = to_.find(m.key);
                    if (b != to_.end()) ApplyHardware(m, b->second);
                }
                NotifyChanged();
            }
            break;
        }

        case TIMER_WATCHDOG: {
            if (!enabled_) break;
            // Another program (Windows Night light, a driver, a game exiting) can
            // overwrite the adjustments, so they are reasserted here.
            for (const auto& m : monitors::All()) {
                gamma_.Reassert(m);
                // The SDR white level reverts to the system default on its own
                // after a display mode change or a resume from sleep.
                hdr_.Reassert(m);
            }
            magnify_.Reassert();
            overlay_.Reassert();
            MirrorInternalBrightness();
            break;
        }

        case TIMER_SCHEDULE:
            // The vision layer follows the clock, so it rides this same tick: 20 s
            // of granularity over a one-hour ramp is finer than the eye resolves.
            UpdateVision();
            if (cfg_->enableSchedule) Recompute();
            break;

        case TIMER_BACKLIGHT_POLL: {
            if (backlight_.PollReady()) {
                // The backend has just become available: the baseline saved at
                // startup lacks the internal panel's original brightness, so it is
                // completed here and the profile reapplied to deliver hwBrightness.
                Baseline b;
                if (LoadBaseline(&b)) {
                    backlight_.ExportBaseline(&b);
                    SaveBaseline(b);
                }
                KLOG_I(L"Backend '%s' ficou disponível: %s",
                       backlight_.Name(), backlight_.Details().c_str());
                ApplyNow();
                NotifyChanged();
            }
            if (!backlight_.InitPending())
                ::KillTimer(host_, TIMER_BACKLIGHT_POLL);
            break;
        }

        case TIMER_REDISCOVER: {
            ::KillTimer(host_, TIMER_REDISCOVER);
            RediscoverHardware();
            static const UINT kNextDelayMs[] = {1000, 4000, 10000, 15000};
            ++rediscoveryStep_;
            if (rediscoveryStep_ >= 0 && rediscoveryStep_ < (int)_countof(kNextDelayMs))
                ::SetTimer(host_, TIMER_REDISCOVER, kNextDelayMs[rediscoveryStep_], nullptr);
            else
                rediscoveryStep_ = -1;
            break;
        }

        default: break;
    }
}

void Engine::UpdateScheduleTimer() {
    if (!host_) return;
    ::KillTimer(host_, TIMER_SCHEDULE);
    // While suspended, SetSessionActive is what restarts the ticks; without this
    // guard a configuration change would rearm the timer during a user switch.
    if (!working_) return;
    // The vision layer needs this same tick to follow the clock, so the timer runs
    // whenever either the schedule or the vision layer is enabled.
    if (cfg_->enableSchedule || cfg_->vision.enabled)
        ::SetTimer(host_, TIMER_SCHEDULE, 20000, nullptr);
}

void Engine::SetSessionActive(bool active) {
    if (sessionActive_ == active) return;
    sessionActive_ = active;
    KLOG_I(active ? L"Sessão em primeiro plano." : L"Sessão em segundo plano.");
    UpdateSuspension();
}

void Engine::SetDisplayOn(bool on) {
    if (!host_) return;
    displayPending_ = on;
    // Rearmed on every notification: the state counts as real only after 4 s of
    // silence. A monitor that flaps its link during DDC/CI traffic emits a burst
    // of notifications that cancel out here.
    ::SetTimer(host_, TIMER_DISPLAYSTATE, 4000, nullptr);
}

void Engine::SettleDisplayState() {
    if (host_) ::KillTimer(host_, TIMER_DISPLAYSTATE);
    if (displayOn_ == displayPending_) return;
    displayOn_ = displayPending_;
    KLOG_I(displayOn_ ? L"Tela acesa." : L"Tela apagada.");
    UpdateSuspension();
}

void Engine::UpdateSuspension() {
    const bool work = sessionActive_ && displayOn_;
    if (work == working_ || !host_) return;
    working_ = work;

    if (!work) {
        // The adjustment stays in place; only the periodic work stops. Otherwise
        // the watchdog drives I2C to a monitor another user's session owns, and
        // with the display off every ramp write wakes the compositor for nothing.
        ::KillTimer(host_, TIMER_WATCHDOG);
        ::KillTimer(host_, TIMER_SCHEDULE);
        KLOG_I(L"Reafirmação suspensa.");
        return;
    }

    KLOG_I(L"Reafirmação retomada.");
    // OnResume is deliberately not called here: rediscovery talks DDC/CI, which
    // flaps the link into another display-off notification and loops. Recompute
    // and ApplyNow suffice, because the DDC/CI apply is uncached and the backend
    // rediscovers a dead handle on its own. Resume from sleep still calls OnResume.
    Recompute(false);
    ApplyNow();
    UpdateWatchdogInterval();
    UpdateScheduleTimer();
}

void Engine::UpdateWatchdogInterval() {
    if (!host_) return;
    ::KillTimer(host_, TIMER_WATCHDOG);
    if (!working_) return;   // see UpdateScheduleTimer
    if (cfg_->watchdogSeconds > 0) {
        const UINT ms = (UINT)((std::max)(2, cfg_->watchdogSeconds)) * 1000u;
        ::SetTimer(host_, TIMER_WATCHDOG, ms, nullptr);
    }
}

// Application

void Engine::ApplyToMonitor(const MonitorTarget& m, const Adjustments& a, bool duringTransition) {
    gamma_.Apply(m, a);

    // Where HDR is on, the ramp above has no effect and brightness comes from
    // here. On non-HDR displays Supports() is false and this does nothing;
    // otherwise brightness would be applied twice.
    hdr_.Apply(m, a);

    // When any monitor lacks vendor hue support, the universal matrix carries hue
    // and the GPU backends must not apply it as well, or supported monitors would
    // receive the adjustment twice.
    const bool vendorHue = VendorHueAvailable();
    nvidia_.SetHandleHue(vendorHue);
    amd_.SetHandleHue(vendorHue);

    nvidia_.Apply(m, a);

    amd_.SetHandleSaturation(UseVendorSaturation());
    amd_.Apply(m, a);

    overlay_.Apply(m, a);

    // DDC/CI and backlight are slow: never during a transition.
    if (!duringTransition) ApplyHardware(m, a);
}

void Engine::ApplyHardware(const MonitorTarget& m, const Adjustments& a) {
    ddc_.Apply(m, HdrBrightnessFallback(m, a));
    backlight_.Apply(m, a);
}

std::vector<std::wstring> Engine::MonitorCoverage(const MonitorTarget& m) const {
    std::vector<std::wstring> out;

    // An available gamma ramp is not enough: with HDR on, Windows accepts the
    // write and ignores it.
    const bool gammaLive = gamma_.Available() && !m.isHdr;
    const bool hdrMixed  = m.isHdr && !AllMonitorsHdr();

    if (gammaLive) {
        std::wstring s = L"brilho, contraste, gamma, temperatura, sombras: rampa de gamma";
        if (gamma_.Limited())
            s += Format(L" (o Windows limitou a %.0f%%)", gamma_.AcceptedFraction() * 100.0);
        out.push_back(s);
    } else if (m.isHdr && hdr_.Supports(m)) {
        // Per display, without I2C, and effective in exclusive fullscreen.
        std::wstring s = L"brilho: nível de branco SDR do Windows";
        const int nits = hdr_.CurrentNits(m.key);
        if (nits > 0) s += Format(L" (%d nits)", nits);
        out.push_back(s);
        out.push_back(AllMonitorsHdr()
            ? std::wstring(L"contraste, temperatura: matriz de cor  ·  gamma e sombras: "
                           L"não valem com HDR ligado")
            : std::wstring(L"contraste, temperatura, gamma, sombras: não valem nesta tela — "
                           L"o HDR desliga a rampa e a matriz é global (as telas sem HDR "
                           L"receberiam o ajuste em dobro)"));
        out.push_back(L"   (o nível de branco SDR governa a área de trabalho e o conteúdo "
                      L"comum; jogo ou filme em HDR desenha na faixa HDR e mantém o brilho "
                      L"próprio)");
    } else if (m.isHdr && AllMonitorsHdr()) {
        out.push_back(L"brilho, contraste, temperatura: matriz de cor "
                      L"(HDR desliga a rampa; gamma e sombras não valem aqui)");
    } else if (hdrMixed) {
        const bool ddcBright = ddc_.SupportsBrightness(m.key);
        out.push_back(ddcBright
            ? std::wstring(L"brilho: DDC/CI, como fração do brilho físico original "
                           L"(HDR desliga a rampa nesta tela)")
            : std::wstring(L"brilho, contraste, gamma, temperatura, sombras: NADA vale "
                           L"aqui — o HDR desliga a rampa e este monitor não responde "
                           L"a DDC/CI. Desligue o HDR nesta tela para os ajustes valerem."));
        if (!AllMonitorsHdr())
            out.push_back(L"   (a matriz não pode assumir: ela é global e as telas sem "
                          L"HDR receberiam o ajuste em dobro)");
    } else {
        out.push_back(L"brilho, contraste, gamma, temperatura, sombras: indisponíveis — " +
                      (gamma_.Details().empty() ? std::wstring(L"sem rampa de gamma")
                                                : gamma_.Details()));
    }

    // Saturation and hue.
    if (nvidia_.Available() || amd_.Available()) {
        out.push_back(std::wstring(L"saturação, vibrance, matiz: ") +
                      (nvidia_.Available() ? L"NVIDIA (NVAPI)" : L"AMD (ADL)") +
                      (magnify_.Available() ? L" + matriz de cor" : L""));
    } else if (magnify_.Available()) {
        out.push_back(L"saturação, matiz: matriz de cor (efeito global, igual em "
                      L"todos os monitores; não alcança jogo em tela cheia exclusiva)");
    } else {
        out.push_back(L"saturação, matiz: indisponíveis nesta máquina");
    }

    // Monitor hardware.
    std::wstring hw;
    if (m.isInternal && backlight_.Available())
        hw = L"brilho físico: luz de fundo do notebook (WMI)";
    else if (ddc_.SupportsBrightness(m.key))
        hw = L"brilho físico: DDC/CI";
    else
        hw = L"brilho físico: indisponível (este monitor não respondeu a DDC/CI)";
    if (ddc_.SupportsContrast(m.key)) hw += L"  ·  contraste físico: DDC/CI";
    out.push_back(hw);

    return out;
}

Adjustments Engine::HdrBrightnessFallback(const MonitorTarget& m,
                                          const Adjustments& a) const {
    // With HDR on, Windows accepts the gamma ramp and ignores it. The color matrix
    // can only take over when every display is HDR, since it covers the whole
    // desktop; in a mixed arrangement software brightness instead becomes a
    // fraction of the monitor's original physical brightness, so 100 stays neutral.
    if (!m.isHdr || AllMonitorsHdr()) return a;
    // The SDR white level already handled brightness for this display, without
    // I2C or EEPROM writes; letting the fallback run would dim it twice.
    if (hdr_.Supports(m)) return a;
    // An explicit physical brightness in the profile takes precedence over the
    // fallback.
    if (a.hwBrightness >= 0) return a;
    if (!ddc_.SupportsBrightness(m.key)) return a;

    const int orig = ddc_.OriginalBrightness(m.key);
    if (orig < 0) return a;

    Adjustments out = a;
    out.hwBrightness = Clamp(orig * Clamp(a.brightness, 0.0, 150.0) / 100.0, 0.0, 100.0);
    return out;
}

bool Engine::AllMonitorsHdr() const {
    const auto& all = monitors::All();
    if (all.empty()) return false;
    for (const auto& m : all)
        if (!m.isHdr) return false;
    return true;
}

void Engine::ApplyGlobalMatrix() {
    if (!magnify_.Available()) return;

    // With HDR on, Windows accepts SetDeviceGammaRamp and ignores the result, so
    // the color matrix also takes over brightness, contrast and temperature. That
    // only holds when every display is HDR: the matrix covers the whole desktop,
    // and a non-HDR display has already received the ramp.
    const bool compensate = AllMonitorsHdr();
    magnify_.SetCompensateGamma(compensate);

    const MonitorTarget* primary = monitors::Primary();
    Profile* act = Active();
    if (!primary || !act) return;

    auto it = shown_.find(primary->key);
    const Adjustments src = it != shown_.end() ? it->second : Effective(*act, *primary);

    const SatEngine eng = act->satEngine;
    Adjustments magAdj = src;

    // Brightness leaves the matrix when the SDR white level covers every display,
    // since it has already been applied per display. Contrast and temperature stay
    // with the matrix, which the white level does not touch.
    if (compensate) {
        bool hdrHandlesBrightness = true;
        for (const auto& m : monitors::All())
            if (!hdr_.Supports(m)) { hdrHandlesBrightness = false; break; }
        if (hdrHandlesBrightness) magAdj.brightness = 100;
    }

    if (eng == SatEngine::Off) {
        magAdj.saturation = 100;
        magAdj.hue = 0;
    } else if (UseVendorSaturation()) {
        // The GPU already handled saturation; the matrix takes the rest.
        magAdj.saturation = 100;
    } else if (!VendorVibranceAvailable()) {
        // Without a vendor GPU, vibrance becomes extra saturation gain so the
        // effect exists on integrated graphics. The clamp is required because
        // saturation 200 with vibrance 100 exceeds the range Sanitize guarantees.
        magAdj.saturation = Clamp(magAdj.saturation * (1.0 + src.vibrance / 200.0),
                                  0.0, 200.0);
    }

    if (VendorHueAvailable()) magAdj.hue = 0;

    magnify_.Apply(*primary, magAdj);
}

bool Engine::UseVendorSaturation() const {
    const Profile* act = Active();
    if (!act) return false;
    const SatEngine eng = act->satEngine;
    if (eng == SatEngine::Universal || eng == SatEngine::Off) return false;
    // Only AMD exposes absolute saturation. NVIDIA offers DVC, which is vibrance
    // (selective saturation) and does not replace the saturation slider, so
    // forcing the GPU on an NVIDIA machine still uses the matrix.
    if (eng == SatEngine::Gpu) return amd_.Available();
    // Automatic: the GPU keeps vibrance only and absolute saturation goes through
    // the matrix, which gives the same result on any machine.
    return false;
}

bool Engine::VendorVibranceAvailable() const {
    return nvidia_.Available() || amd_.Available();
}

bool Engine::VendorHueAvailable() const {
    // Hue in the universal matrix is global, so it can only be zeroed when every
    // monitor really exposes vendor hue. A backend being available is not enough,
    // because the display itself may not expose SetHue.
    const auto& all = monitors::All();
    if (all.empty()) return false;
    for (const auto& m : all)
        if (!nvidia_.HasHue(m) && !amd_.HasHue(m)) return false;
    return true;
}

void Engine::OnDisplayChanged() {
    KLOG_I(L"Mudança de monitores recebida; redescobrindo e agendando confirmacoes.");
    RediscoverHardware();
    ScheduleRediscovery();
}

void Engine::OnResume() {
    KLOG_I(L"Sistema retomado; redescobrindo hardware agora e em etapas.");

    // Waking triggers several reapplications of the same profile (power event,
    // WM_DISPLAYCHANGE, staged rediscovery). Holding commands briefly makes the
    // queue deliver one write per monitor; panel EEPROM has a finite write life.
    ddc_.HoldCommands(kResumeHoldMs);

    RediscoverHardware();
    ScheduleRediscovery();
}

void Engine::RediscoverHardware() {
    const bool changed = monitors::Refresh();
    // HDR is toggled without restarting the program and Refresh() already treats
    // that as a layout change; reprobing before applying avoids a window in which
    // brightness would take the wrong path.
    hdr_.Probe();
    for (const auto& m : monitors::All()) hdr_.CaptureBaseline(m);
    ddc_.Discover();
    backlight_.Reconnect();
    nvidia_.Rediscover();
    amd_.Rediscover();
    // Overlay windows for monitors that left would stay on top of the remaining
    // display, click-through and unreachable, because every loop iterates
    // monitors::All().
    overlay_.SyncMonitors(monitors::All());
    SnapTo(Active());
    if (changed) KLOG_I(L"Arranjo de monitores atualizado; perfil reaplicado.");
    NotifyChanged();
}

void Engine::ScheduleRediscovery() {
    if (!host_) return;
    ::KillTimer(host_, TIMER_REDISCOVER);
    rediscoveryStep_ = 0;
    // In addition to the immediate attempt: 1 s, 5 s, 15 s and 30 s cumulative.
    ::SetTimer(host_, TIMER_REDISCOVER, 1000, nullptr);
}

void Engine::ResetAll() {
    for (const auto& m : monitors::All())
        for (Backend* b : all_) b->Reset(m);
}

// Utilities

void Engine::MutateActive(void (*fn)(Adjustments&, double), double value) {
    Profile* act = Active();
    if (!act || !fn) return;
    fn(act->global, value);
    for (auto& kv : act->perMonitor) fn(kv.second, value);
    ApplyNow();
    NotifyChanged();
}

void Engine::NudgeBrightness(double delta) {
    MutateActive([](Adjustments& a, double d) {
        a.brightness = Clamp(a.brightness + d, 10.0, 150.0);
    }, delta);
}

void Engine::NudgeSaturation(double delta) {
    MutateActive([](Adjustments& a, double d) {
        a.saturation = Clamp(a.saturation + d, 0.0, 200.0);
    }, delta);
}

void Engine::NudgeTemperature(double delta) {
    MutateActive([](Adjustments& a, double d) {
        a.temperature = Clamp(a.temperature + d, 1500.0, 10000.0);
    }, delta);
}

int Engine::AvailableBackendCount() const {
    int n = 0;
    for (const Backend* b : all_) if (b->Available()) ++n;
    return n;
}

std::wstring Engine::DescribeBackends() const {
    std::wstring out;
    for (const Backend* b : all_) {
        out += b->Available() ? L"  [ok]  " : L"  [--]  ";
        out += b->Name();
        out += L"\r\n         ";
        out += b->Details();
        out += L"\r\n";
    }
    if (all_.empty()) out = L"  Nenhum backend habilitado.\r\n";
    return out;
}

}  // namespace zdisplay
