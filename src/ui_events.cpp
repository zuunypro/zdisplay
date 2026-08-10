// Settings window event handling: sliders, buttons, lists and tabs.
#include "ui.h"
#include "ui_ids.h"
#include "ui_dpi.h"
#include "ui_theme.h"

#include <commdlg.h>

namespace zdisplay {

namespace {

std::wstring GetTextOf(HWND h) {
    if (!h) return L"";
    const int n = ::GetWindowTextLengthW(h);
    if (n <= 0) return L"";
    std::wstring s((size_t)n, L'\0');
    ::GetWindowTextW(h, &s[0], n + 1);
    return s;
}

void SetTextOf(HWND h, const std::wstring& s) { if (h) ::SetWindowTextW(h, s.c_str()); }

int PriorityOf(HWND edit) {
    double value = 0;
    if (!ParseDouble(GetTextOf(edit), &value) || !std::isfinite(value)) return 0;
    // Priority only needs to order rules; the cap avoids undefined conversion of
    // huge pasted values without limiting any real use.
    return (int)llround(Clamp(value, -100000.0, 100000.0));
}

bool Checked(HWND h) { return h && ::SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; }
void SetCheck(HWND h, bool on) { if (h) ::SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }

std::wstring ComboSelText(HWND h) {
    const int i = (int)::SendMessageW(h, CB_GETCURSEL, 0, 0);
    if (i < 0) return L"";
    const int len = (int)::SendMessageW(h, CB_GETLBTEXTLEN, i, 0);
    if (len <= 0) return L"";
    std::wstring s((size_t)len, L'\0');
    ::SendMessageW(h, CB_GETLBTEXT, i, (LPARAM)&s[0]);
    return s;
}

void ComboSelect(HWND h, const std::wstring& text) {
    const int n = (int)::SendMessageW(h, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < n; ++i) {
        const int len = (int)::SendMessageW(h, CB_GETLBTEXTLEN, i, 0);
        if (len <= 0) continue;
        std::wstring s((size_t)len, L'\0');
        ::SendMessageW(h, CB_GETLBTEXT, i, (LPARAM)&s[0]);
        if (IEquals(s, text)) { ::SendMessageW(h, CB_SETCURSEL, i, 0); return; }
    }
}

int SelectedRow(HWND listView) {
    return listView ? ListView_GetNextItem(listView, -1, LVNI_SELECTED) : -1;
}

/// File dialog used by profile export and import.
bool PickFile(HWND owner, bool save, std::wstring* path) {
    wchar_t buf[MAX_PATH * 2] = {};
    if (save) wcscpy_s(buf, L"zdisplay-perfis.ini");

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Perfis do Zdisplay (*.ini)\0*.ini\0Todos os arquivos\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = _countof(buf);
    ofn.lpstrDefExt = L"ini";
    ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST |
                (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);

    const BOOL ok = save ? ::GetSaveFileNameW(&ofn) : ::GetOpenFileNameW(&ofn);
    if (!ok) return false;
    *path = buf;
    return true;
}

}  // namespace

// Message loop

LRESULT App::OnSettingsMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND:
            // Esc closes the window: IsDialogMessage already translates Esc to
            // IDCANCEL, which is handled here.
            if (LOWORD(wp) == IDCANCEL && HIWORD(wp) == 0 && !lp) {
                ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
            OnCommand(LOWORD(wp), (HWND)lp, HIWORD(wp));
            return 0;

        case WM_HSCROLL:
            // TB_THUMBTRACK is the only code that repeats while the thumb is held.
            // Click, keyboard and wheel send a single event and apply immediately.
            if (lp) OnSlider((HWND)lp, LOWORD(wp) == TB_THUMBTRACK);
            return 0;

        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(lp);
            if (!hdr) break;

            // Buttons and trackbars only accept color through custom draw.
            if (hdr->code == NM_CUSTOMDRAW) {
                LRESULT r = 0;
                if (theme::HandleCustomDraw(lp, &r)) return r;
            }

            if (hdr->hwndFrom == tabs_ && hdr->code == TCN_SELCHANGE) {
                ShowTab((int)::SendMessageW(tabs_, TCM_GETCURSEL, 0, 0));
                return 0;
            }

            if (hdr->code == LVN_ITEMCHANGED) {
                auto* nm = reinterpret_cast<NMLISTVIEW*>(lp);

                // The row checkbox enables or disables the rule.
                const UINT before = (nm->uOldState & LVIS_STATEIMAGEMASK) >> 12;
                const UINT after  = (nm->uNewState & LVIS_STATEIMAGEMASK) >> 12;
                if (before != 0 && after != 0 && before != after && !loadingUi_) {
                    const bool on = (after == 2);
                    if (hdr->hwndFrom == appListView_ &&
                        nm->iItem >= 0 && (size_t)nm->iItem < config_.appRules.size()) {
                        config_.appRules[(size_t)nm->iItem].enabled = on;
                        MarkDirty();
                        engine_->Recompute();
                    } else if (hdr->hwndFrom == schedList_ &&
                               nm->iItem >= 0 && (size_t)nm->iItem < config_.scheduleRules.size()) {
                        config_.scheduleRules[(size_t)nm->iItem].enabled = on;
                        MarkDirty();
                        engine_->Recompute();
                    }
                }

                // Selecting a row loads the edit fields below it.
                if ((nm->uNewState & LVIS_SELECTED) && !(nm->uOldState & LVIS_SELECTED)) {
                    if (hdr->hwndFrom == appListView_ &&
                        nm->iItem >= 0 && (size_t)nm->iItem < config_.appRules.size()) {
                        const auto& r = config_.appRules[(size_t)nm->iItem];
                        SetTextOf(appProcessEdit_, r.process);
                        ComboSelect(appProfileCombo_, r.profile);
                        SetTextOf(appPriorityEdit_, std::to_wstring(r.priority));
                        SetCheck(appEnabledCheck_, r.enabled);
                    } else if (hdr->hwndFrom == schedList_ &&
                               nm->iItem >= 0 && (size_t)nm->iItem < config_.scheduleRules.size()) {
                        const auto& r = config_.scheduleRules[(size_t)nm->iItem];
                        SetTextOf(schedStartEdit_, r.start);
                        SetTextOf(schedEndEdit_, r.end);
                        ComboSelect(schedProfileCombo_, r.profile);
                        SetTextOf(schedPriorityEdit_, std::to_wstring(r.priority));
                        SetCheck(schedEnabledCheck_, r.enabled);
                    }
                }
                return 0;
            }
            break;
        }

        case WM_ERASEBKGND:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            LRESULT r = 0;
            if (theme::HandleColorMessage(hwnd, msg, wp, lp, &r)) return r;
            break;
        }

        case WM_MEASUREITEM: {
            // With owner draw the row height is set here; the default does not
            // follow the window font.
            auto* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
            if (mis && mis->CtlType == ODT_LISTBOX) {
                mis->itemHeight = (UINT)dpi::S(18);
                return TRUE;
            }
            break;
        }

        case WM_DRAWITEM:
            if (theme::HandleDrawItem(reinterpret_cast<const DRAWITEMSTRUCT*>(lp)))
                return TRUE;
            break;

        case WM_CLOSE:
            CommitProfileEditor();
            CommitVision();
            ::KillTimer(host_, TIMER_VISION_PREVIEW);
            if (engine_) engine_->EndPreviewVision();
            SaveConfig(config_);
            dirty_ = false;
            ::DestroyWindow(hwnd);
            return 0;

        case WM_DPICHANGED:
            // The suggested rectangle is applied immediately, but the rebuild waits
            // for the end of the drag: destroying the window under the mouse would
            // cancel the drag itself.
            if (!settingsBuilt_) return 0;
            if (lp) pendingDpiRect_ = *reinterpret_cast<const RECT*>(lp);
            pendingDpiChange_ = true;
            if (!inSizeMove_) RebuildSettingsForDpi();
            return 0;

        case WM_ENTERSIZEMOVE:
            inSizeMove_ = true;
            break;

        case WM_EXITSIZEMOVE:
            inSizeMove_ = false;
            if (pendingDpiChange_) RebuildSettingsForDpi();
            break;

        case WM_DESTROY:
            for (int t = 0; t < 6; ++t) tabControls_[t].clear();
            if (fontMono_) { ::DeleteObject(fontMono_); fontMono_ = nullptr; }
            settingsBuilt_ = false;
            settings_ = nullptr;
            tabs_ = nullptr;
            statusBar_ = nullptr;
            hotkeyWarning_ = nullptr;
            diagEdit_ = nullptr;
            profileList_ = nullptr;
            appListView_ = nullptr;
            schedList_ = nullptr;
            for (int i = 0; i < F_COUNT; ++i) sliders_[i] = SliderRow{};
            return 0;

        default: break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// Vision tab

void App::CommitVision() {
    if (loadingUi_ || !visEnable_) return;
    Vision& v = config_.vision;

    v.enabled = Checked(visEnable_);
    double d = 0;
    if (ParseDouble(GetTextOf(visDayTemp_), &d))     v.dayTemperature = d;
    if (ParseDouble(GetTextOf(visNightTemp_), &d))   v.nightTemperature = d;
    if (ParseDouble(GetTextOf(visNightBright_), &d)) v.nightBrightness = d;
    if (ParseDouble(GetTextOf(visTransition_), &d))  v.transitionMinutes = (int)d;
    if (ParseDouble(GetTextOf(visBreak_), &d))       v.breakMinutes = (int)d;

    const std::wstring ns = Trim(GetTextOf(visNightStart_));
    const std::wstring ds = Trim(GetTextOf(visDayStart_));
    if (!ns.empty()) v.nightStart = ns;
    if (!ds.empty()) v.dayStart = ds;
    v.Sanitize();

    MarkDirty();
    // Enabling the layer here has to rearm the clock tick that keeps it in step
    // with sunset; that tick can be stopped when the profile schedule is off.
    engine_->UpdateScheduleTimer();
    engine_->UpdateVision();
    engine_->ApplyNow();
    ScheduleBreakReminder();
    LoadVision();
    UpdateStatusBar();
}

// Sliders

void App::OnSlider(HWND bar, bool dragging) {
    if (loadingUi_) return;

    for (int i = 0; i < F_COUNT; ++i) {
        if (sliders_[i].bar != bar) continue;
        theme::SyncTrackbarVisual(bar);
        sliders_[i].UpdateValueLabel();

        // The Windows trackbar only invalidates the old and new thumb rectangles,
        // while the fill spans the whole track up to the thumb. Invalidating the
        // entire control without erasing lets custom draw rebuild it in one pass.
        ::RedrawWindow(bar, nullptr, nullptr,
                       RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);

        Adjustments* a = CurrentAdjustments();
        if (!a) return;

        const double v = sliders_[i].Get();
        const AdjField f = (AdjField)i;

        // The hardware fields only apply when their checkbox is set.
        if (f == F_HWBRIGHT && !Checked(manageHwBright_)) return;
        if (f == F_HWCONTRAST && !Checked(manageHwContrast_)) return;

        *FieldPtr(*a, f) = v;
        ApplyLive(dragging);
        return;
    }
}

// Commands

void App::OnCommand(int id, HWND control, int code) {
    // Opening the process list re-enumerates running programs so the entries
    // reflect the current moment.
    if (id == IDC_APP_PROCESS && code == CBN_DROPDOWN) {
        ReloadRunningApps();
        return;
    }

    // Per-slider reset-to-neutral buttons.
    if (id >= IDC_SLIDER_RESET_BASE && id < IDC_SLIDER_RESET_BASE + (int)F_COUNT) {
        const int i = id - IDC_SLIDER_RESET_BASE;
        Adjustments* a = CurrentAdjustments();
        if (!a) return;
        sliders_[i].Set(sliders_[i].defValue);
        const AdjField f = (AdjField)i;
        if (f == F_HWBRIGHT || f == F_HWCONTRAST) {
            if ((f == F_HWBRIGHT && !Checked(manageHwBright_)) ||
                (f == F_HWCONTRAST && !Checked(manageHwContrast_))) return;
        }
        *FieldPtr(*a, f) = sliders_[i].defValue;
        ApplyLive();
        return;
    }

    // Hotkey fields apply on focus loss.
    if (id >= IDC_HK_BASE && id < IDC_HK_BASE + 7 && code == EN_KILLFOCUS) {
        const int i = id - IDC_HK_BASE;
        const std::wstring v = Trim(GetTextOf(hkEdits_[i]));
        std::wstring* targets[7] = {
            &config_.hkBrightnessUp, &config_.hkBrightnessDown,
            &config_.hkSaturationUp, &config_.hkSaturationDown,
            &config_.hkToggle, &config_.hkShow, &config_.hkPanic,
        };
        *targets[i] = v;
        RegisterHotkeys();
        // The emergency hotkey is never empty; the default goes back in the field.
        if (i == 6) SetTextOf(hkEdits_[6], config_.hkPanic);
        MarkDirty();
        return;
    }

    switch (id) {
        // Adjustments tab
        case IDC_PROFILE_COMBO:
            if (code == CBN_SELCHANGE && !loadingUi_) {
                engine_->SetManualProfile(ComboSelText(profileCombo_));
                LoadAdjustments();
                UpdateStatusBar();
            }
            return;

        case IDC_MONITOR_COMBO:
            if (code == CBN_SELCHANGE && !loadingUi_) LoadAdjustments();
            return;

        case IDC_PER_MONITOR: {
            if (loadingUi_) return;
            Profile* p = EditingProfile();
            const std::wstring key = SelectedMonitorKey();
            if (!p || key.empty()) return;
            if (Checked(perMonitorCheck_)) p->Ensure(key);
            else p->perMonitor.erase(key);
            LoadAdjustments();
            ApplyLive();
            return;
        }

        case IDC_AUTO_BTN:
            engine_->ClearManualProfile();
            ReloadProfileCombos();
            LoadAdjustments();
            UpdateStatusBar();
            return;

        case IDC_PAUSE_BTN:
            TogglePause(L"botão Pausar");
            return;

        case IDC_INVERT: {
            if (loadingUi_) return;
            Adjustments* a = CurrentAdjustments();
            if (!a) return;
            a->invert = Checked(invertCheck_);
            ApplyLive();
            return;
        }

        case IDC_MANAGE_HWBRIGHT: {
            if (loadingUi_) return;
            Adjustments* a = CurrentAdjustments();
            if (!a) return;
            const bool on = Checked(manageHwBright_);
            a->hwBrightness = on ? sliders_[F_HWBRIGHT].Get() : -1;
            sliders_[F_HWBRIGHT].Enable(on);
            ApplyLive();
            return;
        }

        case IDC_MANAGE_HWCONTRAST: {
            if (loadingUi_) return;
            Adjustments* a = CurrentAdjustments();
            if (!a) return;
            const bool on = Checked(manageHwContrast_);
            a->hwContrast = on ? sliders_[F_HWCONTRAST].Get() : -1;
            sliders_[F_HWCONTRAST].Enable(on);
            ApplyLive();
            return;
        }

        case IDC_RESET_ALL: {
            // Clears the profile values, which is what gets saved.
            Adjustments* a = CurrentAdjustments();
            if (!a) return;
            *a = Adjustments{};
            LoadAdjustments();
            ApplyLive();
            return;
        }

        case IDC_RESTORE_SCREEN:
            // Restores the screen without changing the saved profile.
            engine_->ResetAll();
            UpdateStatusBar();
            return;

        // Vision tab
        case IDC_VIS_ENABLE:
            CommitVision();
            return;

        case IDC_VIS_DAY_TEMP:
        case IDC_VIS_NIGHT_TEMP:
        case IDC_VIS_NIGHT_BRIGHT:
        case IDC_VIS_TRANSITION:
        case IDC_VIS_NIGHT_START:
        case IDC_VIS_DAY_START:
        case IDC_VIS_BREAK:
            if (code == EN_KILLFOCUS) CommitVision();
            return;

        case IDC_VIS_PREVIEW_DAY:
        case IDC_VIS_PREVIEW_NIGHT:
            // A single click holds the preview for five seconds.
            if (code == BN_CLICKED) {
                engine_->PreviewVision(id == IDC_VIS_PREVIEW_NIGHT ? 1.0 : 0.0);
                ::SetTimer(host_, TIMER_VISION_PREVIEW, kVisionPreviewMs, nullptr);
                LoadVision();
            }
            return;

        case IDC_VIS_TEST_BREAK:
            if (code == BN_CLICKED) {
                // With notifications globally off the toast is accepted and
                // discarded without an error, so the reason is explained here.
                if (ToastsGloballyOff()) {
                    const int r = ::MessageBoxW(settings_,
                        L"As notificações estão desligadas no Windows, então "
                        L"este aviso não apareceria na tela.\n\n"
                        L"Quer abrir Configurações > Sistema > Notificações "
                        L"para ligar?",
                        L"Zdisplay", MB_YESNO | MB_ICONWARNING);
                    if (r == IDYES)
                        ::ShellExecuteW(settings_, L"open", L"ms-settings:notifications",
                                        nullptr, nullptr, SW_SHOWNORMAL);
                    return;
                }
                ShowTrayBalloon(L"Zdisplay — pausa para os olhos",
                                L"Olhe 20 segundos para algo a uns 6 metros. "
                                L"Isso relaxa o músculo que mantém o foco de perto.");
            }
            return;

        // IDC_COMPARE is absent here: press-and-hold is handled directly from the
        // mouse messages in CompareProc (ui_settings.cpp), because the BN_PUSHED
        // and BN_UNPUSHED notifications do not arrive.

        // Profiles tab
        case IDC_PROFILE_LIST:
            if (code == LBN_SELCHANGE && !loadingUi_) LoadProfileEditor();
            return;

        case IDC_PROFILE_NAME:
        case IDC_PROFILE_HOTKEY:
        case IDC_PROFILE_TRANSITION:
            if (code == EN_KILLFOCUS) CommitProfileEditor();
            return;

        case IDC_PROFILE_SATENGINE:
            if (code == CBN_SELCHANGE && !loadingUi_) {
                if (Profile* p = SelectedProfileInList()) {
                    const int i = (int)::SendMessageW(satEngineCombo_, CB_GETCURSEL, 0, 0);
                    p->satEngine = (SatEngine)Clamp(i, 0, 3);
                    MarkDirty();
                    engine_->ApplyNow();
                }
            }
            return;

        case IDC_PROFILE_NEW: {
            Profile p;
            p.name = config_.UniqueName(L"Perfil");
            config_.profiles.push_back(p);
            MarkDirty();
            engine_->OnProfilesChanged();
            ReloadProfileCombos();
            ::SendMessageW(profileList_, LB_SETCURSEL, config_.profiles.size() - 1, 0);
            LoadProfileEditor();
            return;
        }

        case IDC_PROFILE_DUP: {
            Profile* src = SelectedProfileInList();
            if (!src) return;
            Profile copy = *src;
            copy.name = config_.UniqueName(src->name);
            copy.hotkey.clear();
            config_.profiles.push_back(copy);
            MarkDirty();
            engine_->OnProfilesChanged();
            ReloadProfileCombos();
            ::SendMessageW(profileList_, LB_SETCURSEL, config_.profiles.size() - 1, 0);
            LoadProfileEditor();
            return;
        }

        case IDC_PROFILE_DELETE: {
            const int idx = (int)::SendMessageW(profileList_, LB_GETCURSEL, 0, 0);
            if (idx < 0 || (size_t)idx >= config_.profiles.size()) return;
            if (config_.profiles.size() <= 1) {
                ::MessageBoxW(settings_, L"E preciso manter pelo menos um perfil.",
                              L"Zdisplay", MB_OK | MB_ICONINFORMATION);
                return;
            }
            const std::wstring name = config_.profiles[(size_t)idx].name;
            const std::wstring question = L"Excluir o perfil '" + name + L"'?";
            if (::MessageBoxW(settings_, question.c_str(), L"Zdisplay",
                              MB_YESNO | MB_ICONQUESTION) != IDYES) return;

            config_.profiles.erase(config_.profiles.begin() + idx);
            if (IEquals(config_.defaultProfile, name))
                config_.defaultProfile = config_.profiles[0].name;

            // Rules that pointed at the deleted profile are repointed at the default.
            for (auto& r : config_.appRules)
                if (IEquals(r.profile, name)) r.profile = config_.defaultProfile;
            for (auto& r : config_.scheduleRules)
                if (IEquals(r.profile, name)) r.profile = config_.defaultProfile;

            MarkDirty();
            engine_->ClearManualProfile();
            engine_->OnProfilesChanged();
            ReloadProfileCombos();
            LoadRuleLists();
            LoadProfileEditor();
            LoadAdjustments();
            RegisterHotkeys();
            return;
        }

        case IDC_PROFILE_DEFAULT: {
            Profile* p = SelectedProfileInList();
            if (!p) return;
            config_.defaultProfile = p->name;
            MarkDirty();
            LoadProfileEditor();
            return;
        }

        case IDC_PROFILE_EXPORT: {
            std::wstring path;
            if (!PickFile(settings_, true, &path)) return;
            const bool ok = ExportProfiles(path, config_.profiles);
            ::MessageBoxW(settings_,
                          ok ? L"Perfis exportados." : L"Não consegui gravar o arquivo.",
                          L"Zdisplay", MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONWARNING));
            return;
        }

        case IDC_PROFILE_IMPORT: {
            std::wstring path;
            if (!PickFile(settings_, false, &path)) return;
            std::vector<Profile> imported;
            if (!ImportProfiles(path, &imported) || imported.empty()) {
                ::MessageBoxW(settings_, L"Nenhum perfil válido no arquivo.",
                              L"Zdisplay", MB_OK | MB_ICONWARNING);
                return;
            }
            for (auto& p : imported) {
                p.name = config_.UniqueName(p.name);
                config_.profiles.push_back(p);
            }
            MarkDirty();
            engine_->OnProfilesChanged();
            SaveConfig(config_);
            ReloadProfileCombos();
            LoadProfileEditor();
            const std::wstring msg = std::to_wstring(imported.size()) + L" perfil(is) importado(s).";
            ::MessageBoxW(settings_, msg.c_str(), L"Zdisplay", MB_OK | MB_ICONINFORMATION);
            return;
        }

        // Automation tab
        case IDC_APP_ADD:
        case IDC_APP_UPDATE: {
            const std::wstring process = Trim(GetTextOf(appProcessEdit_));
            if (process.empty()) {
                ::MessageBoxW(settings_, L"Informe o nome do processo (ex.: cs2, chrome).",
                              L"Zdisplay", MB_OK | MB_ICONINFORMATION);
                return;
            }
            AppRule r;
            r.process = process;
            r.profile = ComboSelText(appProfileCombo_);
            r.priority = PriorityOf(appPriorityEdit_);
            r.enabled = Checked(appEnabledCheck_);

            const int row = SelectedRow(appListView_);
            if (id == IDC_APP_UPDATE && row >= 0 && (size_t)row < config_.appRules.size())
                config_.appRules[(size_t)row] = r;
            else
                config_.appRules.push_back(r);

            MarkDirty();
            LoadRuleLists();
            engine_->Recompute();
            return;
        }

        case IDC_APP_DELETE: {
            const int row = SelectedRow(appListView_);
            if (row < 0 || (size_t)row >= config_.appRules.size()) return;
            config_.appRules.erase(config_.appRules.begin() + row);
            MarkDirty();
            LoadRuleLists();
            engine_->Recompute();
            return;
        }

        case IDC_APP_PICK: {
            const std::wstring fg = engine_->ForegroundProcess();
            if (fg.empty()) {
                ::MessageBoxW(settings_, L"Ainda não detectei nenhum programa em foco.",
                              L"Zdisplay", MB_OK | MB_ICONINFORMATION);
                return;
            }
            SetTextOf(appProcessEdit_, fg);
            SetCheck(appEnabledCheck_, true);
            return;
        }

        case IDC_SCHED_ADD:
        case IDC_SCHED_UPDATE: {
            ScheduleRule r;
            r.start = Trim(GetTextOf(schedStartEdit_));
            r.end = Trim(GetTextOf(schedEndEdit_));
            r.profile = ComboSelText(schedProfileCombo_);
            r.enabled = Checked(schedEnabledCheck_);
            // Update rebuilds the rule from the fields, so priority has to be read
            // back here or it would be reset to zero.
            r.priority = PriorityOf(schedPriorityEdit_);

            // Validation uses the same criterion as the engine, so solar times are
            // accepted alongside clock times.
            if (!IsValidRuleTime(r.start) || !IsValidRuleTime(r.end)) {
                ::MessageBoxW(settings_,
                              L"Use o relógio (21:30) ou o próprio sol: "
                              L"'por', 'nascer', 'por-30', 'nascer+45'.",
                              L"Zdisplay", MB_OK | MB_ICONINFORMATION);
                return;
            }

            const int row = SelectedRow(schedList_);
            if (id == IDC_SCHED_UPDATE && row >= 0 && (size_t)row < config_.scheduleRules.size())
                config_.scheduleRules[(size_t)row] = r;
            else
                config_.scheduleRules.push_back(r);

            MarkDirty();
            LoadRuleLists();
            engine_->Recompute();
            return;
        }

        case IDC_SCHED_DELETE: {
            const int row = SelectedRow(schedList_);
            if (row < 0 || (size_t)row >= config_.scheduleRules.size()) return;
            config_.scheduleRules.erase(config_.scheduleRules.begin() + row);
            MarkDirty();
            LoadRuleLists();
            engine_->Recompute();
            return;
        }

        // System tab
        case IDC_CHK_STARTUP:
            config_.startWithWindows = Checked(checkStartup_);
            startup::Set(config_.startWithWindows);
            MarkDirty();
            return;

        case IDC_CHK_MINIMIZED:
            config_.startMinimized = Checked(checkMinimized_);
            MarkDirty();
            return;

        case IDC_CHK_APPRULES:
            config_.enableAppRules = Checked(checkAppRules_);
            MarkDirty();
            // The foreground hook is started and stopped here as well as in
            // App::Init, so the checkbox takes effect without a restart.
            if (config_.enableAppRules) {
                foreground_.Start(OnForegroundChanged, this);
                ::SetTimer(host_, TIMER_FOREGROUND_POLL, kForegroundPollMs, nullptr);
            } else {
                ::KillTimer(host_, TIMER_FOREGROUND_POLL);
                foreground_.Stop();
            }
            engine_->Recompute();
            return;

        case IDC_CHK_SCHEDULE:
            config_.enableSchedule = Checked(checkSchedule_);
            MarkDirty();
            // Rearms the clock tick so enabling or disabling the schedule takes
            // effect immediately instead of at the next start.
            engine_->UpdateScheduleTimer();
            engine_->Recompute();
            return;

        case IDC_CHK_RESTORE:
            config_.restoreOnExit = Checked(checkRestore_);
            MarkDirty();
            return;

        case IDC_CHK_CONFIRM_DARK:
            config_.confirmDarkSettings = Checked(checkConfirmDark_);
            MarkDirty();
            if (!config_.confirmDarkSettings) {
                ::MessageBoxW(settings_,
                    L"A confirmação está desligada. Se um ajuste deixar a tela "
                    L"escura demais, use o atalho de emergência (por padrão "
                    L"Ctrl+Alt+Shift+K) para devolver a tela.",
                    L"Zdisplay", MB_OK | MB_ICONWARNING);
            }
            return;

        case IDC_WATCHDOG:
            if (code == EN_KILLFOCUS) {
                double v = 0;
                ParseDouble(GetTextOf(watchdogEdit_), &v);
                config_.watchdogSeconds = Clamp((int)v, 0, 300);
                engine_->UpdateWatchdogInterval();
                MarkDirty();
            }
            return;

        case IDC_LATITUDE:
        case IDC_LONGITUDE:
            if (code == EN_KILLFOCUS) {
                // An empty field means "no location" (999), which makes solar rules
                // fail to match rather than match at the wrong place.
                const std::wstring lat = Trim(GetTextOf(latitudeEdit_));
                const std::wstring lon = Trim(GetTextOf(longitudeEdit_));
                double v = 0;
                config_.latitude  = (!lat.empty() && ParseDouble(lat, &v)) ? v : 999.0;
                config_.longitude = (!lon.empty() && ParseDouble(lon, &v)) ? v : 999.0;
                if (!config_.HasLocation()) { config_.latitude = 999; config_.longitude = 999; }
                MarkDirty();
                engine_->Recompute();
            }
            return;

        case IDC_CHK_MIRROR_KEYS:
            config_.mirrorInternalBrightness = Checked(checkMirrorKeys_);
            MarkDirty();
            return;

        case IDC_DDC_MONITOR_MODE:
            if (code == CBN_SELCHANGE && !loadingUi_) {
                const std::wstring key = SelectedMonitorKey();
                if (key.empty()) return;
                const int selected = (int)::SendMessageW(ddcModeCombo_, CB_GETCURSEL, 0, 0);
                const DdcMonitorMode mode = selected == 1 ? DdcMonitorMode::Slow
                    : selected == 2 ? DdcMonitorMode::Disabled : DdcMonitorMode::Auto;
                if (mode == DdcMonitorMode::Auto) config_.ddcMonitorModes.erase(key);
                else config_.ddcMonitorModes[key] = mode;
                MarkDirty();
                UpdateStatusBar();
            }
            return;

        case IDC_MON_INPUT:
        case IDC_MON_PRESET:
        case IDC_MON_POWER: {
            if (code != CBN_SELCHANGE || loadingUi_) return;
            const std::wstring key = SelectedMonitorKey();
            if (key.empty()) return;

            HWND combo = id == IDC_MON_INPUT ? monInputCombo_
                       : id == IDC_MON_PRESET ? monPresetCombo_ : monPowerCombo_;
            const std::vector<unsigned char>& values =
                  id == IDC_MON_INPUT ? monInputValues_
                : id == IDC_MON_PRESET ? monPresetValues_ : monPowerValues_;
            const unsigned char vcp = id == IDC_MON_INPUT ? kVcpInputSource
                                    : id == IDC_MON_PRESET ? kVcpColorPreset : kVcpPowerMode;

            const int sel = (int)::SendMessageW(combo, CB_GETCURSEL, 0, 0);
            if (sel < 0 || (size_t)sel >= values.size()) return;

            // Powering the display down needs confirmation: on some monitors the
            // way back is the panel button, not the program.
            if (vcp == kVcpPowerMode && values[sel] != 0x01) {
                if (::MessageBoxW(settings_,
                        L"Vou colocar este monitor em modo de baixa energia.\n\n"
                        L"Alguns monitores só voltam pelo botão do painel: eles param\n"
                        L"de responder a DDC/CI enquanto estão desligados.\n\n"
                        L"Continuar?",
                        L"Zdisplay — energia do monitor",
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
                    LoadMonitorFeatures();   // revert the combo selection
                    return;
                }
            }

            engine_->Ddc()->SetFeature(key, vcp, (int)values[sel]);
            return;
        }

        case IDC_CHK_VENDOR:    config_.enableVendorApis = Checked(checkVendor_); MarkDirty(); return;
        case IDC_CHK_MAGNIFY:   config_.enableMagnification = Checked(checkMagnify_); MarkDirty(); return;
        case IDC_CHK_DDC:       config_.enableDdcCi = Checked(checkDdc_); MarkDirty(); return;
        case IDC_CHK_BACKLIGHT: config_.enableBacklight = Checked(checkBacklight_); MarkDirty(); return;
        case IDC_CHK_OVERLAY:   config_.enableOverlay = Checked(checkOverlay_); MarkDirty(); return;

        case IDC_HK_STEP:
            if (code == EN_KILLFOCUS) {
                double v = 5;
                ParseDouble(GetTextOf(stepEdit_), &v);
                config_.hotkeyStep = Clamp(v, 1.0, 25.0);
                MarkDirty();
            }
            return;

        case IDC_UNLOCK_GAMMA: {
            const int answer = ::MessageBoxW(settings_,
                L"Isto grava GdiIcmGammaRange=256 no registro do Windows, liberando "
                L"brilho e contraste bem além do limite padrão.\n\n"
                L"Precisa de permissão de administrador e só passa a valer depois de "
                L"reiniciar a sessão do Windows.\n\nContinuar?",
                L"Zdisplay", MB_YESNO | MB_ICONWARNING);
            if (answer != IDYES) return;

            if (GammaBackend::TryUnlockRange(true)) {
                SetTextOf(unlockButton_, L"Restaurar a faixa padrão do Windows (admin)");
                ::SetWindowLongPtrW(unlockButton_, GWLP_ID, IDC_RELOCK_GAMMA);
                ::MessageBoxW(settings_,
                    L"Pronto. A faixa completa passa a valer depois de reiniciar a "
                    L"sessão do Windows.\n\nEste mesmo botão agora desfaz a alteração.",
                    L"Zdisplay", MB_OK | MB_ICONINFORMATION);
            } else {
                ::MessageBoxW(settings_,
                    L"Não consegui gravar no registro. Feche o Zdisplay e execute-o como "
                    L"administrador uma única vez para aplicar esta opção.",
                    L"Zdisplay", MB_OK | MB_ICONWARNING);
            }
            return;
        }

        case IDC_RELOCK_GAMMA: {
            // Every system-level change the program makes is reversible.
            if (::MessageBoxW(settings_,
                    L"Remover GdiIcmGammaRange do registro e voltar ao limite "
                    L"padrão do Windows?\n\nPrecisa de administrador e só vale "
                    L"após reiniciar a sessão.",
                    L"Zdisplay", MB_YESNO | MB_ICONQUESTION) != IDYES) return;

            if (GammaBackend::TryUnlockRange(false)) {
                SetTextOf(unlockButton_, L"Liberar faixa completa de gamma (admin)");
                ::SetWindowLongPtrW(unlockButton_, GWLP_ID, IDC_UNLOCK_GAMMA);
            } else {
                ::MessageBoxW(settings_,
                    L"Não consegui alterar o registro. Execute o Zdisplay como "
                    L"administrador uma única vez.", L"Zdisplay", MB_OK | MB_ICONWARNING);
            }
            return;
        }

        case IDC_OPEN_FOLDER:
            ::ShellExecuteW(settings_, L"open", ConfigDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return;

        case IDC_FACTORY_RESET: {
            if (code != BN_CLICKED) return;
            // MB_DEFBUTTON2 makes "No" the Enter default so a stray Enter cannot
            // erase the entire configuration.
            const int r = ::MessageBoxW(settings_,
                L"Isto apaga tudo o que você configurou: perfis, regras por "
                L"aplicativo, regras por horário, atalhos, localização e as "
                L"opções desta aba. O Zdisplay volta como veio instalado.\n\n"
                L"A tela é devolvida ao estado original antes de qualquer coisa.\n\n"
                L"Uma cópia do arquivo atual fica guardada na pasta de "
                L"configuração como zdisplay.ini.antes-do-reset.\n\n"
                L"A faixa completa de gamma liberada no Windows não é mexida — "
                L"é uma opção do sistema e tem botão próprio, que pede "
                L"administrador.\n\n"
                L"Continuar?",
                L"Zdisplay — padrão de fábrica",
                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            if (r != IDYES) return;

            FactoryReset();
            ::MessageBoxW(settings_,
                L"Pronto. O Zdisplay está como veio instalado.",
                L"Zdisplay", MB_OK | MB_ICONINFORMATION);
            return;
        }

        // Diagnostics tab
        case IDC_DIAG_REFRESH:
            LoadDiagnostics();
            return;

        case IDC_DIAG_CAPS: {
            // Explicit request only: reading the capabilities string is the one
            // DDC/CI command here that can bring the machine down, through a
            // Windows kernel defect triggered by malformed strings. Nothing in the
            // program needs it; RGB gain is detected by reading the register.
            const int r = ::MessageBoxW(
                settings_,
                L"Vou perguntar a cada monitor quais recursos DDC/CI ele declara.\n\n"
                L"Aviso: em alguns monitores com a resposta malformada, essa leitura\n"
                L"esbarra num defeito do próprio Windows que pode travar o sistema.\n"
                L"O risco e do Windows, não do Zdisplay, e atinge poucos modelos — mas\n"
                L"salve o que estiver aberto antes de continuar.\n\n"
                L"Isto serve só para diagnóstico. O Zdisplay funciona sem essa leitura.\n\n"
                L"Continuar?",
                L"Zdisplay — ler capacidades do monitor",
                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            if (r != IDYES) return;

            engine_->Ddc()->RequestCapabilities();
            ::MessageBoxW(settings_,
                          L"Leitura pedida. Ela roda em segundo plano e pode levar\n"
                          L"alguns segundos por monitor.\n\n"
                          L"Use 'Atualizar' daqui a pouco para ver o resultado.",
                          L"Zdisplay", MB_OK | MB_ICONINFORMATION);
            return;
        }

        case IDC_DIAG_ROUNDTRIP: {
            // Unlike the capabilities read, this test uses the same brightness
            // commands the program already issues, so it does not hit the kernel
            // defect. It does write to panel EEPROM, so it is never automatic.
            if (::MessageBoxW(settings_,
                    L"Vou mudar o brilho de cada monitor um passo, ler de volta para\n"
                    L"conferir e devolver o valor que estava.\n\n"
                    L"A tela pode piscar de leve durante o teste. É a única forma de\n"
                    L"distinguir um monitor que obedece de um que aceita o comando e\n"
                    L"não muda nada.\n\n"
                    L"Testar agora?",
                    L"Zdisplay — testar o monitor",
                    MB_YESNO | MB_ICONQUESTION) != IDYES) return;

            engine_->Ddc()->RequestRoundTrip();
            ::MessageBoxW(settings_,
                          L"Teste pedido. Ele roda em segundo plano e leva alguns\n"
                          L"segundos por monitor.\n\n"
                          L"Use 'Atualizar' daqui a pouco para ver o resultado.",
                          L"Zdisplay", MB_OK | MB_ICONINFORMATION);
            return;
        }

        case IDC_DIAG_DDC_RESET: {
            if (::MessageBoxW(settings_,
                    L"Isto permite que monitores bloqueados depois de uma queda sejam "
                    L"testados novamente. A liberação não envia comandos agora.\n\n"
                    L"Liberar a quarentena DDC/CI?",
                    L"Zdisplay", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
            const int count = engine_->Ddc()->ClearSafetyBlocks();
            LoadDiagnostics();
            ::MessageBoxW(settings_,
                          count ? L"Quarentena liberada. Nenhum comando perigoso foi enviado."
                                : L"Não havia monitor em quarentena.",
                          L"Zdisplay", MB_OK | MB_ICONINFORMATION);
            return;
        }

        case IDC_DIAG_COPY: {
            const std::wstring text = GetTextOf(diagEdit_);
            if (text.empty() || !::OpenClipboard(settings_)) return;
            ::EmptyClipboard();
            const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
            if (HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
                if (void* dst = ::GlobalLock(mem)) {
                    memcpy(dst, text.c_str(), bytes);
                    ::GlobalUnlock(mem);
                    ::SetClipboardData(CF_UNICODETEXT, mem);
                }
            }
            ::CloseClipboard();
            return;
        }

        case IDC_DIAG_OPENLOG: {
            const std::wstring path = LogPath();
            if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                ::MessageBoxW(settings_, L"Ainda não há log.", L"Zdisplay", MB_OK | MB_ICONINFORMATION);
                return;
            }
            ::ShellExecuteW(settings_, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }

        default:
            (void)control;
            return;
    }
}

// Profile editor commit

void App::CommitProfileEditor() {
    if (loadingUi_ || !profileList_ || !::IsWindow(profileList_)) return;

    Profile* p = SelectedProfileInList();
    if (!p) return;

    // Hotkey and transition.
    const std::wstring hotkey = Trim(GetTextOf(profileHotkeyEdit_));
    if (hotkey != p->hotkey) {
        p->hotkey = hotkey;
        RegisterHotkeys();
        MarkDirty();
    }

    double transition = p->transitionMs;
    if (ParseDouble(GetTextOf(transitionEdit_), &transition)) {
        const int v = Clamp((int)transition, 0, 10000);
        if (v != p->transitionMs) { p->transitionMs = v; MarkDirty(); }
    }

    // Renaming requires updating every reference.
    const std::wstring typed = Trim(GetTextOf(profileNameEdit_));
    // "|monitor:" is the section header separator in the INI file, so a name
    // containing it would not survive a save/load round trip.
    const std::wstring newName = SanitizeProfileName(typed);
    if (newName != typed) {
        ::MessageBoxW(settings_,
                      L"O nome de um perfil não pode conter “|”, “[” ou “]” — "
                      L"esses símbolos separam as seções do arquivo de configuração.",
                      L"Zdisplay", MB_OK | MB_ICONINFORMATION);
        SetTextOf(profileNameEdit_, newName.empty() ? p->name : newName);
        if (newName.empty()) return;
    }
    if (newName.empty() || newName == p->name) return;

    for (const auto& other : config_.profiles) {
        if (&other != p && IEquals(other.name, newName)) {
            ::MessageBoxW(settings_, L"Já existe um perfil com esse nome.",
                          L"Zdisplay", MB_OK | MB_ICONINFORMATION);
            SetTextOf(profileNameEdit_, p->name);
            return;
        }
    }

    const std::wstring oldName = p->name;
    p->name = newName;

    if (IEquals(config_.defaultProfile, oldName)) config_.defaultProfile = newName;
    for (auto& r : config_.appRules)
        if (IEquals(r.profile, oldName)) r.profile = newName;
    for (auto& r : config_.scheduleRules)
        if (IEquals(r.profile, oldName)) r.profile = newName;
    // The engine tracks the active and manual profiles by name, so a rename of
    // the profile in effect has to be reported here.
    engine_->OnProfileRenamed(oldName, newName);

    MarkDirty();
    RegisterHotkeys();
    ReloadProfileCombos();
    LoadRuleLists();
    LoadProfileEditor();
}

}  // namespace zdisplay
