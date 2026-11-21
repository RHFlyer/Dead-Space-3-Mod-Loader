// A bounded reader of DLL files. See pefile.h.

#include "pefile.h"

#include <cstdio>
#include <cstring>

namespace {

// ---- SHA-256 (FIPS 180-4), so the loader needs no crypto DLL ----

constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

uint32_t Rotr(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

void Sha256Block(uint32_t h[8], const uint8_t* p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (static_cast<uint32_t>(p[i * 4]) << 24) | (static_cast<uint32_t>(p[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(p[i * 4 + 2]) << 8) | p[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = k + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
        const uint32_t t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += k;
}

uint16_t U16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t U32(const uint8_t* p)
{
    return p[0] | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

constexpr unsigned kSectionSize = 40;     // IMAGE_SECTION_HEADER
constexpr unsigned kMaxSections = 96;
constexpr unsigned kMaxNames = 65536;     // export names looked at
constexpr unsigned kMaxImports = 512;     // import descriptors looked at

}  // namespace

PeFile::PeFile(const wchar_t* path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && static_cast<unsigned long long>(size.QuadPart) <= kMaxBytes) {
        size_ = static_cast<unsigned long long>(size.QuadPart);
        data_ = static_cast<uint8_t*>(HeapAlloc(GetProcessHeap(), 0, static_cast<SIZE_T>(size_)));
        DWORD got = 0;
        if (data_ && (!ReadFile(f, data_, static_cast<DWORD>(size_), &got, nullptr) || got != size_)) {
            HeapFree(GetProcessHeap(), 0, data_);
            data_ = nullptr;
        }
    }
    CloseHandle(f);
    if (!data_) return;

    // DOS header, then "PE\0\0", the file header (20 bytes) and the 32-bit optional header.
    if (size_ < 0x40 || U16(data_) != IMAGE_DOS_SIGNATURE) return;
    const uint32_t nt = U32(data_ + 0x3C);
    if (nt > size_ || size_ - nt < 24 || U32(data_ + nt) != IMAGE_NT_SIGNATURE) return;
    const uint8_t* fh = data_ + nt + 4;
    const uint16_t machine = U16(fh);
    sectionCount_ = U16(fh + 2);
    const uint16_t optSize = U16(fh + 16);
    const uint16_t characteristics = U16(fh + 18);
    const unsigned long long opt = static_cast<unsigned long long>(nt) + 24;
    if (sectionCount_ == 0 || sectionCount_ > kMaxSections) return;
    if (opt + optSize + static_cast<unsigned long long>(sectionCount_) * kSectionSize > size_) return;
    sections_ = data_ + opt + optSize;
    image_ = true;
    dll_ = (characteristics & IMAGE_FILE_DLL) != 0;
    x86_ = machine == IMAGE_FILE_MACHINE_I386 && optSize >= 96 && U16(data_ + opt) == IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    if (!x86_) return;
    // The headers are mapped as they are in the file, so a relative address below SizeOfHeaders is a file offset.
    headers_ = U32(data_ + opt + 60);
    if (headers_ > size_) headers_ = static_cast<uint32_t>(size_);
    // Data directories of a PE32 header start at +96: export is entry 0, import entry 1.
    const uint32_t dirs = U32(data_ + opt + 92);
    if (dirs >= 1 && optSize >= 96 + 8) {
        exportRva_ = U32(data_ + opt + 96);
        exportSize_ = U32(data_ + opt + 100);
    }
    if (dirs >= 2 && optSize >= 96 + 16) {
        importRva_ = U32(data_ + opt + 104);
        importSize_ = U32(data_ + opt + 108);
    }
}

PeFile::~PeFile()
{
    if (data_) HeapFree(GetProcessHeap(), 0, data_);
}

const uint8_t* PeFile::At(uint32_t rva, uint32_t size) const
{
    if (!image_ || !rva) return nullptr;
    if (rva < headers_) return size <= headers_ - rva ? data_ + rva : nullptr;
    for (unsigned i = 0; i < sectionCount_; ++i) {
        const uint8_t* s = sections_ + i * kSectionSize;
        const uint32_t va = U32(s + 12), rawSize = U32(s + 16), raw = U32(s + 20);
        if (rva < va || rva - va >= rawSize) continue;
        const unsigned long long off = static_cast<unsigned long long>(raw) + (rva - va);
        if (size > rawSize - (rva - va) || off + size > size_) return nullptr;
        return data_ + off;
    }
    return nullptr;
}

const char* PeFile::TextAt(uint32_t rva) const
{
    const uint8_t* p = At(rva, 1);
    if (!p) return nullptr;
    const uint8_t* end = data_ + size_;
    for (unsigned n = 0; n < 256 && p + n < end; ++n) {
        if (!p[n]) return reinterpret_cast<const char*>(p);
    }
    return nullptr;
}

bool PeFile::Exports(const char* name) const
{
    const uint8_t* dir = exportSize_ >= 40 ? At(exportRva_, 40) : nullptr;  // IMAGE_EXPORT_DIRECTORY
    if (!dir) return false;
    const uint32_t count = U32(dir + 24), namesRva = U32(dir + 32);
    if (!count || count > kMaxNames) return false;
    const uint8_t* names = At(namesRva, count * 4);
    if (!names) return false;
    for (uint32_t i = 0; i < count; ++i) {
        const char* text = TextAt(U32(names + i * 4));
        if (text && std::strcmp(text, name) == 0) return true;
    }
    return false;
}

void PeFile::Imports(bool (*each)(const char* name, void* user), void* user) const
{
    if (!importRva_) return;
    for (unsigned i = 0; i < kMaxImports; ++i) {
        // Each IMAGE_IMPORT_DESCRIPTOR is 20 bytes. Stay inside the directory the header declares, and inside 32 bits.
        const unsigned long long offset = i * 20ull;
        if (importSize_ && offset + 20 > importSize_) return;
        if (importRva_ + offset > 0xFFFFFFFFull) return;
        const uint8_t* d = At(static_cast<uint32_t>(importRva_ + offset), 20);
        if (!d) return;
        const uint32_t nameRva = U32(d + 12);
        if (!nameRva && !U32(d) && !U32(d + 16)) return;  // the empty one that ends the table
        const char* text = TextAt(nameRva);
        if (text && !each(text, user)) return;
    }
}

void PeFile::Sha256(char out[65]) const
{
    out[0] = '\0';
    if (!data_) return;
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned long long done = 0;
    for (; done + 64 <= size_; done += 64) Sha256Block(h, data_ + done);
    uint8_t tail[128] = {};
    const unsigned rest = static_cast<unsigned>(size_ - done);
    std::memcpy(tail, data_ + done, rest);
    tail[rest] = 0x80;
    const unsigned total = rest < 56 ? 64 : 128;
    const unsigned long long bits = size_ * 8;
    for (int i = 0; i < 8; ++i) tail[total - 1 - i] = static_cast<uint8_t>(bits >> (8 * i));
    Sha256Block(h, tail);
    if (total == 128) Sha256Block(h, tail + 64);
    for (int i = 0; i < 8; ++i) sprintf_s(out + i * 8, 9, "%08x", h[i]);
}
