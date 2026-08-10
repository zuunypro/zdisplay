// Zdisplay entry point.
#include "ui.h"
#include "version.h"

namespace zdisplay {
namespace {

/// Single instance per session, not per machine.
///
/// The mutex lives in the session namespace, so each logged-on user gets an
/// independent instance backed by its own command channel, and no unrelated
/// local process can claim a machine-wide name first and block startup.
const wchar_t* kMutexName = L"Local\\ZdisplaySingleInstance_v1";

/// Application identity for the notification system.
///
/// Shell_NotifyIcon balloons surface as toasts, and a toast must belong to a
/// registered application; without this identity Zdisplay does not appear under
/// Settings > Notifications and its reminders cannot be managed there. The
/// installer stamps the same string on the Start menu shortcut, in
/// StampShortcutIdentity; the two values must match.
const wchar_t* kAppUserModelId = L"Zdisplay";

/// shell32 is resolved at run time so a single call does not turn it into a
/// link-time dependency.
void DeclareAppIdentity() {
    using Fn = HRESULT(WINAPI*)(PCWSTR);
    static DynLib shell(L"shell32.dll");
    if (auto set = shell.Get<Fn>("SetCurrentProcessExplicitAppUserModelID"))
        set(kAppUserModelId);
}

void HardenCurrentProcess() {
    // Fail closed on heap corruption and refuse images loaded from the network
    // or from a low-integrity process. The optional GPU and monitor libraries
    // resolve explicitly from System32 and are unaffected. The mitigation API is
    // resolved at run time so systems without it still run.
    ::HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);

    typedef BOOL (WINAPI *PfnSetMitigation)(PROCESS_MITIGATION_POLICY, PVOID, SIZE_T);
    HMODULE kernel = ::GetModuleHandleW(L"kernel32.dll");
    PfnSetMitigation set = kernel
        ? (PfnSetMitigation)(void*)::GetProcAddress(kernel, "SetProcessMitigationPolicy")
        : nullptr;
    if (set) {
        PROCESS_MITIGATION_IMAGE_LOAD_POLICY image = {};
        image.NoRemoteImages = 1;
        image.NoLowMandatoryLabelImages = 1;
        image.PreferSystem32Images = 1;
        set(ProcessImageLoadPolicy, &image, sizeof(image));
    }
}

std::vector<std::wstring> ParseCommandLine() {
    std::vector<std::wstring> args;
    int count = 0;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &count);
    if (!argv) return args;
    for (int i = 1; i < count; ++i) args.push_back(argv[i]);
    ::LocalFree(argv);
    return args;
}

bool HasFlag(const std::vector<std::wstring>& args, const wchar_t* flag) {
    for (const auto& a : args) if (IEquals(a, flag)) return true;
    return false;
}

std::wstring Quote(const std::wstring& s) {
    return s.find(L' ') == std::wstring::npos ? s : L"\"" + s + L"\"";
}

/// Prints a command reply. When invoked from a console, writes to that console;
/// otherwise falls back to a dialog box, so command line use inside a script
/// never leaves windows to dismiss by hand.
void Report(const std::wstring& text, bool isError) {
    if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE out = ::GetStdHandle(isError ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
        if (out && out != INVALID_HANDLE_VALUE) {
            DWORD mode = 0;
            DWORD written = 0;
            // A real console takes UTF-16 directly: UTF-8 bytes are mangled by a
            // console code page that is rarely 65001, which corrupts accented
            // characters. When output is redirected to a file or a pipe,
            // GetConsoleMode fails and UTF-8 is the correct encoding.
            if (::GetConsoleMode(out, &mode)) {
                const std::wstring line = text + L"\r\n";
                ::WriteConsoleW(out, line.c_str(), (DWORD)line.size(), &written, nullptr);
            } else {
                const std::string utf8 = WideToUtf8(text) + "\r\n";
                ::WriteFile(out, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
            }
            ::FreeConsole();
            return;
        }
        ::FreeConsole();
    }
    ::MessageBoxW(nullptr, text.c_str(), L"Zdisplay",
                  MB_OK | (isError ? MB_ICONWARNING : MB_ICONINFORMATION));
}

void ShowHelp() {
    const wchar_t* text =
        L"Zdisplay " ZDISPLAY_VERSION_WSTR
        L" — controle de brilho, contraste, saturação, gamma e temperatura de cor.\n\n"
        L"Sem argumentos, abre o programa (ou traz a janela da instância já aberta).\n"
        L"Com argumentos, envia o comando para a instância que esta rodando.\n\n"
        L"ARRANQUE\n"
        L"  --tray                 inicia direto na bandeja, sem janela\n"
        L"  --verbose              log detalhado\n"
        L"  --make-icon            gera assets\\zdisplay.ico e sai\n\n"
        L"COMANDOS\n"
        L"  --profile \"Jogo\"       ativa um perfil\n"
        L"  --auto                 volta ao modo automático\n"
        L"  --brightness 80        brilho por software, 10..150\n"
        L"  --contrast 110         contraste, 0..200\n"
        L"  --saturation 130       saturação, 0..200\n"
        L"  --vibrance 50          vibrance da GPU, 0..100\n"
        L"  --temperature 3400     temperatura de cor em Kelvin\n"
        L"  --gamma 1.1            gamma, 0.3..3.0\n"
        L"  --shadows 70           levanta só os tons escuros, 0..100\n"
        L"  --clarity 50           detalhe nas sombras, 0..100\n"
        L"  --hue 15               matiz, -180..180\n"
        L"  --dim 20               escurecimento por sobreposição, 0..90\n"
        L"  --hwbrightness 60      brilho físico (DDC/CI ou backlight)\n"
        L"  --toggle | --on | --off\n"
        L"  --reset                restaura a tela ao estado original\n"
        L"  --panic                EMERGÊNCIA: devolve a tela e pausa\n"
        L"  --status               mostra o estado atual\n"
        L"  --diag                 lista os backends detectados\n"
        L"  --list                 lista os perfis\n"
        L"  --show                 abre a janela de configuração\n"
        L"  --aba 5                abre a janela numa aba: 0 Ajustes, 1 Visão,\n"
        L"                         2 Perfis, 3 Automação, 4 Sistema, 5 Diagnóstico\n"
        L"  --quit                 encerra o programa\n";

    // Help follows the same path as every other reply: the console when there is
    // one, a dialog otherwise.
    Report(text, false);
}

int RunApp(HINSTANCE inst, const std::vector<std::wstring>& args) {
    App app;
    if (!app.Init(inst, args)) return 1;

    // Arguments that are not plain startup flags become a command.
    std::wstring command;
    for (const auto& a : args) {
        if (IEquals(a, L"--tray") || IEquals(a, L"--minimized") ||
            IEquals(a, L"--background") || IEquals(a, L"--verbose") || IEquals(a, L"-v"))
            continue;
        if (!command.empty()) command += L" ";
        command += Quote(a);
    }
    if (!command.empty()) app.HandleCommand(command);

    return app.Run();
}

}  // namespace
}  // namespace zdisplay

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    using namespace zdisplay;

    HardenCurrentProcess();

    // Before anything else loads: remove the executable's directory and the
    // working directory from the DLL search order. This complements the
    // LOAD_LIBRARY_SEARCH_SYSTEM32 in DynLib::Load and additionally covers the
    // libraries Windows loads on its own (late dependencies of comctl32, ole32,
    // shell32).
    ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 |
                               LOAD_LIBRARY_SEARCH_USER_DIRS);

    // Must run before the first window and the first tray icon: Windows fixes
    // the process identity on first use and ignores later changes.
    DeclareAppIdentity();

    const auto args = ParseCommandLine();

    // Disposable helper process for the capabilities query. Handled before the
    // mutex: it is a child of the main instance, not a second instance trying to
    // take over the application.
    for (size_t i = 0; i < args.size(); ++i) {
        if (IEquals(args[i], L"--ddc-caps-worker")) {
            if (i + 2 >= args.size()) return 3;
            return RunDdcCapabilitiesProbe(args[i + 1], args[i + 2]);
        }
    }

    if (HasFlag(args, L"--help") || HasFlag(args, L"-h") || HasFlag(args, L"/?")) {
        ShowHelp();
        return 0;
    }

    // Writes the application icon and exits. The build script uses this so the
    // binary depends on no external image tooling.
    if (HasFlag(args, L"--make-icon")) {
        // Silent by design: the build script calls this between its two passes
        // and must not block on a dialog.
        ::CreateDirectoryW(L"assets", nullptr);
        return WriteIcoFile(L"assets\\zdisplay.ico", 64) ? 0 : 1;
    }

    // Single instance: a second invocation forwards its arguments as a command
    // over the named pipe and exits.
    HANDLE mutex = ::CreateMutexW(nullptr, TRUE, kMutexName);
    const bool alreadyRunning = mutex && ::GetLastError() == ERROR_ALREADY_EXISTS;

    if (alreadyRunning) {
        std::wstring command;
        for (const auto& a : args) {
            if (!command.empty()) command += L" ";
            command += Quote(a);
        }
        if (command.empty()) command = L"--show";

        const std::wstring reply = PipeServer::SendCommand(command);
        if (mutex) ::CloseHandle(mutex);

        if (reply.rfind(L"erro:", 0) == 0) {
            Report(reply, true);
            return 1;
        }
        // Query commands print their reply; the rest act silently.
        if (HasFlag(args, L"--status") || HasFlag(args, L"--list") ||
            HasFlag(args, L"--diag") || HasFlag(args, L"--perfis") ||
            HasFlag(args, L"--diagnostico"))
            Report(reply, false);

        return 0;
    }

    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    LogInit(HasFlag(args, L"--verbose") || HasFlag(args, L"-v"));
    KLOG_I(L"=== Zdisplay " ZDISPLAY_VERSION_WSTR L" iniciando ===");
    KLOG_I(L"Executável: %s", ExePath().c_str());
    KLOG_I(L"Configuração: %s", ConfigPath().c_str());

    const int result = RunApp(inst, args);

    KLOG_I(L"=== Zdisplay encerrado ===");
    ::CoUninitialize();
    if (mutex) { ::ReleaseMutex(mutex); ::CloseHandle(mutex); }
    return result;
}
