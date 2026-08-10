// Zdisplay installer: the install and uninstall work.
//
// Everything here is a per-user install (HKCU and %LOCALAPPDATA%). No operation
// requires elevation, so the installer runs on any account, including
// unprivileged ones.
//
// The embedded executable is compressed with the XPRESS Huffman codec already
// present in Windows (ntdll), which keeps the installer dependency-free.
#include "setup.h"
#include "payload.h"
#include "../src/version.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <objidl.h>
#include <bcrypt.h>
#include <vector>

namespace setup {

const wchar_t* const kAppName      = L"Zdisplay";
const wchar_t* const kExeName      = L"zdisplay.exe";
const wchar_t* const kUninstName   = L"desinstalar.exe";
const wchar_t* const kPublisher    = L"Zdisplay";
const wchar_t* const kVersionStr   = ZDISPLAY_VERSION_WSTR;
const wchar_t* const kAppKey       = L"Software\\Zdisplay";
const wchar_t* const kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Zdisplay";
const wchar_t* const kRunKey       = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
// Must match the name used by startup::Set in src/services.cpp; if the two
// diverge, the autostart toggle inside the application edits a different Run
// entry than the one the installer created.
const wchar_t* const kRunValue     = L"Zdisplay";

namespace {

// The GUIDs are written out by hand: linking -luuid for CLSID_ShellLink and the
// FOLDERID_* values would tie the installer to how the toolchain packages that
// library, and these eight constants let the file link on its own.
/// Notification identity. Must be IDENTICAL to kAppUserModelId in src/main.cpp;
/// it is what links the running process to the installed shortcut.
const wchar_t* const kAppUserModelId = L"Zdisplay";

const GUID kCLSID_ShellLink      = {0x00021401,0,0,{0xC0,0,0,0,0,0,0,0x46}};
const GUID kIID_IShellLinkW      = {0x000214F9,0,0,{0xC0,0,0,0,0,0,0,0x46}};
const GUID kIID_IPersistFile     = {0x0000010b,0,0,{0xC0,0,0,0,0,0,0,0x46}};
const GUID kCLSID_FileOpenDialog = {0xDC1C5A9C,0xE88A,0x4DDE,{0xA5,0xA1,0x60,0xF8,0x2A,0x20,0xAE,0xF7}};
const GUID kIID_IFileDialog      = {0x42F85136,0xDB7E,0x439C,{0x85,0xF1,0xE4,0x07,0x5D,0x13,0x5F,0xC8}};
const GUID kIID_IShellItem       = {0x43826D1E,0xE718,0x42EE,{0xBC,0x55,0xA1,0xE2,0x61,0xC3,0x7B,0xFE}};

const GUID kFOLDERID_LocalAppData   = {0xF1B32785,0x6FBA,0x4FCF,{0x9D,0x55,0x7B,0x8E,0x7F,0x15,0x70,0x91}};
const GUID kFOLDERID_RoamingAppData = {0x3EB685DB,0x65F9,0x4CF6,{0xA0,0x3A,0xE3,0xEF,0x65,0x72,0x9F,0x3D}};
const GUID kFOLDERID_Programs       = {0xA77F5D77,0x2E2B,0x44C3,{0xA6,0xA2,0xAB,0xA6,0x01,0x05,0x4A,0x51}};
const GUID kFOLDERID_Desktop        = {0xB4BFCC3A,0xDB2C,0x424C,{0xB0,0x29,0x7F,0xE9,0x9A,0x87,0xC6,0x41}};

const wchar_t* const kInstallMarker = L".zdisplay-install";
const char kInstallMarkerBody[] = "Zdisplay user installation\r\n";

// Utilities

void Report(ProgressFn fn, void* ctx, int pct, const wchar_t* step) {
    if (fn) fn(ctx, pct, step);
}

std::wstring Join(const std::wstring& dir, const wchar_t* leaf) {
    if (dir.empty()) return leaf;
    std::wstring out = dir;
    if (out.back() != L'\\' && out.back() != L'/') out += L'\\';
    out += leaf;
    return out;
}

std::wstring Quoted(const std::wstring& s) { return L"\"" + s + L"\""; }

std::wstring LeafName(const std::wstring& path) {
    const size_t cut = path.find_last_of(L"\\/");
    return cut == std::wstring::npos ? path : path.substr(cut + 1);
}

bool Sha256(const void* data, size_t size, uint8_t out[zdpack::kSha256Size]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    PUCHAR object = nullptr;
    DWORD objectSize = 0, hashSize = 0, got = 0;
    bool ok = false;
    const BYTE* p = static_cast<const BYTE*>(data);

    if (::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        goto done;
    if (::BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH,
                            reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
                            &got, 0) < 0 || got != sizeof(objectSize) || objectSize == 0)
        goto done;
    if (::BCryptGetProperty(alg, BCRYPT_HASH_LENGTH,
                            reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize),
                            &got, 0) < 0 || got != sizeof(hashSize) ||
        hashSize != zdpack::kSha256Size)
        goto done;

    object = static_cast<PUCHAR>(::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, objectSize));
    if (!object) goto done;
    if (::BCryptCreateHash(alg, &hash, object, objectSize, nullptr, 0, 0) < 0)
        goto done;

    while (size) {
        const ULONG chunk = size > 1024u * 1024u ? 1024u * 1024u : (ULONG)size;
        if (::BCryptHashData(hash, const_cast<PUCHAR>(p), chunk, 0) < 0)
            goto done;
        p += chunk;
        size -= chunk;
    }
    if (::BCryptFinishHash(hash, out, zdpack::kSha256Size, 0) < 0)
        goto done;
    ok = true;

done:
    if (hash) ::BCryptDestroyHash(hash);
    if (object) {
        ::SecureZeroMemory(object, objectSize);
        ::HeapFree(::GetProcessHeap(), 0, object);
    }
    if (alg) ::BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) ::SecureZeroMemory(out, zdpack::kSha256Size);
    return ok;
}

bool SameHash(const uint8_t* a, const uint8_t* b) {
    uint8_t different = 0;
    for (uint32_t i = 0; i < zdpack::kSha256Size; ++i) different |= a[i] ^ b[i];
    return different == 0;
}

bool ValidatePeImage(const std::vector<BYTE>& image, std::wstring* why) {
    if (image.size() < sizeof(IMAGE_DOS_HEADER)) {
        if (why) *why = L"O programa embutido está truncado.";
        return false;
    }

    IMAGE_DOS_HEADER dos = {};
    ::memcpy(&dos, image.data(), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
        (size_t)dos.e_lfanew > image.size() - sizeof(IMAGE_NT_HEADERS64)) {
        if (why) *why = L"O programa embutido não é um executável PE válido.";
        return false;
    }

    IMAGE_NT_HEADERS64 nt = {};
    ::memcpy(&nt, image.data() + dos.e_lfanew, sizeof(nt));
    if (nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.Subsystem != IMAGE_SUBSYSTEM_WINDOWS_GUI) {
        if (why) *why = L"O programa embutido não corresponde ao Zdisplay para Windows x64.";
        return false;
    }
    return true;
}

std::wstring KnownFolder(const GUID& id) {
    wchar_t* p = nullptr;
    std::wstring out;
    if (SUCCEEDED(::SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) out = p;
    if (p) ::CoTaskMemFree(p);
    return out;
}

bool FileExists(const std::wstring& path) {
    const DWORD a = ::GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const std::wstring& path) {
    const DWORD a = ::GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool NormalizeInstallDirImpl(const std::wstring& input, std::wstring* normalized,
                             std::wstring* why) {
    if (input.empty()) {
        if (why) *why = L"Escolha uma pasta de instalacao.";
        return false;
    }
    // Only a local absolute path (X:\...) is accepted. UNC paths, device
    // namespaces and relative paths make a safe uninstall much harder to
    // guarantee.
    if (input.size() < 4 || input[1] != L':' ||
        (input[2] != L'\\' && input[2] != L'/') ||
        input.find(L':', 2) != std::wstring::npos) {
        if (why) *why = L"Use uma pasta local completa, como C:\\Programas\\Zdisplay.";
        return false;
    }

    const DWORD need = ::GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (need == 0 || need > 32767) {
        if (why) *why = L"O caminho de instalacao nao e valido.";
        return false;
    }
    std::vector<wchar_t> buf((size_t)need + 1);
    const DWORD got = ::GetFullPathNameW(input.c_str(), (DWORD)buf.size(), buf.data(), nullptr);
    if (got == 0 || got >= buf.size()) {
        if (why) *why = L"Nao consegui normalizar a pasta de instalacao.";
        return false;
    }

    std::wstring full(buf.data(), got);
    while (full.size() > 3 && (full.back() == L'\\' || full.back() == L'/'))
        full.pop_back();
    if (::lstrcmpiW(LeafName(full).c_str(), L"Zdisplay") != 0) {
        if (why) *why = L"A pasta final precisa se chamar Zdisplay.";
        return false;
    }

    const DWORD attr = ::GetFileAttributesW(full.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) {
        if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            if (why) *why = L"Ja existe um arquivo com o nome da pasta de instalacao.";
            return false;
        }
        if (attr & FILE_ATTRIBUTE_REPARSE_POINT) {
            if (why) *why = L"A pasta de instalacao nao pode ser um link ou junction.";
            return false;
        }
    }

    if (normalized) *normalized = full;
    return true;
}

/// Creates the whole tree one level at a time, so failures surface as plain
/// Win32 error codes that FormatError can translate.
bool EnsureDir(const std::wstring& path, DWORD* err) {
    if (DirExists(path)) return true;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] != L'\\' && path[i] != L'/') continue;
        if (i < 3) continue;  // do not try to create the drive root
        const std::wstring part = path.substr(0, i);
        if (!DirExists(part)) ::CreateDirectoryW(part.c_str(), nullptr);
    }
    if (::CreateDirectoryW(path.c_str(), nullptr)) return true;
    const DWORD e = ::GetLastError();
    if (e == ERROR_ALREADY_EXISTS && DirExists(path)) return true;
    if (err) *err = e;
    return false;
}

bool WriteInstallMarker(const std::wstring& dir, std::wstring* why) {
    const std::wstring path = Join(dir, kInstallMarker);
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (why) *why = L"Nao consegui marcar a pasta de instalacao: " + FormatError(::GetLastError());
        return false;
    }
    DWORD written = 0;
    const DWORD size = (DWORD)(sizeof(kInstallMarkerBody) - 1);
    const bool ok = ::WriteFile(h, kInstallMarkerBody, size, &written, nullptr) && written == size &&
                    ::FlushFileBuffers(h);
    const DWORD err = ok ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(h);
    if (!ok) {
        ::DeleteFileW(path.c_str());
        if (why) *why = L"Nao consegui concluir a pasta de instalacao: " + FormatError(err);
    }
    return ok;
}

bool DeleteKnownFile(const std::wstring& path, std::wstring* why) {
    const DWORD attr = ::GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        const DWORD e = ::GetLastError();
        return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        if (why) *why = L"Encontrei uma pasta onde deveria existir um arquivo: " + path;
        return false;
    }
    ::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (::DeleteFileW(path.c_str())) return true;
    if (why) *why = L"Nao consegui remover " + path + L": " + FormatError(::GetLastError());
    return false;
}

/// Closes the running instance and waits until the executable is writable.
/// The application is single-instance: running its own exe with --quit forwards
/// the request over the named pipe and exits.
bool CloseRunningApp(const std::wstring& exePath, std::wstring* why) {
    if (!FileExists(exePath)) return true;

    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask  = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"open";
    info.lpFile = exePath.c_str();
    info.lpParameters = L"--quit";
    info.nShow  = SW_HIDE;
    if (::ShellExecuteExW(&info) && info.hProcess) {
        ::WaitForSingleObject(info.hProcess, 5000);
        ::CloseHandle(info.hProcess);
    }

    // Process exit does not guarantee the file is free: Windows releases the
    // image lock a moment later, so the actual condition to wait on is opening
    // the file for writing.
    for (int i = 0; i < 25; ++i) {
        HANDLE h = ::CreateFileW(exePath.c_str(), GENERIC_WRITE, 0, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) { ::CloseHandle(h); return true; }
        ::Sleep(200);
    }
    if (why) *why = L"O Zdisplay continua em execução e o arquivo não pôde ser "
                    L"substituído. Feche-o pela bandeja e tente de novo.";
    return false;
}

// Embedded payload

typedef NTSTATUS (WINAPI *PfnGetWorkSpace)(USHORT, PULONG, PULONG);
typedef NTSTATUS (WINAPI *PfnDecompressEx)(USHORT, PUCHAR, ULONG, PUCHAR, ULONG, PULONG, PVOID);

const USHORT kFormatXpressHuff = 4;  // COMPRESSION_FORMAT_XPRESS_HUFF

const zdpack::Header* PayloadHeader(const BYTE** body, DWORD* bodySize) {
    HRSRC res = ::FindResourceW(nullptr, MAKEINTRESOURCEW(1000), RT_RCDATA);
    if (!res) return nullptr;
    const DWORD size = ::SizeofResource(nullptr, res);
    HGLOBAL mem = ::LoadResource(nullptr, res);
    if (!mem || size < sizeof(zdpack::Header)) return nullptr;
    const BYTE* base = static_cast<const BYTE*>(::LockResource(mem));
    if (!base) return nullptr;

    const zdpack::Header* h = reinterpret_cast<const zdpack::Header*>(base);
    if (h->magic != zdpack::kMagic || h->version != zdpack::kVersion) return nullptr;
    if (h->rawSize == 0 || h->rawSize > zdpack::kMaxRawSize) return nullptr;
    if (h->storedSize == 0 || h->storedSize > zdpack::kMaxStoredSize) return nullptr;
    if (h->storedSize != size - sizeof(zdpack::Header)) return nullptr;

    if (body) *body = base + sizeof(zdpack::Header);
    if (bodySize) *bodySize = h->storedSize;
    return h;
}

/// Returns the zdisplay.exe image ready to write, after CRC-32, SHA-256 and PE
/// header validation.
bool UnpackPayload(std::vector<BYTE>* out, std::wstring* why) {
    const BYTE* body = nullptr;
    DWORD bodySize = 0;
    const zdpack::Header* h = PayloadHeader(&body, &bodySize);
    if (!h) {
        if (why) *why = L"Este instalador foi montado sem o programa dentro dele.";
        return false;
    }

    out->resize(h->rawSize);
    if (out->size() != h->rawSize) {
        if (why) *why = L"Sem memória para descompactar o programa.";
        return false;
    }

    if (h->method == zdpack::kStore) {
        if (bodySize != h->rawSize) {
            if (why) *why = L"O conteúdo embutido está inconsistente.";
            return false;
        }
        ::memcpy(out->data(), body, bodySize);
    } else if (h->method == zdpack::kXpressHuff) {
        HMODULE nt = ::GetModuleHandleW(L"ntdll.dll");
        PfnGetWorkSpace getWs = nt ? (PfnGetWorkSpace)(void*)::GetProcAddress(nt, "RtlGetCompressionWorkSpaceSize") : nullptr;
        PfnDecompressEx dec   = nt ? (PfnDecompressEx)(void*)::GetProcAddress(nt, "RtlDecompressBufferEx") : nullptr;
        if (!getWs || !dec) {
            if (why) *why = L"Este Windows não oferece a descompactação usada pelo instalador.";
            return false;
        }
        ULONG wsBuf = 0, wsFrag = 0;
        if (getWs(kFormatXpressHuff, &wsBuf, &wsFrag) < 0) {
            if (why) *why = L"Falha ao preparar a descompactação.";
            return false;
        }
        std::vector<BYTE> ws(wsBuf > wsFrag ? wsBuf : wsFrag);
        ULONG finalSize = 0;
        const NTSTATUS st = dec(kFormatXpressHuff, out->data(), (ULONG)out->size(),
                                const_cast<BYTE*>(body), bodySize, &finalSize, ws.data());
        if (st < 0 || finalSize != h->rawSize) {
            if (why) *why = L"O programa embutido não pôde ser descompactado.";
            return false;
        }
    } else {
        if (why) *why = L"Formato desconhecido no conteúdo embutido.";
        return false;
    }

    // A corrupt payload must fail here, before anything reaches the disk: a
    // truncated exe would only surface on the user's machine.
    if (zdpack::Crc32(out->data(), out->size()) != h->rawCrc32) {
        if (why) *why = L"O programa embutido não passou na conferência de integridade.";
        return false;
    }
    uint8_t digest[zdpack::kSha256Size] = {};
    if (!Sha256(out->data(), out->size(), digest) || !SameHash(digest, h->rawSha256)) {
        ::SecureZeroMemory(digest, sizeof(digest));
        if (why) *why = L"O SHA-256 do programa embutido nao confere.";
        return false;
    }
    ::SecureZeroMemory(digest, sizeof(digest));
    return ValidatePeImage(*out, why);
}

/// Writes to a .tmp file and only then swaps it into place, so an interrupted
/// write leaves the temporary behind, never a half-written zdisplay.exe.
bool WriteExe(const std::wstring& dest, const std::vector<BYTE>& data,
              ProgressFn fn, void* ctx, int pctFrom, int pctTo, std::wstring* why) {
    const std::wstring tmp = dest + L".tmp";
    ::SetFileAttributesW(tmp.c_str(), FILE_ATTRIBUTE_NORMAL);
    ::DeleteFileW(tmp.c_str());

    HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (why) *why = L"Não consegui criar o arquivo: " + FormatError(::GetLastError());
        return false;
    }

    const size_t kChunk = 64 * 1024;
    size_t done = 0;
    while (done < data.size()) {
        const DWORD n = (DWORD)((data.size() - done < kChunk) ? data.size() - done : kChunk);
        DWORD written = 0;
        if (!::WriteFile(h, data.data() + done, n, &written, nullptr) || written != n) {
            const DWORD e = ::GetLastError();
            ::CloseHandle(h);
            ::DeleteFileW(tmp.c_str());
            if (why) *why = L"Falha ao gravar: " + FormatError(e);
            return false;
        }
        done += n;
        Report(fn, ctx, pctFrom + (int)((pctTo - pctFrom) * done / data.size()), L"Copiando o Zdisplay...");
    }
    ::FlushFileBuffers(h);
    ::CloseHandle(h);

    ::SetFileAttributesW(dest.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!::MoveFileExW(tmp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        const DWORD e = ::GetLastError();
        ::DeleteFileW(tmp.c_str());
        if (why) *why = L"Não consegui concluir a gravação: " + FormatError(e);
        return false;
    }
    return true;
}

// Shortcuts

std::wstring StartMenuLink() {
    const std::wstring dir = KnownFolder(kFOLDERID_Programs);
    return dir.empty() ? std::wstring() : Join(dir, L"Zdisplay.lnk");
}

std::wstring DesktopLink() {
    const std::wstring dir = KnownFolder(kFOLDERID_Desktop);
    return dir.empty() ? std::wstring() : Join(dir, L"Zdisplay.lnk");
}

/// Stamps the shortcut with the application's notification identity.
///
/// The string must match what the application declares in DeclareAppIdentity
/// (src/main.cpp), otherwise Windows never links the running process to the
/// installed application and its notifications do not appear. Failure here does
/// not block the install, so there is no return value.
void StampShortcutIdentity(IShellLinkW* link) {
    // {9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3}, 5 is PKEY_AppUserModel_ID,
    // written out by hand for the same reason as the other keys in this file.
    static const PROPERTYKEY kAppIdKey = {
        {0x9f4c2855, 0x9f79, 0x4b39, {0xa8, 0xd0, 0xe1, 0xd4, 0x2d, 0xe1, 0xd5, 0xf3}}, 5};
    static const GUID kIID_IPropertyStore =
        {0x886d8eeb, 0x8cf2, 0x4446, {0x8d, 0x02, 0xcd, 0xba, 0x1d, 0xbd, 0xcf, 0x99}};

    IPropertyStore* store = nullptr;
    if (FAILED(link->QueryInterface(kIID_IPropertyStore, (void**)&store)) || !store)
        return;

    // The string must come from CoTaskMemAlloc because that is what
    // PropVariantClear frees; a literal here would free static memory.
    const size_t bytes = (wcslen(kAppUserModelId) + 1) * sizeof(wchar_t);
    PROPVARIANT pv;
    ::PropVariantInit(&pv);
    pv.vt = VT_LPWSTR;
    pv.pwszVal = static_cast<LPWSTR>(::CoTaskMemAlloc(bytes));
    if (pv.pwszVal) {
        memcpy(pv.pwszVal, kAppUserModelId, bytes);
        if (SUCCEEDED(store->SetValue(kAppIdKey, pv))) store->Commit();
    }
    ::PropVariantClear(&pv);
    store->Release();
}

bool CreateShortcut(const std::wstring& linkPath, const std::wstring& target,
                    const std::wstring& workDir) {
    if (linkPath.empty()) return false;
    IShellLinkW* link = nullptr;
    if (FAILED(::CoCreateInstance(kCLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  kIID_IShellLinkW, (void**)&link)) || !link)
        return false;

    link->SetPath(target.c_str());
    link->SetWorkingDirectory(workDir.c_str());
    link->SetDescription(L"Brilho, contraste, saturação e temperatura de cor");
    link->SetIconLocation(target.c_str(), 0);
    StampShortcutIdentity(link);

    IPersistFile* file = nullptr;
    bool ok = false;
    if (SUCCEEDED(link->QueryInterface(kIID_IPersistFile, (void**)&file)) && file) {
        ok = SUCCEEDED(file->Save(linkPath.c_str(), TRUE));
        file->Release();
    }
    link->Release();
    return ok;
}

// Registry

bool RegSetStr(HKEY root, const wchar_t* sub, const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    if (::RegCreateKeyExW(root, sub, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    const LONG r = ::RegSetValueExW(key, name, 0, REG_SZ,
                                    reinterpret_cast<const BYTE*>(value.c_str()),
                                    (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    ::RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

bool RegSetDword(HKEY root, const wchar_t* sub, const wchar_t* name, DWORD value) {
    HKEY key = nullptr;
    if (::RegCreateKeyExW(root, sub, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    const LONG r = ::RegSetValueExW(key, name, 0, REG_DWORD,
                                    reinterpret_cast<const BYTE*>(&value), sizeof(value));
    ::RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

// Windows gamma range

const wchar_t* const kIcmKey = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ICM";
const wchar_t* const kIcmValue = L"GdiIcmGammaRange";
/// 256 (0x100) is the documented value for the full gamma range.
const DWORD kIcmFullRange = 256;

}  // namespace

bool FullGammaRangeEnabled() {
    HKEY key = nullptr;
    // KEY_WOW64_64KEY is explicit so that a future 32-bit build cannot read the
    // redirected view and conclude the range is closed when it is not.
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kIcmKey, 0,
                        KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return false;
    DWORD value = 0, size = sizeof(value), type = 0;
    const LONG r = ::RegQueryValueExW(key, kIcmValue, nullptr, &type,
                                      reinterpret_cast<BYTE*>(&value), &size);
    ::RegCloseKey(key);
    return r == ERROR_SUCCESS && type == REG_DWORD && value >= kIcmFullRange;
}

bool WriteFullGammaRange(std::wstring* error) {
    HKEY key = nullptr;
    const LONG open = ::RegCreateKeyExW(HKEY_LOCAL_MACHINE, kIcmKey, 0, nullptr, 0,
                                        KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr,
                                        &key, nullptr);
    if (open != ERROR_SUCCESS) {
        if (error)
            *error = L"Não consegui abrir a chave do sistema (erro " +
                     std::to_wstring(open) + L").";
        return false;
    }
    const DWORD value = kIcmFullRange;
    const LONG r = ::RegSetValueExW(key, kIcmValue, 0, REG_DWORD,
                                    reinterpret_cast<const BYTE*>(&value), sizeof(value));
    ::RegCloseKey(key);
    if (r != ERROR_SUCCESS) {
        if (error)
            *error = L"Não consegui gravar o valor (erro " + std::to_wstring(r) + L").";
        return false;
    }
    return true;
}

bool RequestFullGammaRange(HWND owner, std::wstring* error) {
    if (FullGammaRangeEnabled()) return true;

    // Relaunches this same executable elevated with a flag that makes the child
    // write the value and exit. Elevating only this step, rather than the whole
    // installer, keeps the install per-user: declining elevation still yields a
    // complete installation, just without the full range.
    SHELLEXECUTEINFOW ei = {};
    ei.cbSize = sizeof(ei);
    ei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    ei.hwnd = owner;
    ei.lpVerb = L"runas";
    const std::wstring self = SelfPath();
    ei.lpFile = self.c_str();
    ei.lpParameters = L"--faixa-gama";
    ei.nShow = SW_HIDE;

    if (!::ShellExecuteExW(&ei) || !ei.hProcess) {
        const DWORD err = ::GetLastError();
        if (error)
            *error = err == ERROR_CANCELLED
                         ? L"Você recusou o pedido de administrador."
                         : L"Não consegui pedir elevação (erro " +
                               std::to_wstring(err) + L").";
        return false;
    }

    ::WaitForSingleObject(ei.hProcess, 60000);
    DWORD code = 1;
    ::GetExitCodeProcess(ei.hProcess, &code);
    ::CloseHandle(ei.hProcess);

    if (code != 0) {
        if (error) *error = L"O processo elevado não conseguiu gravar o valor.";
        return false;
    }
    // Read the value back: exit code 0 alone does not prove the key changed.
    if (!FullGammaRangeEnabled()) {
        if (error) *error = L"O valor não ficou gravado.";
        return false;
    }
    return true;
}

namespace {

void RegDeleteValueIfAny(HKEY root, const wchar_t* sub, const wchar_t* name) {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, sub, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    ::RegDeleteValueW(key, name);
    ::RegCloseKey(key);
}

std::wstring TodayStamp() {
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    wchar_t buf[16];
    ::wsprintfW(buf, L"%04u%02u%02u", st.wYear, st.wMonth, st.wDay);
    return buf;
}

// Removal

/// Deletes a whole tree except one file at its root (the running uninstaller).
/// Written by hand because SHFileOperation shows its own dialog on failure,
/// while the installer already reports errors on its own screen.
void RemoveTree(const std::wstring& dir, const wchar_t* keep) {
    if (dir.empty() || !DirExists(dir)) return;

    WIN32_FIND_DATAW fd;
    HANDLE h = ::FindFirstFileW(Join(dir, L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!::lstrcmpW(fd.cFileName, L".") || !::lstrcmpW(fd.cFileName, L"..")) continue;
            if (keep && ::lstrcmpiW(fd.cFileName, keep) == 0) continue;
            const std::wstring full = Join(dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                    ::RemoveDirectoryW(full.c_str());  // never follow a junction
                else
                    RemoveTree(full, nullptr);
            } else {
                ::SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
                ::DeleteFileW(full.c_str());
            }
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    }
    if (!keep) ::RemoveDirectoryW(dir.c_str());
}

/// Leaves a cmd.exe behind to delete the uninstaller after it exits, the only
/// way for an executable to remove itself without waiting for a reboot.
void ScheduleSelfDelete(const std::wstring& dir) {
    wchar_t sys[MAX_PATH] = {};
    if (!::GetSystemDirectoryW(sys, MAX_PATH)) return;
    const std::wstring cmd = std::wstring(sys) + L"\\cmd.exe";

    // ping provides the delay: timeout.exe requires an interactive console.
    std::wstring line = L"\"" + cmd + L"\" /c ping 127.0.0.1 -n 3 >nul & del /f /q "
                      + Quoted(Join(dir, kUninstName)) + L" & rmdir " + Quoted(dir);

    std::vector<wchar_t> mutableLine(line.begin(), line.end());
    mutableLine.push_back(L'\0');

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (::CreateProcessW(cmd.c_str(), mutableLine.data(), nullptr, nullptr, FALSE,
                         CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
    }
}

}  // namespace

// Utilities

bool NormalizeInstallDir(const std::wstring& input, std::wstring* normalized,
                         std::wstring* why) {
    return NormalizeInstallDirImpl(input, normalized, why);
}

std::wstring SelfPath() {
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n == 0) return std::wstring();
        if (n < buf.size() - 1) return std::wstring(buf.data(), n);
        buf.resize(buf.size() * 2);
    }
}

std::wstring SelfDir() {
    const std::wstring p = SelfPath();
    const size_t cut = p.find_last_of(L"\\/");
    return cut == std::wstring::npos ? std::wstring() : p.substr(0, cut);
}

std::wstring DefaultInstallDir() {
    std::wstring base = KnownFolder(kFOLDERID_LocalAppData);
    if (base.empty()) {
        wchar_t buf[MAX_PATH] = {};
        if (::GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH))
            base = std::wstring(buf) + L"\\AppData\\Local";
    }
    if (base.empty()) return L"C:\\Zdisplay";
    return base + L"\\Programs\\Zdisplay";
}

bool FindInstalled(std::wstring* dir) {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kAppKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    wchar_t buf[1024] = {};
    DWORD size = sizeof(buf), type = 0;
    const LONG r = ::RegQueryValueExW(key, L"InstallDir", nullptr, &type,
                                      reinterpret_cast<BYTE*>(buf), &size);
    ::RegCloseKey(key);
    if (r != ERROR_SUCCESS || type != REG_SZ || buf[0] == L'\0') return false;
    std::wstring normalized;
    if (!NormalizeInstallDir(buf, &normalized, nullptr)) return false;
    if (dir) *dir = normalized;
    return true;
}

DWORD PayloadSize() {
    const zdpack::Header* h = PayloadHeader(nullptr, nullptr);
    return h ? h->rawSize : 0;
}

Result VerifyEmbeddedPayload() {
    Result res;
    std::vector<BYTE> image;
    if (!UnpackPayload(&image, &res.message)) return res;
    res.ok = true;
    return res;
}

std::wstring FormatError(DWORD err) {
    wchar_t* text = nullptr;
    const DWORD n = ::FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&text), 0, nullptr);

    std::wstring msg;
    if (n && text) {
        msg.assign(text, n);
        while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n' || msg.back() == L' '))
            msg.pop_back();
    }
    if (text) ::LocalFree(text);

    wchar_t num[32];
    ::wsprintfW(num, L"erro %lu", err);
    return msg.empty() ? std::wstring(num) : (std::wstring(num) + L" — " + msg);
}

bool LaunchInstalled(const std::wstring& dir) {
    const std::wstring exe = Join(dir, kExeName);
    const HINSTANCE r = ::ShellExecuteW(nullptr, L"open", exe.c_str(), L"--tray",
                                        dir.c_str(), SW_SHOWNORMAL);
    return (INT_PTR)r > 32;
}

bool PickFolder(HWND owner, const std::wstring& current, std::wstring* out) {
    IFileDialog* dlg = nullptr;
    if (SUCCEEDED(::CoCreateInstance(kCLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                     kIID_IFileDialog, (void**)&dlg)) && dlg) {
        DWORD flags = 0;
        dlg->GetOptions(&flags);
        dlg->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dlg->SetTitle(L"Onde instalar o Zdisplay");

        // The previously chosen folder may not exist yet, since the installer
        // creates it; in that case walk up to the first existing parent.
        std::wstring start = current;
        while (!start.empty() && !DirExists(start)) {
            const size_t cut = start.find_last_of(L"\\/");
            if (cut == std::wstring::npos) { start.clear(); break; }
            start.erase(cut);
        }
        if (!start.empty()) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(::SHCreateItemFromParsingName(start.c_str(), nullptr,
                                                        kIID_IShellItem, (void**)&item)) && item) {
                dlg->SetFolder(item);
                item->Release();
            }
        }

        bool ok = false;
        if (SUCCEEDED(dlg->Show(owner))) {
            IShellItem* pick = nullptr;
            if (SUCCEEDED(dlg->GetResult(&pick)) && pick) {
                wchar_t* path = nullptr;
                if (SUCCEEDED(pick->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    if (out) *out = path;
                    ::CoTaskMemFree(path);
                    ok = true;
                }
                pick->Release();
            }
        }
        dlg->Release();
        return ok;
    }

    // On Windows versions without IFileDialog, fall back to the older dialog.
    BROWSEINFOW bi = {};
    wchar_t display[MAX_PATH] = {};
    bi.hwndOwner = owner;
    bi.pszDisplayName = display;
    bi.lpszTitle = L"Onde instalar o Zdisplay";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST idl = ::SHBrowseForFolderW(&bi);
    if (!idl) return false;
    wchar_t path[MAX_PATH] = {};
    const bool ok = ::SHGetPathFromIDListW(idl, path) != FALSE;
    ::CoTaskMemFree(idl);
    if (ok && out) *out = path;
    return ok;
}

// Install

Result Install(const Options& opt, ProgressFn progress, void* ctx) {
    Result res;
    std::wstring dir;
    if (!NormalizeInstallDir(opt.dir, &dir, &res.message)) return res;
    const std::wstring exe  = Join(dir, kExeName);
    const std::wstring unin = Join(dir, kUninstName);

    // Unpack before touching the disk: if the payload is bad, nothing has been
    // created yet and no half-finished install is left behind.
    Report(progress, ctx, 3, L"Preparando...");
    std::vector<BYTE> image;
    if (!UnpackPayload(&image, &res.message)) return res;

    Report(progress, ctx, 6, L"Fechando o Zdisplay...");
    if (!CloseRunningApp(exe, &res.message)) return res;

    Report(progress, ctx, 12, L"Criando a pasta...");
    DWORD err = 0;
    if (!EnsureDir(dir, &err)) {
        res.message = L"Não consegui criar a pasta de instalação: " + FormatError(err);
        return res;
    }

    Report(progress, ctx, 20, L"Copiando o Zdisplay...");
    if (!WriteExe(exe, image, progress, ctx, 20, 70, &res.message)) return res;

    Report(progress, ctx, 76, L"Preparando o desinstalador...");
    if (!::CopyFileW(SelfPath().c_str(), unin.c_str(), FALSE)) {
        // Without an uninstaller the installation would work but offer no clean
        // way out, so this counts as an error rather than a warning.
        res.message = L"Não consegui criar o desinstalador: " + FormatError(::GetLastError());
        return res;
    }

    if (!WriteInstallMarker(dir, &res.message)) return res;

    Report(progress, ctx, 85, L"Criando os atalhos...");
    if (!CreateShortcut(StartMenuLink(), exe, dir)) {
        res.message = L"Nao consegui criar o atalho no menu Iniciar.";
        return res;
    }
    if (opt.desktopIcon) {
        if (!CreateShortcut(DesktopLink(), exe, dir)) {
            res.message = L"Nao consegui criar o atalho na area de trabalho.";
            return res;
        }
    } else {
        ::DeleteFileW(DesktopLink().c_str());
    }

    Report(progress, ctx, 92, L"Registrando...");
    const bool registered =
        RegSetStr(HKEY_CURRENT_USER, kAppKey, L"InstallDir", dir) &&
        RegSetStr(HKEY_CURRENT_USER, kAppKey, L"Version", kVersionStr) &&
        RegSetStr(HKEY_CURRENT_USER, kUninstallKey, L"DisplayName", kAppName) &&
        RegSetStr(HKEY_CURRENT_USER, kUninstallKey, L"DisplayVersion", kVersionStr) &&
        RegSetStr(HKEY_CURRENT_USER, kUninstallKey, L"Publisher", kPublisher) &&
        RegSetStr(HKEY_CURRENT_USER, kUninstallKey, L"DisplayIcon", exe + L",0") &&
        RegSetStr(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation", dir) &&
        RegSetStr(HKEY_CURRENT_USER, kUninstallKey, L"UninstallString", Quoted(unin) + L" /uninstall") &&
        RegSetStr(HKEY_CURRENT_USER, kUninstallKey, L"QuietUninstallString", Quoted(unin) + L" /uninstall /S") &&
        RegSetStr(HKEY_CURRENT_USER, kUninstallKey, L"InstallDate", TodayStamp()) &&
        RegSetDword(HKEY_CURRENT_USER, kUninstallKey, L"NoModify", 1) &&
        RegSetDword(HKEY_CURRENT_USER, kUninstallKey, L"NoRepair", 1) &&
        RegSetDword(HKEY_CURRENT_USER, kUninstallKey, L"EstimatedSize",
                    (DWORD)((image.size() + 1023) / 1024));
    if (!registered) {
        res.message = L"O programa foi copiado, mas o Windows recusou o registro em Aplicativos instalados.";
        return res;
    }

    if (opt.autostart) {
        if (!RegSetStr(HKEY_CURRENT_USER, kRunKey, kRunValue, Quoted(exe) + L" --tray")) {
            res.message = L"O Windows recusou a configuracao de inicio automatico.";
            return res;
        }
    } else {
        RegDeleteValueIfAny(HKEY_CURRENT_USER, kRunKey, kRunValue);
    }

    Report(progress, ctx, 100, L"Pronto.");
    res.ok = true;
    return res;
}

// Uninstall

Result Uninstall(const Options& opt, ProgressFn progress, void* ctx) {
    Result res;
    std::wstring dir;
    if (!NormalizeInstallDir(opt.dir, &dir, &res.message)) return res;
    const std::wstring exe = Join(dir, kExeName);

    Report(progress, ctx, 10, L"Fechando o Zdisplay...");
    if (!CloseRunningApp(exe, &res.message)) return res;

    Report(progress, ctx, 30, L"Removendo o início automático...");
    RegDeleteValueIfAny(HKEY_CURRENT_USER, kRunKey, kRunValue);

    Report(progress, ctx, 45, L"Removendo os atalhos...");
    ::DeleteFileW(StartMenuLink().c_str());
    ::DeleteFileW(DesktopLink().c_str());

    Report(progress, ctx, 60, L"Limpando o registro...");
    ::RegDeleteKeyW(HKEY_CURRENT_USER, kUninstallKey);
    ::RegDeleteKeyW(HKEY_CURRENT_USER, kAppKey);

    if (opt.removeSettings) {
        Report(progress, ctx, 75, L"Apagando as configurações...");
        const std::wstring roaming = KnownFolder(kFOLDERID_RoamingAppData);
        if (!roaming.empty()) {
            const std::wstring cfg = Join(roaming, L"Zdisplay");
            // Prefix check: recursive deletion is the most dangerous operation
            // in the installer and may only happen inside AppData.
            if (cfg.size() > roaming.size() && DirExists(cfg))
                RemoveTree(cfg, nullptr);
        }
    }

    Report(progress, ctx, 90, L"Removendo os arquivos...");
    if (!dir.empty() && DirExists(dir)) {
        // The install root is never deleted recursively: only the files
        // recorded at install time are removed, so user files placed in the
        // folder survive.
        if (!DeleteKnownFile(exe, &res.message) ||
            !DeleteKnownFile(exe + L".tmp", &res.message) ||
            !DeleteKnownFile(Join(dir, kInstallMarker), &res.message))
            return res;

        const std::wstring unin = Join(dir, kUninstName);
        if (::lstrcmpiW(SelfPath().c_str(), unin.c_str()) == 0) {
            ScheduleSelfDelete(dir);
        } else {
            if (!DeleteKnownFile(unin, &res.message)) return res;
            ::RemoveDirectoryW(dir.c_str());  // succeeds only if empty
        }
    }

    Report(progress, ctx, 100, L"Pronto.");
    res.ok = true;
    return res;
}

}  // namespace setup
