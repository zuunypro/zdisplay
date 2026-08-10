// Protection against the user locking themselves into a screen they cannot
// read.
//
// The pattern is the one Windows uses when changing resolution: the risky
// adjustment is applied behind a confirmation dialog that reverts on its own,
// so a screen too dark to find the button still comes back.
#include "ui.h"
#include "ui_ids.h"
#include "ui_dpi.h"
#include "ui_theme.h"

#include <shlobj.h>

namespace zdisplay {

namespace {

/// Reports whether a fullscreen game, a presentation or another mode that must
/// not be interrupted is running.
///
/// The confirmation window is WS_EX_TOPMOST and calls SetForegroundWindow,
/// which pulls focus away from an exclusive fullscreen game and makes most of
/// them minimize. QUNS_QUIET_TIME is deliberately not consulted: do-not-disturb
/// silences notifications, and this is a safety prompt rather than a
/// notification.
bool PopupWouldInterrupt() {
    QUERY_USER_NOTIFICATION_STATE state = QUNS_ACCEPTS_NOTIFICATIONS;
    if (FAILED(::SHQueryUserNotificationState(&state))) return false;
    return state == QUNS_BUSY ||
           state == QUNS_RUNNING_D3D_FULL_SCREEN ||
           state == QUNS_PRESENTATION_MODE;
}
const wchar_t* kConfirmClass = L"ZdisplayConfirmWindow";
constexpr int kConfirmSeconds = 15;
constexpr UINT_PTR kConfirmTimer = 77;
constexpr int IDC_KEEP = 1;
constexpr int IDC_UNDO = 2;
}  // namespace

void App::EmergencyRestore() {
    // Close any pending confirmation without applying it.
    if (confirmWnd_ && ::IsWindow(confirmWnd_)) {
        ::KillTimer(confirmWnd_, kConfirmTimer);
        ::DestroyWindow(confirmWnd_);
        confirmWnd_ = nullptr;
        confirmText_ = nullptr;
        if (confirmFont_) { ::DeleteObject(confirmFont_); confirmFont_ = nullptr; }
        engine_->Overlay()->Suspend(false);
    }
    engine_->EmergencyRestore();
    UpdateTrayTip();
    RefreshUi();

    // The user has to know why the screen reverted on its own.
    if (trayAdded_) {
        tray_.uFlags = NIF_INFO;
        wcscpy_s(tray_.szInfoTitle, L"Zdisplay pausado");
        wcscpy_s(tray_.szInfo,
                 L"A tela foi devolvida ao estado original pelo atalho de emergência.");
        tray_.dwInfoFlags = NIIF_INFO;
        ::Shell_NotifyIconW(NIM_MODIFY, &tray_);
        tray_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    }
}

void App::GuardDarkScreen() {
    if (reverting_ || exiting_) return;
    if (!config_.confirmDarkSettings) return;
    if (!engine_->Enabled() || !engine_->Active()) return;

    const double luminance = engine_->CurrentLuminance();

    if (luminance >= kRiskyLuminance) {
        // Safe state: record it as the revert point and close any pending
        // confirmation, since the screen is already readable again.
        safeSnapshot_ = *engine_->Active();
        hasSafeSnapshot_ = true;
        // Back above the light floor, so a previous acceptance expires: going
        // dark again later is a new decision and is worth asking about.
        acceptedDarkLuminance_ = -1.0;
        acceptedDarkProfile_.clear();
        if (confirmWnd_ && ::IsWindow(confirmWnd_)) CloseDarkConfirm(true);
        return;
    }

    if (confirmWnd_ && ::IsWindow(confirmWnd_)) return;   // already asking

    // Once the dark screen has been accepted, the prompt only returns when the
    // screen gets noticeably darker than what was accepted. Re-asking on every
    // change turns the protection into a nuisance, and a prompt that appears
    // constantly gets dismissed unread. The margin absorbs the one or two
    // points that contrast or temperature changes move the estimated light by.
    if (acceptedDarkLuminance_ >= 0.0 &&
        acceptedDarkProfile_ == engine_->Active()->name &&
        luminance >= acceptedDarkLuminance_ - kDarkReaskMargin)
        return;

    // Fullscreen game or presentation: postpone rather than give up. Stealing
    // focus here would minimize the game, and the emergency shortcut keeps
    // working meanwhile.
    if (PopupWouldInterrupt()) {
        if (host_) ::SetTimer(host_, TIMER_DARKGUARD, 30000, nullptr);
        return;
    }

    // Rate limit of one prompt per cooldown period. Dragging a slider to the
    // bottom passes through dozens of dark states, each of which would
    // otherwise open its own window. The check reschedules instead of giving
    // up, so a screen still dark when the cooldown expires is asked about.
    const ULONGLONG now = ::GetTickCount64();
    if (lastDarkAskTick_ != 0 && now - lastDarkAskTick_ < kDarkAskCooldownMs) {
        if (host_) {
            const UINT remaining = (UINT)(kDarkAskCooldownMs - (now - lastDarkAskTick_));
            ::SetTimer(host_, TIMER_DARKGUARD, remaining + 100, nullptr);
        }
        return;
    }

    // When the app starts already dark (profile saved that way, config edited
    // by hand) no revert point was ever recorded. The fallback point is then
    // the neutral profile, so there is always somewhere to revert to.
    if (!hasSafeSnapshot_) {
        safeSnapshot_ = *engine_->Active();
        safeSnapshot_.global = Adjustments{};
        for (auto& kv : safeSnapshot_.perMonitor) kv.second = Adjustments{};
        hasSafeSnapshot_ = true;
        KLOG_W(L"O Zdisplay iniciou com ajustes escuros; o ponto de retorno será o neutro.");
    }

    lastDarkAskTick_ = now;
    ShowDarkConfirm();
}

void App::ShowDarkConfirm() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ConfirmProc;
        wc.hInstance = inst_;
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;   // painted by the theme (theme::HandleColorMessage)
        wc.lpszClassName = kConfirmClass;
        wc.hIcon = icon_;
        if (!::RegisterClassExW(&wc)) return;
        registered = true;
    }

    // The dimming overlay steps aside while the prompt is up. It is topmost and
    // the watchdog keeps pushing it above this window, so the very adjustment
    // that triggers the prompt would otherwise hide it, and the screen has to
    // be readable for the question to be read.
    engine_->Overlay()->Suspend(true);

    // Size from the client area, at the DPI of the monitor holding the cursor,
    // so text and buttons stay inside the window when the font scales.
    dpi::Current() = dpi::ForCursor();
    using dpi::S;

    RECT rc{0, 0, S(460), S(180)};
    ::AdjustWindowRectEx(&rc, WS_POPUPWINDOW | WS_CAPTION, FALSE,
                         WS_EX_TOPMOST | WS_EX_TOOLWINDOW);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;

    // Placed on the cursor's monitor inside its work area rather than on the
    // primary: CurrentLuminance() reports the worst monitor, so the dark screen
    // is not necessarily the main one.
    const RECT work = dpi::WorkAreaForCursor();
    int x = work.left + ((work.right - work.left) - w) / 2;
    int y = work.top + ((work.bottom - work.top) - h) / 3;
    x = Clamp(x, (int)work.left, (int)(work.right - w));
    y = Clamp(y, (int)work.top, (int)(work.bottom - h));

    confirmWnd_ = ::CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kConfirmClass, L"Zdisplay",
        WS_POPUPWINDOW | WS_CAPTION, x, y, w, h,
        nullptr, nullptr, inst_, nullptr);
    if (!confirmWnd_) { engine_->Overlay()->Suspend(false); return; }

    const int cw = S(460);

    ::CreateWindowExW(0, L"STATIC",
        L"Estes ajustes deixam a tela bem escura.\n"
        L"Se você não conseguir enxergar direito, não faça nada: o Zdisplay "
        L"desfaz sozinho.",
        WS_CHILD | WS_VISIBLE | SS_LEFT, S(16), S(14), cw - S(40), S(56),
        confirmWnd_, nullptr, inst_, nullptr);

    confirmText_ = ::CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT, S(16), S(76), cw - S(40), S(22),
        confirmWnd_, nullptr, inst_, nullptr);

    HWND keep = ::CreateWindowExW(0, L"BUTTON", L"&Manter assim",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        cw - S(300), S(108), S(130), S(30), confirmWnd_, (HMENU)(INT_PTR)IDC_KEEP, inst_, nullptr);

    HWND undo = ::CreateWindowExW(0, L"BUTTON", L"&Desfazer agora",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        cw - S(160), S(108), S(140), S(30), confirmWnd_, (HMENU)(INT_PTR)IDC_UNDO, inst_, nullptr);

    // Font at the same DPI used to size the controls. font_ follows the system
    // DPI and overflows the scaled controls in a mixed-DPI layout.
    if (confirmFont_) { ::DeleteObject(confirmFont_); confirmFont_ = nullptr; }
    confirmFont_ = dpi::MessageFontFor(dpi::Current());
    ::EnumChildWindows(confirmWnd_, [](HWND child, LPARAM fp) -> BOOL {
        ::SendMessageW(child, WM_SETFONT, (WPARAM)fp, TRUE);
        return TRUE;
    }, (LPARAM)(confirmFont_ ? confirmFont_ : font_));

    confirmSecondsLeft_ = kConfirmSeconds;
    ::SetWindowTextW(confirmText_,
        Format(L"Desfazendo em %d segundos...", confirmSecondsLeft_).c_str());

    ::SetTimer(confirmWnd_, kConfirmTimer, 1000, nullptr);
    ::ShowWindow(confirmWnd_, SW_SHOW);
    ::SetForegroundWindow(confirmWnd_);
    // Focus has to be set explicitly, otherwise no control takes the caret: Tab
    // goes nowhere and BS_DEFPUSHBUTTON on the revert button is appearance only.
    ::SetFocus(undo);
    (void)keep;

    KLOG_W(L"Ajustes deixam a tela escura (luz estimada %.0f%%); pedindo confirmação.",
           engine_->CurrentLuminance() * 100.0);
}

void App::CloseDarkConfirm(bool keep) {
    if (confirmWnd_ && ::IsWindow(confirmWnd_)) {
        ::KillTimer(confirmWnd_, kConfirmTimer);
        ::DestroyWindow(confirmWnd_);
    }
    confirmWnd_ = nullptr;
    confirmText_ = nullptr;
    if (confirmFont_) { ::DeleteObject(confirmFont_); confirmFont_ = nullptr; }

    // The dimming overlay resumes; if the adjustment is reverted below, the
    // ApplyNow call sets it to the right value.
    if (engine_) engine_->Overlay()->Suspend(false);

    if (keep) {
        // Accepted: this becomes the new revert point and is also recorded as
        // accepted, together with its light level and profile. Both are needed,
        // since the acceptance is what keeps the prompt from returning on the
        // next change.
        if (engine_->Active()) {
            safeSnapshot_ = *engine_->Active();
            hasSafeSnapshot_ = true;
            acceptedDarkLuminance_ = engine_->CurrentLuminance();
            acceptedDarkProfile_   = engine_->Active()->name;
        }
        KLOG_I(L"Usuário manteve os ajustes escuros (luz %d%%); "
               L"só pergunto de novo se cair abaixo de %d%%.",
               (int)(acceptedDarkLuminance_ * 100 + 0.5),
               (int)((acceptedDarkLuminance_ - kDarkReaskMargin) * 100 + 0.5));
        return;
    }

    // Reverted: nothing is accepted, so the next dark state is asked about.
    acceptedDarkLuminance_ = -1.0;
    acceptedDarkProfile_.clear();

    if (!hasSafeSnapshot_) return;

    // Return the profile to the last readable state.
    reverting_ = true;
    if (Profile* active = engine_->Active()) {
        const std::wstring name = active->name;
        active->global = safeSnapshot_.global;
        active->perMonitor = safeSnapshot_.perMonitor;
        active->name = name;
        engine_->ApplyNow();
    }
    reverting_ = false;

    MarkDirty();
    UpdateTrayTip();
    RefreshUi();
    KLOG_I(L"Ajustes escuros desfeitos automaticamente.");
}

LRESULT CALLBACK App::ConfirmProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* self = App::Get();
    if (!self) return ::DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_TIMER:
            if (wp == kConfirmTimer) {
                if (--self->confirmSecondsLeft_ <= 0) {
                    self->CloseDarkConfirm(false);
                } else if (self->confirmText_ && ::IsWindow(self->confirmText_)) {
                    ::SetWindowTextW(self->confirmText_,
                        Format(L"Desfazendo em %d segundos...",
                               self->confirmSecondsLeft_).c_str());
                }
                return 0;
            }
            break;

        case WM_COMMAND:
            if (LOWORD(wp) == IDC_KEEP) { self->CloseDarkConfirm(true); return 0; }
            if (LOWORD(wp) == IDC_UNDO) { self->CloseDarkConfirm(false); return 0; }
            break;

        case WM_CLOSE:
            // Closing the window counts as reverting: it is the safe choice.
            self->CloseDarkConfirm(false);
            return 0;

        case WM_ACTIVATE:
            // Keep focus on a button whenever the window comes back to the
            // front, otherwise the keyboard reaches nothing.
            if (LOWORD(wp) != WA_INACTIVE)
                ::SetFocus(::GetDlgItem(hwnd, IDC_UNDO));
            break;

        case WM_ERASEBKGND:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            LRESULT r = 0;
            if (theme::HandleColorMessage(hwnd, msg, wp, lp, &r)) return r;
            break;
        }

        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(lp);
            if (hdr && hdr->code == NM_CUSTOMDRAW) {
                LRESULT r = 0;
                if (theme::HandleCustomDraw(lp, &r)) return r;
            }
            break;
        }

        default: break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace zdisplay
