#pragma once

// Small read helpers and game calls shared by the loader's parts (the SDK's reads, the Mods panel, the overlay).
// Add-ons (sdk/seal_store, sdk/auto_pickup) keep their own copies of the few they need.

#include <cstddef>
#include <cstdint>

namespace game {

struct Guid {
    uint32_t d[4];
};

bool IsNullGuid(const Guid& g);
bool BytesMatch(uintptr_t va, const unsigned char* expected, size_t len);
bool SafeReadU32(uintptr_t addr, uint32_t* out);
bool SafeRead(uintptr_t addr, void* out, size_t len);

// MH_Initialize, tolerating a second call (the store hooks may have initialised MinHook already).
bool InitMinHook();
// Creates and enables one hook if the target's bytes match. Logs and returns false otherwise.
bool HookChecked(const char* name, uintptr_t va, const unsigned char* bytes, size_t len, void* detour, void** orig);

// The local player (FUN_00436170(0)), 0 on the main menu. Its byte check runs once.
uint8_t* LocalPlayer();
// Runs the byte checks of the game functions LocalPlayer and ItemName call, now. The proxy does this before loading
// drop-in mods: a mod may hook one of them (its first bytes then change), and they still work through the hook.
void CheckGameReads();

// Player layout: resource block +0xEA4 when bit 13 of +0x594 is set, else +0xE3C; six
// resources at block +0x48 (tungsten, semiconductor, scrap metal, somatic gel, transducer, ration seals); the
// inventory at +0xF0C, entries at +0xF10 (count +0xF14), 0x28 bytes each: GUID, flags +0x10, count +0x14. The block
// is itself an inventory of the same kind: consumables such as ammo and med packs, entries at block +4
// (count +8), slot capacity +0x2C, slots used +0x30.
constexpr uintptr_t kPlayerFlagsOffset = 0x594;
constexpr uintptr_t kBlockInventoryEntriesOffset = 0x4;
constexpr uintptr_t kInventoryEntriesOffset = 0xF10;
constexpr uintptr_t kInventoryStride = 0x28;
constexpr uintptr_t kInventoryItemCount = 0x14;
constexpr int kResourceCount = 6;
uintptr_t ResourceBlock(const uint8_t* player);
extern const char* const kResourceNames[kResourceCount];

// The item's UI name as the game shows it ("" if unknown). Printable ASCII only; anything else becomes '?'.
bool ItemName(const Guid& g, char* buf, size_t size);

// The player's two item lists: 0 = the resource block's own inventory (ammo, med and stasis packs, some mission items
// such as "Probe Frame"), 1 = player +0xF0C (suits, parts, circuits, key items). Count is 0 if the list can't be
// read; an entry is false then.
int ItemListCount(const uint8_t* player, int list);
bool ItemListEntry(const uint8_t* player, int list, int index, Guid* guid, uint32_t* count);

// Whether the pause menu ("SP Pause", vtable 0x00D494B8) is in the game's list of open screens at 0x012DEEFC
// False if that vtable's slots do not look like this game build's (checked once).
bool PauseMenuOpen();

}  // namespace game
