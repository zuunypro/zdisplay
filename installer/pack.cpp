// Payload packer. Runs only when the installer is assembled; it is not part of
// the final executable.
//
// Compresses zdisplay.exe with the XPRESS Huffman codec already present in
// Windows (ntdll) and writes the payload.h header in front of it, which keeps
// the project free of any compression dependency.
//
//   pack.exe <input.exe> <output.bin>
#include "payload.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <vector>

namespace {

typedef NTSTATUS (WINAPI *PfnWorkSpace)(USHORT, PULONG, PULONG);
typedef NTSTATUS (WINAPI *PfnCompress)(USHORT, PUCHAR, ULONG, PUCHAR, ULONG, ULONG, PULONG, PVOID);
typedef NTSTATUS (WINAPI *PfnDecompress)(USHORT, PUCHAR, ULONG, PUCHAR, ULONG, PULONG, PVOID);

const USHORT kFormat = 4;       // COMPRESSION_FORMAT_XPRESS_HUFF
const USHORT kEngineMax = 0x0100;  // COMPRESSION_ENGINE_MAXIMUM

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

bool ReadWholeFile(const wchar_t* path, std::vector<BYTE>* out) {
    HANDLE h = ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size = {};
    if (!::GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > 0x7FFFFFFF) {
        ::CloseHandle(h);
        return false;
    }
    out->resize((size_t)size.QuadPart);

    size_t done = 0;
    while (done < out->size()) {
        DWORD got = 0;
        const DWORD want = (DWORD)((out->size() - done > 1u << 20) ? (1u << 20) : out->size() - done);
        if (!::ReadFile(h, out->data() + done, want, &got, nullptr) || got == 0) {
            ::CloseHandle(h);
            return false;
        }
        done += got;
    }
    ::CloseHandle(h);
    return true;
}

bool WriteWholeFile(const wchar_t* path, const void* header, size_t headerSize,
                    const void* body, size_t bodySize) {
    HANDLE h = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    bool ok = ::WriteFile(h, header, (DWORD)headerSize, &n, nullptr) && n == headerSize;
    if (ok && bodySize)
        ok = ::WriteFile(h, body, (DWORD)bodySize, &n, nullptr) && n == bodySize;
    ::FlushFileBuffers(h);
    ::CloseHandle(h);
    return ok;
}

/// Compresses and verifies the result by decompressing it back to the original,
/// so a payload that does not round-trip fails at build time rather than on the
/// user's machine.
bool TryCompress(const std::vector<BYTE>& raw, std::vector<BYTE>* out) {
    HMODULE nt = ::GetModuleHandleW(L"ntdll.dll");
    if (!nt) return false;
    PfnWorkSpace ws   = (PfnWorkSpace)(void*)::GetProcAddress(nt, "RtlGetCompressionWorkSpaceSize");
    PfnCompress  comp = (PfnCompress)(void*)::GetProcAddress(nt, "RtlCompressBuffer");
    PfnDecompress dec = (PfnDecompress)(void*)::GetProcAddress(nt, "RtlDecompressBufferEx");
    if (!ws || !comp || !dec) return false;

    ULONG wsBuf = 0, wsFrag = 0;
    if (ws((USHORT)(kFormat | kEngineMax), &wsBuf, &wsFrag) < 0) return false;

    std::vector<BYTE> space(wsBuf > wsFrag ? wsBuf : wsFrag);
    out->resize(raw.size() + raw.size() / 16 + 4096);

    ULONG finalSize = 0;
    const NTSTATUS st = comp((USHORT)(kFormat | kEngineMax),
                             const_cast<BYTE*>(raw.data()), (ULONG)raw.size(),
                             out->data(), (ULONG)out->size(), 4096, &finalSize, space.data());
    if (st < 0 || finalSize == 0 || finalSize >= raw.size()) return false;
    out->resize(finalSize);

    std::vector<BYTE> back(raw.size());
    std::vector<BYTE> decSpace(wsBuf > wsFrag ? wsBuf : wsFrag);
    ULONG backSize = 0;
    if (dec(kFormat, back.data(), (ULONG)back.size(), out->data(), (ULONG)out->size(),
            &backSize, decSpace.data()) < 0)
        return false;
    if (backSize != raw.size() || ::memcmp(back.data(), raw.data(), raw.size()) != 0)
        return false;
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        ::fwprintf(stderr, L"uso: pack.exe <entrada.exe> <saida.bin>\n");
        return 1;
    }

    std::vector<BYTE> raw;
    if (!ReadWholeFile(argv[1], &raw)) {
        ::fwprintf(stderr, L"nao consegui ler %s\n", argv[1]);
        return 1;
    }

    zdpack::Header h = {};
    h.magic = zdpack::kMagic;
    h.version = zdpack::kVersion;
    h.rawSize = (uint32_t)raw.size();
    h.rawCrc32 = zdpack::Crc32(raw.data(), raw.size());
    if (!Sha256(raw.data(), raw.size(), h.rawSha256)) {
        ::fwprintf(stderr, L"nao consegui calcular o SHA-256 do programa\n");
        return 1;
    }

    std::vector<BYTE> packed;
    const bool compressed = TryCompress(raw, &packed);
    if (compressed) {
        h.method = zdpack::kXpressHuff;
        h.storedSize = (uint32_t)packed.size();
    } else {
        h.method = zdpack::kStore;
        h.storedSize = (uint32_t)raw.size();
        ::wprintf(L"aviso: compressao indisponivel ou sem ganho; gravando cru.\n");
    }

    const void* body = compressed ? (const void*)packed.data() : (const void*)raw.data();
    if (!WriteWholeFile(argv[2], &h, sizeof(h), body, h.storedSize)) {
        ::fwprintf(stderr, L"nao consegui gravar %s\n", argv[2]);
        return 1;
    }

    const double ratio = raw.empty() ? 0.0 : 100.0 * h.storedSize / raw.size();
    ::wprintf(L"payload: %u -> %u bytes (%.1f%%)\n", h.rawSize, h.storedSize, ratio);
    return 0;
}
