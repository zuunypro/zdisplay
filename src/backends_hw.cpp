// Hardware-level display backends: DDC/CI for external monitors and WMI
// backlight for notebook panels. Both reduce actual light output instead of
// darkening the rendered image.
//
// Both are slow (tens to hundreds of milliseconds per command) and DDC/CI
// writes to the monitor EEPROM, so they run on a dedicated thread with
// coalescing and a minimum interval between commands.
#include "backends.h"

#include <wbemidl.h>

namespace zdisplay {

// DDC/CI

namespace ddc {

struct PhysicalMonitorRec {
    HANDLE hPhysicalMonitor;
    WCHAR  szPhysicalMonitorDescription[128];
};

enum FnIndex {
    FN_GetCount = 0, FN_GetMonitors, FN_Destroy,
    FN_GetBrightness, FN_SetBrightness,
    FN_GetContrast, FN_SetContrast,
    FN_GetVcp, FN_SetVcp, FN_CapsLen, FN_Caps, FN_COUNT
};

typedef BOOL (WINAPI *PfnGetCount)(HMONITOR, LPDWORD);
typedef BOOL (WINAPI *PfnGetMonitors)(HMONITOR, DWORD, PhysicalMonitorRec*);
typedef BOOL (WINAPI *PfnDestroy)(DWORD, PhysicalMonitorRec*);
typedef BOOL (WINAPI *PfnGetBrightness)(HANDLE, LPDWORD, LPDWORD, LPDWORD);
typedef BOOL (WINAPI *PfnSetBrightness)(HANDLE, DWORD);
typedef BOOL (WINAPI *PfnGetContrast)(HANDLE, LPDWORD, LPDWORD, LPDWORD);
typedef BOOL (WINAPI *PfnSetContrast)(HANDLE, DWORD);
// Low-level path: issues VCP requests directly, bypassing the capability
// string validation the high-level API performs internally.
typedef BOOL (WINAPI *PfnGetVcp)(HANDLE, BYTE, DWORD*, LPDWORD, LPDWORD);
typedef BOOL (WINAPI *PfnSetVcp)(HANDLE, BYTE, DWORD);
typedef BOOL (WINAPI *PfnCapsLen)(HANDLE, LPDWORD);
typedef BOOL (WINAPI *PfnCaps)(HANDLE, LPSTR, DWORD);

/// MCCS VCP codes used by this backend.
constexpr BYTE VCP_LUMINANCE  = 0x10;
constexpr BYTE VCP_CONTRAST   = 0x12;
constexpr BYTE VCP_GAIN_RED   = 0x16;
constexpr BYTE VCP_GAIN_GREEN = 0x18;
constexpr BYTE VCP_GAIN_BLUE  = 0x1A;
constexpr BYTE kGainCodes[3] = { VCP_GAIN_RED, VCP_GAIN_GREEN, VCP_GAIN_BLUE };

constexpr double kMinIntervalMs = 140.0;
constexpr int    kMaxFailures = 4;
constexpr int    kMaxAttempts = 3;
constexpr DWORD  kRetryDelayMs = 150;
/// Hard ceiling on writes per minute to a single monitor. Bounds EEPROM wear
/// regardless of how often callers request changes.
constexpr int    kMaxCommandsPerMinute = 40;

DWORD ScaleTo(double percent, DWORD lo, DWORD hi) {
    percent = Clamp(percent, 0.0, 100.0);
    return (DWORD)llround(lo + percent / 100.0 * (double)(hi - lo));
}

struct IoResult {
    bool ok = false;
    DWORD error = 0;
    DdcErrorKind kind = DdcErrorKind::None;
    int attempts = 0;
};

template <typename Call>
IoResult Invoke(Call call, int maxAttempts = kMaxAttempts,
                DWORD retryDelayMs = kRetryDelayMs) {
    IoResult result;
    maxAttempts = Clamp(maxAttempts, 1, kMaxAttempts);
    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
        ::SetLastError(ERROR_SUCCESS);
        ++result.attempts;
        if (call() != FALSE) {
            result.ok = true;
            result.error = ERROR_SUCCESS;
            result.kind = DdcErrorKind::None;
            return result;
        }

        result.error = ::GetLastError();
        // Some drivers fail without setting LastError. That is not evidence of
        // a transient fault, so it must not be classified as retryable.
        if (result.error == ERROR_SUCCESS) result.error = ERROR_GEN_FAILURE;
        result.kind = ClassifyDdcError(result.error);
        if (result.kind != DdcErrorKind::Transient || attempt == maxAttempts) break;
        ::Sleep(retryDelayMs);
    }
    return result;
}

const wchar_t* ErrorKindName(DdcErrorKind kind) {
    switch (kind) {
        case DdcErrorKind::None:        return L"nenhum";
        case DdcErrorKind::Unsupported: return L"não suportado";
        case DdcErrorKind::Transient:   return L"transitorio";
        case DdcErrorKind::Unavailable: return L"monitor/handle indisponível";
        case DdcErrorKind::Permanent:   return L"permanente/desconhecido";
    }
    return L"desconhecido";
}

/// Reads a feature through the high-level API. Only brightness and contrast
/// are available there; any other code fails as unsupported.
bool ReadHigh(void* const* fns, HANDLE h, BYTE code, DWORD* lo, DWORD* cur, DWORD* hi,
              IoResult* outcome = nullptr) {
    IoResult result;
    if (code == VCP_LUMINANCE) {
        auto f = (PfnGetBrightness)fns[FN_GetBrightness];
        if (f) result = Invoke([&]() { return f(h, lo, cur, hi); });
        else { result.error = ERROR_PROC_NOT_FOUND; result.kind = DdcErrorKind::Permanent; }
        if (outcome) *outcome = result;
        return result.ok;
    }
    if (code == VCP_CONTRAST) {
        auto f = (PfnGetContrast)fns[FN_GetContrast];
        if (f) result = Invoke([&]() { return f(h, lo, cur, hi); });
        else { result.error = ERROR_PROC_NOT_FOUND; result.kind = DdcErrorKind::Permanent; }
        if (outcome) *outcome = result;
        return result.ok;
    }
    result.error = ERROR_NOT_SUPPORTED;
    result.kind = DdcErrorKind::Unsupported;
    if (outcome) *outcome = result;
    return false;
}

/// Reads a feature by issuing a raw VCP request to the monitor.
bool ReadVcp(void* const* fns, HANDLE h, BYTE code, DWORD* lo, DWORD* cur, DWORD* hi,
             DWORD* codeType = nullptr, IoResult* outcome = nullptr) {
    auto f = (PfnGetVcp)fns[FN_GetVcp];
    if (!f) {
        if (outcome) {
            outcome->error = ERROR_PROC_NOT_FOUND;
            outcome->kind = DdcErrorKind::Permanent;
        }
        return false;
    }
    DWORD type = 0, c = 0, m = 0;
    const IoResult result = Invoke([&]() { return f(h, code, &type, &c, &m); });
    if (outcome) *outcome = result;
    if (!result.ok) return false;
    if (m == 0) return false;          // no usable range: the code is not exposed
    if (codeType) *codeType = type;
    *lo = 0; *cur = c; *hi = m;
    return true;
}

/// Reads through the high-level API, falling back to raw VCP. Reports in
/// `usedVcp` which path answered so writes can take the same one.
bool Read(void* const* fns, HANDLE h, BYTE code,
          DWORD* lo, DWORD* cur, DWORD* hi, bool* usedVcp,
          DWORD* codeType = nullptr, IoResult* outcome = nullptr) {
    IoResult highResult;
    if (ReadHigh(fns, h, code, lo, cur, hi, &highResult) && *hi > *lo) {
        if (usedVcp) *usedVcp = false;
        if (codeType) *codeType = 1;
        if (outcome) *outcome = highResult;
        return true;
    }
    DWORD type = 0;
    IoResult lowResult;
    if (ReadVcp(fns, h, code, lo, cur, hi, &type, &lowResult) && *hi > *lo) {
        if (usedVcp) *usedVcp = true;
        if (codeType) *codeType = type;
        if (outcome) *outcome = lowResult;
        return true;
    }
    if (outcome) *outcome = lowResult.error ? lowResult : highResult;
    return false;
}

IoResult Write(void* const* fns, HANDLE h, BYTE code, DWORD value, bool lowLevel,
               int maxAttempts = kMaxAttempts, DWORD retryDelayMs = kRetryDelayMs) {
    // RGB gain has no high-level equivalent and always goes through VCP.
    const bool highExists = (code == VCP_LUMINANCE || code == VCP_CONTRAST);
    if (!lowLevel && highExists) {
        if (code == VCP_LUMINANCE) {
            auto f = (PfnSetBrightness)fns[FN_SetBrightness];
            if (f) return Invoke([&]() { return f(h, value); }, maxAttempts, retryDelayMs);
            IoResult r; r.error = ERROR_PROC_NOT_FOUND; r.kind = DdcErrorKind::Permanent; return r;
        }
        auto f = (PfnSetContrast)fns[FN_SetContrast];
        if (f) return Invoke([&]() { return f(h, value); }, maxAttempts, retryDelayMs);
        IoResult r; r.error = ERROR_PROC_NOT_FOUND; r.kind = DdcErrorKind::Permanent; return r;
    }
    auto f = (PfnSetVcp)fns[FN_SetVcp];
    if (f) return Invoke([&]() { return f(h, code, value); }, maxAttempts, retryDelayMs);
    IoResult r; r.error = ERROR_PROC_NOT_FOUND; r.kind = DdcErrorKind::Permanent; return r;
}

/// Returns the raw MCCS capability string, or an empty string on failure.
/// Slow: call only from the queue thread, never from the startup path.
std::string ReadCapabilities(void* const* fns, HANDLE h) {
    auto len = (PfnCapsLen)fns[FN_CapsLen];
    auto get = (PfnCaps)fns[FN_Caps];
    if (!len || !get) return std::string();

    DWORD n = 0;
    if (!len(h, &n) || n == 0 || n > 65536) return std::string();

    std::vector<char> buf((size_t)n + 1, 0);
    if (!get(h, buf.data(), n)) return std::string();
    buf[(size_t)n] = 0;
    return std::string(buf.data());
}

bool ReadSmallFile(const std::wstring& path, std::string* out) {
    if (!out) return false;
    out->clear();
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = ::GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart <= 1024 * 1024;
    if (ok) {
        out->assign((size_t)size.QuadPart, 0);
        DWORD got = 0;
        ok = out->empty() || (::ReadFile(h, &(*out)[0], (DWORD)out->size(), &got, nullptr) &&
                              got == out->size());
    }
    ::CloseHandle(h);
    if (!ok) out->clear();
    return ok;
}

bool WriteSmallFileAtomic(const std::wstring& path, const std::string& data) {
    const std::wstring tmp = path + L".tmp";
    HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = data.empty() || (::WriteFile(h, data.data(), (DWORD)data.size(), &wrote, nullptr) &&
                               wrote == data.size());
    if (ok) ok = ::FlushFileBuffers(h) != FALSE;
    ::CloseHandle(h);
    if (!ok) { ::DeleteFileW(tmp.c_str()); return false; }
    if (!::MoveFileExW(tmp.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ::DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

/// Validates the output path accepted by the internal `--ddc-caps-worker` mode.
/// Only a `caps-result-*.tmp` file directly inside the config directory is
/// allowed; everything else returns false.
bool IsAllowedCapsOutput(const std::wstring& path) {
    const std::wstring dir = ConfigDir() + L"\\";
    if (path.size() <= dir.size()) return false;
    if (!IEquals(path.substr(0, dir.size()), dir)) return false;

    const std::wstring name = path.substr(dir.size());
    if (name.find(L'\\') != std::wstring::npos) return false;
    if (name.find(L'/')  != std::wstring::npos) return false;
    if (name.find(L"..") != std::wstring::npos) return false;
    if (name.rfind(L"caps-result-", 0) != 0) return false;
    return name.size() > 4 && IEquals(name.substr(name.size() - 4), L".tmp");
}

}  // namespace ddc

int RunDdcCapabilitiesProbe(const std::wstring& monitorKey,
                            const std::wstring& outputPath) {
    if (!ddc::IsAllowedCapsOutput(outputPath)) return 2;

    // Runs in a throwaway process so a hang or crash inside dxva2 or the
    // display driver cannot take the main process down with it.
    monitors::Refresh();
    const MonitorTarget* target = monitors::ByKey(monitorKey);
    if (!target) return 2;

    DynLib lib;
    if (!lib.Load(L"dxva2.dll")) return 2;
    auto getCount = lib.Get<ddc::PfnGetCount>("GetNumberOfPhysicalMonitorsFromHMONITOR");
    auto getMonitors = lib.Get<ddc::PfnGetMonitors>("GetPhysicalMonitorsFromHMONITOR");
    auto destroy = lib.Get<ddc::PfnDestroy>("DestroyPhysicalMonitors");
    auto capsLen = lib.Get<ddc::PfnCapsLen>("GetCapabilitiesStringLength");
    auto capsGet = lib.Get<ddc::PfnCaps>("CapabilitiesRequestAndCapabilitiesReply");
    if (!getCount || !getMonitors || !capsLen || !capsGet) return 2;

    DWORD count = 0;
    if (!getCount(target->handle, &count) || count == 0 || count > 64) return 2;
    auto* arr = (ddc::PhysicalMonitorRec*)::calloc(count, sizeof(ddc::PhysicalMonitorRec));
    if (!arr) return 2;
    if (!getMonitors(target->handle, count, arr)) { ::free(arr); return 2; }

    void* fns[ddc::FN_COUNT] = {};
    fns[ddc::FN_CapsLen] = (void*)capsLen;
    fns[ddc::FN_Caps] = (void*)capsGet;
    std::string caps;
    for (DWORD i = 0; i < count && caps.empty(); ++i)
        caps = ddc::ReadCapabilities(fns, arr[i].hPhysicalMonitor);

    if (destroy) destroy(count, arr);
    ::free(arr);
    if (caps.empty()) return 2; // completed safely, but the monitor answered nothing
    return ddc::WriteSmallFileAtomic(outputPath, caps) ? 0 : 3;
}

namespace ddc {

std::wstring QuoteChildArgument(const std::wstring& value) {
    // PnP keys never contain quotes, so rejecting them is preferable to
    // building an ambiguous command line.
    if (value.find(L'"') != std::wstring::npos) return std::wstring();
    return L"\"" + value + L"\"";
}

bool ReadCapabilitiesIsolated(const std::wstring& monitorKey, std::string* caps,
                              bool* dangerousFailure) {
    if (caps) caps->clear();
    if (dangerousFailure) *dangerousFailure = false;
    const std::wstring qExe = QuoteChildArgument(ExePath());
    const std::wstring qKey = QuoteChildArgument(monitorKey);
    const std::wstring resultPath = ConfigDir() + Format(L"\\caps-result-%lu-%llu.tmp",
        ::GetCurrentProcessId(), (unsigned long long)::GetTickCount64());
    const std::wstring qOut = QuoteChildArgument(resultPath);
    if (qExe.empty() || qKey.empty() || qOut.empty()) return false;

    std::wstring command = qExe + L" --ddc-caps-worker " + qKey + L" " + qOut;
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL started = ::CreateProcessW(ExePath().c_str(), mutableCommand.data(), nullptr, nullptr,
                                           FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!started) return false;
    ::CloseHandle(pi.hThread);

    const DWORD wait = ::WaitForSingleObject(pi.hProcess, 15000);
    DWORD exitCode = 3;
    if (wait == WAIT_TIMEOUT) {
        if (dangerousFailure) *dangerousFailure = true;
        ::TerminateProcess(pi.hProcess, ERROR_TIMEOUT);
        ::WaitForSingleObject(pi.hProcess, 1000);
    } else if (wait == WAIT_OBJECT_0) {
        ::GetExitCodeProcess(pi.hProcess, &exitCode);
        // 0 = capability string retrieved; 2 = monitor answered without one.
        // Any other code means a crash, a FailFast, or an internal helper error.
        if (dangerousFailure) *dangerousFailure = exitCode != 0 && exitCode != 2;
    } else if (dangerousFailure) {
        *dangerousFailure = true;
    }
    ::CloseHandle(pi.hProcess);

    bool ok = false;
    if (wait == WAIT_OBJECT_0 && exitCode == 0 && caps)
        ok = ReadSmallFile(resultPath, caps) && !caps->empty();
    ::DeleteFileW(resultPath.c_str());
    return ok;
}

}  // namespace ddc

void DdcciBackend::LoadSafetyState() {
    capsUnsafe_.clear();
    const std::wstring statePath = ConfigDir() + L"\\ddc-unstable.txt";
    std::string raw;
    if (ddc::ReadSmallFile(statePath, &raw)) {
        size_t start = 0;
        while (start <= raw.size()) {
            const size_t end = raw.find('\n', start);
            std::string line = raw.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t tab = line.find('\t');
            if (tab != std::string::npos && tab > 0)
                capsUnsafe_[Utf8ToWide(line.substr(0, tab))] = Utf8ToWide(line.substr(tab + 1));
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }

    // Imports the marker left behind by a run that died inside the capability
    // query. An empty marker names no monitor, so every monitor stays
    // quarantined until the user clears the block explicitly.
    const std::wstring marker = ConfigDir() + L"\\caps-probe.lock";
    if (ddc::ReadSmallFile(marker, &raw)) {
        const size_t nl = raw.find('\n');
        std::wstring key = Utf8ToWide(raw.substr(0, nl));
        if (key.empty()) key = L"*";
        capsUnsafe_[key] = L"queda durante leitura de capacidades";
        SaveSafetyState();
        ::DeleteFileW(marker.c_str());
        KLOG_W(L"DDC/CI: quarentena restaurada para '%s' após queda durante capabilities.",
               key.c_str());
    }
}

void DdcciBackend::SaveSafetyState() const {
    std::string data;
    {
        Guard g(const_cast<Lock&>(lock_));
        for (const auto& item : capsUnsafe_)
            data += WideToUtf8(item.first) + "\t" + WideToUtf8(item.second) + "\r\n";
    }
    const std::wstring path = ConfigDir() + L"\\ddc-unstable.txt";
    if (data.empty()) ::DeleteFileW(path.c_str());
    else if (!ddc::WriteSmallFileAtomic(path, data))
        KLOG_W(L"DDC/CI: não consegui persistir a lista de quarentena.");
}

void DdcciBackend::MarkCapsUnsafe(const std::wstring& monitorKey, const std::wstring& stage) {
    {
        Guard g(lock_);
        capsUnsafe_[monitorKey.empty() ? L"*" : monitorKey] = stage;
        auto it = monitors_.find(monitorKey);
        if (it != monitors_.end()) it->second.capsUnsafe = true;
    }
    SaveSafetyState();
}

int DdcciBackend::ClearSafetyBlocks() {
    int count = 0;
    {
        Guard g(lock_);
        count = (int)capsUnsafe_.size();
        capsUnsafe_.clear();
        for (auto& item : monitors_) item.second.capsUnsafe = false;
    }
    ::DeleteFileW((ConfigDir() + L"\\ddc-unstable.txt").c_str());
    ::DeleteFileW((ConfigDir() + L"\\caps-probe.lock").c_str());
    return count;
}

bool DdcciBackend::Init() {
    if (!lib_.Load(L"dxva2.dll")) {
        details_ = L"dxva2.dll indisponível";
        return false;
    }

    fns_[ddc::FN_GetCount]      = (void*)lib_.Get<ddc::PfnGetCount>("GetNumberOfPhysicalMonitorsFromHMONITOR");
    fns_[ddc::FN_GetMonitors]   = (void*)lib_.Get<ddc::PfnGetMonitors>("GetPhysicalMonitorsFromHMONITOR");
    fns_[ddc::FN_Destroy]       = (void*)lib_.Get<ddc::PfnDestroy>("DestroyPhysicalMonitors");
    fns_[ddc::FN_GetBrightness] = (void*)lib_.Get<ddc::PfnGetBrightness>("GetMonitorBrightness");
    fns_[ddc::FN_SetBrightness] = (void*)lib_.Get<ddc::PfnSetBrightness>("SetMonitorBrightness");
    fns_[ddc::FN_GetContrast]   = (void*)lib_.Get<ddc::PfnGetContrast>("GetMonitorContrast");
    fns_[ddc::FN_SetContrast]   = (void*)lib_.Get<ddc::PfnSetContrast>("SetMonitorContrast");
    fns_[ddc::FN_GetVcp]        = (void*)lib_.Get<ddc::PfnGetVcp>("GetVCPFeatureAndVCPFeatureReply");
    fns_[ddc::FN_SetVcp]        = (void*)lib_.Get<ddc::PfnSetVcp>("SetVCPFeature");
    fns_[ddc::FN_CapsLen]       = (void*)lib_.Get<ddc::PfnCapsLen>("GetCapabilitiesStringLength");
    fns_[ddc::FN_Caps]          = (void*)lib_.Get<ddc::PfnCaps>("CapabilitiesRequestAndCapabilitiesReply");

    // SetVCPFeature alone is sufficient: it works on monitors the high-level
    // API rejects, and the high-level path is only a preference.
    if (!fns_[ddc::FN_GetCount] || !fns_[ddc::FN_GetMonitors] ||
        (!fns_[ddc::FN_SetBrightness] && !fns_[ddc::FN_SetVcp])) {
        details_ = L"dxva2.dll sem as funções de DDC/CI";
        return false;
    }

    LoadSafetyState();
    Discover();
    if (monitors_.empty()) {
        details_ = L"nenhum monitor respondeu a DDC/CI (normal em tela de notebook)";
        return false;
    }

    wake_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wake_) { details_ = L"não consegui criar o evento da fila"; return false; }

    ::InterlockedExchange(&running_, 1);
    thread_ = ::CreateThread(nullptr, 0, WorkerThunk, this, 0, nullptr);
    if (!thread_) {
        ::InterlockedExchange(&running_, 0);
        details_ = L"não consegui criar a thread de DDC/CI";
        return false;
    }
    ::SetThreadPriority(thread_, THREAD_PRIORITY_BELOW_NORMAL);

    std::wstring desc;
    for (const auto& kv : monitors_) {
        if (!desc.empty()) desc += L"; ";
        desc += kv.second.description;
        desc += kv.second.hasBrightness ? L": brilho" : L": -";
        if (kv.second.hasContrast) desc += L"+contraste";
        if (kv.second.hasGain)     desc += L"+ganho RGB";
        if (kv.second.viaVcp)      desc += L" (VCP cru)";
    }
    details_ = desc;
    available_ = true;
    return true;
}

void DdcciBackend::RequestCapabilities() {
    if (!available_ || !wake_) return;
    ::InterlockedExchange(&needCaps_, 1);
    ::SetEvent(wake_);
}

void DdcciBackend::RequestRoundTrip() {
    if (!available_ || !wake_) return;
    ::InterlockedExchange(&needRoundTrip_, 1);
    ::SetEvent(wake_);
}

void DdcciBackend::RequestFeatureProbe() {
    if (!available_ || !wake_) return;
    ::InterlockedExchange(&needFeatures_, 1);
    ::SetEvent(wake_);
}

std::vector<DdcciBackend::Feature> DdcciBackend::Features(const std::wstring& monitorKey) const {
    Guard g(const_cast<Lock&>(lock_));
    auto it = monitors_.find(monitorKey);
    if (it == monitors_.end()) return {};
    return it->second.features;
}

bool DdcciBackend::FeaturesProbed(const std::wstring& monitorKey) const {
    Guard g(const_cast<Lock&>(lock_));
    auto it = monitors_.find(monitorKey);
    return it != monitors_.end() && it->second.featuresProbed;
}

void DdcciBackend::SetFeature(const std::wstring& monitorKey, unsigned char code, int value) {
    if (!available_ || !wake_) return;
    {
        Guard g(lock_);
        if (monitors_.find(monitorKey) == monitors_.end()) return;
        featureQueue_.push_back(std::make_pair(monitorKey, std::make_pair(code, value)));
    }
    ::SetEvent(wake_);
}

void DdcciBackend::ProbeFeatures() {
    // Queue thread only. Reads each VCP code directly instead of consulting the
    // capability string, which is slower and riskier.
    struct Job { std::wstring key; HANDLE handle; bool viaVcp; std::wstring caps; };
    std::vector<Job> jobs;
    {
        Guard g(lock_);
        for (const auto& kv : monitors_) {
            if (kv.second.handleUnavailable || kv.second.featuresProbed) continue;
            Job j;
            j.key = kv.first;
            j.handle = kv.second.handle;
            j.viaVcp = kv.second.viaVcp;
            j.caps = kv.second.caps;
            jobs.push_back(j);
        }
    }
    if (jobs.empty()) return;

    // All three are discrete VCP codes whose value comes from a list. Volume
    // (0x62) is excluded deliberately: it is continuous and would need a
    // slider. SetFeature accepts any code, so adding it is a UI-only change.
    static const unsigned char kProbeCodes[] = {
        kVcpColorPreset, kVcpInputSource, kVcpPowerMode
    };

    for (const auto& j : jobs) {
        if (::InterlockedCompareExchange(&running_, 0, 0) == 0) return;

        // When the capability string is already known it lists the values the
        // panel actually accepts, which is narrower than the full MCCS set.
        std::vector<VcpFeature> declared;
        if (!j.caps.empty()) declared = ParseVcpFeatures(WideToUtf8(j.caps));

        std::vector<Feature> found;
        for (const unsigned char code : kProbeCodes) {
            if (::InterlockedCompareExchange(&running_, 0, 0) == 0) return;
            ::Sleep((DWORD)ddc::kMinIntervalMs);

            DWORD lo = 0, cur = 0, hi = 0;
            bool usedVcp = false;
            if (!ddc::Read(fns_, j.handle, code, &lo, &cur, &hi, &usedVcp)) continue;

            Feature f;
            f.code = code;
            f.current = (int)cur;
            f.maximum = (int)hi;
            for (const auto& d : declared)
                if (d.code == code) { f.values = d.values; break; }
            found.push_back(f);
        }

        Guard g(lock_);
        auto it = monitors_.find(j.key);
        if (it == monitors_.end()) continue;
        it->second.features = found;
        it->second.featuresProbed = true;
        KLOG_I(L"DDC/CI: '%s' expõe %d recurso(s) extra(s).",
               it->second.description.c_str(), (int)found.size());
    }
}

void DdcciBackend::RunRoundTrip() {
    // Queue thread only: every step is a slow command to the monitor.
    struct Job { std::wstring key, description; HANDLE handle; BYTE code;
                 DWORD lo, hi, current; bool viaVcp; };
    std::vector<Job> jobs;
    {
        Guard g(lock_);
        for (const auto& kv : monitors_) {
            if (!kv.second.hasBrightness || kv.second.handleUnavailable) continue;
            if (kv.second.brightnessState.blocked) continue;
            Job j;
            j.key = kv.first;
            j.description = kv.second.description;
            j.handle = kv.second.handle;
            j.code = kv.second.brightnessCode;
            j.lo = kv.second.bMin;
            j.hi = kv.second.bMax;
            j.current = (DWORD)(kv.second.lastWrittenB >= 0 ? kv.second.lastWrittenB : (int)kv.second.bMin);
            j.viaVcp = kv.second.viaVcp;
            jobs.push_back(j);
        }
    }

    for (const auto& j : jobs) {
        if (::InterlockedCompareExchange(&running_, 0, 0) == 0) return;

        std::wstring verdict;

        // Read the live value so the probe does not start from a stale cache.
        DWORD lo = j.lo, cur = j.current, hi = j.hi;
        bool usedVcp = false;
        if (!ddc::Read(fns_, j.handle, j.code, &lo, &cur, &hi, &usedVcp)) {
            verdict = L"não respondeu à leitura";
        } else if (hi <= lo) {
            verdict = Format(L"faixa inválida (%lu..%lu)", lo, hi);
        } else {
            // A step that fits inside the range and stays clear of both ends:
            // a panel saturating at its minimum or maximum would otherwise look
            // like it ignored the command.
            const DWORD span = hi - lo;
            const DWORD step = span >= 10 ? span / 10 : 1;
            const DWORD probe = (cur + step <= hi) ? cur + step
                              : (cur >= lo + step ? cur - step : hi);
            if (probe == cur) {
                verdict = L"faixa curta demais para testar";
            } else {
                const ddc::IoResult w = ddc::Write(fns_, j.handle, j.code, probe, j.viaVcp);
                if (!w.ok) {
                    verdict = Format(L"a escrita falhou (%s)", ddc::ErrorKindName(w.kind));
                } else {
                    ::Sleep((DWORD)ddc::kMinIntervalMs);
                    DWORD lo2 = 0, back = 0, hi2 = 0;
                    bool vcp2 = false;
                    if (!ddc::Read(fns_, j.handle, j.code, &lo2, &back, &hi2, &vcp2))
                        verdict = L"escreveu, mas não respondeu à releitura";
                    else if (back == probe)
                        verdict = Format(L"OK — obedeceu (%lu -> %lu, faixa %lu..%lu)",
                                         cur, probe, lo, hi);
                    else
                        verdict = Format(L"NÃO obedeceu — pedi %lu e ficou %lu "
                                         L"(o monitor aceitou o comando e ignorou)",
                                         probe, back);

                    // Restore the previous value regardless of the outcome.
                    ::Sleep((DWORD)ddc::kMinIntervalMs);
                    ddc::Write(fns_, j.handle, j.code, cur, j.viaVcp);
                }
            }
        }

        KLOG_I(L"DDC/CI: teste de ida e volta em '%s': %s",
               j.description.c_str(), verdict.c_str());
        {
            Guard g(lock_);
            auto it = monitors_.find(j.key);
            if (it != monitors_.end()) it->second.roundTrip = verdict;
        }
    }
}

void DdcciBackend::HoldCommands(int milliseconds) {
    if (milliseconds <= 0) return;
    {
        Guard g(lock_);
        const double until = NowMs() + milliseconds;
        // Never shortens a hold already in progress: resume delivers several
        // events at once and the last one must not release the queue early.
        if (until > holdUntilMs_) holdUntilMs_ = until;
    }
    // Wake the thread so it recomputes its own wait timeout.
    if (wake_) ::SetEvent(wake_);
}

void DdcciBackend::Discover() {
    // Re-enumeration destroys the PHYSICAL_MONITOR handles the queue thread is
    // using, and GetMonitorBrightness/GetMonitorContrast are synchronous and
    // slow. Only the worker may run it; this call just posts the request.
    if (thread_ && ::InterlockedCompareExchange(&running_, 1, 1) == 1) {
        ::InterlockedExchange(&rediscover_, 1);
        ::SetEvent(wake_);
        return;
    }
    DiscoverNow();  // still in Init: no worker thread exists yet
}

void DdcciBackend::DiscoverNow() {
    auto getCount    = (ddc::PfnGetCount)fns_[ddc::FN_GetCount];
    auto getMonitors = (ddc::PfnGetMonitors)fns_[ddc::FN_GetMonitors];
    if (!getCount || !getMonitors) return;

    // Capture the originals before releasing the handles: ReleaseHandles()
    // clears monitors_, and a value re-read afterwards would be one this
    // process wrote rather than the user's own.
    struct PrevState {
        int b = -1, c = -1;
        int gain[3] = {-1, -1, -1};
        bool changedB = false, changedC = false;
        bool changedGain[3] = {false, false, false};
        FeatureState bState, cState, gainState[3];
        std::wstring caps;
    };
    std::map<std::wstring, PrevState> previous;
    {
        Guard g(lock_);
        for (const auto& kv : monitors_) {
            PrevState s;
            s.b = kv.second.origBrightness;
            s.c = kv.second.origContrast;
            s.changedB = kv.second.changedBrightness;
            s.changedC = kv.second.changedContrast;
            s.bState = kv.second.brightnessState;
            s.cState = kv.second.contrastState;
            for (int i = 0; i < 3; ++i) {
                s.gain[i] = kv.second.origGain[i];
                s.changedGain[i] = kv.second.changedGain[i];
                s.gainState[i] = kv.second.gainState[i];
            }
            s.caps = kv.second.caps;   // re-reading would cost seconds per monitor
            previous[kv.first] = s;
        }
    }

    Guard g(lock_);
    ReleaseHandles();
    ::InterlockedExchange(&anyGain_, 0);
    for (const auto& target : monitors::All()) {
        DdcMonitorMode mode = DdcMonitorMode::Auto;
        auto modeIt = monitorModes_.find(target.key);
        if (modeIt != monitorModes_.end()) mode = modeIt->second;
        if (mode == DdcMonitorMode::Disabled) {
            KLOG_I(L"DDC/CI: '%s' excluido pelo usuário antes de qualquer sondagem.",
                   target.friendlyName.c_str());
            continue;
        }
        const std::wstring edidId = target.edid.valid
            ? Format(L"%s%04X", target.edid.manufacturer.c_str(), target.edid.product)
            : std::wstring();
        const MonitorQuirk* quirk = FindMonitorQuirk(edidId);
        if (quirk && quirk->block) {
            KLOG_W(L"DDC/CI: '%s' (%s) tem regra de bloqueio (%s); nenhuma chamada DDC será feita.",
                   target.friendlyName.c_str(), edidId.c_str(),
                   quirk->note.empty() ? L"regra do usuário" : quirk->note.c_str());
            continue;
        }
        // Some panels expose brightness on a non-standard VCP code: they accept
        // a write to 0x10 and silently ignore it.
        const BYTE brightnessCode = (quirk && quirk->brightnessVcp > 0)
            ? (BYTE)quirk->brightnessVcp : ddc::VCP_LUMINANCE;
        if (brightnessCode != ddc::VCP_LUMINANCE)
            KLOG_I(L"DDC/CI: '%s' (%s) usa o registrador 0x%02X para brilho.",
                   target.friendlyName.c_str(), edidId.c_str(), (unsigned)brightnessCode);

        DWORD count = 0;
        if (!getCount(target.handle, &count) || count == 0) continue;

        auto* arr = (ddc::PhysicalMonitorRec*)::calloc(count, sizeof(ddc::PhysicalMonitorRec));
        if (!arr) continue;

        if (!getMonitors(target.handle, count, arr)) { ::free(arr); continue; }
        owned_.push_back(std::make_pair((HANDLE)arr, (size_t)count));

        // One HMONITOR can expose several physical monitors; the first one that
        // actually answers a read wins. Reading is more reliable than
        // GetMonitorCapabilities, which many monitors misreport.
        for (DWORD i = 0; i < count; ++i) {
            Phys p;
            p.mode = mode;
            p.brightnessCode = brightnessCode;
            // The capability string is the only command here that can bring the
            // machine down; models known for it are never probed.
            if (quirk && quirk->unsafeCaps) p.capsUnsafe = true;
            p.handle = arr[i].hPhysicalMonitor;
            p.description = arr[i].szPhysicalMonitorDescription[0]
                          ? Trim(arr[i].szPhysicalMonitorDescription) : L"Monitor";

            DWORD lo = 0, cur = 0, hi = 0, typeB = 0, typeC = 0;
            bool vcpB = false, vcpC = false;
            if (ddc::Read(fns_, p.handle, p.brightnessCode, &lo, &cur, &hi,
                          &vcpB, &typeB)) {
                p.hasBrightness = true; p.bMin = lo; p.bMax = hi;
                p.lastWrittenB = (int)cur;
                p.brightnessState.liveProven = true;
                p.brightnessState.codeType = typeB;
                p.brightnessState.blocked = typeB != 1;
                p.brightnessState.retryAfterMs = typeB != 1 ? -1 : 0;
                p.brightnessState.rawCurrent = cur;
                p.brightnessState.rawMaximum = hi;
                // Keep the pre-existing value for reset, clamped to the reported
                // range: a panel answering outside its own range must not
                // produce a nonsensical baseline.
                if (cur < lo || cur > hi)
                    KLOG_W(L"DDC/CI: '%s' respondeu brilho %lu fora da faixa %lu..%lu; "
                           L"tratando como o limite mais próximo.",
                           p.description.c_str(), cur, lo, hi);
                p.origBrightness = DdcRawToPercent(cur, lo, hi);
                if (typeB != 1)
                    KLOG_W(L"DDC/CI: '%s' devolveu tipo %lu para brilho continuo; escrita bloqueada.",
                           p.description.c_str(), typeB);
            }
            if (ddc::Read(fns_, p.handle, ddc::VCP_CONTRAST, &lo, &cur, &hi,
                          &vcpC, &typeC)) {
                p.hasContrast = true; p.cMin = lo; p.cMax = hi;
                p.lastWrittenC = (int)cur;
                p.contrastState.liveProven = true;
                p.contrastState.codeType = typeC;
                p.contrastState.blocked = typeC != 1;
                p.contrastState.retryAfterMs = typeC != 1 ? -1 : 0;
                p.contrastState.rawCurrent = cur;
                p.contrastState.rawMaximum = hi;
                p.origContrast = DdcRawToPercent(cur, lo, hi);
                if (typeC != 1)
                    KLOG_W(L"DDC/CI: '%s' devolveu tipo %lu para contraste continuo; escrita bloqueada.",
                           p.description.c_str(), typeC);
            }
            // If any feature needed the raw path, all of them use it: the
            // high-level API wraps the same VCP command, so mixing both paths
            // adds behavior differences without any benefit.
            p.viaVcp = vcpB || vcpC;

            // RGB gain is probed by reading the VCP codes directly: the
            // capability string costs seconds per monitor and is unreliable on
            // low-end panels.
            int gains[3] = {-1, -1, -1};
            DWORD gMax = 0;
            bool allGains = true;
            for (int c = 0; c < 3 && allGains; ++c) {
                DWORD glo = 0, gcur = 0, ghi = 0;
                DWORD type = 0;
                ddc::IoResult readResult;
                if (ddc::ReadVcp(fns_, p.handle, ddc::kGainCodes[c], &glo, &gcur, &ghi,
                                 &type, &readResult) && ghi > glo && type == 1) {
                    // Clamped to the reported range: this value is multiplied by
                    // the temperature factor and written back to the panel.
                    gains[c] = (int)Clamp((long)gcur, (long)glo, (long)ghi);
                    p.lastWrittenGain[c] = gains[c];
                    p.gainState[c].liveProven = true;
                    p.gainState[c].codeType = type;
                    p.gainState[c].rawCurrent = gcur;
                    p.gainState[c].rawMaximum = ghi;
                    if (ghi > gMax) gMax = ghi;
                } else {
                    if (readResult.ok && type != 1)
                        KLOG_W(L"DDC/CI: '%s' declarou ganho 0x%02X como momentaneo; ignorando por seguranca.",
                               p.description.c_str(), (unsigned)ddc::kGainCodes[c]);
                    allGains = false;
                }
            }
            if (allGains && gMax > 0) {
                p.hasGain = true;
                p.gMax = gMax;
                for (int c = 0; c < 3; ++c) p.origGain[c] = gains[c];
            }

            if (p.hasBrightness || p.hasContrast) {
                // A previously captured original wins: the value just read may
                // be one this process wrote.
                auto prev = previous.find(target.key);
                if (prev != previous.end()) {
                    if (prev->second.b >= 0) p.origBrightness = prev->second.b;
                    if (prev->second.c >= 0) p.origContrast   = prev->second.c;
                    p.changedBrightness = prev->second.changedB;
                    p.changedContrast = prev->second.changedC;
                    for (int c = 0; c < 3; ++c) {
                        if (prev->second.gain[c] >= 0) p.origGain[c] = prev->second.gain[c];
                        p.changedGain[c] = prev->second.changedGain[c];
                    }
                    // Failure and block state survives re-enumeration, while the
                    // freshly read value, range and type stay authoritative.
                    // This prevents a failure/hot-plug/retry loop on a code that
                    // is permanently unsupported.
                    const auto mergeState = [](const FeatureState& live,
                                               const FeatureState& old) {
                        FeatureState merged = old;
                        merged.liveProven = live.liveProven;
                        merged.codeType = live.codeType;
                        merged.rawCurrent = live.rawCurrent;
                        merged.rawMaximum = live.rawMaximum;
                        return merged;
                    };
                    p.brightnessState = mergeState(p.brightnessState, prev->second.bState);
                    p.contrastState = mergeState(p.contrastState, prev->second.cState);
                    for (int c = 0; c < 3; ++c)
                        p.gainState[c] = mergeState(p.gainState[c], prev->second.gainState[c]);
                    p.everChanged = p.changedBrightness || p.changedContrast ||
                                    p.changedGain[0] || p.changedGain[1] || p.changedGain[2];
                    p.caps = prev->second.caps;
                }

                // Or-assign, not assign: the flag set above from the monitor
                // quirk table must not be cleared here.
                p.capsUnsafe = p.capsUnsafe ||
                               capsUnsafe_.find(L"*") != capsUnsafe_.end() ||
                               capsUnsafe_.find(target.key) != capsUnsafe_.end();

                if (p.hasGain) ::InterlockedExchange(&anyGain_, 1);
                KLOG_I(L"DDC/CI: %s em %s (original: brilho %d%%, contraste %d%%)%s%s",
                       p.description.c_str(), target.friendlyName.c_str(),
                       p.origBrightness, p.origContrast,
                       p.viaVcp ? L" [VCP cru: a API de alto nível recusou este monitor]" : L"",
                       p.hasGain ? L" [ganho RGB por hardware]" : L"");
                monitors_[target.key] = p;
                break;
            }
        }
    }

}

void DdcciBackend::FetchCapabilities() {
    // Queue thread only: the query blocks for hundreds of milliseconds to
    // several seconds per monitor. A marker is written to disk beforehand, so
    // that if a kernel fault takes the machine down mid-read it survives the
    // crash and the next run refuses to retry.
    const std::wstring marker = ConfigDir() + L"\\caps-probe.lock";
    std::vector<std::pair<std::wstring, HANDLE>> todo;
    {
        Guard g(lock_);
        const bool allUnsafe = capsUnsafe_.find(L"*") != capsUnsafe_.end();
        for (const auto& kv : monitors_) {
            const bool unsafe = allUnsafe || capsUnsafe_.find(kv.first) != capsUnsafe_.end();
            if (unsafe) {
                KLOG_W(L"DDC/CI: capabilities de '%s' em quarentena; usando apenas evidencia viva.",
                       kv.second.description.c_str());
            } else if (kv.second.caps.empty()) {
                todo.push_back(std::make_pair(kv.first, kv.second.handle));
            }
        }
    }
    if (todo.empty()) return;

    for (const auto& item : todo) {
        if (::InterlockedCompareExchange(&running_, 0, 0) == 0) break;

        const std::string payload = WideToUtf8(item.first) + "\ncapabilities\n";
        if (!ddc::WriteSmallFileAtomic(marker, payload)) {
            KLOG_W(L"DDC/CI: não consegui armar a proteção de crash; capabilities de '%s' puladas.",
                   item.first.c_str());
            continue;
        }

        std::string caps;
        bool dangerousFailure = false;
        const bool gotCaps = ddc::ReadCapabilitiesIsolated(item.first, &caps, &dangerousFailure);
        ::DeleteFileW(marker.c_str());
        if (dangerousFailure) {
            MarkCapsUnsafe(item.first, L"crash ou timeout no processo auxiliar de capabilities");
            KLOG_W(L"DDC/CI: helper de capabilities falhou em '%s'; monitor colocado em quarentena.",
                   item.first.c_str());
            continue;
        }
        if (!gotCaps || caps.empty()) {
            KLOG_W(L"DDC/CI: '%s' não devolveu uma string de capabilities válida.", item.first.c_str());
            continue;
        }

        const std::vector<unsigned char> codes = ParseVcpCodes(caps);
        std::wstring wide = Utf8ToWide(caps);

        Guard g(lock_);
        auto it = monitors_.find(item.first);
        if (it == monitors_.end()) continue;
        it->second.caps = wide;
        const auto advertised = [&](BYTE code) {
            return std::find(codes.begin(), codes.end(), code) != codes.end();
        };
        it->second.brightnessState.advertised = advertised(it->second.brightnessCode);
        it->second.contrastState.advertised = advertised(ddc::VCP_CONTRAST);
        for (int c = 0; c < 3; ++c)
            it->second.gainState[c].advertised = advertised(ddc::kGainCodes[c]);
        KLOG_I(L"DDC/CI: '%s' declara %d codigos VCP.", it->second.description.c_str(),
               (int)codes.size());
        KLOG_D(L"DDC/CI: capacidades de '%s': %s", it->second.description.c_str(), wide.c_str());
    }

    // The machine survived the read, so the crash marker is cleared.
    ::DeleteFileW(marker.c_str());
}

bool DdcciBackend::Supports(const std::wstring& monitorKey) const {
    Guard g(const_cast<Lock&>(lock_));
    return monitors_.find(monitorKey) != monitors_.end();
}

int DdcciBackend::OriginalBrightness(const std::wstring& monitorKey) const {
    Guard g(const_cast<Lock&>(lock_));
    auto it = monitors_.find(monitorKey);
    if (it == monitors_.end() || !it->second.hasBrightness) return -1;
    return it->second.origBrightness;
}

bool DdcciBackend::SupportsBrightness(const std::wstring& monitorKey) const {
    Guard g(const_cast<Lock&>(lock_));
    auto it = monitors_.find(monitorKey);
    return it != monitors_.end() && it->second.hasBrightness &&
           !it->second.brightnessState.blocked && !it->second.handleUnavailable;
}

bool DdcciBackend::SupportsContrast(const std::wstring& monitorKey) const {
    Guard g(const_cast<Lock&>(lock_));
    auto it = monitors_.find(monitorKey);
    return it != monitors_.end() && it->second.hasContrast &&
           !it->second.contrastState.blocked && !it->second.handleUnavailable;
}

double DdcciBackend::ReadBrightness(const std::wstring& monitorKey) {
    Guard g(lock_);
    auto it = monitors_.find(monitorKey);
    if (it == monitors_.end() || !it->second.hasBrightness) return -1;

    // Only the worker touches the physical handles; discovery and every
    // successful write keep this cached value current.
    const FeatureState& s = it->second.brightnessState;
    return s.liveProven ? DdcRawToPercent(s.rawCurrent, it->second.bMin, it->second.bMax) : -1;
}

std::vector<std::wstring> DdcciBackend::Diagnose() const {
    std::vector<std::wstring> out;
    Guard g(const_cast<Lock&>(lock_));
    for (const auto& kv : monitors_) {
        const Phys& p = kv.second;
        std::wstring line = p.description + L"  [" + (p.viaVcp ? L"VCP cru" : L"API padrão") + L"]";
        if (p.mode == DdcMonitorMode::Slow) line += L"  [modo lento: 350 ms]";
        if (p.hasBrightness) line += Format(L"  brilho %lu..%lu", p.bMin, p.bMax);
        if (p.hasContrast)   line += Format(L"  contraste %lu..%lu", p.cMin, p.cMax);
        if (p.hasGain)       line += Format(L"  ganho RGB 0..%lu (R%d G%d B%d)", p.gMax,
                                            p.origGain[0], p.origGain[1], p.origGain[2]);
        if (p.handleUnavailable) line += L"  [handle indisponível; aguardando redescoberta]";
        if (p.capsUnsafe) line += L"  [capabilities em quarentena]";
        out.push_back(line);
        const auto evidence = [&](const wchar_t* name, const FeatureState& s) {
            std::wstring e = L"    " + std::wstring(name) + L": ";
            e += s.liveProven ? L"comprovado por leitura" : L"não comprovado";
            if (s.advertised) e += L", anunciado em capabilities";
            if (s.blocked) e += L", bloqueado temporariamente";
            if (s.lastError)
                e += Format(L", último erro 0x%08lX (%s), tentativas falhas %d",
                            s.lastError, ddc::ErrorKindName(s.lastKind), s.failures);
            out.push_back(e);
        };
        if (p.hasBrightness || p.brightnessState.lastError)
            evidence(Format(L"brilho 0x%02X", (unsigned)p.brightnessCode).c_str(),
                     p.brightnessState);
        if (!p.roundTrip.empty())
            out.push_back(L"    teste de ida e volta: " + p.roundTrip);
        if (p.hasContrast || p.contrastState.lastError)
            evidence(L"contraste 0x12", p.contrastState);
        for (int c = 0; c < 3; ++c)
            if (p.hasGain || p.gainState[c].lastError)
                evidence(c == 0 ? L"ganho R 0x16" : c == 1 ? L"ganho G 0x18" : L"ganho B 0x1A",
                         p.gainState[c]);
        if (!p.caps.empty()) out.push_back(L"    capacidades: " + p.caps);
    }
    for (const auto& mode : monitorModes_) {
        if (mode.second != DdcMonitorMode::Disabled) continue;
        const MonitorTarget* target = monitors::ByKey(mode.first);
        out.push_back((target ? target->friendlyName : mode.first) +
                      L"  [DDC/CI excluido pelo usuário antes de qualquer sondagem]");
    }
    return out;
}

void DdcciBackend::Apply(const MonitorTarget& m, const Adjustments& a) {
    if (!available_) return;

    Want w;
    w.brightness = a.hwBrightness;
    w.contrast   = a.hwContrast;

    // With HDR enabled, SetDeviceGammaRamp reports success without changing a
    // pixel, so temperature and white balance cannot be applied through the
    // gamma ramp. Monitor RGB gain is the only remaining path, and it does not
    // compete with the ramp precisely because the ramp has no effect.
    if (m.isHdr) {
        double tr = 1, tg = 1, tb = 1;
        TemperatureToRgb(a.temperature, &tr, &tg, &tb);
        const double user[3] = { a.redGain, a.greenGain, a.blueGain };
        const double temp[3] = { tr, tg, tb };
        for (int i = 0; i < 3; ++i)
            w.gainFactor[i] = Clamp(temp[i] * user[i] / 100.0, 0.0, 1.0);
    }

    if (w.brightness < 0 && w.contrast < 0 && w.gainFactor[0] < 0) return;
    w.dirty = true;

    {
        Guard g(lock_);
        if (monitors_.find(m.key) == monitors_.end()) return;
        w.generation = ++nextGeneration_;
        pending_[m.key] = w;
    }
    ::SetEvent(wake_);
}

void DdcciBackend::Reset(const MonitorTarget& m) {
    if (!available_) return;

    Guard g(lock_);
    // Restore never waits. The resume hold exists to skip attempts doomed to
    // fail, but restore is both the emergency exit and the shutdown path, and
    // holding it back would leave the monitor stuck on the adjusted value.
    holdUntilMs_ = 0;

    pending_.erase(m.key);

    auto it = monitors_.find(m.key);
    if (it == monitors_.end() || !it->second.everChanged) return;

    // Restores the hardware values the monitor had before. Reached only when
    // something was actually changed, so the EEPROM is not written needlessly.
    Want w{};
    w.brightness = it->second.changedBrightness && it->second.origBrightness >= 0
                 ? (double)it->second.origBrightness : -1.0;
    w.contrast   = it->second.changedContrast && it->second.origContrast >= 0
                 ? (double)it->second.origContrast : -1.0;
    // A factor of 1.0 restores each channel to its original gain.
    if (it->second.hasGain)
        for (int i = 0; i < 3; ++i)
            if (it->second.changedGain[i]) w.gainFactor[i] = 1.0;
    w.dirty = true;
    w.restoring = true;
    w.generation = ++nextGeneration_;
    if (w.brightness < 0 && w.contrast < 0 && w.gainFactor[0] < 0) return;

    pending_[m.key] = w;
    // `everChanged` stays set: clearing it at enqueue time would drop the
    // restore whenever the command never leaves the queue (per-minute ceiling,
    // monitor failure). Re-sending is cheap because lastWritten deduplication
    // discards the repeat before it reaches the EEPROM.
    ::SetEvent(wake_);
}

void DdcciBackend::ForceRestore() {
    if (!available_) return;
    {
        Guard g(lock_);
        for (auto& kv : monitors_)
            if (kv.second.origBrightness >= 0 || kv.second.origContrast >= 0) {
                kv.second.everChanged = true;
                kv.second.changedBrightness = kv.second.origBrightness >= 0;
                kv.second.changedContrast = kv.second.origContrast >= 0;
            }
    }
    for (const auto& m : monitors::All()) Reset(m);
}

void DdcciBackend::AdoptBaseline(const Baseline& b) {
    Guard g(lock_);
    for (const auto& kv : b.hardware) {
        auto it = monitors_.find(kv.first);
        if (it == monitors_.end()) {
            // Older baselines could be keyed on the EDID model alone or on
            // model|DISPLAY1. Migration happens only on a unique match:
            // restoring the wrong panel is worse than not restoring at all.
            std::wstring resolved;
            int matches = 0;
            for (const auto& m : monitors::All()) {
                const bool same = kv.first == m.legacyKey || kv.first == m.connectionKey ||
                    kv.first == m.modelKey ||
                    (!m.modelKey.empty() && kv.first == m.modelKey + L"|" + m.deviceName);
                if (same) { resolved = m.key; ++matches; }
            }
            if (matches == 1) it = monitors_.find(resolved);
        }
        if (it == monitors_.end()) continue;
        if (kv.second.first >= 0)  it->second.origBrightness = kv.second.first;
        if (kv.second.second >= 0) it->second.origContrast = kv.second.second;
    }
}

void DdcciBackend::ExportBaseline(Baseline* b) const {
    Guard g(const_cast<Lock&>(lock_));
    for (const auto& kv : monitors_)
        b->hardware[kv.first] = std::make_pair(kv.second.origBrightness, kv.second.origContrast);
}

DWORD WINAPI DdcciBackend::WorkerThunk(LPVOID self) {
    static_cast<DdcciBackend*>(self)->WorkerLoop();
    return 0;
}

void DdcciBackend::WorkerLoop() {
    while (::InterlockedCompareExchange(&running_, 1, 1) == 1) {
        // INFINITE: the loop is entirely event driven, so it burns no CPU while
        // idle and does not keep the processor out of deeper sleep states. The
        // only timed wait is the resume hold, which has a known deadline at
        // which the queued work must be released.
        DWORD waitMs = INFINITE;
        {
            Guard g(lock_);
            if (holdUntilMs_ > 0) {
                const double left = holdUntilMs_ - NowMs();
                waitMs = left > 0 ? (DWORD)left + 1 : 0;
            }
        }
        if (waitMs != 0) ::WaitForSingleObject(wake_, waitMs);
        if (::InterlockedCompareExchange(&running_, 0, 0) == 0) break;

        // Re-enumeration requested by the UI runs here, on the only thread that
        // touches the physical handles.
        if (::InterlockedExchange(&rediscover_, 0) == 1) DiscoverNow();

        // While the resume hold is active nothing is sent and queued work stays
        // marked for later. Looping back instead of sleeping here keeps
        // re-enumeration and shutdown requests responsive.
        {
            Guard g(lock_);
            if (holdUntilMs_ > 0) {
                if (NowMs() < holdUntilMs_) continue;
                holdUntilMs_ = 0;
                KLOG_I(L"DDC/CI: fim da espera da retomada; aplicando o que ficou guardado.");
            }
        }

        // Snapshot the queue so the lock is not held during the slow monitor
        // commands.
        std::vector<std::pair<std::wstring, Want>> batch;
        {
            Guard g(lock_);
            for (auto& kv : pending_)
                if (kv.second.dirty) { batch.push_back(kv); kv.second.dirty = false; }
        }

        bool deferred = false;

        for (const auto& item : batch) {
            if (::InterlockedCompareExchange(&running_, 0, 0) == 0) return;

            Phys snapshot;
            {
                Guard g(lock_);
                auto it = monitors_.find(item.first);
                if (it == monitors_.end() || it->second.handleUnavailable) continue;

                // Hard per-minute write ceiling: the monitor EEPROM has a
                // finite write life.
                const double now = NowMs();
                if (now - it->second.minuteStartMs > 60000.0) {
                    it->second.minuteStartMs = now;
                    it->second.commandsThisMinute = 0;
                }
                const auto releaseCooldown = [&](FeatureState& s) {
                    if (s.blocked && s.retryAfterMs > 0 && now >= s.retryAfterMs) {
                        s.blocked = false;
                        s.failures = 0;
                        s.retryAfterMs = 0;
                    }
                };
                releaseCooldown(it->second.brightnessState);
                releaseCooldown(it->second.contrastState);
                for (int c = 0; c < 3; ++c) releaseCooldown(it->second.gainState[c]);
                snapshot = it->second;
            }

            // Values already in place are never rewritten: every slider routes
            // through ApplyNow -> ddc_.Apply, so an unrelated adjustment must
            // not send an unchanged brightness to the EEPROM.
            const bool doB = item.second.brightness >= 0 && snapshot.hasBrightness &&
                             !snapshot.brightnessState.blocked;
            const bool doC = item.second.contrast >= 0 && snapshot.hasContrast &&
                             !snapshot.contrastState.blocked;
            const int wantB = doB ? (int)ddc::ScaleTo(item.second.brightness, snapshot.bMin, snapshot.bMax) : -1;
            const int wantC = doC ? (int)ddc::ScaleTo(item.second.contrast,   snapshot.cMin, snapshot.cMax) : -1;

            const bool needB = doB && wantB != snapshot.lastWrittenB;
            const bool needC = doC && wantC != snapshot.lastWrittenC;

            // RGB gain is always derived from the channel's original value, so
            // no channel is pushed above what the monitor started with.
            int wantG[3] = {-1, -1, -1};
            bool needG[3] = {false, false, false};
            bool anyG = false;
            if (snapshot.hasGain) {
                for (int c = 0; c < 3; ++c) {
                    if (item.second.gainFactor[c] < 0 || snapshot.origGain[c] < 0 ||
                        snapshot.gainState[c].blocked) continue;
                    const double v = (double)snapshot.origGain[c] * item.second.gainFactor[c];
                    wantG[c] = (int)Clamp(llround(v), 0LL, (long long)snapshot.gMax);
                    needG[c] = wantG[c] != snapshot.lastWrittenGain[c];
                    if (needG[c]) anyG = true;
                }
            }

            if (!needB && !needC && !anyG) {
                // A restore that already matches the cache writes nothing, but
                // must still clear the changed flags; otherwise it is
                // re-queued on every future exit.
                if (item.second.restoring) {
                    Guard g(lock_);
                    auto it = monitors_.find(item.first);
                    if (it != monitors_.end()) {
                        if (doB) it->second.changedBrightness = false;
                        if (doC) it->second.changedContrast = false;
                        for (int c = 0; c < 3; ++c)
                            if (item.second.gainFactor[c] >= 0) it->second.changedGain[c] = false;
                        it->second.everChanged = it->second.changedBrightness ||
                            it->second.changedContrast || it->second.changedGain[0] ||
                            it->second.changedGain[1] || it->second.changedGain[2];
                    }
                }
                continue;
            }

            const int writes = (needB ? 1 : 0) + (needC ? 1 : 0) +
                               (needG[0] ? 1 : 0) + (needG[1] ? 1 : 0) + (needG[2] ? 1 : 0);
            {
                Guard g(lock_);
                auto it = monitors_.find(item.first);
                if (it == monitors_.end()) continue;
                // A newer request that arrived while this snapshot was being
                // computed always wins, so a batch deferred by the ceiling
                // cannot overwrite the latest value.
                auto queued = pending_.find(item.first);
                if (queued != pending_.end() && queued->second.dirty &&
                    queued->second.generation > item.second.generation) continue;

                if (!DdcWriteBatchFits(it->second.commandsThisMinute, writes,
                                       ddc::kMaxCommandsPerMinute)) {
                    KLOG_D(L"DDC/CI: lote de %d excederia o limite de %d comandos/min em '%s'; adiando.",
                           writes, ddc::kMaxCommandsPerMinute, it->second.description.c_str());
                    if (queued == pending_.end() || !queued->second.dirty ||
                        queued->second.generation <= item.second.generation) {
                        pending_[item.first] = item.second;
                        pending_[item.first].dirty = true;
                        deferred = true;
                    }
                    continue;
                }
            }

            const double commandInterval = snapshot.mode == DdcMonitorMode::Slow
                                         ? 350.0 : ddc::kMinIntervalMs;
            const double elapsed = NowMs() - snapshot.lastCommandMs;
            if (elapsed < commandInterval)
                ::Sleep((DWORD)(commandInterval - elapsed));

            struct Sent { bool attempted = false; ddc::IoResult result; };
            Sent sentB, sentC, sentG[3];
            bool first = true;
            bool handleDead = false;
            int attemptsUsed = 0;
            int featuresLeft = writes;
            int budgetLeft = ddc::kMaxCommandsPerMinute - snapshot.commandsThisMinute;
            auto send = [&](BYTE code, int value, Sent* sent) {
                if (!sent) return;
                if (handleDead) { --featuresLeft; return; }
                if (!first) ::Sleep((DWORD)commandInterval);
                first = false;
                // Reserve one attempt for each feature still pending; whatever
                // remains of the budget may be spent on retries.
                const int maxForThis = Clamp(budgetLeft - (featuresLeft - 1), 1, ddc::kMaxAttempts);
                sent->attempted = true;
                sent->result = ddc::Write(fns_, snapshot.handle, code, (DWORD)value,
                                          snapshot.viaVcp, maxForThis, (DWORD)commandInterval);
                attemptsUsed += sent->result.attempts;
                budgetLeft -= sent->result.attempts;
                --featuresLeft;
                handleDead = sent->result.kind == DdcErrorKind::Unavailable;
            };

            if (needB) send(snapshot.brightnessCode, wantB, &sentB);
            if (needC) send(ddc::VCP_CONTRAST, wantC, &sentC);
            for (int c = 0; c < 3; ++c)
                if (needG[c]) send(ddc::kGainCodes[c], wantG[c], &sentG[c]);

            bool requestRediscovery = false;
            {
                Guard g(lock_);
                auto it = monitors_.find(item.first);
                if (it == monitors_.end()) continue;
                it->second.lastCommandMs = NowMs();
                it->second.commandsThisMinute += attemptsUsed;

                auto update = [&](BYTE code, int value, const Sent& sent,
                                  FeatureState& state, int* lastWritten, bool* changed,
                                  int originalRaw) {
                    if (!sent.attempted) return;
                    if (sent.result.ok) {
                        // SetVCPFeature can report success while the monitor
                        // ignores the command, so a successful write updates the
                        // cached value but never establishes support on its own.
                        if (state.liveProven) state.rawCurrent = (DWORD)value;
                        state.failures = 0;
                        state.lastError = 0;
                        state.lastKind = DdcErrorKind::None;
                        state.blocked = false;
                        state.retryAfterMs = 0;
                        *lastWritten = value;
                        *changed = originalRaw >= 0 && value != originalRaw;
                        return;
                    }

                    ++state.failures;
                    state.lastError = sent.result.error;
                    state.lastKind = sent.result.kind;
                    if (sent.result.kind == DdcErrorKind::Unsupported) {
                        state.blocked = true;
                        state.retryAfterMs = -1; // until re-discovery or a manual unblock
                    } else if (sent.result.kind == DdcErrorKind::Transient &&
                               state.failures >= ddc::kMaxFailures) {
                        state.blocked = true;
                        state.retryAfterMs = NowMs() + 30000.0;
                    } else if (sent.result.kind == DdcErrorKind::Permanent &&
                               state.failures >= 2) {
                        state.blocked = true;
                        state.retryAfterMs = NowMs() + 300000.0;
                    } else if (sent.result.kind == DdcErrorKind::Unavailable) {
                        it->second.handleUnavailable = true;
                        requestRediscovery = true;
                    }
                    KLOG_W(L"DDC/CI: '%s', VCP 0x%02X falhou: erro 0x%08lX (%s), %d tentativa(s).",
                           it->second.description.c_str(), (unsigned)code,
                           sent.result.error, ddc::ErrorKindName(sent.result.kind),
                           sent.result.attempts);
                };

                const int originalB = it->second.origBrightness >= 0
                    ? (int)ddc::ScaleTo(it->second.origBrightness, it->second.bMin, it->second.bMax) : -1;
                const int originalC = it->second.origContrast >= 0
                    ? (int)ddc::ScaleTo(it->second.origContrast, it->second.cMin, it->second.cMax) : -1;
                if (needB) update(ddc::VCP_LUMINANCE, wantB, sentB,
                                  it->second.brightnessState, &it->second.lastWrittenB,
                                  &it->second.changedBrightness, originalB);
                if (needC) update(ddc::VCP_CONTRAST, wantC, sentC,
                                  it->second.contrastState, &it->second.lastWrittenC,
                                  &it->second.changedContrast, originalC);
                for (int c = 0; c < 3; ++c)
                    if (needG[c]) update(ddc::kGainCodes[c], wantG[c], sentG[c],
                                         it->second.gainState[c], &it->second.lastWrittenGain[c],
                                         &it->second.changedGain[c], it->second.origGain[c]);

                it->second.everChanged = it->second.changedBrightness ||
                    it->second.changedContrast || it->second.changedGain[0] ||
                    it->second.changedGain[1] || it->second.changedGain[2];
            }
            if (requestRediscovery) {
                ::InterlockedExchange(&rediscover_, 1);
                ::SetEvent(wake_);
            }
        }

        // After the adjustments, never before: the capability query blocks for
        // seconds and would delay the brightness change. Nothing waits on it;
        // it only feeds diagnostics.
        if (::InterlockedExchange(&needCaps_, 0) == 1) FetchCapabilities();
        if (::InterlockedExchange(&needRoundTrip_, 0) == 1) RunRoundTrip();
        if (::InterlockedExchange(&needFeatures_, 0) == 1) ProbeFeatures();

        // Extra features requested by the UI (input source, power mode) are
        // one-shot actions: sent in request order, each respecting the minimum
        // interval between commands.
        for (;;) {
            std::wstring key;
            unsigned char code = 0;
            int value = 0;
            HANDLE handle = nullptr;
            bool viaVcp = false;
            {
                Guard g(lock_);
                if (featureQueue_.empty()) break;
                key = featureQueue_.front().first;
                code = featureQueue_.front().second.first;
                value = featureQueue_.front().second.second;
                featureQueue_.erase(featureQueue_.begin());
                auto it = monitors_.find(key);
                if (it == monitors_.end() || it->second.handleUnavailable) continue;
                handle = it->second.handle;
                viaVcp = it->second.viaVcp;
            }

            ::Sleep((DWORD)ddc::kMinIntervalMs);
            const ddc::IoResult r = ddc::Write(fns_, handle, code, (DWORD)value, viaVcp);
            KLOG_I(L"DDC/CI: recurso 0x%02X = %d em '%s': %s", (unsigned)code, value,
                   key.c_str(), r.ok ? L"ok" : ddc::ErrorKindName(r.kind));

            if (r.ok) {
                Guard g(lock_);
                auto it = monitors_.find(key);
                if (it == monitors_.end()) continue;
                for (auto& f : it->second.features)
                    if (f.code == code) { f.current = value; break; }
            }
        }

        // When the per-minute ceiling deferred an item, wake again shortly so it
        // can retry once the window rolls over. Otherwise sleep indefinitely.
        if (deferred) {
            ::Sleep(1000);
            ::SetEvent(wake_);
        }
    }
}

void DdcciBackend::ReleaseHandles() {
    auto destroy = (ddc::PfnDestroy)fns_[ddc::FN_Destroy];
    monitors_.clear();
    for (auto& block : owned_) {
        auto* arr = (ddc::PhysicalMonitorRec*)block.first;
        if (destroy) destroy((DWORD)block.second, arr);
        ::free(arr);
    }
    owned_.clear();
}

bool DdcciBackend::DrainPending() {
    // Snapshot the data under the lock; the slow commands run outside it.
    struct Job { std::wstring key; Phys mon; Want want; };
    std::vector<Job> jobs;
    std::map<std::wstring, bool> scheduled;
    {
        Guard g(lock_);
        for (auto& kv : pending_) {
            if (!kv.second.dirty) continue;
            auto it = monitors_.find(kv.first);
            if (it == monitors_.end()) continue;
            jobs.push_back(Job{kv.first, it->second, kv.second});
            scheduled[kv.first] = true;
        }
        // A restore the worker attempted and failed is no longer dirty, but the
        // changed flags still hold. Retry once synchronously before declaring
        // the exit clean.
        for (const auto& kv : monitors_) {
            if (scheduled.find(kv.first) != scheduled.end() || !kv.second.everChanged) continue;
            Want w;
            w.restoring = true;
            w.brightness = kv.second.changedBrightness && kv.second.origBrightness >= 0
                         ? (double)kv.second.origBrightness : -1.0;
            w.contrast = kv.second.changedContrast && kv.second.origContrast >= 0
                       ? (double)kv.second.origContrast : -1.0;
            for (int c = 0; c < 3; ++c)
                if (kv.second.changedGain[c]) w.gainFactor[c] = 1.0;
            jobs.push_back(Job{kv.first, kv.second, w});
        }
        pending_.clear();
    }

    bool restored = true;
    for (const auto& job : jobs) {
        const double commandInterval = job.mon.mode == DdcMonitorMode::Slow
                                     ? 350.0 : ddc::kMinIntervalMs;
        const double elapsed = NowMs() - job.mon.lastCommandMs;
        if (elapsed < commandInterval) ::Sleep((DWORD)(commandInterval - elapsed));

        bool first = true;
        auto send = [&](BYTE code, DWORD value) {
            if (!first) ::Sleep((DWORD)commandInterval);
            first = false;
            const ddc::IoResult result = ddc::Write(fns_, job.mon.handle, code, value,
                job.mon.viaVcp, ddc::kMaxAttempts, (DWORD)commandInterval);
            if (!result.ok && job.want.restoring) {
                restored = false;
                KLOG_W(L"DDC/CI: restauro final de '%s', VCP 0x%02X falhou: 0x%08lX (%s).",
                       job.mon.description.c_str(), (unsigned)code, result.error,
                       ddc::ErrorKindName(result.kind));
            }
            return result.ok;
        };

        if (job.want.brightness >= 0 && job.mon.hasBrightness)
            send(ddc::VCP_LUMINANCE, ddc::ScaleTo(job.want.brightness, job.mon.bMin, job.mon.bMax));
        if (job.want.contrast >= 0 && job.mon.hasContrast)
            send(ddc::VCP_CONTRAST, ddc::ScaleTo(job.want.contrast, job.mon.cMin, job.mon.cMax));
        if (job.mon.hasGain) {
            for (int c = 0; c < 3; ++c) {
                if (job.want.gainFactor[c] < 0 || job.mon.origGain[c] < 0) continue;
                const double v = (double)job.mon.origGain[c] * job.want.gainFactor[c];
                send(ddc::kGainCodes[c],
                     (DWORD)Clamp(llround(v), 0LL, (long long)job.mon.gMax));
            }
        }
    }
    return restored;
}

void DdcciBackend::Shutdown() {
    // Stop the thread first to avoid contending on the queue, then flush what
    // is left, typically the restore of the original brightness.
    bool workerStopped = true;
    if (::InterlockedExchange(&running_, 0) == 1 && thread_) {
        ::SetEvent(wake_);
        // A single DDC/CI command can take over a second on a problematic
        // monitor, so the timeout is generous, but its result is checked.
        workerStopped = ::WaitForSingleObject(thread_, 5000) == WAIT_OBJECT_0;
    }

    if (!workerStopped) {
        // The worker is still alive and using the physical handles, the event
        // and the critical section. Freeing any of them now would be a
        // use-after-free; leaking in an exiting process is safer, and the
        // system reclaims everything anyway.
        KLOG_W(L"DDC/CI: a thread da fila não encerrou a tempo; "
               L"deixando os recursos para o sistema recolher.");
        thread_ = nullptr;
        wake_ = nullptr;
        owned_.clear();
        available_ = false;
        restoreIncomplete_ = true;
        return;
    }

    if (thread_) { ::CloseHandle(thread_); thread_ = nullptr; }

    restoreIncomplete_ = !DrainPending();

    if (wake_) { ::CloseHandle(wake_); wake_ = nullptr; }

    Guard g(lock_);
    ReleaseHandles();
    available_ = false;
}

// Backlight (WMI)

namespace {

// GUIDs defined locally to avoid linking against wbemuuid.
const CLSID kCLSID_WbemLocator =
    {0x4590f811, 0x1d3a, 0x11d0, {0x89, 0x1f, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24}};
const IID kIID_IWbemLocator =
    {0xdc12a687, 0x737f, 0x11cf, {0x88, 0x4d, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24}};

struct Bstr {
    BSTR v = nullptr;
    explicit Bstr(const wchar_t* s) : v(::SysAllocString(s)) {}
    ~Bstr() { if (v) ::SysFreeString(v); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    operator BSTR() const { return v; }
};

}  // namespace

/// A single brightness panel exposed by WMI.
///
/// The internal display is not always a single panel: some notebooks ship two
/// built-in panels and WMI reports one instance for each, so reads and writes
/// must be addressed per instance.
struct BacklightPanel {
    std::wstring instance;    ///< WMI InstanceName
    /// The same instance in QueryDisplayConfig canonical form; this is what
    /// matches MonitorTarget::devicePath.
    std::wstring devicePath;
    std::wstring methodPath;  ///< __PATH of WmiMonitorBrightnessMethods
    /// Levels the panel declares it accepts. Many panels accept only a few
    /// discrete steps and round anything else in firmware, so a read-back would
    /// never match the requested value. Empty means any value from 0 to 100.
    std::vector<int> levels;
    int current = -1;
    int lastWritten = -1;
    int pending = -1;         ///< requested value not yet applied
    int original = -1;        ///< brightness the panel had before any change
    bool everChanged = false;
};

/// Private state. The WMI connection lives entirely inside the worker thread,
/// which is where COM was initialized.
struct BacklightBackend::Impl {
    HANDLE thread = nullptr;
    HANDLE wake = nullptr;
    HANDLE ready = nullptr;
    volatile LONG running = 0;
    volatile LONG current = -1;     // reference panel (the first one)
    volatile LONG supported = 0;
    volatile LONG refresh = 0;      // request to re-read the live value
    volatile LONG reconnect = 0;    // request to rebuild the WMI connection
    volatile LONG lastWritten = -1; // last value written by this process
    volatile LONG external = -1;    // externally made change, not yet consumed

    /// Guards `panels`. The WMI thread reads and writes it; the UI thread
    /// enqueues requests and queries the match against monitors.
    Lock lock;
    std::vector<BacklightPanel> panels;

    IWbemServices* svc = nullptr;
    IWbemLocator*  loc = nullptr;
    bool comOwned = false;

    bool Connect();
    void Disconnect();
    /// Re-discovers the panels and their levels. WMI thread only.
    bool Enumerate();
    /// Re-reads the current brightness of each panel. WMI thread only.
    bool QueryAll();
    bool SetPanelBrightness(const BacklightPanel& p, int percent);
    /// Queues a value for the panel matching `devicePath`. An empty or unmatched
    /// path falls back to the only panel when exactly one exists. Returns false
    /// when no panel can be selected.
    bool Enqueue(const std::wstring& devicePath, int percent);
    void Loop();
    static DWORD WINAPI Thunk(LPVOID self);
};

/// Snaps `want` to the nearest declared level, or returns it unchanged when the
/// panel declares no levels.
int SnapToLevel(const std::vector<int>& levels, int want) {
    if (levels.empty()) return want;
    int best = levels[0], bestDist = -1;
    for (const int l : levels) {
        const int d = l > want ? l - want : want - l;
        if (bestDist < 0 || d < bestDist) { bestDist = d; best = l; }
    }
    return best;
}

bool BacklightBackend::Impl::Connect() {
    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // Records whether COM was initialized here: CoUninitialize must be called
    // only when CoInitializeEx succeeded, since it can fail on an already
    // initialized thread (RPC_E_CHANGED_MODE).
    comOwned = SUCCEEDED(hr);

    // RPC_E_TOO_LATE is ignored: another component may have already set the
    // process-wide security.
    ::CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                           RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
                           nullptr, EOAC_NONE, nullptr);

    hr = ::CoCreateInstance(kCLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                            kIID_IWbemLocator, (void**)&loc);
    if (FAILED(hr) || !loc) {
        KLOG_D(L"WMI: CoCreateInstance falhou (0x%08lX).", (unsigned long)hr);
        return false;
    }

    Bstr ns(L"root\\WMI");
    hr = loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc);
    if (FAILED(hr) || !svc) {
        KLOG_D(L"WMI: ConnectServer(root\\WMI) falhou (0x%08lX).", (unsigned long)hr);
        return false;
    }

    // Without this the WMI calls fail for lack of an identity.
    hr = ::CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                             RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                             nullptr, EOAC_NONE);
    if (FAILED(hr)) KLOG_D(L"WMI: CoSetProxyBlanket falhou (0x%08lX).", (unsigned long)hr);
    return true;
}

void BacklightBackend::Impl::Disconnect() {
    if (svc) { svc->Release(); svc = nullptr; }
    if (loc) { loc->Release(); loc = nullptr; }
    if (comOwned) { ::CoUninitialize(); comOwned = false; }
}

namespace {

/// Reads a numeric WMI property, accepting the several integer widths WMI uses.
bool GetWmiInt(IWbemClassObject* obj, const wchar_t* name, int* out) {
    if (!obj || !out) return false;
    VARIANT v;
    ::VariantInit(&v);
    bool ok = false;
    if (SUCCEEDED(obj->Get(name, 0, &v, nullptr, nullptr))) {
        if (v.vt == VT_UI1)      { *out = v.bVal;        ok = true; }
        else if (v.vt == VT_I4)  { *out = v.lVal;        ok = true; }
        else if (v.vt == VT_UI4) { *out = (int)v.ulVal;  ok = true; }
        else if (v.vt == VT_I2)  { *out = v.iVal;        ok = true; }
        else if (v.vt == VT_UI2) { *out = (int)v.uiVal;  ok = true; }
    }
    ::VariantClear(&v);
    return ok;
}

bool GetWmiString(IWbemClassObject* obj, const wchar_t* name, std::wstring* out) {
    if (!obj || !out) return false;
    VARIANT v;
    ::VariantInit(&v);
    bool ok = false;
    if (SUCCEEDED(obj->Get(name, 0, &v, nullptr, nullptr)) && v.vt == VT_BSTR && v.bstrVal) {
        out->assign(v.bstrVal);
        ok = true;
    }
    ::VariantClear(&v);
    return ok;
}

/// Reads the array of accepted levels (uint8[]). Absent or empty means the
/// panel is continuous.
void GetWmiLevels(IWbemClassObject* obj, std::vector<int>* out) {
    if (!obj || !out) return;
    out->clear();

    VARIANT v;
    ::VariantInit(&v);
    if (SUCCEEDED(obj->Get(L"Level", 0, &v, nullptr, nullptr)) &&
        (v.vt & VT_ARRAY) && v.parray) {
        SAFEARRAY* arr = v.parray;
        LONG lo = 0, hi = -1;
        if (SUCCEEDED(::SafeArrayGetLBound(arr, 1, &lo)) &&
            SUCCEEDED(::SafeArrayGetUBound(arr, 1, &hi))) {
            // Sanity ceiling: the array comes from the panel firmware.
            if (hi - lo < 4096) {
                for (LONG i = lo; i <= hi; ++i) {
                    unsigned char b = 0;
                    if (FAILED(::SafeArrayGetElement(arr, &i, &b))) break;
                    const int level = (int)b;
                    if (level >= 0 && level <= 100) out->push_back(level);
                }
            }
        }
    }
    ::VariantClear(&v);

    // A single-entry list offers no choice; treating it as continuous avoids
    // pinning brightness to one value because of unusual firmware.
    if (out->size() < 2) out->clear();
}

}  // namespace

bool BacklightBackend::Impl::Enumerate() {
    if (!svc) return false;

    std::vector<BacklightPanel> found;

    Bstr lang(L"WQL");
    Bstr query(L"SELECT InstanceName, CurrentBrightness, Level FROM WmiMonitorBrightness");
    IEnumWbemClassObject* en = nullptr;
    if (FAILED(svc->ExecQuery(lang, query,
                              WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                              nullptr, &en)) || !en)
        return false;

    IWbemClassObject* obj = nullptr;
    ULONG returned = 0;
    while (en->Next(2000, 1, &obj, &returned) == WBEM_S_NO_ERROR && returned == 1 && obj) {
        BacklightPanel p;
        int level = -1;
        if (GetWmiInt(obj, L"CurrentBrightness", &level)) p.current = level;
        GetWmiString(obj, L"InstanceName", &p.instance);
        GetWmiLevels(obj, &p.levels);
        p.devicePath = DevicePathFromWmiInstance(p.instance);
        if (p.current >= 0) found.push_back(std::move(p));
        obj->Release();
        obj = nullptr;
    }
    en->Release();

    if (found.empty()) return false;

    // The method path lives on a different class and is matched by InstanceName.
    Bstr methodQuery(L"SELECT __PATH, InstanceName FROM WmiMonitorBrightnessMethods");
    IEnumWbemClassObject* men = nullptr;
    if (SUCCEEDED(svc->ExecQuery(lang, methodQuery,
                                 WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                 nullptr, &men)) && men) {
        IWbemClassObject* mobj = nullptr;
        ULONG mret = 0;
        while (men->Next(2000, 1, &mobj, &mret) == WBEM_S_NO_ERROR && mret == 1 && mobj) {
            std::wstring instance, path;
            if (GetWmiString(mobj, L"InstanceName", &instance) &&
                GetWmiString(mobj, L"__PATH", &path)) {
                for (auto& p : found)
                    if (IEquals(p.instance, instance)) { p.methodPath = path; break; }
            }
            mobj->Release();
            mobj = nullptr;
        }
        men->Release();
    }

    {
        Guard g(lock);
        // Carry the original value and the changed flag over from a previous
        // enumeration: reconnecting after suspend runs this path, and losing the
        // original would make restore write back a value this process set.
        for (auto& p : found) {
            for (const auto& old : panels) {
                if (!old.instance.empty() && IEquals(old.instance, p.instance)) {
                    p.original = old.original;
                    p.everChanged = old.everChanged;
                    p.lastWritten = old.lastWritten;
                    break;
                }
            }
            if (p.original < 0) p.original = p.current;
        }
        panels.swap(found);
        if (!panels.empty()) ::InterlockedExchange(&current, panels[0].current);
    }
    return true;
}

bool BacklightBackend::Impl::QueryAll() {
    if (!svc) return false;

    Bstr lang(L"WQL");
    Bstr query(L"SELECT InstanceName, CurrentBrightness FROM WmiMonitorBrightness");
    IEnumWbemClassObject* en = nullptr;
    if (FAILED(svc->ExecQuery(lang, query,
                              WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                              nullptr, &en)) || !en)
        return false;

    std::vector<std::pair<std::wstring, int>> read;
    IWbemClassObject* obj = nullptr;
    ULONG returned = 0;
    while (en->Next(2000, 1, &obj, &returned) == WBEM_S_NO_ERROR && returned == 1 && obj) {
        std::wstring instance;
        int level = -1;
        GetWmiString(obj, L"InstanceName", &instance);
        if (GetWmiInt(obj, L"CurrentBrightness", &level))
            read.emplace_back(instance, level);
        obj->Release();
        obj = nullptr;
    }
    en->Release();
    if (read.empty()) return false;

    Guard g(lock);
    for (auto& p : panels) {
        for (const auto& r : read) {
            // With exactly one instance, match positionally: an empty
            // InstanceName still identifies the machine's only panel.
            if (IEquals(p.instance, r.first) || (panels.size() == 1 && read.size() == 1)) {
                p.current = r.second;
                if (p.original < 0) p.original = r.second;
                break;
            }
        }
    }
    if (!panels.empty()) ::InterlockedExchange(&current, panels[0].current);
    return true;
}

bool BacklightBackend::Impl::Enqueue(const std::wstring& devicePath, int percent) {
    Guard g(lock);
    if (panels.empty()) return false;

    BacklightPanel* chosen = nullptr;
    if (!devicePath.empty()) {
        for (auto& p : panels)
            if (!p.devicePath.empty() && IEquals(p.devicePath, devicePath)) { chosen = &p; break; }
    }
    // Fallback: with a single panel it is the target even when device-path
    // conversion fails, which would otherwise leave brightness uncontrollable.
    if (!chosen && panels.size() == 1) chosen = &panels[0];
    if (!chosen) return false;

    chosen->pending = SnapToLevel(chosen->levels, Clamp(percent, 0, 100));
    return true;
}

bool BacklightBackend::Impl::SetPanelBrightness(const BacklightPanel& panel, int percent) {
    if (!svc) return false;
    percent = Clamp(percent, 0, 100);

    Bstr className(L"WmiMonitorBrightnessMethods");
    IWbemClassObject* cls = nullptr;
    if (FAILED(svc->GetObject(className, 0, nullptr, &cls, nullptr)) || !cls) return false;

    IWbemClassObject* inDef = nullptr;
    IWbemClassObject* inParams = nullptr;
    bool ok = false;

    if (SUCCEEDED(cls->GetMethod(L"WmiSetBrightness", 0, &inDef, nullptr)) && inDef &&
        SUCCEEDED(inDef->SpawnInstance(0, &inParams)) && inParams) {

        VARIANT timeout;
        ::VariantInit(&timeout);
        timeout.vt = VT_I4;
        timeout.lVal = 1;  // seconds
        inParams->Put(L"Timeout", 0, &timeout, 0);
        ::VariantClear(&timeout);

        VARIANT level;
        ::VariantInit(&level);
        level.vt = VT_UI1;
        level.bVal = (BYTE)percent;
        inParams->Put(L"Brightness", 0, &level, 0);
        ::VariantClear(&level);

        // Targets one panel, not all: writing to every instance would move both
        // panels of a dual-panel notebook together.
        if (!panel.methodPath.empty()) {
            Bstr path(panel.methodPath.c_str());
            Bstr method(L"WmiSetBrightness");
            ok = SUCCEEDED(svc->ExecMethod(path, method, 0, nullptr,
                                           inParams, nullptr, nullptr));
        }
    }

    if (inParams) inParams->Release();
    if (inDef) inDef->Release();
    cls->Release();
    return ok;
}

DWORD WINAPI BacklightBackend::Impl::Thunk(LPVOID self) {
    static_cast<Impl*>(self)->Loop();
    return 0;
}

void BacklightBackend::Impl::Loop() {
    const bool connected = Connect();

    if (connected && Enumerate()) ::InterlockedExchange(&supported, 1);
    ::SetEvent(ready);

    if (!connected || !::InterlockedCompareExchange(&supported, 1, 1)) {
        Disconnect();
        return;
    }

    // Writes everything pending. The write happens outside the lock: WMI
    // ExecMethod can block for seconds when the service is unhealthy, and
    // holding the lock there would stall the UI thread on its next Enqueue.
    const auto flush = [&]() {
        for (;;) {
            BacklightPanel snapshot;
            size_t index = (size_t)-1;
            {
                Guard g(lock);
                for (size_t i = 0; i < panels.size(); ++i) {
                    if (panels[i].pending < 0) continue;
                    if (panels[i].pending == panels[i].current) { panels[i].pending = -1; continue; }
                    snapshot = panels[i];
                    panels[i].pending = -1;
                    index = i;
                    break;
                }
            }
            if (index == (size_t)-1) break;

            const bool ok = SetPanelBrightness(snapshot, snapshot.pending);
            Guard g(lock);
            if (index >= panels.size()) break;   // re-enumerated meanwhile
            if (ok) {
                panels[index].current = snapshot.pending;
                panels[index].lastWritten = snapshot.pending;
                panels[index].everChanged = true;
                if (index == 0) ::InterlockedExchange(&current, snapshot.pending);
                // Recorded so the next refresh can tell this write apart from a
                // change made with the brightness keys.
                ::InterlockedExchange(&lastWritten, snapshot.pending);
            } else {
                KLOG_D(L"WmiSetBrightness(%d) falhou.", snapshot.pending);
            }
        }
    };

    while (::InterlockedCompareExchange(&running, 1, 1) == 1) {
        // INFINITE for the same reason as DDC/CI: the loop is event driven and
        // periodic wake-ups would cost battery for nothing.
        ::WaitForSingleObject(wake, INFINITE);
        if (::InterlockedCompareExchange(&running, 0, 0) == 0) break;

        // Reconnect requested on resume. It happens here because the COM
        // pointers belong to this thread and may only be used on it.
        if (::InterlockedExchange(&reconnect, 0) == 1) {
            Disconnect();
            if (Connect() && Enumerate()) {
                KLOG_I(L"Backlight: conexao com o WMI refeita após a suspensao.");
            } else {
                KLOG_W(L"Backlight: não consegui refazer a conexao com o WMI.");
            }
        }

        // Re-read requested by the watchdog: this is how a brightness change
        // made with the keyboard keys is detected without polling.
        if (::InterlockedExchange(&refresh, 0) == 1) {
            const LONG prev = ::InterlockedCompareExchange(&current, 0, 0);
            if (QueryAll()) {
                const LONG real = ::InterlockedCompareExchange(&current, 0, 0);
                const LONG ours = ::InterlockedCompareExchange(&lastWritten, 0, 0);
                if (prev >= 0 && real != prev && real != ours)
                    ::InterlockedExchange(&external, real);
            }
        }

        // Re-read the live value before deciding: brightness changed with the Fn
        // keys, or across a suspend, would otherwise make the "already at this
        // value" shortcut skip the write.
        {
            bool anyPending = false;
            {
                Guard g(lock);
                for (const auto& p : panels)
                    if (p.pending >= 0) { anyPending = true; break; }
            }
            if (anyPending) QueryAll();
        }

        flush();
    }

    // Apply whatever is still pending before exiting: on shutdown that request
    // is the restore of the original brightness.
    flush();

    Disconnect();
}

bool BacklightBackend::Init() {
    impl_ = new Impl();
    impl_->wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    impl_->ready = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!impl_->wake || !impl_->ready) { details_ = L"falha ao criar eventos"; return false; }

    ::InterlockedExchange(&impl_->running, 1);
    impl_->thread = ::CreateThread(nullptr, 0, Impl::Thunk, impl_, 0, nullptr);
    if (!impl_->thread) { details_ = L"falha ao criar a thread do WMI"; return false; }
    ::SetThreadPriority(impl_->thread, THREAD_PRIORITY_BELOW_NORMAL);

    // The first WMI query can take seconds, and this Init runs on the UI thread
    // before the tray icon exists. The short wait keeps fast machines
    // synchronous; elsewhere PollReady() finishes startup once the thread
    // answers.
    if (::WaitForSingleObject(impl_->ready, 150) != WAIT_OBJECT_0) {
        pendingInit_ = true;
        details_ = L"aguardando a primeira resposta do WMI";
        return false;
    }

    if (!::InterlockedCompareExchange(&impl_->supported, 1, 1)) {
        details_ = L"nenhuma tela interna com controle de brilho";
        return false;
    }

    details_ = Format(L"tela interna detectada (brilho atual %ld%%)",
                      ::InterlockedCompareExchange(&impl_->current, 0, 0));
    available_ = true;
    return true;
}

bool BacklightBackend::PollReady() {
    if (available_ || !pendingInit_ || !impl_ || !impl_->ready) return false;
    if (::WaitForSingleObject(impl_->ready, 0) != WAIT_OBJECT_0) return false;

    pendingInit_ = false;
    if (!::InterlockedCompareExchange(&impl_->supported, 1, 1)) {
        details_ = L"nenhuma tela interna com controle de brilho";
        return false;
    }
    details_ = Format(L"tela interna detectada (brilho atual %ld%%)",
                      ::InterlockedCompareExchange(&impl_->current, 0, 0));
    available_ = true;
    return true;
}

void BacklightBackend::Apply(const MonitorTarget& m, const Adjustments& a) {
    if (!available_ || !impl_) return;
    // WMI controls the built-in panel, so the test is isInternal and not
    // isPrimary: a docked notebook can have an external monitor as primary.
    if (!m.isInternal) return;
    if (a.hwBrightness < 0) return;

    const int want = (int)llround(Clamp(a.hwBrightness, 0.0, 100.0));

    // The request goes to the panel that is this monitor, matched by the
    // canonical device path. Deduplication and spacing belong to the WMI thread;
    // discarding here would drop the final value of a slider drag.
    if (!impl_->Enqueue(m.devicePath, want)) return;

    if (original_ < 0) original_ = (int)::InterlockedCompareExchange(&impl_->current, 0, 0);
    lastWriteMs_ = NowMs();
    everChanged_ = true;
    ::SetEvent(impl_->wake);
}

void BacklightBackend::Reset(const MonitorTarget&) {
    if (!available_ || !impl_ || !everChanged_) return;

    // Each panel returns to its own previous value; one stored original is not
    // enough on a machine with two built-in panels.
    everChanged_ = false;
    bool any = false;
    {
        Guard g(impl_->lock);
        for (auto& p : impl_->panels) {
            if (!p.everChanged || p.original < 0) continue;
            p.pending = p.original;
            p.everChanged = false;
            any = true;
        }
    }
    if (any) ::SetEvent(impl_->wake);
}

void BacklightBackend::ForceRestore() {
    if (!available_ || !impl_) return;

    // The change came from a previous run, so this session's changed flags are
    // clear and the restore has to be forced.
    bool any = false;
    {
        Guard g(impl_->lock);
        for (auto& p : impl_->panels) {
            const int back = p.original >= 0 ? p.original : original_;
            if (back < 0) continue;
            p.pending = back;
            p.everChanged = false;
            any = true;
        }
    }
    everChanged_ = false;
    if (any) ::SetEvent(impl_->wake);
}

void BacklightBackend::AdoptBaseline(const Baseline& b) {
    if (b.backlight < 0) return;
    original_ = b.backlight;
    if (!impl_) return;
    // The baseline file stores a single value for the reference panel. It
    // applies to the first panel; the rest keep what enumeration read.
    Guard g(impl_->lock);
    if (!impl_->panels.empty()) impl_->panels[0].original = b.backlight;
}

void BacklightBackend::ExportBaseline(Baseline* b) const {
    if (original_ >= 0) { b->backlight = original_; return; }
    if (!impl_) return;
    Guard g(impl_->lock);
    if (!impl_->panels.empty() && impl_->panels[0].original >= 0)
        b->backlight = impl_->panels[0].original;
    else
        b->backlight = (int)::InterlockedCompareExchange(&impl_->current, 0, 0);
}

void BacklightBackend::Reconnect() {
    if (!available_ || !impl_ || !impl_->wake) return;
    ::InterlockedExchange(&impl_->reconnect, 1);
    ::SetEvent(impl_->wake);
}

void BacklightBackend::RequestRefresh() {
    if (!available_ || !impl_ || !impl_->wake) return;
    ::InterlockedExchange(&impl_->refresh, 1);
    ::SetEvent(impl_->wake);
}

bool BacklightBackend::TakeExternalChange(int* percent) {
    if (!available_ || !impl_ || !percent) return false;
    const LONG v = ::InterlockedExchange(&impl_->external, -1);
    if (v < 0) return false;
    *percent = (int)v;
    return true;
}

double BacklightBackend::Read() {
    if (!available_ || !impl_) return -1;
    const LONG v = ::InterlockedCompareExchange(&impl_->current, 0, 0);
    return v < 0 ? -1.0 : (double)v;
}

void BacklightBackend::Shutdown() {
    if (!impl_) return;

    bool stopped = true;
    if (::InterlockedExchange(&impl_->running, 0) == 1 && impl_->thread) {
        ::SetEvent(impl_->wake);
        stopped = ::WaitForSingleObject(impl_->thread, 5000) == WAIT_OBJECT_0;
    }

    if (!stopped) {
        // The thread may be stuck inside svc->ExecMethod, which can block for
        // several seconds when the WMI service is unhealthy. Deleting impl_ here
        // would free the COM connection, the events and the object itself out
        // from under it; leaking in an exiting process is the lesser evil.
        KLOG_W(L"Backlight: a thread do WMI não encerrou a tempo; "
               L"deixando os recursos para o sistema recolher.");
        impl_ = nullptr;
        available_ = false;
        return;
    }

    if (impl_->thread) ::CloseHandle(impl_->thread);
    if (impl_->wake) ::CloseHandle(impl_->wake);
    if (impl_->ready) ::CloseHandle(impl_->ready);
    delete impl_;
    impl_ = nullptr;
    available_ = false;
}

}  // namespace zdisplay
