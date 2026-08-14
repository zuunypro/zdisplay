#include "services.h"

#include <tlhelp32.h>

#include <psapi.h>
#include <sddl.h>

namespace zdisplay {

// Global hotkeys

bool Hotkeys::Parse(const std::wstring& combo, UINT* mods, UINT* vk) {
    *mods = 0;
    *vk = 0;
    if (Trim(combo).empty()) return false;

    // Split on "+" and treat the last part as the key.
    std::vector<std::wstring> parts;
    std::wstring cur;
    for (wchar_t c : combo) {
        if (c == L'+') { parts.push_back(Trim(cur)); cur.clear(); }
        else cur.push_back(c);
    }
    parts.push_back(Trim(cur));

    for (const auto& raw : parts) {
        if (raw.empty()) continue;
        const std::wstring p = ToLower(raw);

        if (p == L"ctrl" || p == L"control" || p == L"controle") { *mods |= MOD_CONTROL; continue; }
        if (p == L"alt")                                        { *mods |= MOD_ALT; continue; }
        if (p == L"shift")                                      { *mods |= MOD_SHIFT; continue; }
        if (p == L"win" || p == L"windows")                     { *mods |= MOD_WIN; continue; }

        // Named keys, accepted in English and Portuguese.
        struct Named { const wchar_t* name; UINT vk; };
        static const Named kNamed[] = {
            {L"up", VK_UP}, {L"cima", VK_UP},
            {L"down", VK_DOWN}, {L"baixo", VK_DOWN},
            {L"left", VK_LEFT}, {L"esquerda", VK_LEFT},
            {L"right", VK_RIGHT}, {L"direita", VK_RIGHT},
            {L"pageup", VK_PRIOR}, {L"pagedown", VK_NEXT},
            {L"home", VK_HOME}, {L"end", VK_END},
            {L"insert", VK_INSERT}, {L"delete", VK_DELETE},
            {L"space", VK_SPACE}, {L"espaco", VK_SPACE},
            {L"enter", VK_RETURN}, {L"tab", VK_TAB},
            {L"escape", VK_ESCAPE}, {L"esc", VK_ESCAPE},
            {L"plus", VK_OEM_PLUS}, {L"mais", VK_OEM_PLUS},
            {L"minus", VK_OEM_MINUS}, {L"menos", VK_OEM_MINUS},
        };
        bool matched = false;
        for (const auto& n : kNamed) {
            if (p == n.name) { *vk = n.vk; matched = true; break; }
        }
        if (matched) continue;

        if (p.size() >= 2 && p[0] == L'f') {  // F1..F24
            int num = _wtoi(p.c_str() + 1);
            if (num >= 1 && num <= 24) { *vk = VK_F1 + (num - 1); continue; }
        }
        if (p.size() == 1) {
            const wchar_t c = (wchar_t)::towupper(p[0]);
            if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) { *vk = (UINT)c; continue; }
        }
        return false;  // unknown part
    }
    return *vk != 0;
}

int Hotkeys::Register(const std::wstring& combo) {
    UINT mods = 0, vk = 0;
    if (!Parse(combo, &mods, &vk)) {
        if (!Trim(combo).empty()) KLOG_W(L"Invalid hotkey: '%s'", combo.c_str());
        return 0;
    }

    const int id = nextId_++;
    // MOD_NOREPEAT prevents repeated firing while the key is held down.
    if (!::RegisterHotKey(owner_, id, mods | MOD_NOREPEAT, vk)) {
        KLOG_W(L"Could not register the hotkey '%s' (most likely already taken by another program).",
               combo.c_str());
        return 0;
    }
    ids_.push_back(id);
    KLOG_D(L"Hotkey registered: %s (id %d)", combo.c_str(), id);
    return id;
}

void Hotkeys::UnregisterAll() {
    for (int id : ids_) ::UnregisterHotKey(owner_, id);
    ids_.clear();
}

// Foreground application

namespace {
ForegroundWatcher::Callback g_fgCallback = nullptr;
void* g_fgContext = nullptr;
std::wstring g_fgLast;
HWND g_fgLastWindow = nullptr;

std::wstring ExeStem(std::wstring name) {
    const size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) name.erase(0, slash + 1);
    if (name.size() > 4 && IEquals(name.substr(name.size() - 4), L".exe"))
        name.resize(name.size() - 4);
    return name;
}

/// Fallback that does not open the process. Elevated or otherwise protected
/// processes can refuse even PROCESS_QUERY_LIMITED_INFORMATION; the Tool Help
/// snapshot still yields the base name the rules need, without reading process
/// memory.
std::wstring ProcessNameFromSnapshot(DWORD pid) {
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return L"";

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::wstring name;
    if (::Process32FirstW(snap, &entry)) {
        do {
            if (entry.th32ProcessID == pid) {
                name = ExeStem(entry.szExeFile);
                break;
            }
        } while (::Process32NextW(snap, &entry));
    }
    ::CloseHandle(snap);
    return name;
}

std::wstring ProcessNameFromPid(DWORD pid) {
    if (!pid) return L"";

    std::wstring name;
    HANDLE proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc) {
        wchar_t path[MAX_PATH * 2] = {};
        DWORD size = _countof(path);
        if (::QueryFullProcessImageNameW(proc, 0, path, &size))
            name = ExeStem(std::wstring(path, size));
        ::CloseHandle(proc);
    }

    return name.empty() ? ProcessNameFromSnapshot(pid) : name;
}

bool IsHostedAppFrame(HWND hwnd, const std::wstring& process) {
    wchar_t cls[128] = {};
    ::GetClassNameW(hwnd, cls, _countof(cls));
    return IEquals(process, L"ApplicationFrameHost") ||
           IEquals(cls, L"ApplicationFrameWindow");
}

bool IsSystemHost(const std::wstring& name) {
    static const wchar_t* hosts[] = {
        L"ApplicationFrameHost", L"RuntimeBroker", L"ShellExperienceHost",
        L"StartMenuExperienceHost", L"SearchHost", L"SearchApp", L"TextInputHost"
    };
    for (const wchar_t* host : hosts)
        if (IEquals(name, host)) return true;
    return false;
}

struct HostedAppSearch {
    DWORD framePid = 0;
    std::wstring fallback;
    std::wstring visible;
};

BOOL CALLBACK FindHostedAppChild(HWND child, LPARAM param) {
    auto* search = reinterpret_cast<HostedAppSearch*>(param);
    DWORD pid = 0;
    ::GetWindowThreadProcessId(child, &pid);
    if (!pid || pid == search->framePid) return TRUE;

    const std::wstring name = ProcessNameFromPid(pid);
    if (name.empty() || IsSystemHost(name)) return TRUE;

    if (search->fallback.empty()) search->fallback = name;
    if (::IsWindowVisible(child)) {
        search->visible = name;
        return FALSE;
    }
    return TRUE;
}

std::wstring HostedAppName(HWND frame, DWORD framePid) {
    HostedAppSearch search;
    search.framePid = framePid;
    ::EnumChildWindows(frame, FindHostedAppChild, reinterpret_cast<LPARAM>(&search));
    return search.visible.empty() ? search.fallback : search.visible;
}

void PublishForeground(HWND hwnd) {
    // A window never changes process during its lifetime, so an unchanged HWND
    // needs no further lookup. The exception is the hosted app frame: the
    // application's child window can appear shortly after the frame, or be
    // replaced, without the top-level HWND changing.
    if (hwnd == g_fgLastWindow) {
        wchar_t cls[128] = {};
        if (!hwnd || !::GetClassNameW(hwnd, cls, _countof(cls)) ||
            !IEquals(cls, L"ApplicationFrameWindow"))
            return;
    }
    g_fgLastWindow = hwnd;

    const std::wstring name = ForegroundWatcher::ProcessNameOf(hwnd);
    // An empty name is a state too: when the foreground process cannot be
    // identified, the previous rule is released rather than left applied.
    if (IEquals(name, g_fgLast)) return;
    g_fgLast = name;
    if (g_fgCallback) g_fgCallback(name, g_fgContext);
}
}  // namespace

std::wstring ForegroundWatcher::ProcessNameOf(HWND hwnd) {
    if (!hwnd) return L"";

    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return L"";

    const std::wstring frameName = ProcessNameFromPid(pid);

    // In the hosted app model the top-level window belongs to
    // ApplicationFrameHost while the child CoreWindow belongs to the actual
    // application, so the top-level PID alone would map every hosted app to
    // the same name.
    if (IsHostedAppFrame(hwnd, frameName)) {
        const std::wstring app = HostedAppName(hwnd, pid);
        if (!app.empty()) return app;
    }
    return frameName;
}

void CALLBACK ForegroundWatcher::Proc(HWINEVENTHOOK, DWORD evt, HWND hwnd,
                                      LONG idObject, LONG, DWORD, DWORD) {
    if (evt != EVENT_SYSTEM_FOREGROUND || idObject != OBJID_WINDOW) return;
    PublishForeground(hwnd);
}

bool ForegroundWatcher::Start(Callback cb, void* ctx) {
    // Idempotent: per-application rules can be toggled repeatedly, and one
    // foreground window hook installed over another would leak the previous one.
    if (hook_) { ::UnhookWinEvent(hook_); hook_ = nullptr; }

    g_fgCallback = cb;
    g_fgContext = ctx;
    g_fgLast.clear();
    g_fgLastWindow = nullptr;

    hook_ = ::SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                              nullptr, Proc, 0, 0,
                              WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!hook_) {
        KLOG_W(L"Could not install the foreground window hook - the "
               L"application rules will fall back to periodic polling.");
        Poll();
        return false;
    }
    Poll();
    return true;
}

void ForegroundWatcher::Poll() {
    PublishForeground(::GetForegroundWindow());
}

void ForegroundWatcher::Stop() {
    if (hook_) { ::UnhookWinEvent(hook_); hook_ = nullptr; }
    g_fgCallback = nullptr;
    g_fgContext = nullptr;
    // Cleared so that the first Poll() after a restart reports the current
    // process instead of comparing against stale state.
    g_fgLast.clear();
    g_fgLastWindow = nullptr;
}

// Running applications

namespace {

struct AppScan {
    std::vector<RunningApp>* out;
    DWORD self;
};

BOOL CALLBACK CollectApp(HWND hwnd, LPARAM lparam) {
    auto* scan = reinterpret_cast<AppScan*>(lparam);

    // Only real application windows: visible, top-level, titled and without
    // WS_EX_TOOLWINDOW. This matches the Alt+Tab criterion.
    if (!::IsWindowVisible(hwnd)) return TRUE;
    if (::GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;

    const LONG_PTR ex = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return TRUE;

    // Hosted app windows can report as visible while the compositor keeps them
    // cloaked; without this filter they would fill the list with names that
    // are not on screen.
    typedef HRESULT (WINAPI *PfnGetAttr)(HWND, DWORD, PVOID, DWORD);
    static PfnGetAttr pGetAttr = [] {
        HMODULE dwm = ::LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return dwm ? (PfnGetAttr)(void*)::GetProcAddress(dwm, "DwmGetWindowAttribute") : nullptr;
    }();
    if (pGetAttr) {
        int cloaked = 0;
        if (pGetAttr(hwnd, 14 /* DWMWA_CLOAKED */, &cloaked, sizeof(cloaked)) == S_OK && cloaked)
            return TRUE;
    }

    wchar_t title[256] = {};
    if (::GetWindowTextW(hwnd, title, _countof(title)) <= 0) return TRUE;

    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == scan->self) return TRUE;   // this process is excluded

    const std::wstring process = ForegroundWatcher::ProcessNameOf(hwnd);
    if (process.empty()) return TRUE;

    for (const auto& existing : *scan->out)
        if (IEquals(existing.process, process)) return TRUE;   // already listed

    scan->out->push_back(RunningApp{process, title});
    return TRUE;
}

}  // namespace

std::vector<RunningApp> ListRunningApps() {
    std::vector<RunningApp> out;
    AppScan scan{&out, ::GetCurrentProcessId()};
    ::EnumWindows(CollectApp, reinterpret_cast<LPARAM>(&scan));

    std::sort(out.begin(), out.end(), [](const RunningApp& a, const RunningApp& b) {
        return ToLower(a.process) < ToLower(b.process);
    });
    return out;
}

// Named pipe

namespace {

/// Owner SID of a token, in string form ("S-1-5-21-...").
std::wstring SidOfToken(HANDLE token) {
    DWORD size = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (size == 0) return L"";

    std::vector<BYTE> buf(size);
    if (!::GetTokenInformation(token, TokenUser, buf.data(), size, &size)) return L"";

    LPWSTR text = nullptr;
    if (!::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &text))
        return L"";
    std::wstring out = text;
    ::LocalFree(text);
    return out;
}

/// SID of the user running a process. Empty when it cannot be determined, which
/// is conclusive in itself: a process owned by another user does not open.
std::wstring SidOfProcess(DWORD pid) {
    HANDLE proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return L"";

    HANDLE token = nullptr;
    std::wstring sid;
    if (::OpenProcessToken(proc, TOKEN_QUERY, &token)) {
        sid = SidOfToken(token);
        ::CloseHandle(token);
    }
    ::CloseHandle(proc);
    return sid;
}

const std::wstring& CurrentUserSid() {
    static const std::wstring cached = [] () -> std::wstring {
        HANDLE token = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return L"";
        std::wstring sid = SidOfToken(token);
        ::CloseHandle(token);
        return sid;
    }();
    return cached;
}

/// Security descriptor for the named pipe.
///
/// The pipe carries an explicit protected DACL (the "P" in the SDDL) granting
/// only the owner and SYSTEM, so a process belonging to another user on the
/// machine cannot issue commands, and no entry is inherited from elsewhere.
class PipeSecurity {
public:
    PipeSecurity() {
        const std::wstring& sid = CurrentUserSid();
        if (sid.empty()) return;
        const std::wstring sddl = L"D:P(A;;GA;;;" + sid + L")(A;;GA;;;SY)";
        if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.c_str(), SDDL_REVISION_1, &sd_, nullptr))
            sd_ = nullptr;
    }
    ~PipeSecurity() { if (sd_) ::LocalFree(sd_); }
    PipeSecurity(const PipeSecurity&) = delete;
    PipeSecurity& operator=(const PipeSecurity&) = delete;

    bool Ok() const { return sd_ != nullptr; }

    SECURITY_ATTRIBUTES* Attributes() {
        if (!sd_) return nullptr;
        sa_.nLength = sizeof(sa_);
        sa_.lpSecurityDescriptor = sd_;
        sa_.bInheritHandle = FALSE;
        return &sa_;
    }

private:
    PSECURITY_DESCRIPTOR sd_ = nullptr;
    SECURITY_ATTRIBUTES  sa_{};
};

/// Verifies that the process on the other end of the pipe belongs to the same
/// user.
///
/// Commands and replies carry profile names and monitor model and serial
/// numbers, which are not disclosed to a process owned by anyone else.
bool ServerIsSameUser(HANDLE pipe) {
    ULONG pid = 0;
    if (!::GetNamedPipeServerProcessId(pipe, &pid) || pid == 0) return false;
    if (pid == ::GetCurrentProcessId()) return true;

    const std::wstring& mine = CurrentUserSid();
    if (mine.empty()) return false;
    const std::wstring theirs = SidOfProcess((DWORD)pid);
    return !theirs.empty() && theirs == mine;
}

}  // namespace

// In-flight requests

namespace command_channel {
namespace {
Lock g_lock;
std::map<UINT_PTR, CommandRequest*> g_live;
UINT_PTR g_nextCookie = 1;
}  // namespace

UINT_PTR Publish(CommandRequest* req) {
    if (!req) return 0;
    Guard g(g_lock);
    const UINT_PTR cookie = g_nextCookie++;
    g_live[cookie] = req;
    return cookie;
}

void Withdraw(UINT_PTR cookie) {
    Guard g(g_lock);
    g_live.erase(cookie);
}

CommandRequest* Resolve(UINT_PTR cookie) {
    Guard g(g_lock);
    const auto it = g_live.find(cookie);
    return it == g_live.end() ? nullptr : it->second;
}

}  // namespace command_channel

/// Pipe name for this session.
///
/// The session id is part of the name, so the single instance is per session
/// rather than per machine: concurrent sessions of the same or different users
/// each get their own channel instead of contending for one.
const wchar_t* PipeServer::PipeName() {
    static const std::wstring name = [] {
        DWORD session = 0;
        if (!::ProcessIdToSessionId(::GetCurrentProcessId(), &session)) session = 0;
        return L"\\\\.\\pipe\\ZdisplayControlPipe-" + std::to_wstring(session);
    }();
    return name.c_str();
}

bool PipeServer::Start(HWND host) {
    host_ = host;
    stop_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);  // manual reset
    if (!stop_) {
        KLOG_W(L"Could not create the command channel stop event.");
        return false;
    }
    ::InterlockedExchange(&running_, 1);
    thread_ = ::CreateThread(nullptr, 0, Thunk, this, 0, nullptr);
    if (!thread_) {
        ::InterlockedExchange(&running_, 0);
        ::CloseHandle(stop_);
        stop_ = nullptr;
        KLOG_W(L"Could not create the command channel thread.");
        return false;
    }
    return true;
}

DWORD WINAPI PipeServer::Thunk(LPVOID self) {
    static_cast<PipeServer*>(self)->Loop();
    return 0;
}

namespace {

/// Waits for an overlapped operation, honouring the stop event and a timeout.
///
/// Returns true only when the operation actually completed. In every other case
/// the I/O is cancelled before returning, so the kernel stops writing to the
/// stack OVERLAPPED once the scope ends.
bool WaitOverlapped(HANDLE pipe, OVERLAPPED* ov, HANDLE stop, DWORD timeoutMs, DWORD* transferred) {
    HANDLE waits[2] = { ov->hEvent, stop };
    const DWORD r = ::WaitForMultipleObjects(2, waits, FALSE, timeoutMs);
    if (r == WAIT_OBJECT_0)
        return ::GetOverlappedResult(pipe, ov, transferred, FALSE) != FALSE;

    ::CancelIoEx(pipe, ov);
    DWORD ignored = 0;
    ::GetOverlappedResult(pipe, ov, &ignored, TRUE);  // wait for the cancel to finish
    return false;
}

}  // namespace

bool PipeServer::ServeClient(HANDLE pipe, OVERLAPPED* ov) {
    wchar_t buf[2048] = {};
    DWORD read = 0;

    ::ResetEvent(ov->hEvent);
    bool got = ::ReadFile(pipe, buf, sizeof(buf) - sizeof(wchar_t), &read, ov) != FALSE;
    if (!got && ::GetLastError() == ERROR_IO_PENDING) {
        // Deadline for the client to speak: a client that connects and stays
        // silent would otherwise hold the single pipe instance indefinitely.
        got = WaitOverlapped(pipe, ov, stop_, 3000, &read);
    }
    if (!got || read == 0) return false;

    CommandRequest req;
    req.command = std::wstring(buf, read / sizeof(wchar_t));

    // The LPARAM carries an opaque cookie, never the address of `req`. The host
    // window has a fixed class name, so a raw pointer would let any process on
    // the same desktop send WM_ZDISPLAY_COMMAND with an address of its choosing
    // and obtain a controlled write inside this process.
    //
    // SendMessage without a timeout is deliberate: with SendMessageTimeout an
    // expired timeout does not cancel the message, which stays in the sending
    // thread's queue and runs later, writing into `req` after it has left scope.
    const UINT_PTR cookie = command_channel::Publish(&req);
    if (!cookie) return false;
    ::SendMessageW(host_, WM_ZDISPLAY_COMMAND, 0, (LPARAM)cookie);
    command_channel::Withdraw(cookie);

    if (req.reply.empty()) req.reply = L"ok";
    DWORD written = 0;
    ::ResetEvent(ov->hEvent);
    if (!::WriteFile(pipe, req.reply.c_str(),
                     (DWORD)(req.reply.size() * sizeof(wchar_t)), &written, ov) &&
        ::GetLastError() == ERROR_IO_PENDING) {
        WaitOverlapped(pipe, ov, stop_, 3000, &written);
    }
    return true;
}

void PipeServer::Loop() {
    PipeSecurity security;
    if (!security.Ok()) {
        KLOG_E(L"Could not build the command channel security descriptor - "
               L"a linha de comando fica desativada.");
        ::InterlockedExchange(&running_, 0);
        return;
    }

    // One instance, created once and held open until shutdown.
    //
    // `\Device\NamedPipe` is a machine-wide namespace in which any local user
    // can create a name belonging to another session, so the handle is never
    // released between clients: DisconnectNamedPipe returns the same instance
    // to the listening state and the name stays owned for the whole run.
    // FILE_FLAG_FIRST_PIPE_INSTANCE makes creation fail if the name is already
    // taken, and PIPE_REJECT_REMOTE_CLIENTS refuses connections arriving over
    // the network.
    HANDLE pipe = ::CreateNamedPipeW(
        PipeName(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 4096, 4096, 0, security.Attributes());

    if (pipe == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        // Failure to create the pipe is fatal for the command channel: if the
        // name is already owned by another process, the command line is
        // disabled and reported rather than retried against that process.
        KLOG_E(L"Could not create the command channel (error %lu). %s", err,
               (err == ERROR_ACCESS_DENIED || err == ERROR_PIPE_BUSY ||
                err == ERROR_ALREADY_EXISTS)
                   ? L"The name already belongs to another process - the command line is "
                     L"DISABLED for safety."
                   : L"A linha de comando fica desativada.");
        ::InterlockedExchange(&running_, 0);
        return;
    }

    OVERLAPPED ov{};
    ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
        KLOG_E(L"Could not create the command channel I/O event.");
        ::CloseHandle(pipe);
        ::InterlockedExchange(&running_, 0);
        return;
    }

    // Overlapped I/O rather than blocking reads: a client that connects and
    // never writes, such as a script interrupted between CreateFile and
    // WriteFile, would otherwise stall this loop and the whole command channel
    // for the rest of the run.
    while (::InterlockedCompareExchange(&running_, 1, 1) == 1) {
        ::ResetEvent(ov.hEvent);

        bool connected = ::ConnectNamedPipe(pipe, &ov) != FALSE;
        if (!connected) {
            const DWORD err = ::GetLastError();
            if (err == ERROR_PIPE_CONNECTED) {
                connected = true;
            } else if (err == ERROR_IO_PENDING) {
                DWORD ignored = 0;
                connected = WaitOverlapped(pipe, &ov, stop_, INFINITE, &ignored);
            }
        }

        if (connected && ::InterlockedCompareExchange(&running_, 1, 1) == 1)
            ServeClient(pipe, &ov);

        ::DisconnectNamedPipe(pipe);
    }

    ::CloseHandle(ov.hEvent);
    ::CloseHandle(pipe);
}

void PipeServer::Stop() {
    ::InterlockedExchange(&running_, 0);

    // `running_` alone does not say whether there is anything to reclaim:
    // Loop() clears it itself when the pipe cannot be created, while the thread
    // and the event still need closing.
    if (!thread_ && !stop_) return;

    if (stop_) ::SetEvent(stop_);

    if (thread_) {
        // Messages are pumped while waiting: the channel thread may be inside a
        // SendMessage to this thread, and waiting without dispatching would
        // deadlock both.
        for (;;) {
            const DWORD r = ::MsgWaitForMultipleObjects(1, &thread_, FALSE, 5000, QS_ALLINPUT);
            if (r == WAIT_OBJECT_0 || r == WAIT_TIMEOUT || r == WAIT_FAILED) break;

            MSG msg;
            while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { ::PostQuitMessage((int)msg.wParam); break; }
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
            }
        }
        ::CloseHandle(thread_);
        thread_ = nullptr;
    }

    if (stop_) { ::CloseHandle(stop_); stop_ = nullptr; }
}

std::wstring PipeServer::SendCommand(const std::wstring& command, DWORD timeoutMs) {
    // WaitNamedPipe and CreateFile form a race, so the pair runs in a loop. The
    // channel has a single instance (nMaxInstances = 1): between the wait
    // reporting a free slot and CreateFile claiming it another client can take
    // it, and the server also closes the slot briefly between
    // DisconnectNamedPipe and the next ConnectNamedPipe. Both cases surface as
    // ERROR_PIPE_BUSY, which is a lost turn rather than a failure.
    const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (;;) {
        const ULONGLONG now = ::GetTickCount64();
        if (now >= deadline) return L"error: Zdisplay is not responding";
        if (!::WaitNamedPipeW(PipeName(), (DWORD)(deadline - now)))
            return L"error: Zdisplay is not responding";

        // SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION lets the server learn
        // the caller's identity but not act as the caller. The default level is
        // SecurityImpersonation, under which whatever process holds the pipe
        // name could call ImpersonateNamedPipeClient and run with the caller's
        // token; the pipe namespace is machine-wide, so such a process need not
        // even be in the caller's session.
        pipe = ::CreateFileW(PipeName(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING,
                             SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;

        // Only the lost-turn case is retried; any other error is final.
        if (::GetLastError() != ERROR_PIPE_BUSY)
            return L"error: could not open the command channel";
    }

    // Defence in depth: even without the ability to impersonate, a foreign
    // server would still read the command and return a fabricated reply, so the
    // owner SID of the process on the other end is verified before anything is
    // sent.
    if (!ServerIsSameUser(pipe)) {
        ::CloseHandle(pipe);
        KLOG_E(L"The command channel is held by a process that does not belong "
               L"to this user - no command was sent.");
        return L"error: the command channel does not belong to this user's Zdisplay";
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    ::SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

    DWORD written = 0;
    ::WriteFile(pipe, command.c_str(), (DWORD)(command.size() * sizeof(wchar_t)), &written, nullptr);

    // The channel is in message mode: a reply larger than the buffer makes
    // ReadFile return FALSE with ERROR_MORE_DATA and the partial data in the
    // buffer. Treating that as a failure would discard the entire reply, so
    // reading continues until the message ends.
    std::wstring reply;
    for (;;) {
        wchar_t buf[2048];
        DWORD read = 0;
        const BOOL ok = ::ReadFile(pipe, buf, sizeof(buf), &read, nullptr);
        if (read > 0) reply.append(buf, read / sizeof(wchar_t));
        if (ok) break;                                   // complete message
        if (::GetLastError() != ERROR_MORE_DATA) break;  // a real error
    }

    ::CloseHandle(pipe);
    return reply;
}

// Autostart

namespace startup {

// Autostart is a single value under the per-user HKCU Run key: it affects only
// the current user, requires no elevation, and is undone by deleting that value.
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kValueName = L"Zdisplay";

bool IsEnabled() {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;

    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = sizeof(buf), type = 0;
    const LONG r = ::RegQueryValueExW(key, kValueName, nullptr, &type,
                                      reinterpret_cast<BYTE*>(buf), &size);
    ::RegCloseKey(key);
    return r == ERROR_SUCCESS && type == REG_SZ && buf[0] != L'\0';
}

bool Set(bool enabled) {
    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                          KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;

    LONG r;
    if (enabled) {
        const std::wstring value = L"\"" + ExePath() + L"\" --tray";
        r = ::RegSetValueExW(key, kValueName, 0, REG_SZ,
                             reinterpret_cast<const BYTE*>(value.c_str()),
                             (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    } else {
        r = ::RegDeleteValueW(key, kValueName);
        if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
    }
    ::RegCloseKey(key);

    if (r == ERROR_SUCCESS)
        KLOG_I(L"Automatic startup %s.", enabled ? L"on" : L"off");
    else
        KLOG_W(L"Could not change the automatic startup (error %ld).", r);
    return r == ERROR_SUCCESS;
}

}  // namespace startup
}  // namespace zdisplay
