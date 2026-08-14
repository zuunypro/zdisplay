#include "common.h"

#include <cstdarg>
#include <deque>

namespace zdisplay {

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
    return out;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return "";
    int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(),
                                  nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string out((size_t)n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring Format(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    wchar_t buf[2048];
    int n = _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) return L"";
    return std::wstring(buf);
}

std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return (wchar_t)::towlower(c); });
    return s;
}

bool IEquals(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

bool WildcardMatch(const std::wstring& pattern, const std::wstring& text) {
    // Simple case-insensitive match supporting '*' and '?'.
    const std::wstring p = ToLower(pattern);
    const std::wstring t = ToLower(text);

    size_t pi = 0, ti = 0, star = std::wstring::npos, mark = 0;
    while (ti < t.size()) {
        if (pi < p.size() && (p[pi] == L'?' || p[pi] == t[ti])) {
            ++pi; ++ti;
        } else if (pi < p.size() && p[pi] == L'*') {
            star = pi++;
            mark = ti;
        } else if (star != std::wstring::npos) {
            pi = star + 1;
            ti = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == L'*') ++pi;
    return pi == p.size();
}

bool ParseDouble(const std::wstring& s, double* out) {
    if (s.empty() || !out) return false;
    // Accepts a comma as the decimal separator but always reads it as a point,
    // so the config file does not depend on the system language.
    std::wstring t = Trim(s);
    for (auto& c : t) if (c == L',') c = L'.';

    wchar_t* end = nullptr;
    double v = ::wcstod(t.c_str(), &end);
    if (end == t.c_str()) return false;
    while (end && *end && iswspace(*end)) ++end;
    if (end && *end) return false;
    *out = v;
    return true;
}

std::wstring FormatDouble(double v, int decimals) {
    wchar_t buf[64];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%.*f", decimals, v);
    // Strip trailing zeros to keep the config file tidy.
    std::wstring s(buf);
    if (s.find(L'.') != std::wstring::npos) {
        while (!s.empty() && s.back() == L'0') s.pop_back();
        if (!s.empty() && s.back() == L'.') s.pop_back();
    }
    return s.empty() ? L"0" : s;
}

double NowMs() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f; ::QueryPerformanceFrequency(&f); return f;
    }();
    LARGE_INTEGER now;
    ::QueryPerformanceCounter(&now);
    return (double)now.QuadPart * 1000.0 / (double)freq.QuadPart;
}

std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = ::GetModuleFileNameW(nullptr, buf, _countof(buf));
    return std::wstring(buf, n);
}

static std::wstring DirOf(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? L"." : path.substr(0, p);
}

bool EnsureDir(const std::wstring& dir) {
    if (dir.empty()) return false;
    if (::CreateDirectoryW(dir.c_str(), nullptr)) return true;
    return ::GetLastError() == ERROR_ALREADY_EXISTS;
}

const std::wstring& ConfigDir() {
    static std::wstring cached = [] () -> std::wstring {
        const std::wstring exeDir = DirOf(ExePath());

        // Portable mode: an empty zdisplay-portable.txt beside the executable
        // makes everything be written to that same folder.
        const std::wstring marker = exeDir + L"\\zdisplay-portable.txt";
        if (::GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES)
            return exeDir;

        wchar_t* appdata = nullptr;
        std::wstring dir;
        if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata)) && appdata) {
            dir = std::wstring(appdata) + L"\\Zdisplay";
            ::CoTaskMemFree(appdata);
        } else {
            dir = exeDir;
        }
        EnsureDir(dir);
        return dir;
    }();
    return cached;
}

std::wstring ConfigPath() { return ConfigDir() + L"\\zdisplay.ini"; }
std::wstring LogPath()    { return ConfigDir() + L"\\zdisplay.log"; }

namespace {
Lock g_logLock;
std::deque<std::wstring> g_recent;
bool g_verbose = false;
const long kMaxLogBytes = 512 * 1024;
}  // namespace

void LogInit(bool verbose) { g_verbose = verbose; }

void LogWrite(LogLevel level, const wchar_t* fmt, ...) {
    if (level == LogLevel::Debug && !g_verbose) return;

    wchar_t body[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(body, _countof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    const wchar_t* tag = L"INFO";
    switch (level) {
        case LogLevel::Debug: tag = L"DEBUG"; break;
        case LogLevel::Info:  tag = L"INFO";  break;
        case LogLevel::Warn:  tag = L"WARN"; break;
        case LogLevel::Error: tag = L"ERROR";  break;
    }

    SYSTEMTIME st;
    ::GetLocalTime(&st);
    std::wstring line = Format(L"%04d-%02d-%02d %02d:%02d:%02d [%s] %s",
                               st.wYear, st.wMonth, st.wDay,
                               st.wHour, st.wMinute, st.wSecond, tag, body);

    Guard g(g_logLock);
    g_recent.push_back(line);
    while (g_recent.size() > 300) g_recent.pop_front();

    // Writing to disk is best effort and never fails the program.
    const std::wstring path = LogPath();
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) &&
        fad.nFileSizeLow > (DWORD)kMaxLogBytes) {
        const std::wstring old = path + L".1";
        ::DeleteFileW(old.c_str());
        ::MoveFileW(path.c_str(), old.c_str());
    }

    HANDLE h = ::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    std::string utf8 = WideToUtf8(line) + "\r\n";
    DWORD written = 0;
    ::WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
    ::CloseHandle(h);
}

std::vector<std::wstring> LogRecent() {
    Guard g(g_logLock);
    return std::vector<std::wstring>(g_recent.begin(), g_recent.end());
}

}  // namespace zdisplay
