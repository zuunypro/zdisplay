// Zdisplay installer: the contract between the interface (setup_ui.cpp) and the
// install work (setup_work.cpp).
//
// The installer is a single dependency-free executable carrying a compressed
// zdisplay.exe inside it. It performs a per-user install into %LOCALAPPDATA%
// and never requires elevation, so it runs on any account, privileged or not.
//
// The same executable is the uninstaller: it copies itself into the install
// root as desinstalar.exe and, invoked with /uninstall, undoes its own work.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>

namespace setup {

// Interface language
//
// English is the primary language and is the key each message is written as, so
// a call site reads as the sentence it produces. The installer runs before any
// configuration exists, so it follows the Windows UI language and nothing else.
/// Reads the Windows UI language. Called once, before any text is produced.
void DetectLanguage();
/// Translation of an English message, or the message itself when there is none.
const wchar_t* Text(const wchar_t* english);

// Identity
extern const wchar_t* const kAppName;      ///< "Zdisplay"
extern const wchar_t* const kExeName;      ///< "zdisplay.exe"
extern const wchar_t* const kUninstName;   ///< "desinstalar.exe"
extern const wchar_t* const kPublisher;
extern const wchar_t* const kVersionStr;   ///< comes from src/version.h
extern const wchar_t* const kAppKey;       ///< HKCU\Software\Zdisplay
extern const wchar_t* const kUninstallKey; ///< HKCU\...\CurrentVersion\Uninstall\Zdisplay
extern const wchar_t* const kRunKey;       ///< HKCU\...\CurrentVersion\Run
extern const wchar_t* const kRunValue;     ///< "Zdisplay", the same name the application uses

// Options from the interface
struct Options {
    std::wstring dir;             ///< install root
    bool autostart     = true;    ///< start with Windows (HKCU\...\Run)
    bool desktopIcon   = false;   ///< desktop shortcut
    bool launchAfter   = true;    ///< open Zdisplay when finished
    /// Unlock the full gamma range (HKLM, requires elevation). On by default
    /// because the main feature arrives halved without it; declining elevation
    /// still completes the install, see RequestFullGammaRange.
    bool fullGammaRange = true;
    bool removeSettings = false;  ///< uninstall only: delete %APPDATA%\Zdisplay
};

struct Result {
    bool ok = false;
    std::wstring message;  ///< user-facing failure text (Portuguese) when !ok
};

/// Called from inside the worker thread. `percent` runs from 0 to 100 and
/// `step` is the short status line shown on screen.
typedef void (*ProgressFn)(void* ctx, int percent, const wchar_t* step);

Result Install(const Options& opt, ProgressFn progress, void* ctx);
Result Uninstall(const Options& opt, ProgressFn progress, void* ctx);

// Utilities
std::wstring DefaultInstallDir();   ///< %LOCALAPPDATA%\Programs\Zdisplay
std::wstring SelfPath();            ///< path of this executable
std::wstring SelfDir();

// Windows gamma range
//
// By default Windows rejects any gamma ramp that departs too far from linear.
// The limit guards against programs that would black out the screen, and it is
// applied silently: SetDeviceGammaRamp reports success while the system applies
// a diluted curve, delivering roughly half of the requested shadow adjustment.
//
// Setting GdiIcmGammaRange = 256 in HKLM releases the limit. It is machine-wide,
// so it requires elevation and only takes effect at the next login.
//
// Enabling it alone changes nothing on screen; it only stops stronger ramps from
// being rejected. Uninstalling therefore leaves the value in place, since other
// calibration software may have come to depend on it.

/// Whether the full range is already enabled. A plain read, no elevation.
bool FullGammaRangeEnabled();

/// Writes the value. Requires elevation; call only from the elevated process.
bool WriteFullGammaRange(std::wstring* error);

/// Relaunches THIS executable elevated only to write the value, and waits for
/// it to finish. Returns false if elevation was declined or the write failed.
bool RequestFullGammaRange(HWND owner, std::wstring* error);

/// Canonicalizes and validates the install root. For safety it must be an
/// absolute local path ending in "Zdisplay" and must not be a reparse point.
bool NormalizeInstallDir(const std::wstring& input, std::wstring* normalized,
                         std::wstring* why);

/// Reads HKCU\Software\Zdisplay\InstallDir. True when an installation exists.
bool FindInstalled(std::wstring* dir);

/// Uncompressed size of the embedded zdisplay.exe, or 0 when the resource is
/// missing, in which case the interface reports it instead of installing.
DWORD PayloadSize();

/// Unpacks and validates the SHA-256 and PE structure without installing.
Result VerifyEmbeddedPayload();

std::wstring FormatError(DWORD err);

/// Launches the installed Zdisplay in the tray at the end of the install.
bool LaunchInstalled(const std::wstring& dir);

/// Asks the user for a folder using the system dialog. False if cancelled.
bool PickFolder(HWND owner, const std::wstring& current, std::wstring* out);

}  // namespace setup
