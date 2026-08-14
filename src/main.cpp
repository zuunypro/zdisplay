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
    // Three blocks rather than one: the flag names have to stay verbatim, and a
    // reworded sentence in one section should not cost the translation of the
    // other two.
    std::wstring text = L"Zdisplay " ZDISPLAY_VERSION_WSTR;
    text += T(L" — brightness, contrast, saturation, gamma and color temperature "
              L"control.\n\n"
              L"With no arguments it opens the program (or brings up the window of the "
              L"instance already running).\n"
              L"With arguments it sends the command to the running instance.\n\n");
    text += T(L"STARTUP\n"
              L"  --tray                 start straight in the tray, with no window\n"
              L"  --verbose              detailed log\n"
              L"  --make-icon            write assets\\zdisplay.ico and exit\n\n");
    text += T(L"COMMANDS\n"
              L"  --profile \"Game\"       activate a profile\n"
              L"  --auto                 return to automatic mode\n"
              L"  --brightness 80        software brightness, 10..150\n"
              L"  --contrast 110         contrast, 0..200\n"
              L"  --saturation 130       saturation, 0..200\n"
              L"  --vibrance 50          GPU vibrance, 0..100\n"
              L"  --temperature 3400     color temperature in kelvin\n"
              L"  --gamma 1.1            gamma, 0.3..3.0\n"
              L"  --shadows 70           raise only the dark tones, 0..100\n"
              L"  --clarity 50           shadow detail, 0..100\n"
              L"  --hue 15               hue, -180..180\n"
              L"  --dim 20               dimming by overlay, 0..90\n"
              L"  --hwbrightness 60      physical brightness (DDC/CI or backlight)\n"
              L"  --toggle | --on | --off\n"
              L"  --reset                restore the display to its original state\n"
              L"  --panic                EMERGENCY: give the display back and pause\n"
              L"  --status               show the current state\n"
              L"  --diag                 list the detected backends\n"
              L"  --list                 list the profiles\n"
              L"  --show                 open the settings window\n"
              L"  --tab 5                open the window on a tab: 0 Adjustments,\n"
              L"                         1 Vision, 2 Profiles, 3 Automation,\n"
              L"                         4 System, 5 Diagnostics\n"
              L"  --quit                 close the program\n");

    // Help follows the same path as every other reply: the console when there is
    // one, a dialog otherwise.
    Report(text.c_str(), false);
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
        // The help is the one piece of text produced before the application
        // exists, so nothing has resolved the language yet. It follows Windows
        // rather than the configuration file on purpose: LoadConfig creates and
        // seeds the file when it is missing, and printing help is not a reason
        // to write anything to disk.
        SetLanguage(LangChoice::Auto);
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

        if (reply.rfind(kCommandErrorPrefix, 0) == 0) {
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
    KLOG_I(L"=== Zdisplay " ZDISPLAY_VERSION_WSTR L" starting ===");
    KLOG_I(L"Executable: %s", ExePath().c_str());
    KLOG_I(L"Configuration: %s", ConfigPath().c_str());

    const int result = RunApp(inst, args);

    KLOG_I(L"=== Zdisplay stopped ===");
    ::CoUninitialize();
    if (mutex) { ::ReleaseMutex(mutex); ::CloseHandle(mutex); }
    return result;
}
