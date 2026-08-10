// Format of the zdisplay.exe embedded in the installer.
//
// The packer (pack.cpp) compresses with the API already present in Windows
// (RtlCompressBuffer, ntdll) and the installer decompresses with its
// counterpart, so the project needs no compression library.
//
// When compression does not help or the API is unavailable, the payload is
// stored raw; `method` records which case applies and the installer handles
// both.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace zdpack {

const uint32_t kMagic   = 0x4B50445A;  ///< 'ZDPK' in little-endian
const uint32_t kVersion = 2;
const uint32_t kSha256Size = 32;

// A normal zdisplay.exe is under 2 MB. The cap leaves room to grow while
// preventing a tampered header from making the installer allocate gigabytes.
const uint32_t kMaxRawSize    = 64u * 1024u * 1024u;
const uint32_t kMaxStoredSize = 64u * 1024u * 1024u;

enum Method : uint32_t {
    kStore      = 0,  ///< no compression
    kXpressHuff = 1,  ///< COMPRESSION_FORMAT_XPRESS_HUFF
};

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    uint32_t version;
    uint32_t method;
    uint32_t rawSize;     ///< size of the original zdisplay.exe
    uint32_t storedSize;  ///< bytes that follow immediately after this header
    uint32_t rawCrc32;    ///< CRC-32 of the original, checked after decompression
    uint8_t  rawSha256[kSha256Size]; ///< SHA-256 of the original executable
};
#pragma pack(pop)

static_assert(sizeof(Header) == 56, "payload header changed without a version bump");

/// CRC-32 (polynomial 0xEDB88320). Defined once in this header so the packer
/// and the installer cannot drift apart.
inline uint32_t Crc32(const void* data, size_t size, uint32_t seed = 0) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    uint32_t crc = seed ^ 0xFFFFFFFFu;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i)
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

}  // namespace zdpack
