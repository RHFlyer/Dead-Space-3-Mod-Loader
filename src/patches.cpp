// Patch mods. See patches.h, and sdk\README.md for the format.

#include "patches.h"

#include "log.h"
#include "mods.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>

namespace {

// Never patchable, whatever the bytes: account, store and paid-content code and data, so no mod can touch logins,
// purchases or what a player owns. Generous on purpose, and not a complete list, so NearProtectedText below also
// refuses anything next to such text. The game's copy-protection code is never patchable either: it lies outside the
// three sections patches are allowed in.
struct Range {
    uintptr_t begin, end;
    const char* what;
};
constexpr const char* kProtectedLabel = "a protected area (accounts, store or paid content)";
constexpr Range kRefused[] = {
    {0x0055B000, 0x0055E000, kProtectedLabel},
    {0x007F4000, 0x007FA000, kProtectedLabel},
    {0x00A28000, 0x00A50000, kProtectedLabel},
    {0x00C88000, 0x00CC4000, kProtectedLabel},
    {0x00E23000, 0x00E24000, kProtectedLabel},
};
// Lower-case text that marks entitlement, ownership, login or Origin data. A patch whose bytes lie within
// kTextMargin bytes of one is refused.
constexpr const char* kProtectedText[] = {"entitlement", "pdlc", "mtx_", "owned", "ownership", "ebisu",
                                          "originsdk", "origin sdk", "blaze", "lsx"};
constexpr int kTextMargin = 64;
// Set by the proxy once the game's .text is known to be decrypted; until then no patch is written.
bool g_patchesAllowed = false;

enum class PatchKind { Fixed, Toggle, Value };
enum class ValueType { Int8, Int16, Int32, UInt8, UInt16, UInt32, Float, Double };

struct ModPatch {
    int mod;
    char name[32];
    uintptr_t va;      // where `expect` starts
    int offset;        // where the written bytes start, from va
    uint8_t expect[kMaxPatchBytes];
    int expectLen;
    uint8_t write[kMaxPatchBytes];  // Fixed / Toggle
    int writeLen;                   // bytes written (Value: the type's size)
    PatchKind kind;
    ValueType type;    // Value
    int setting;       // Toggle / Value: index into ModSettingAt, else -1
    bool applied;      // checked and claimed; its bytes are ours
};
ModPatch g_patches[kMaxPatches] = {};
int g_patchCount = 0;

struct Claim {
    uintptr_t begin, end;
    char owner[40];
};
constexpr int kMaxClaims = 192;
Claim g_claims[kMaxClaims] = {};
int g_claimCount = 0;
SRWLOCK g_claimLock = SRWLOCK_INIT;

// ---- Text ----

void Trim(wchar_t* s)
{
    size_t len = wcslen(s);
    while (len && iswspace(s[len - 1])) s[--len] = L'\0';
    size_t lead = 0;
    while (s[lead] && iswspace(s[lead])) ++lead;
    if (lead) wmemmove(s, s + lead, len - lead + 1);
}

void Read(const wchar_t* path, const wchar_t* section, const wchar_t* key, wchar_t* out, DWORD size)
{
    GetPrivateProfileStringW(section, key, L"", out, size, path);
    Trim(out);
}

// "55 8B EC" (also "558BEC" or with commas). Returns the count, or -1 if it is not hex bytes or too long.
int ParseBytes(const wchar_t* text, uint8_t* out)
{
    int n = 0;
    const wchar_t* p = text;
    while (*p) {
        if (iswspace(*p) || *p == L',') {
            ++p;
            continue;
        }
        if (!iswxdigit(p[0]) || !iswxdigit(p[1]) || n >= kMaxPatchBytes) return -1;
        wchar_t pair[3] = {p[0], p[1], 0};
        out[n++] = static_cast<uint8_t>(wcstoul(pair, nullptr, 16));
        p += 2;
    }
    return n;
}

// "0x0040D820", "0040D820", or "deadspace3.exe+D820" (Cheat Engine's style).
bool ParseAddress(const wchar_t* text, uintptr_t* out)
{
    uintptr_t base = 0;
    const wchar_t* p = text;
    if (_wcsnicmp(p, L"deadspace3.exe", 14) == 0) {
        p += 14;
        while (iswspace(*p)) ++p;
        if (*p != L'+') return false;
        ++p;
        base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    }
    while (iswspace(*p)) ++p;
    if (p[0] == L'0' && (p[1] == L'x' || p[1] == L'X')) p += 2;
    if (!iswxdigit(*p)) return false;
    wchar_t* end = nullptr;
    unsigned long v = wcstoul(p, &end, 16);
    while (end && iswspace(*end)) ++end;
    if (!end || *end) return false;
    *out = base + v;
    return true;
}

bool ParseType(const wchar_t* text, ValueType* type, int* size)
{
    struct {
        const wchar_t* name;
        ValueType type;
        int size;
    } constexpr kTypes[] = {{L"int8", ValueType::Int8, 1},     {L"int16", ValueType::Int16, 2},
                            {L"int32", ValueType::Int32, 4},   {L"uint8", ValueType::UInt8, 1},
                            {L"uint16", ValueType::UInt16, 2}, {L"uint32", ValueType::UInt32, 4},
                            {L"float", ValueType::Float, 4},   {L"double", ValueType::Double, 8},
                            {L"byte", ValueType::UInt8, 1},    {L"int", ValueType::Int32, 4}};
    for (const auto& t : kTypes) {
        if (_wcsicmp(text, t.name) == 0) {
            *type = t.type;
            *size = t.size;
            return true;
        }
    }
    return false;
}

// The smallest and largest value a type holds (for checking a setting's range against it).
void TypeRange(ValueType t, double* lo, double* hi)
{
    switch (t) {
    case ValueType::Int8: *lo = -128, *hi = 127; return;
    case ValueType::Int16: *lo = -32768, *hi = 32767; return;
    case ValueType::Int32: *lo = -2147483648.0, *hi = 2147483647.0; return;
    case ValueType::UInt8: *lo = 0, *hi = 255; return;
    case ValueType::UInt16: *lo = 0, *hi = 65535; return;
    case ValueType::UInt32: *lo = 0, *hi = 4294967295.0; return;
    default: *lo = -1e300, *hi = 1e300; return;
    }
}

int FindSetting(int modIndex, const wchar_t* name)
{
    char ascii[32] = {};
    for (int i = 0; name[i] && i < 31; ++i) ascii[i] = name[i] < 0x80 ? static_cast<char>(name[i]) : '?';
    for (int i = 0; i < ModSettingCount(); ++i) {
        const ModSetting& s = ModSettingAt(i);
        if (s.mod == modIndex && _stricmp(s.name, ascii) == 0) return i;
    }
    return -1;
}

// ---- The game's memory ----

// Inside the exe's own .text, .rdata or .data (codeOnly: .text), read from the loaded image's section table.
bool InExeSections(uintptr_t va, size_t size, bool codeOnly = false)
{
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        char name[9] = {};
        memcpy(name, sec->Name, 8);
        const bool code = strcmp(name, ".text") == 0;
        if (!code && (codeOnly || (strcmp(name, ".rdata") != 0 && strcmp(name, ".data") != 0))) continue;
        const uintptr_t begin = base + sec->VirtualAddress, end = begin + sec->Misc.VirtualSize;
        if (va >= begin && va + size <= end) return true;
    }
    return false;
}

const char* RefusedRange(uintptr_t va, size_t size)
{
    for (const Range& r : kRefused) {
        if (va < r.end && va + size > r.begin) return r.what;
    }
    return nullptr;
}

bool ReadBytes(uintptr_t va, void* out, size_t size)
{
    __try {
        memcpy(out, reinterpret_cast<const void*>(va), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The protected word found within kTextMargin bytes of [va, va + size), or nullptr. Reads the game's memory as it is
// now (only the exe's sections; a part that can't be read counts as no text).
const char* NearProtectedText(uintptr_t va, size_t size)
{
    uint8_t buf[kMaxPatchBytes + 2 * kTextMargin + 16];
    const uintptr_t begin = va - kTextMargin;
    const size_t len = size + 2 * kTextMargin;
    if (!InExeSections(begin, len) || !ReadBytes(begin, buf, len)) {
        // At an edge of a section: read byte by byte what can be read.
        for (size_t i = 0; i < len; ++i) {
            if (!InExeSections(begin + i, 1) || !ReadBytes(begin + i, buf + i, 1)) buf[i] = 0;
        }
    }
    for (size_t i = 0; i < len; ++i) buf[i] = static_cast<uint8_t>(tolower(buf[i]));
    for (const char* word : kProtectedText) {
        const size_t n = strlen(word);
        for (size_t i = 0; i + n <= len; ++i) {
            if (memcmp(buf + i, word, n) == 0) return word;
        }
    }
    return nullptr;
}

// Writes with the page made writable for the moment. If the bytes lie in one aligned 8-byte block they are written
// with one atomic store, so a thread running that code meanwhile sees the old bytes or the new, never a mix.
bool WriteBytesRaw(uintptr_t va, const uint8_t* bytes, size_t size)
{
    __try {
        const uintptr_t block = va & ~static_cast<uintptr_t>(7);
        if (((va + size - 1) & ~static_cast<uintptr_t>(7)) == block) {
            auto* q = reinterpret_cast<volatile LONG64*>(block);
            for (;;) {
                LONG64 old = *q, next = old;
                memcpy(reinterpret_cast<uint8_t*>(&next) + (va - block), bytes, size);
                if (InterlockedCompareExchange64(q, next, old) == old) break;
            }
        } else {
            memcpy(reinterpret_cast<void*>(va), bytes, size);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteBytes(uintptr_t va, const uint8_t* bytes, size_t size)
{
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(va), size, PAGE_EXECUTE_READWRITE, &old)) return false;
    bool ok = WriteBytesRaw(va, bytes, size);
    VirtualProtect(reinterpret_cast<void*>(va), size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(va), size);
    return ok;
}

// Whether a rewrite while the game runs is one atomic store (see WriteBytesRaw).
bool LiveWritable(const ModPatch& p)
{
    const uintptr_t at = p.va + p.offset;
    return ((at + p.writeLen - 1) & ~static_cast<uintptr_t>(7)) == (at & ~static_cast<uintptr_t>(7));
}

// What a patch puts at va + offset now: its bytes, the original (a toggle that is off), or the setting's value.
void PatchBytes(const ModPatch& p, uint8_t* out)
{
    if (p.kind == PatchKind::Fixed) {
        memcpy(out, p.write, p.writeLen);
        return;
    }
    const double v = ModSettingAt(p.setting).value;
    if (p.kind == PatchKind::Toggle) {
        memcpy(out, v != 0 ? p.write : p.expect + p.offset, p.writeLen);
        return;
    }
    const double r = std::round(v);
    switch (p.type) {
    case ValueType::Int8: { int8_t x = static_cast<int8_t>(r); memcpy(out, &x, 1); return; }
    case ValueType::Int16: { int16_t x = static_cast<int16_t>(r); memcpy(out, &x, 2); return; }
    case ValueType::Int32: { int32_t x = static_cast<int32_t>(r); memcpy(out, &x, 4); return; }
    case ValueType::UInt8: { uint8_t x = static_cast<uint8_t>(r); memcpy(out, &x, 1); return; }
    case ValueType::UInt16: { uint16_t x = static_cast<uint16_t>(r); memcpy(out, &x, 2); return; }
    case ValueType::UInt32: { uint32_t x = static_cast<uint32_t>(r); memcpy(out, &x, 4); return; }
    case ValueType::Float: { float x = static_cast<float>(v); memcpy(out, &x, 4); return; }
    case ValueType::Double: memcpy(out, &v, 8); return;
    }
}

// ---- Parsing ----

bool ParsePatch(int modIndex, const wchar_t* manifest, const wchar_t* sec, ModPatch& p)
{
    const Mod& m = ModAt(modIndex);
    p = {};
    p.mod = modIndex;
    p.setting = -1;
    for (int i = 0; sec[i] && i < 31; ++i) p.name[i] = sec[i] < 0x80 ? static_cast<char>(sec[i]) : '?';

    wchar_t text[512];
    Read(manifest, sec, L"address", text, 64);
    if (!ParseAddress(text, &p.va)) {
        Log("mod %s: [%ls] needs address= (e.g. 0x0040D820 or deadspace3.exe+D820); skipped mod", m.id, sec);
        return false;
    }
    Read(manifest, sec, L"expect", text, 512);
    p.expectLen = ParseBytes(text, p.expect);
    if (p.expectLen <= 0) {
        Log("mod %s: [%ls] needs expect= with the bytes the game has there now, in hex (e.g. 55 8B EC), at most %d; "
            "skipped mod", m.id, sec, kMaxPatchBytes);
        return false;
    }
    Read(manifest, sec, L"offset", text, 16);
    p.offset = text[0] ? _wtoi(text) : 0;

    wchar_t write[512], value[32], toggle[32], type[16];
    Read(manifest, sec, L"write", write, 512);
    Read(manifest, sec, L"value", value, 32);
    Read(manifest, sec, L"toggle", toggle, 32);
    Read(manifest, sec, L"type", type, 16);
    if (value[0]) {
        p.kind = PatchKind::Value;
        if (write[0] || toggle[0]) {
            Log("mod %s: [%ls] has value= with write= or toggle=; a patch is one or the other; skipped mod", m.id, sec);
            return false;
        }
        if (!ParseType(type, &p.type, &p.writeLen)) {
            Log("mod %s: [%ls] value= needs type= int8, int16, int32, uint8, uint16, uint32, float or double; skipped "
                "mod", m.id, sec);
            return false;
        }
        p.setting = FindSetting(modIndex, value);
        if (p.setting < 0) {
            Log("mod %s: [%ls] value=%ls: no [setting.%ls] in this mod.ini; skipped mod", m.id, sec, value, value);
            return false;
        }
        const ModSetting& s = ModSettingAt(p.setting);
        double lo = 0, hi = 0;
        TypeRange(p.type, &lo, &hi);
        if (s.kind == ModSettingKind::Key || s.kind == ModSettingKind::Choice) {
            Log("mod %s: [%ls] value=%ls must be a bool, int or float setting; skipped mod", m.id, sec, value);
            return false;
        }
        if (s.minValue < lo || s.maxValue > hi) {
            Log("mod %s: [%ls] setting %ls goes from %g to %g, which does not fit in %ls; skipped mod", m.id, sec,
                value, s.minValue, s.maxValue, type);
            return false;
        }
    } else {
        p.kind = toggle[0] ? PatchKind::Toggle : PatchKind::Fixed;
        p.writeLen = ParseBytes(write, p.write);
        if (p.writeLen <= 0) {
            Log("mod %s: [%ls] needs write= with the new bytes in hex (or value= with a setting); skipped mod", m.id,
                sec);
            return false;
        }
        if (toggle[0]) {
            p.setting = FindSetting(modIndex, toggle);
            if (p.setting < 0 || ModSettingAt(p.setting).kind != ModSettingKind::Bool) {
                Log("mod %s: [%ls] toggle=%ls must name a [setting.%ls] of kind=bool in this mod.ini; skipped mod",
                    m.id, sec, toggle, toggle);
                return false;
            }
        }
    }
    if (p.offset < 0 || p.offset + p.writeLen > p.expectLen) {
        Log("mod %s: [%ls] expect= must cover every byte written (offset %d + %d bytes, expect has %d); skipped mod",
            m.id, sec, p.offset, p.writeLen, p.expectLen);
        return false;
    }
    if (const char* what = RefusedRange(p.va, p.expectLen)) {
        Log("mod %s: [%ls] 0x%08X is in %s, which patch mods may never change; skipped mod", m.id, sec,
            static_cast<unsigned>(p.va), what);
        return false;
    }
    if (!InExeSections(p.va, p.expectLen)) {
        Log("mod %s: [%ls] 0x%08X is not in deadspace3.exe's code or data (.text, .rdata, .data); skipped mod", m.id,
            sec, static_cast<unsigned>(p.va));
        return false;
    }
    return true;
}

// Gives back every range `owner` holds (a mod whose patches were not applied after all).
void ReleaseClaims(const char* owner)
{
    AcquireSRWLockExclusive(&g_claimLock);
    int kept = 0;
    for (int i = 0; i < g_claimCount; ++i) {
        if (strcmp(g_claims[i].owner, owner) != 0) g_claims[kept++] = g_claims[i];
    }
    g_claimCount = kept;
    ReleaseSRWLockExclusive(&g_claimLock);
}

}  // namespace

bool ClaimRange(uintptr_t va, size_t size, const char* owner, const char** holder, bool* added)
{
    if (added) *added = false;
    AcquireSRWLockExclusive(&g_claimLock);
    bool ok = true, held = false;
    for (int i = 0; i < g_claimCount; ++i) {
        const Claim& c = g_claims[i];
        const bool mine = strcmp(c.owner, owner) == 0;
        if (va < c.end && va + size > c.begin && !mine) {
            if (holder) *holder = c.owner;
            ok = false;
            break;
        }
        // Already held whole by the same owner (an SDK mod patching and unpatching the same bytes): no new entry.
        if (mine && c.begin <= va && va + size <= c.end) held = true;
    }
    if (ok && held) {
        // nothing to add
    } else if (ok && g_claimCount < kMaxClaims) {
        Claim& c = g_claims[g_claimCount++];
        c.begin = va;
        c.end = va + size;
        strncpy_s(c.owner, owner, _TRUNCATE);
        if (added) *added = true;
    } else if (ok) {
        if (holder) *holder = "(too many claims)";
        ok = false;
    }
    ReleaseSRWLockExclusive(&g_claimLock);
    return ok;
}

bool ClaimHook(const char* name, uintptr_t va)
{
    const char* holder = nullptr;
    // One owner for every hook (mod ids have no spaces, so a mod can never share it).
    if (ClaimRange(va, 16, "the proxy's own hooks", &holder)) return true;
    Log("%s @ %08X: a patch or hook of mod %s holds these bytes; this part of the proxy is not installed", name,
        static_cast<unsigned>(va), holder ? holder : "?");
    return false;
}

void ReleaseRange(uintptr_t va, size_t size, const char* owner)
{
    AcquireSRWLockExclusive(&g_claimLock);
    int kept = 0;
    for (int i = 0; i < g_claimCount; ++i) {
        const Claim& c = g_claims[i];
        if (!(c.begin == va && c.end == va + size && strcmp(c.owner, owner) == 0)) g_claims[kept++] = c;
    }
    g_claimCount = kept;
    ReleaseSRWLockExclusive(&g_claimLock);
}

bool ClaimHeldBy(uintptr_t va, const char* owner)
{
    AcquireSRWLockShared(&g_claimLock);
    bool held = false;
    for (int i = 0; i < g_claimCount && !held; ++i) {
        const Claim& c = g_claims[i];
        held = c.begin <= va && va < c.end && strcmp(c.owner, owner) == 0;
    }
    ReleaseSRWLockShared(&g_claimLock);
    return held;
}

bool PatchesAllowed()
{
    return g_patchesAllowed;
}

bool ModRangeAllowed(uintptr_t va, size_t size, bool codeOnly, char* why, size_t whySize)
{
    why[0] = '\0';
    if (!size || size > static_cast<size_t>(kMaxPatchBytes)) {
        sprintf_s(why, whySize, "%u bytes is not 1 to %d", static_cast<unsigned>(size), kMaxPatchBytes);
    } else if (!InExeSections(va, size, codeOnly)) {
        sprintf_s(why, whySize, "0x%08X-0x%08X is not in deadspace3.exe's %s", static_cast<unsigned>(va),
                  static_cast<unsigned>(va + size - 1), codeOnly ? "code (.text)" : ".text, .rdata or .data");
    } else if (const char* what = RefusedRange(va, size)) {
        sprintf_s(why, whySize, "0x%08X is in the area of %s, which mods may never change", static_cast<unsigned>(va),
                  what);
    } else if (NearProtectedText(va, size)) {
        sprintf_s(why, whySize, "0x%08X is next to protected text (accounts, store or paid content), which mods may "
                  "never change", static_cast<unsigned>(va));
    }
    return why[0] == '\0';
}

bool ReadGameBytes(uintptr_t va, void* out, size_t size)
{
    return ReadBytes(va, out, size);
}

bool WriteGameBytes(uintptr_t va, const uint8_t* bytes, size_t size)
{
    return WriteBytes(va, bytes, size);
}

void AllowPatches(bool decrypted)
{
    g_patchesAllowed = decrypted;
    if (!decrypted) Log("patch mods: the game's code is not decrypted; no patch will be written this run");
}

bool ParseModPatches(int modIndex, const wchar_t* manifest)
{
    const Mod& m = ModAt(modIndex);
    static wchar_t names[8192];
    DWORD len = GetPrivateProfileSectionNamesW(names, 8192, manifest);
    const int first = g_patchCount;
    for (const wchar_t* sec = names; len && *sec; sec += wcslen(sec) + 1) {
        if (_wcsnicmp(sec, L"patch", 5) != 0) continue;
        if (g_patchCount >= kMaxPatches) {
            Log("mod %s: more than %d patches over all mods; skipped mod", m.id, kMaxPatches);
            g_patchCount = first;
            return false;
        }
        ModPatch& p = g_patches[g_patchCount];
        if (!ParsePatch(modIndex, manifest, sec, p)) {
            g_patchCount = first;
            return false;
        }
        // Two patches of one mod must not write the same bytes.
        for (int i = first; i < g_patchCount; ++i) {
            const ModPatch& q = g_patches[i];
            if (p.va < q.va + q.expectLen && p.va + p.expectLen > q.va) {
                Log("mod %s: [%ls] overlaps [%s]; skipped mod", m.id, sec, q.name);
                g_patchCount = first;
                return false;
            }
        }
        ++g_patchCount;
    }
    // A setting a patch takes its bytes from changes them live if every such patch can be rewritten atomically.
    for (int i = first; i < g_patchCount; ++i) {
        const ModPatch& p = g_patches[i];
        if (p.setting < 0) continue;
        ModSetting& s = ModSettingAt(p.setting);
        if (!s.patchBound) s.patchLive = true;
        s.patchBound = true;
        s.patchLive = s.patchLive && LiveWritable(p);
    }
    return true;
}

void DropModPatches(int modIndex)
{
    while (g_patchCount > 0 && g_patches[g_patchCount - 1].mod == modIndex) --g_patchCount;
}

int ModPatchCount(int modIndex)
{
    int n = 0;
    for (int i = 0; i < g_patchCount; ++i) n += g_patches[i].mod == modIndex;
    return n;
}

bool ApplyModPatches(int modIndex)
{
    const Mod& m = ModAt(modIndex);
    if (!g_patchesAllowed) {
        Log("mod %s: the game's code is not decrypted yet; patches not applied, nothing was written", m.id);
        return false;
    }
    // Check all first: the game's bytes, and that nobody else holds them.
    bool ok = true;
    for (int i = 0; i < g_patchCount; ++i) {
        const ModPatch& p = g_patches[i];
        if (p.mod != modIndex) continue;
        uint8_t now[kMaxPatchBytes];
        if (NearProtectedText(p.va, p.expectLen)) {
            Log("mod %s: [%s] 0x%08X is next to protected text (accounts, store or paid content), which patch mods "
                "may never change", m.id, p.name, static_cast<unsigned>(p.va));
            ok = false;
        } else if (!ReadBytes(p.va, now, p.expectLen)) {
            Log("mod %s: [%s] 0x%08X cannot be read", m.id, p.name, static_cast<unsigned>(p.va));
            ok = false;
        } else if (memcmp(now, p.expect, p.expectLen) != 0) {
            char got[3 * 16 + 4] = {};
            for (int b = 0, at = 0; b < std::min(p.expectLen, 16); ++b, at += b > 1 ? 3 : 2)
                sprintf_s(got + at, sizeof(got) - at, b ? " %02X" : "%02X", now[b]);
            Log("mod %s: [%s] the game's bytes at 0x%08X are not the expected ones (they are %s%s): a different "
                "game build, or another mod changed them", m.id, p.name, static_cast<unsigned>(p.va), got,
                p.expectLen > 16 ? "..." : "");
            ok = false;
        }
    }
    if (!ok) {
        Log("mod %s: patches not applied; nothing was written", m.id);
        return false;
    }
    for (int i = 0; i < g_patchCount; ++i) {
        ModPatch& p = g_patches[i];
        if (p.mod != modIndex) continue;
        const char* holder = nullptr;
        if (!ClaimRange(p.va, p.expectLen, m.id, &holder)) {
            Log("mod %s: [%s] 0x%08X is already taken by %s; patches not applied", m.id, p.name,
                static_cast<unsigned>(p.va), holder ? holder : "?");
            ok = false;
        }
    }
    if (!ok) {
        ReleaseClaims(m.id);  // nothing of this mod is written, so its other ranges are free for others
        return false;
    }
    for (int i = 0; i < g_patchCount; ++i) {
        ModPatch& p = g_patches[i];
        if (p.mod != modIndex) continue;
        uint8_t bytes[kMaxPatchBytes];
        PatchBytes(p, bytes);
        if (!WriteBytes(p.va + p.offset, bytes, p.writeLen)) {
            Log("mod %s: [%s] writing 0x%08X FAILED", m.id, p.name, static_cast<unsigned>(p.va + p.offset));
            continue;
        }
        p.applied = true;
        const unsigned at = static_cast<unsigned>(p.va + p.offset);
        if (p.kind == PatchKind::Toggle && ModSettingAt(p.setting).value == 0)
            Log("mod %s: [%s] %d byte(s) at 0x%08X-0x%08X checked; switched off, so the game's own bytes stay", m.id,
                p.name, p.writeLen, at, at + p.writeLen - 1);
        else
            Log("mod %s: [%s] %d byte(s) at 0x%08X-0x%08X written", m.id, p.name, p.writeLen, at, at + p.writeLen - 1);
    }
    return true;
}

void ReapplyPatchesFor(int settingIndex)
{
    for (int i = 0; i < g_patchCount; ++i) {
        ModPatch& p = g_patches[i];
        if (p.setting != settingIndex || !p.applied || !LiveWritable(p)) continue;
        uint8_t bytes[kMaxPatchBytes];
        PatchBytes(p, bytes);
        bool ok = WriteBytes(p.va + p.offset, bytes, p.writeLen);
        const unsigned at = static_cast<unsigned>(p.va + p.offset);
        Log("mod %s: [%s] %d byte(s) at 0x%08X-0x%08X rewritten%s", ModAt(p.mod).id, p.name, p.writeLen, at,
            at + p.writeLen - 1, ok ? "" : " FAILED");
    }
}
