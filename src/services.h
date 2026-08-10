#pragma once
#include "common.h"

namespace zdisplay {

/// Registers global hotkey combinations with Windows. Event driven: the system
/// posts WM_HOTKEY, so nothing polls the keyboard.
class Hotkeys {
public:
    explicit Hotkeys(HWND owner) : owner_(owner) {}
    ~Hotkeys() { UnregisterAll(); }

    /// Owns the ids registered with the system: a copy would leave two objects
    /// owning the same combinations, and the first destructor would unregister
    /// hotkeys the second still considers its own.
    Hotkeys(const Hotkeys&) = delete;
    Hotkeys& operator=(const Hotkeys&) = delete;

    /// Converts "Ctrl+Alt+K" into modifiers plus a virtual key.
    static bool Parse(const std::wstring& combo, UINT* mods, UINT* vk);

    /// Returns the hotkey id, or 0 when the combination is invalid or already
    /// held by another program.
    int Register(const std::wstring& combo);
    void UnregisterAll();

private:
    HWND owner_;
    std::vector<int> ids_;
    int nextId_ = 0xB000;
};

/// Tracks the foreground program through SetWinEventHook. A light periodic check
/// covers events missed across Explorer restarts, elevated windows and desktop
/// switches; it only queries the current HWND and never enumerates processes.
class ForegroundWatcher {
public:
    typedef void (*Callback)(const std::wstring& processName, void* ctx);

    ~ForegroundWatcher() { Stop(); }
    bool Start(Callback cb, void* ctx);
    void Stop();
    /// Reads the process currently in the foreground (used at startup).
    void Poll();

    static std::wstring ProcessNameOf(HWND hwnd);

private:
    static void CALLBACK Proc(HWINEVENTHOOK hook, DWORD evt, HWND hwnd,
                              LONG idObject, LONG idChild, DWORD thread, DWORD time);
    HWINEVENTHOOK hook_ = nullptr;
};

/// A program the user currently has open.
struct RunningApp {
    std::wstring process;   ///< executable name without .exe; this is what a rule matches
    std::wstring title;     ///< window title, shown so the program is recognizable
};

/// Lists programs with a visible window, deduplicated and sorted alphabetically.
///
/// The rules tab offers this list so the executable name does not have to be
/// typed from memory.
std::vector<RunningApp> ListRunningApps();

/// Posted from the named pipe thread to the UI thread.
constexpr UINT WM_ZDISPLAY_COMMAND = WM_APP + 21;
constexpr UINT WM_ZDISPLAY_TRAY    = WM_APP + 22;
constexpr UINT WM_ZDISPLAY_FOREGROUND = WM_APP + 23;

struct CommandRequest {
    std::wstring command;
    std::wstring reply;
};

/// Hands a request from the channel thread to the UI thread.
///
/// WM_ZDISPLAY_COMMAND carries an opaque cookie rather than a `CommandRequest*`:
/// the message number and the window class name are both fixed, so any process
/// on the same desktop could otherwise supply an address of its choosing and
/// turn `req->reply = ...` into a controlled write. The real pointer lives in
/// this table and a cookie resolves only while it is published.
namespace command_channel {

/// Registers the request and returns the cookie carried in the LPARAM.
UINT_PTR Publish(CommandRequest* req);
/// Removes the request from the table. Required before the object leaves scope.
void Withdraw(UINT_PTR cookie);
/// Returns the request behind a live cookie, or nullptr for an unknown cookie.
CommandRequest* Resolve(UINT_PTR cookie);

}  // namespace command_channel

/// Named pipe command channel. Lets an external caller drive Zdisplay from the
/// command line while it runs in the background.
class PipeServer {
public:
    PipeServer() = default;
    ~PipeServer() { Stop(); }

    /// Owns the channel thread and both handles: copying would duplicate the
    /// handles without duplicating the thread, and the second Stop() would close
    /// what the first already closed.
    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    bool Start(HWND host);
    void Stop();

    /// Sends a command to the instance that is already running.
    ///
    /// Verifies who answered before writing anything: the pipe is opened with
    /// SECURITY_IDENTIFICATION, so the server can identify the client but never
    /// impersonate it, and the server process must belong to the same user.
    static std::wstring SendCommand(const std::wstring& command, DWORD timeoutMs = 3000);

    /// Named pipe name, suffixed with the session id.
    ///
    /// One pipe per session gives each logged-on user an independent instance.
    /// The suffix isolates nothing on its own — the named pipe namespace is
    /// machine-wide and any local user can create another session's name — so
    /// isolation comes from the server security descriptor plus the check the
    /// client performs in SendCommand.
    static const wchar_t* PipeName();

private:
    static DWORD WINAPI Thunk(LPVOID self);
    void Loop();
    /// Serves a client already connected on `pipe`. Returns false when shutdown
    /// is requested midway.
    bool ServeClient(HANDLE pipe, OVERLAPPED* ov);

    HWND   host_ = nullptr;
    HANDLE thread_ = nullptr;
    /// Signalled on shutdown; cancels the pending connect and read waits.
    HANDLE stop_ = nullptr;
    volatile LONG running_ = 0;
};

namespace startup {
bool IsEnabled();
bool Set(bool enabled);
}  // namespace startup

}  // namespace zdisplay
