#include "game.h"

#include "log.h"
#include "patches.h"

#include <windows.h>

#include <MinHook.h>

#include <cstring>

namespace game {

namespace {

// FUN_00436170: __cdecl Player* GetPlayer(int index).
constexpr uintptr_t kGetPlayerVa = 0x00436170;
constexpr unsigned char kGetPlayerBytes[] = {0x55, 0x8B, 0xEC, 0xA1, 0x78, 0x3B, 0x1D, 0x01,
                                             0x57, 0x33, 0xFF, 0x85, 0xC0, 0x74, 0x0B, 0x8B};
using GetPlayerFn = uint8_t*(__cdecl*)(int index);

// FUN_005d29f0: __cdecl uint32 ItemNameId(const Guid*), which also calls the lookup FUN_0043fe80.
// FUN_00b6a250: __thiscall const char* LookupText(uint32 id), this = *(0x011D1A00). Both read-only.
constexpr uintptr_t kItemNameVa = 0x005D29F0;
constexpr unsigned char kItemNameBytes[] = {0x55, 0x8B, 0xEC, 0x56, 0x57, 0x8B, 0x7D, 0x08,
                                            0x57, 0x33, 0xF6, 0xE8, 0x10, 0x7D, 0xF3, 0xFF};
constexpr uintptr_t kItemLookupVa = 0x0043FE80;
constexpr unsigned char kLookupBytes[] = {0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x08, 0x8B, 0x11,
                                          0x83, 0xEC, 0x24, 0x33, 0xC0, 0x81, 0xFA, 0xBA};
constexpr uintptr_t kLookupTextVa = 0x00B6A250;
constexpr unsigned char kLookupTextBytes[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C, 0x8B, 0x51,
                                              0x34, 0x53, 0x8B, 0x5D, 0x08, 0x33, 0xC0, 0x56};
constexpr uintptr_t kStringTablesPtrVa = 0x011D1A00;
using ItemNameFn = uint32_t(__cdecl*)(const Guid* guid);
using LookupTextFn = const char*(__fastcall*)(void* tables, void* edx, uint32_t id);

// -1 = not checked yet, 0 = bytes differ, 1 = callable.
int g_playerOk = -1;
int g_namesOk = -1;

bool SafeCopyText(const char* src, char* dst, size_t size)
{
    __try {
        size_t i = 0;
        for (; i + 1 < size && src[i]; ++i) {
            unsigned char c = static_cast<unsigned char>(src[i]);
            dst[i] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
        }
        dst[i] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        dst[0] = '\0';
        return false;
    }
}

}  // namespace

const char* const kResourceNames[kResourceCount] = {"Tungsten",    "Semiconductor", "Scrap Metal",
                                                    "Somatic Gel", "Transducer",    "Ration Seals"};

bool IsNullGuid(const Guid& g)
{
    bool zero = !g.d[0] && !g.d[1] && !g.d[2] && !g.d[3];
    bool bad = g.d[0] == 0xBADBADBA && g.d[1] == 0xBEEFBEEF && g.d[2] == 0xEAC15A55 && g.d[3] == 0x91100911;
    return zero || bad;
}

bool BytesMatch(uintptr_t va, const unsigned char* expected, size_t len)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(va), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    return std::memcmp(reinterpret_cast<void*>(va), expected, len) == 0;
}

bool SafeRead(uintptr_t addr, void* out, size_t len)
{
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(addr), len);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeReadU32(uintptr_t addr, uint32_t* out)
{
    return SafeRead(addr, out, sizeof(*out));
}

bool InitMinHook()
{
    MH_STATUS st = MH_Initialize();
    if (st == MH_OK || st == MH_ERROR_ALREADY_INITIALIZED) return true;
    Log("MH_Initialize failed: %s", MH_StatusToString(st));
    return false;
}

bool HookChecked(const char* name, uintptr_t va, const unsigned char* bytes, size_t len, void* detour, void** orig)
{
    if (!ClaimHook(name, va)) return false;
    if (bytes && !BytesMatch(va, bytes, len)) {
        Log("%s @ %08X does not match expected bytes (different game build?)", name, static_cast<unsigned>(va));
        return false;
    }
    MH_STATUS st = MH_CreateHook(reinterpret_cast<void*>(va), detour, orig);
    if (st != MH_OK) {
        Log("MH_CreateHook(%s) failed: %s", name, MH_StatusToString(st));
        return false;
    }
    st = MH_EnableHook(reinterpret_cast<void*>(va));
    if (st != MH_OK) {
        Log("MH_EnableHook(%s) failed: %s", name, MH_StatusToString(st));
        MH_RemoveHook(reinterpret_cast<void*>(va));
        return false;
    }
    return true;
}

void CheckGameReads()
{
    if (g_playerOk < 0) g_playerOk = BytesMatch(kGetPlayerVa, kGetPlayerBytes, sizeof(kGetPlayerBytes)) ? 1 : 0;
    if (g_namesOk < 0) {
        g_namesOk = BytesMatch(kItemNameVa, kItemNameBytes, sizeof(kItemNameBytes)) &&
                    BytesMatch(kItemLookupVa, kLookupBytes, sizeof(kLookupBytes)) &&
                    BytesMatch(kLookupTextVa, kLookupTextBytes, sizeof(kLookupTextBytes));
        if (!g_namesOk) Log("item names unavailable: name functions do not match");
    }
    if (!g_playerOk) Log("player reads unavailable: the player getter does not match");
}

uint8_t* LocalPlayer()
{
    if (g_playerOk < 0) g_playerOk = BytesMatch(kGetPlayerVa, kGetPlayerBytes, sizeof(kGetPlayerBytes)) ? 1 : 0;
    if (!g_playerOk) return nullptr;
    __try {
        return reinterpret_cast<GetPlayerFn>(kGetPlayerVa)(0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

uintptr_t ResourceBlock(const uint8_t* player)
{
    uint32_t flags = 0;
    SafeReadU32(reinterpret_cast<uintptr_t>(player) + kPlayerFlagsOffset, &flags);
    return reinterpret_cast<uintptr_t>(player) + ((flags >> 13 & 1) ? 0xEA4 : 0xE3C);
}

bool ItemName(const Guid& g, char* buf, size_t size)
{
    buf[0] = '\0';
    if (g_namesOk < 0) {
        g_namesOk = BytesMatch(kItemNameVa, kItemNameBytes, sizeof(kItemNameBytes)) &&
                    BytesMatch(kItemLookupVa, kLookupBytes, sizeof(kLookupBytes)) &&
                    BytesMatch(kLookupTextVa, kLookupTextBytes, sizeof(kLookupTextBytes));
        if (!g_namesOk) Log("item names unavailable: name functions do not match");
    }
    uint32_t tables = 0;
    if (!g_namesOk || IsNullGuid(g) || !SafeReadU32(kStringTablesPtrVa, &tables) || !tables) return false;
    const char* text = nullptr;
    __try {
        uint32_t id = reinterpret_cast<ItemNameFn>(kItemNameVa)(&g);
        if (id) text = reinterpret_cast<LookupTextFn>(kLookupTextVa)(reinterpret_cast<void*>(tables), nullptr, id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return text && SafeCopyText(text, buf, size) && buf[0];
}

namespace {

constexpr int kMaxListEntries = 256;

// The list's entries pointer and count, or false if they don't look sane.
bool ItemList(const uint8_t* player, int list, uint32_t* entries, uint32_t* count)
{
    if (!player || list < 0 || list > 1) return false;
    const uintptr_t at = list == 0 ? ResourceBlock(player) + kBlockInventoryEntriesOffset
                                   : reinterpret_cast<uintptr_t>(player) + kInventoryEntriesOffset;
    return SafeReadU32(at, entries) && SafeReadU32(at + 4, count) && *count <= kMaxListEntries &&
           (*entries || !*count);
}

// The pause screen (modmenu.cpp has its own copy of this check, for the panel).
constexpr uintptr_t kScreenListVa = 0x012DEEFC;
constexpr int kScreenSlots = 15;
constexpr uintptr_t kPauseVtable = 0x00D494B8;
constexpr uintptr_t kPauseActivate = 0x0053CCC0;  // its slot 31
constexpr uintptr_t kPauseRowLabel = 0x0050FDD0;  // its slot 46
int g_pauseOk = -1;

}  // namespace

int ItemListCount(const uint8_t* player, int list)
{
    uint32_t entries = 0, count = 0;
    return ItemList(player, list, &entries, &count) ? static_cast<int>(count) : 0;
}

bool ItemListEntry(const uint8_t* player, int list, int index, Guid* guid, uint32_t* count)
{
    uint32_t entries = 0, n = 0;
    if (!ItemList(player, list, &entries, &n) || index < 0 || static_cast<uint32_t>(index) >= n) return false;
    const uintptr_t e = entries + static_cast<uint32_t>(index) * kInventoryStride;
    return SafeRead(e, guid, sizeof(*guid)) && SafeReadU32(e + kInventoryItemCount, count);
}

bool PauseMenuOpen()
{
    if (g_pauseOk < 0) {
        uint32_t activate = 0, label = 0;
        g_pauseOk = SafeReadU32(kPauseVtable + 31 * 4, &activate) && activate == kPauseActivate &&
                    SafeReadU32(kPauseVtable + 46 * 4, &label) && label == kPauseRowLabel;
        if (!g_pauseOk) Log("pause menu check unavailable: the pause screen's vtable differs");
    }
    if (!g_pauseOk) return false;
    uint32_t slots[kScreenSlots] = {};
    if (!SafeRead(kScreenListVa, slots, sizeof(slots))) return false;
    for (uint32_t s : slots) {
        uint32_t vt = 0;
        if (s && SafeReadU32(s, &vt) && vt == kPauseVtable) return true;
    }
    return false;
}

}  // namespace game
