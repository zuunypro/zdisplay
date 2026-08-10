// Zdisplay — brightness, contrast, saturation, gamma and color temperature
// control. Pure Win32, with no runtime and no external dependencies.
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00   // Windows 10
#endif

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace zdisplay {

// Utilities

/// UTF-8 <-> UTF-16 conversion. The config file on disk is UTF-8; the Windows
/// API is UTF-16.
std::wstring Utf8ToWide(const std::string& s);
std::string  WideToUtf8(const std::wstring& s);

/// printf into a std::wstring. Used for log lines and labels.
std::wstring Format(const wchar_t* fmt, ...);

std::wstring Trim(const std::wstring& s);
std::wstring ToLower(std::wstring s);
bool IEquals(const std::wstring& a, const std::wstring& b);

/// Matches with the '*' wildcard (used by the per-application rules).
bool WildcardMatch(const std::wstring& pattern, const std::wstring& text);

/// Parses a number using '.' as the decimal separator, independent of the
/// system language.
bool ParseDouble(const std::wstring& s, double* out);
std::wstring FormatDouble(double v, int decimals = 2);

/// Clamps v to [lo, hi]. NaN maps to `lo`.
///
/// NaN compares false against every bound, so a plain ternary returns NaN
/// itself, which then reaches a gamma ramp entry — `(WORD)llround(NaN * 65535.0)`
/// blanks the display — and slips past safety comparisons, since
/// `std::min(x, NaN)` returns x.
template <typename T>
inline T Clamp(T v, T lo, T hi) {
    if (!(v == v)) return lo;
    return v < lo ? lo : (v > hi ? hi : v);
}

inline double Lerp(double a, double b, double t) { return a + (b - a) * t; }

/// Interpolation factor with acceleration and deceleration (smoothstep).
inline double Ease(double t) { return t * t * (3.0 - 2.0 * t); }

// Paths

/// Config directory. Next to the executable when "zdisplay-portable.txt" exists
/// (portable mode), otherwise %APPDATA%\Zdisplay.
const std::wstring& ConfigDir();
std::wstring ConfigPath();
std::wstring LogPath();
std::wstring ExePath();
bool EnsureDir(const std::wstring& dir);

// Log

enum class LogLevel { Debug, Info, Warn, Error };

void LogInit(bool verbose);
void LogWrite(LogLevel level, const wchar_t* fmt, ...);
/// Most recent lines held in memory, shown on the diagnostics tab.
std::vector<std::wstring> LogRecent();

#define KLOG_D(...) ::zdisplay::LogWrite(::zdisplay::LogLevel::Debug, __VA_ARGS__)
#define KLOG_I(...) ::zdisplay::LogWrite(::zdisplay::LogLevel::Info,  __VA_ARGS__)
#define KLOG_W(...) ::zdisplay::LogWrite(::zdisplay::LogLevel::Warn,  __VA_ARGS__)
#define KLOG_E(...) ::zdisplay::LogWrite(::zdisplay::LogLevel::Error, __VA_ARGS__)

// RAII

/// Loads a DLL at run time. When the library is absent the object stays empty
/// and the matching backend is simply not used, so one binary runs unchanged
/// whatever graphics hardware and drivers are present.
class DynLib {
public:
    DynLib() = default;
    explicit DynLib(const wchar_t* name) { Load(name); }
    ~DynLib() { Free(); }

    DynLib(const DynLib&) = delete;
    DynLib& operator=(const DynLib&) = delete;
    DynLib(DynLib&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }

    /// Always resolves from System32.
    ///
    /// Every library loaded here lives in System32 and none of them is a
    /// KnownDLL, so with a bare name the executable's own directory would
    /// precede the system directory in the search order.
    /// LOAD_LIBRARY_SEARCH_SYSTEM32 is what keeps a file dropped beside the
    /// executable from being loaded in place of the system library.
    bool Load(const wchar_t* name) {
        Free();
        h_ = ::LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return h_ != nullptr;
    }
    void Free() {
        if (h_) { ::FreeLibrary(h_); h_ = nullptr; }
    }
    bool Ok() const { return h_ != nullptr; }
    HMODULE Handle() const { return h_; }

    template <typename Fn>
    Fn Get(const char* name) const {
        if (!h_) return nullptr;
        return reinterpret_cast<Fn>(
            reinterpret_cast<void*>(::GetProcAddress(h_, name)));
    }

private:
    HMODULE h_ = nullptr;
};

/// Device DC released automatically.
class DeviceDC {
public:
    explicit DeviceDC(const std::wstring& device)
        : dc_(::CreateDCW(nullptr, device.c_str(), nullptr, nullptr)) {}
    ~DeviceDC() { if (dc_) ::DeleteDC(dc_); }
    DeviceDC(const DeviceDC&) = delete;
    DeviceDC& operator=(const DeviceDC&) = delete;
    HDC Get() const { return dc_; }
    bool Ok() const { return dc_ != nullptr; }
private:
    HDC dc_;
};

/// RAII wrapper around a critical section.
class Lock {
public:
    Lock() { ::InitializeCriticalSection(&cs_); }
    ~Lock() { ::DeleteCriticalSection(&cs_); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    void Enter() { ::EnterCriticalSection(&cs_); }
    void Leave() { ::LeaveCriticalSection(&cs_); }
private:
    CRITICAL_SECTION cs_;
};

class Guard {
public:
    explicit Guard(Lock& l) : l_(l) { l_.Enter(); }
    ~Guard() { l_.Leave(); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
private:
    Lock& l_;
};

/// Monotonic clock in milliseconds (used by transitions).
double NowMs();

}  // namespace zdisplay
