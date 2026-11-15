// Auto Pickup: items are collected as if every weapon had an Ammo Sweeper attached, all around you, with a small
// RIG-style list of what you just picked up in the bottom-right corner.
//
// This is the sample to read for a mod that CHANGES how the game plays. It shows:
//   - five hooks on game functions, made with host->hook (the loader checks the bytes and logs each one);
//   - calling the game's own functions and reading its objects, with every read guarded;
//   - settings that apply at once (DS3ModSettingsChanged), so a switch in the Mods panel works without a restart;
//   - drawing with a chosen font and fading boxes (DS3Frame.setFont / gradient).
// Read resource_counter first: it explains the basics this file takes for granted.
//
// How the game picks things up. The player's item-pickup state machine goes from idle to "attemptAutoPickup" when the
// equipped weapon has an Ammo Sweeper in one of its two attachment slots. It asks AttachmentCheck (0x0043D7B0) with
// each slot's sweeper; with none, both calls get 0 and nothing is collected.
//   - AttachmentCheck: for the local player, a 0 sweeper is replaced by one of our own. The game then runs its own
//     sweeper check: a nearby item (the five candidates the pickup prompt uses) whose categories overlap the
//     sweeper's, that the inventory has room for while keeping the sweeper's number of slots open, inside the pickup
//     range. Before the call, the candidates we must leave (weapon parts, quest items, packs at their carry limit) get
//     the item flag the check already skips, cleared again right after.
//   - PickupAction (0x0046F250): entering "attemptAutoPickup" (action 0x2A) reads the sweeper from the weapon slot
//     again to copy its slots-to-keep-open; with our sweeper the slot is empty, so that is done here instead. Every
//     action of the local player also feeds the list: inventory and resource increases within a few seconds of a
//     pickup are shown.
//   - All around: the five candidates come from a query that only accepts items in a cone in front of the player,
//     refreshed a few times a second. ConeTest (0x00577C60) is made to pass an item our sweeper would take in any
//     direction, RangeTest (0x00577E80) to pass it within the Reach setting, and the idle action (0x21) runs with the
//     refresh timer at its end, so the query runs every frame. Distance, line of sight and ordering stay the game's.
//   - RoomFor (0x007051F0): the sweeper check asks it for every candidate, resources too, so with only the kept-open
//     slots left it stopped collecting resources although they take no slot. Inside our check a resource is given room.
// Only the local player's pickup component is ever changed. A real Ammo Sweeper on the weapon works as before.
//
// Not hot-swappable (no DS3ModShutdown): between AttachmentCheck handing out our sweeper and action 0x2A the game
// expects this mod's hooks to stay; unloading in between would leave the game reading a sweeper that is not there.

#include "../ds3mod.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

const DS3ModHost* g_host = nullptr;

using Guid = DS3Guid;

// ---- Small helpers ----

void Logf(const char* format, ...)
{
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf_s(text, sizeof(text), _TRUNCATE, format, args);
    va_end(args);
    g_host->log(g_host, text);
}

// Reads game memory without crashing if the address is bad (an object that went away, another game version).
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

// Whether the game has these bytes at this address: the check before calling a game function by its address.
bool BytesMatch(uintptr_t va, const unsigned char* expected, size_t len)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(va), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    return std::memcmp(reinterpret_cast<void*>(va), expected, len) == 0;
}

bool IsNullGuid(const Guid& g)
{
    bool zero = !g.d[0] && !g.d[1] && !g.d[2] && !g.d[3];
    bool bad = g.d[0] == 0xBADBADBA && g.d[1] == 0xBEEFBEEF && g.d[2] == 0xEAC15A55 && g.d[3] == 0x91100911;
    return zero || bad;
}

// Player layout (sdk\GAME_NOTES.md): the resource block is at player +0xEA4 when bit 13 of +0x594 is set, else
// +0xE3C. It holds the six resources at +0x48 and is itself an inventory (ammo, med and stasis packs): entries at
// +4, count at +8. The second inventory (key items, parts, circuits) has its entries at player +0xF10, count +0xF14.
// An entry is 0x28 bytes: the item's GUID, then its count at +0x14.
constexpr uintptr_t kPlayerFlags = 0x594;
constexpr uintptr_t kBlockInventoryEntries = 0x4;
constexpr uintptr_t kPlayerInventoryEntries = 0xF10;
constexpr uintptr_t kInventoryStride = 0x28;
constexpr uintptr_t kInventoryItemCount = 0x14;
constexpr uintptr_t kBlockResources = 0x48;
constexpr uintptr_t kBlockMinOpenSlots = 0x64;

uintptr_t ResourceBlock(const uint8_t* player)
{
    uint32_t flags = 0;
    SafeReadU32(reinterpret_cast<uintptr_t>(player) + kPlayerFlags, &flags);
    return reinterpret_cast<uintptr_t>(player) + ((flags >> 13 & 1) ? 0xEA4 : 0xE3C);
}

// ---- Settings ----
// mod.ini lists these for the Mods panel; here they are read from auto_pickup.ini. The section and key names are the
// ones the loader's test builds used in ds3_mods.ini, so those sections can be copied over as they are.

struct Settings {
    bool collect = true;             // [auto_pickup] enabled
    unsigned categories = 0xFFFFFFFF;  // which item categories: 1 med packs, 2 ammo, 8 resources, 0x20 quest items...
    bool allAround = true;
    int radius = 4;                  // metres, 0 = the game's reach
    bool skipWeaponParts = true;
    bool skipQuestItems = true;
    unsigned maxMedPacks = 5;        // 0 = no limit
    unsigned maxStasisPacks = 5;
    unsigned minOpenSlots = 5;
    bool list = true;                // [pickup_log] enabled
    int listLines = 6;
    int listSeconds = 5;
    int listTextSize = 100;          // percent
    bool logCandidates = false;      // [debug] log_pickup_candidates, by hand only
};

int ReadInt(const wchar_t* section, const wchar_t* key, int def, int lo, int hi)
{
    int v = static_cast<int>(GetPrivateProfileIntW(section, key, def, g_host->iniPath));
    return std::clamp(v, lo, hi);
}

Settings ReadSettings()
{
    Settings s;
    s.collect = ReadInt(L"auto_pickup", L"enabled", 1, 0, 1) != 0;
    wchar_t text[32];
    GetPrivateProfileStringW(L"auto_pickup", L"categories", L"0xFFFFFFFF", text, 32, g_host->iniPath);
    s.categories = static_cast<unsigned>(wcstoul(text, nullptr, 0));  // decimal or 0x hex
    s.allAround = ReadInt(L"auto_pickup", L"all_around", 1, 0, 1) != 0;
    s.radius = ReadInt(L"auto_pickup", L"radius", 4, 0, 10);
    s.skipWeaponParts = ReadInt(L"auto_pickup", L"skip_weapon_parts", 1, 0, 1) != 0;
    s.skipQuestItems = ReadInt(L"auto_pickup", L"skip_quest_items", 1, 0, 1) != 0;
    s.maxMedPacks = static_cast<unsigned>(ReadInt(L"auto_pickup", L"max_med_packs", 5, 0, 99));
    s.maxStasisPacks = static_cast<unsigned>(ReadInt(L"auto_pickup", L"max_stasis_packs", 5, 0, 99));
    s.minOpenSlots = static_cast<unsigned>(ReadInt(L"auto_pickup", L"min_open_slots", 5, 0, 25));
    s.list = ReadInt(L"pickup_log", L"enabled", 1, 0, 1) != 0;
    s.listLines = ReadInt(L"pickup_log", L"lines", 6, 1, 12);
    s.listSeconds = ReadInt(L"pickup_log", L"seconds", 5, 1, 60);
    s.listTextSize = ReadInt(L"pickup_log", L"text_size", 100, 50, 300);
    s.logCandidates = ReadInt(L"debug", L"log_pickup_candidates", 0, 0, 1) != 0;
    return s;
}

// ---- The pickup list: lines added by the hooks, drawn by the frame callback ----

constexpr int kMaxLines = 12;
constexpr ULONGLONG kFadeMs = 600;

struct Line {
    char text[64];
    unsigned amount;
    ULONGLONG bornAt;   // first shown: the line projects in from here
    ULONGLONG shownAt;  // last added to: it flickers and stays from here
};

// The hooks run where the game runs its pickup code, the frame callback on the render thread: a lock keeps the two
// from touching the lines at the same time. (An SRWLOCK needs no setup and no cleanup.)
SRWLOCK g_linesLock = SRWLOCK_INIT;
Line g_lines[kMaxLines] = {};  // oldest first
int g_lineCount = 0;
volatile bool g_listOn = false;
volatile int g_listLines = 6;
volatile ULONGLONG g_listLifetimeMs = 5000;
volatile int g_listTextSize = 100;

// Adds "+amount text". A line with the same text still on screen is topped up and moved to the bottom.
void AddLine(const char* text, unsigned amount)
{
    if (!g_listOn || !amount) return;
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG lifetime = g_listLifetimeMs;
    AcquireSRWLockExclusive(&g_linesLock);
    int found = -1;
    for (int i = 0; i < g_lineCount; ++i) {
        if (now - g_lines[i].shownAt < lifetime && std::strcmp(g_lines[i].text, text) == 0) found = i;
    }
    Line line{};
    if (found >= 0) {
        line = g_lines[found];
        std::memmove(g_lines + found, g_lines + found + 1, (g_lineCount - found - 1) * sizeof(Line));
        --g_lineCount;
        line.amount += amount;
    } else {
        strncpy_s(line.text, text, _TRUNCATE);
        line.amount = amount;
        line.bornAt = now;
    }
    line.shownAt = now;
    if (g_lineCount == kMaxLines) {
        std::memmove(g_lines, g_lines + 1, (kMaxLines - 1) * sizeof(Line));
        --g_lineCount;
    }
    g_lines[g_lineCount++] = line;
    ReleaseSRWLockExclusive(&g_linesLock);
}

// Copies the lines still on screen (oldest first) with their opacity; drops expired ones.
int SnapshotLines(Line* out, float* alpha)
{
    if (!g_listOn) return 0;
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG lifetime = g_listLifetimeMs;
    AcquireSRWLockExclusive(&g_linesLock);
    int kept = 0;
    for (int i = 0; i < g_lineCount; ++i) {
        if (now - g_lines[i].shownAt < lifetime) g_lines[kept++] = g_lines[i];
    }
    g_lineCount = kept;
    const int first = std::max(0, kept - g_listLines);
    int n = 0;
    for (int i = first; i < kept; ++i, ++n) {
        out[n] = g_lines[i];
        const ULONGLONG left = lifetime - (now - g_lines[i].shownAt);
        alpha[n] = left < kFadeMs ? static_cast<float>(left) / kFadeMs : 1.0f;
    }
    ReleaseSRWLockExclusive(&g_linesLock);
    return n;
}

// RIG hologram palette: the blue-white light of the game's projected inventory and readouts (0xRRGGBB).
constexpr unsigned kHoloCore = 0xE4F8FF;   // text
constexpr unsigned kHoloLight = 0x7DDCFF;  // lines, glow, the amount
constexpr unsigned kHoloDeep = 0x03141F;   // the projection's tint behind the text
constexpr ULONGLONG kProjectMs = 240;      // a new line slides in from the emitter edge with a scan bar
constexpr ULONGLONG kFlickerMs = 300;      // a new or added-to line flickers like a hologram powering up
// A narrow, squared face like the game's hologram text. It comes with Windows 10 and later; without it the loader
// draws its usual face.
constexpr const char* kListFace = "Bahnschrift SemiCondensed";

// A colour (0xRRGGBB) with an opacity from 0 to 1, as the 0xAARRGGBB the drawing calls take.
unsigned WithAlpha(unsigned rgb, float a)
{
    return static_cast<unsigned>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f) << 24 | (rgb & 0x00FFFFFF);
}

float EaseOut(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

// Text drawn letter by letter, with `track` extra pixels after each: text() returns where it ended, so the next
// letter starts there. Returns the pen position after the last one.
float Spaced(const DS3Frame* f, float x, float y, const char* s, unsigned color, float track)
{
    char one[2] = {0, 0};
    for (; *s; ++s) {
        one[0] = *s;
        x = f->text(f, x, y, one, color) + track;
    }
    return x;
}

float SpacedWidth(const DS3Frame* f, const char* s, float track)
{
    return std::round(f->textWidth(f, s) + track * static_cast<float>(std::strlen(s)));
}

// Text with a soft halo, as projected light.
void HoloText(const DS3Frame* f, float x, float y, const char* s, unsigned core, float a, float track)
{
    const unsigned halo = WithAlpha(kHoloLight, 0.16f * a);
    Spaced(f, x - 1, y, s, halo, track);
    Spaced(f, x + 1, y, s, halo, track);
    Spaced(f, x, y - 1, s, halo, track);
    Spaced(f, x, y + 1, s, halo, track);
    Spaced(f, x, y, s, WithAlpha(core, a), track);
}

// A box that fades from `left` to `right`. gradient came with DS3 Mod Loader 1.0.0; a test build before it gets a
// flat box in the right-hand colour at half strength.
void Fade(const DS3Frame* f, float x, float y, float w, float h, unsigned left, unsigned right)
{
    if (DS3FRAME_HAS(f, gradient))
        f->gradient(f, x, y, w, h, left, right, 0);
    else
        f->rect(f, x, y, w, h, (right & 0x00FFFFFF) | ((right >> 25) << 24));
}

// One readout per line, newest at the bottom, anchored to an emitter edge on the right: the name in capitals, the
// amount in a column against the edge, a tint fading out to the left, bracket corners and faint scanlines.
void __cdecl OnFrame(const DS3Frame* f, void*)
{
    Line lines[kMaxLines];
    float alpha[kMaxLines];
    // The lines keep ageing while the Mods panel is open (it has the input then), but are not drawn under it.
    const int n = SnapshotLines(lines, alpha);
    if (!n || g_host->inputBlocked()) return;

    const float px = std::max(12.0f, std::round(f->height / 60.0f * static_cast<float>(g_listTextSize) / 100.0f));
    const float lineH = DS3FRAME_HAS(f, setFont) ? f->setFont(f, kListFace, px) : f->setTextSize(f, px);
    if (!(lineH > 0)) return;  // no font to draw with
    const float padX = std::round(lineH * 0.55f), padY = std::round(lineH * 0.16f), gap = std::round(lineH * 0.22f);
    const float track = std::max(1.0f, std::round(lineH * 0.05f));
    const float bandH = lineH + 2 * padY;
    const float tick = std::max(1.0f, std::round(f->height / 1080.0f));  // hairline width
    const float right = std::round(f->width - f->height * 0.03f);
    const ULONGLONG now = GetTickCount64();

    char names[kMaxLines][64];
    char amounts[kMaxLines][16];
    float amountW = 0;
    for (int i = 0; i < n; ++i) {
        size_t k = 0;
        for (const char* s = lines[i].text; *s && k + 1 < sizeof(names[i]); ++s)
            names[i][k++] = (*s >= 'a' && *s <= 'z') ? static_cast<char>(*s - 'a' + 'A') : *s;
        names[i][k] = '\0';
        sprintf_s(amounts[i], "+%u", lines[i].amount);
        amountW = std::max(amountW, SpacedWidth(f, amounts[i], track));
    }

    float y = std::round(f->height * 0.86f) - bandH;
    for (int i = n - 1; i >= 0; --i) {
        const ULONGLONG born = now - std::min(now, lines[i].bornAt);
        const ULONGLONG touched = now - std::min(now, lines[i].shownAt);
        const float project = EaseOut(static_cast<float>(born) / kProjectMs);
        float a = alpha[i] * (0.25f + 0.75f * project);
        if (touched < kFlickerMs && (touched / 50) % 2 == 1) a *= 0.55f;

        const float nameW = SpacedWidth(f, names[i], track);
        const float bandW = std::round(padX + nameW + padX * 1.4f + amountW + padX);
        const float slide = std::round((1.0f - project) * bandW * 0.35f);
        const float r = right + slide, x = r - bandW;

        // Tint: clear at the left, deepest at the emitter.
        Fade(f, x, y, bandW, bandH, WithAlpha(kHoloDeep, 0.2f * a), WithAlpha(kHoloDeep, 0.62f * a));
        Fade(f, x, y, bandW, bandH, WithAlpha(kHoloLight, 0.0f), WithAlpha(kHoloLight, 0.10f * a));
        // Emitter edge and its bracket arms along the top and bottom.
        f->rect(f, r - 2 * tick, y, 2 * tick, bandH, WithAlpha(kHoloLight, 0.95f * a));
        const float arm = std::round(bandW * 0.4f);
        Fade(f, r - arm, y, arm, tick, WithAlpha(kHoloLight, 0.0f), WithAlpha(kHoloLight, 0.8f * a));
        Fade(f, r - arm, y + bandH - tick, arm, tick, WithAlpha(kHoloLight, 0.0f), WithAlpha(kHoloLight, 0.8f * a));
        // Left bracket corners.
        const float corner = std::round(padX * 0.5f);
        f->rect(f, x, y, tick, bandH, WithAlpha(kHoloLight, 0.45f * a));
        f->rect(f, x, y, corner, tick, WithAlpha(kHoloLight, 0.45f * a));
        f->rect(f, x, y + bandH - tick, corner, tick, WithAlpha(kHoloLight, 0.45f * a));
        // Divider before the amount column.
        const float amountX = r - padX - amountW;
        f->rect(f, amountX - std::round(padX * 0.7f), y + padY, tick, lineH, WithAlpha(kHoloLight, 0.35f * a));

        HoloText(f, x + padX, y + padY, names[i], kHoloCore, 0.95f * a, track);
        HoloText(f, r - padX - SpacedWidth(f, amounts[i], track), y + padY, amounts[i], kHoloLight, a, track);

        // Scanlines over the whole readout.
        const float pitch = std::max(3.0f, std::round(lineH / 8.0f));
        for (float sy = y + tick; sy < y + bandH - tick; sy += pitch)
            f->rect(f, x + tick, sy, bandW - 3 * tick, tick, WithAlpha(kHoloDeep, 0.22f * a));
        // Scan bar sweeping from the emitter to the left edge as the line projects in.
        if (project < 1.0f) {
            const float sx = std::round(r - project * bandW);
            f->rect(f, sx, y - tick, 2 * tick, bandH + 2 * tick, WithAlpha(kHoloCore, (1.0f - project) * alpha[i]));
        }
        y -= bandH + gap;
    }
}

// ---- The game's functions and layouts ----
// Addresses are as Ghidra shows them for deadspace3.exe. The bytes are each function's first 16 in an unchanged game:
// host->hook refuses to hook anything else, and BytesMatch checks them before a function is called directly.

// __thiscall bool AttachmentCheck(Attachment* sweeper), this = the pickup component, ret 4.
constexpr unsigned kAttachmentCheckVa = 0x0043D7B0;
constexpr unsigned char kAttachmentCheckBytes[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10, 0x53, 0x56,
                                                   0x57, 0x8B, 0xF9, 0x8B, 0x4D, 0x08, 0x32, 0xC0};
// __thiscall bool PickupAction(?, ?, ?, int action, ?), this = the pickup component, ret 14h.
constexpr unsigned kPickupActionVa = 0x0046F250;
constexpr unsigned char kPickupActionBytes[] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4,
                                                0xF0, 0x83, 0xC4, 0x04, 0x55, 0x8B, 0x6B, 0x04};
// The state machine's actions: pickupItem enter 0x1C / exit 0x2B, remotePickup (co-op) enter 0x25, attemptAutoPickup
// enter 0x2A, and the idle update 0x21, which refreshes the candidates once the timer at +0x13C runs out.
constexpr int kActionEnterPickupItem = 0x1C;
constexpr int kActionEnterRemotePickup = 0x25;
constexpr int kActionExitPickupItem = 0x2B;
constexpr int kActionEnterAutoPickup = 0x2A;
constexpr int kActionUpdateIdleScan = 0x21;
// __thiscall bool ConeTest(QuerySystem*, QueryNode* node, Query* query, uint32_t* flags, void*), ret 10h. Node +8 is
// the item's handle (object = handle - 0x10); query +8 is the querier (the player).
// __thiscall bool RangeTest(QuerySystem*, QueryNode* node, Query* query, uint32_t* flags), ret 0Ch. Same first bytes.
constexpr unsigned kConeTestVa = 0x00577C60;
constexpr unsigned kRangeTestVa = 0x00577E80;
constexpr unsigned char kConeTestBytes[] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4,
                                            0xF0, 0x83, 0xC4, 0x04, 0x55, 0x8B, 0x6B, 0x04};
// __thiscall uint RoomFor(Inventory* block, const Guid* guid), ret 4: how many of the item the inventory can take.
constexpr unsigned kRoomForVa = 0x007051F0;
constexpr unsigned char kRoomForBytes[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x24, 0x53, 0x56,
                                           0x57, 0x8B, 0x7D, 0x08, 0x33, 0xDB, 0x53, 0x57};

// The pickup component.
constexpr uintptr_t kComponentPlayer = 0x60;
constexpr uintptr_t kComponentActiveItem = 0x78;  // handle of the item being picked up
constexpr uintptr_t kComponentQuery = 0x104;
constexpr uintptr_t kComponentCandidates = 0x110;  // five handles, 8 bytes apart
constexpr int kCandidateCount = 5;
constexpr uintptr_t kComponentScanTimer = 0x13C;  // float seconds since the last refresh
constexpr uintptr_t kComponentRange = 0x140;      // float, the sweeper's reach
constexpr uintptr_t kPosition = 0x100;            // float x, y, z on the player and on items
constexpr uintptr_t kNodeHandle = 0x8;
// A pickup item: its class, its data (+0xC) with the GUID and categories, and its flags.
constexpr uint32_t kPickupItemClass = 0xE7CF170F;
constexpr uintptr_t kItemDataGuid = 0x108;
constexpr uintptr_t kItemDataCategories = 0x178;
constexpr uintptr_t kItemFlags = 0x264;
constexpr uint32_t kItemSkipBit = 0x4000;  // bit 14: the sweeper check passes over the item

// The only two fields the game reads from a sweeper: attachment +0xC -> data, data +0x3A0 categories, +0x3A4 slots to
// keep open. This is the sweeper handed to the game in place of the missing attachment.
struct SweeperData {
    uint8_t unused[0x3A0];
    uint32_t categories;
    uint32_t minOpenSlots;
};
struct Sweeper {
    uint8_t unused[0xC];
    SweeperData* data;
};
SweeperData g_sweeperData = {};
Sweeper g_sweeper = {{}, &g_sweeperData};

// A __thiscall game function is hooked with a __fastcall detour: ECX is `this`, EDX is unused, the rest are the
// stack arguments.
using AttachmentCheckFn = uint32_t(__fastcall*)(void* component, void* edx, void* sweeper);
using PickupActionFn = char(__fastcall*)(void* component, void* edx, uint32_t a, uint32_t b, uint32_t c, int action,
                                         uint32_t e);
using ConeTestFn = uint32_t(__fastcall*)(void* system, void* edx, void* node, uintptr_t query, uint32_t* flags,
                                         void* extra);
using RangeTestFn = uint32_t(__fastcall*)(void* system, void* edx, void* node, uintptr_t query, uint32_t* flags);
using RoomForFn = uint32_t(__fastcall*)(void* block, void* edx, const Guid* guid);
using CastFn = char(__fastcall*)(void* obj, void* edx, uint32_t classHash, uint32_t* out);
AttachmentCheckFn g_origCheck = nullptr;
PickupActionFn g_origAction = nullptr;
ConeTestFn g_origCone = nullptr;
RangeTestFn g_origRange = nullptr;
RoomForFn g_origRoom = nullptr;
// Which hooks are in place (set once, in DS3ModInit).
bool g_actionHook = false, g_checkHook = false, g_coneHook = false, g_rangeHook = false, g_roomHook = false;

// What is on now: the settings, limited to what the hooks in place can do.
bool g_autoPickup = false;
bool g_pickupList = false;
bool g_allAround = false;
float g_radius = 0;  // metres; 0 = the game's reach
bool g_skipWeaponParts = true;
bool g_skipQuestItems = true;
unsigned g_maxMedPacks = 0;
unsigned g_maxStasisPacks = 0;
bool g_logCandidates = false;

void* g_builtInUsedBy = nullptr;   // the component whose last check passed with our sweeper
void* g_localComponent = nullptr;  // the local player's pickup component, seen by the action hook
bool g_inBuiltInCheck = false;     // inside AttachmentCheck with our sweeper

uint8_t* ComponentPlayer(void* component)
{
    uint32_t p = 0;
    SafeReadU32(reinterpret_cast<uintptr_t>(component) + kComponentPlayer, &p);
    return reinterpret_cast<uint8_t*>(p);
}

bool IsLocal(void* component)
{
    uint8_t* player = ComponentPlayer(component);
    return player && player == g_host->localPlayer();
}

// ---- Which nearby items our sweeper may take ----

// Typed asset lookups, all __cdecl data* Lookup(const Guid*): 0 unless the asset is of that class. Same first bytes
// for all of them.
constexpr unsigned char kLookupBytes[] = {0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x08, 0x8B, 0x11,
                                          0x83, 0xEC, 0x24, 0x33, 0xC0, 0x81, 0xFA, 0xBA};
constexpr uintptr_t kWeaponPartLookups[] = {
    0x0043FC50,  // GunStockProxy (frames)
    0x0043FCC0,  // FiringModuleProxy (tools, tips)
    0x0043FD30,  // GunAttachment
    0x0043FDA0,  // GunCircuit
    0x004CDDF0,  // WeaponComponentPickup
    0x004CDE60,  // RandomItemTreasurePickup (may hold weapon parts)
};
constexpr uintptr_t kPowerUpLookupVa = 0x00636000;  // PowerUp: bonus health float +0x300, bonus energy +0x308
constexpr uintptr_t kPowerUpBonusHealth = 0x300;
constexpr uintptr_t kPowerUpBonusEnergy = 0x308;
constexpr uintptr_t kQuestKeyLookupVa = 0x0043FAC0;  // QuestKeyItem
using LookupFn = const uint8_t*(__cdecl*)(const Guid* guid);

// Quest items: keys (category 0x20), mission items (0x40) and any QuestKeyItem asset. Picking one up can start a
// scene, so they are left for the player.
constexpr uint32_t kQuestCategories = 0x20 | 0x40;
// Resources (tungsten, semiconductor, scrap metal, somatic gel, transducer) go to the counters in the resource block
// and take no inventory slot.
constexpr uint32_t kResourceCategory = 0x8;
// Weapons category: parts and circuits, but also whole weapons, the Scavenger Bot and the Spare Parts Box, which are
// not of the part classes. All left with "Leave weapons, parts and circuits".
constexpr uint32_t kWeaponCategory = 0x100;

bool g_lookupsOk = false;
bool g_questLookupOk = false;

enum class Kind { Other, WeaponPart, MedPack, StasisPack };

const uint8_t* Lookup(uintptr_t fn, const Guid& g)
{
    __try {
        return reinterpret_cast<LookupFn>(fn)(&g);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

Kind KindOf(const Guid& g)
{
    if (!g_lookupsOk || IsNullGuid(g)) return Kind::Other;
    for (uintptr_t fn : kWeaponPartLookups) {
        if (Lookup(fn, g)) return Kind::WeaponPart;
    }
    if (const uint8_t* p = Lookup(kPowerUpLookupVa, g)) {
        float health = 0, energy = 0;
        SafeRead(reinterpret_cast<uintptr_t>(p) + kPowerUpBonusHealth, &health, sizeof(health));
        SafeRead(reinterpret_cast<uintptr_t>(p) + kPowerUpBonusEnergy, &energy, sizeof(energy));
        if (health > 0) return Kind::MedPack;
        if (energy > 0) return Kind::StasisPack;
    }
    return Kind::Other;
}

bool IsQuest(const Guid& g, uint32_t categories)
{
    if (categories & kQuestCategories) return true;
    return g_questLookupOk && !IsNullGuid(g) && Lookup(kQuestKeyLookupVa, g) != nullptr;
}

const char* KindName(Kind k)
{
    switch (k) {
    case Kind::WeaponPart: return "weapon part / circuit";
    case Kind::MedPack: return "med pack";
    case Kind::StasisPack: return "stasis pack";
    default: return "other";
    }
}

// How many of a kind the player carries (the consumables inventory in the resource block). False if the inventory
// can't be read right now: then the count is unknown, and a pack limit treats it as reached.
bool Carried(uint8_t* player, Kind kind, unsigned* out)
{
    uintptr_t list = ResourceBlock(player) + kBlockInventoryEntries;
    uint32_t entries = 0, count = 0;
    if (!player || !SafeReadU32(list, &entries) || !SafeReadU32(list + 4, &count) || count > 256) return false;
    if (count && !entries) return false;
    unsigned total = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t e = entries + i * kInventoryStride;
        Guid g{};
        uint32_t n = 0;
        if (!SafeRead(e, &g, sizeof(g)) || !SafeReadU32(e + kInventoryItemCount, &n)) return false;
        if (KindOf(g) == kind) total += n;
    }
    *out = total;
    return true;
}

// The pickup item behind a handle (the cast the sweeper check makes) and its data, whose +0x108 is the item's GUID.
bool ItemFromHandle(uint32_t handle, uintptr_t* item, uintptr_t* data)
{
    __try {
        if (!handle || handle == 0x10) return false;
        auto* obj = reinterpret_cast<void*>(static_cast<uintptr_t>(handle - 0x10));
        auto cast = reinterpret_cast<CastFn>((*reinterpret_cast<void***>(obj))[4]);
        uint32_t found = 0;
        if (!cast(obj, nullptr, kPickupItemClass, &found) || !found) return false;
        *item = found;
        *data = *reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(found) + 0xC);
        return *data != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CandidateItem(void* component, int i, uintptr_t* item, uintptr_t* data)
{
    uint32_t handle = 0;
    return SafeReadU32(reinterpret_cast<uintptr_t>(component) + kComponentCandidates + 8 * i, &handle) &&
           ItemFromHandle(handle, item, data);
}

// [debug] log_pickup_candidates: each item kind the check sees, once, with its categories and kind.
constexpr int kSeenMax = 256;
Guid g_seen[kSeenMax];
int g_seenCount = 0;

void LogCandidate(const Guid& g, uint32_t categories, Kind kind)
{
    if (g_seenCount >= kSeenMax) return;
    for (int s = 0; s < g_seenCount; ++s) {
        if (std::memcmp(&g_seen[s], &g, sizeof(g)) == 0) return;
    }
    g_seen[g_seenCount++] = g;
    char name[64];
    g_host->itemName(&g, name, sizeof(name));
    Logf("pickup candidate: %08X-%08X-%08X-%08X \"%s\" categories 0x%08X, %s%s", g.d[0], g.d[1], g.d[2], g.d[3], name,
         categories, KindName(kind), (categories & g_sweeperData.categories) ? "" : " (category not collected)");
}

void SetSkip(uintptr_t item, bool on)
{
    __try {
        auto* flags = reinterpret_cast<uint32_t*>(item + kItemFlags);
        *flags = on ? (*flags | kItemSkipBit) : (*flags & ~kItemSkipBit);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Carried med / stasis pack counts, read only when an item needs them (-1 = not read yet, -2 = unknown).
struct Carry {
    uint8_t* player;
    int med = -1;
    int stasis = -1;
};

// At or over a pack limit; an unknown count counts as reached, so the pack is left rather than taken past the limit.
bool AtLimit(uint8_t* player, Kind kind, int* cached, unsigned limit)
{
    if (*cached == -1) {
        unsigned n = 0;
        *cached = Carried(player, kind, &n) ? static_cast<int>(n) : -2;
    }
    return *cached == -2 || static_cast<unsigned>(*cached) >= limit;
}

// Whether our sweeper must leave an item of this kind: weapon parts and circuits, and med / stasis packs while the
// player carries the configured number.
bool MustLeave(Kind kind, Carry* carry)
{
    if (kind == Kind::WeaponPart) return g_skipWeaponParts;
    if (kind == Kind::MedPack && g_maxMedPacks) return AtLimit(carry->player, kind, &carry->med, g_maxMedPacks);
    if (kind == Kind::StasisPack && g_maxStasisPacks)
        return AtLimit(carry->player, kind, &carry->stasis, g_maxStasisPacks);
    return false;
}

struct Held {
    uintptr_t items[kCandidateCount];
    int count;
};

// Marks the candidates our sweeper must leave. Only items whose bit was clear are marked, so Release restores
// exactly them.
void HoldBack(void* component, Held* held)
{
    held->count = 0;
    Carry carry{ComponentPlayer(component)};
    for (int i = 0; i < kCandidateCount; ++i) {
        uintptr_t item = 0, data = 0;
        Guid g{};
        uint32_t categories = 0, flags = 0;
        if (!CandidateItem(component, i, &item, &data) || !SafeRead(data + kItemDataGuid, &g, sizeof(g)) ||
            !SafeReadU32(data + kItemDataCategories, &categories) || !SafeReadU32(item + kItemFlags, &flags))
            continue;
        Kind kind = KindOf(g);
        if (g_logCandidates) LogCandidate(g, categories, kind);
        bool hold = MustLeave(kind, &carry) || (g_skipQuestItems && IsQuest(g, categories)) ||
                    (g_skipWeaponParts && (categories & kWeaponCategory));
        if (hold && !(flags & kItemSkipBit)) {
            SetSkip(item, true);
            held->items[held->count++] = item;
        }
    }
}

void Release(const Held& held)
{
    for (int i = 0; i < held.count; ++i) SetSkip(held.items[i], false);
}

// ---- All around ----

Carry g_scanCarry{};  // pack counts for one refresh of the query

bool ReadPosition(uintptr_t obj, float pos[3])
{
    return SafeRead(obj + kPosition, pos, 3 * sizeof(float));
}

float SweeperRange(void* component)
{
    float range = 0;
    SafeRead(reinterpret_cast<uintptr_t>(component) + kComponentRange, &range, sizeof(range));
    return range > 0 ? range : 0;
}

// How far the auto-pickup reaches: the Reach setting, or the sweeper's own range if that is longer.
float Reach(void* component)
{
    return std::max(g_radius, SweeperRange(component));
}

bool IsLocalQuery(uintptr_t query)
{
    void* component = g_localComponent;
    return g_allAround && g_autoPickup && component &&
           query == reinterpret_cast<uintptr_t>(component) + kComponentQuery;
}

// [debug] log_pickup_candidates: the first time each kind of item is let in from beyond the game's reach.
Guid g_reachSeen[64];
int g_reachSeenCount = 0;

void LogReach(void* component, const Guid& g, float dist, float ownRange, const char* how)
{
    if (g_reachSeenCount >= 64) return;
    for (int s = 0; s < g_reachSeenCount; ++s) {
        if (std::memcmp(&g_reachSeen[s], &g, sizeof(g)) == 0) return;
    }
    g_reachSeen[g_reachSeenCount++] = g;
    char name[64];
    g_host->itemName(&g, name, sizeof(name));
    Logf("all around: \"%s\" let in by %s at %.2f m (its own range value %.2f, sweeper reach %.2f m, radius %.0f m)",
         name, how, dist, ownRange, SweeperRange(component), g_radius);
}

// An item our sweeper would take (its categories are collected, it is not one to leave) within the reach in any
// direction. *dist is its distance from the player.
bool WouldTake(void* component, uint32_t handle, float* dist, Guid* guid)
{
    uintptr_t item = 0, data = 0;
    uint32_t categories = 0, flags = 0;
    float me[3], it[3];
    if (!IsLocal(component) || !ItemFromHandle(handle, &item, &data) ||
        !ReadPosition(reinterpret_cast<uintptr_t>(ComponentPlayer(component)), me) || !ReadPosition(item, it))
        return false;
    const float dx = it[0] - me[0], dy = it[1] - me[1], dz = it[2] - me[2];
    const float reach = Reach(component);
    const float d2 = dx * dx + dy * dy + dz * dz;
    if (!(reach > 0) || !(d2 < reach * reach)) return false;  // cheap test first: most of the level is far away
    if (!SafeRead(data + kItemDataGuid, guid, sizeof(*guid)) || !SafeReadU32(data + kItemDataCategories, &categories) ||
        !SafeReadU32(item + kItemFlags, &flags))
        return false;
    if (!(categories & g_sweeperData.categories) || (flags & kItemSkipBit)) return false;
    *dist = std::sqrt(d2);
    if (g_skipQuestItems && IsQuest(*guid, categories)) return false;
    if (g_skipWeaponParts && (categories & kWeaponCategory)) return false;
    return !MustLeave(KindOf(*guid), &g_scanCarry);
}

uint32_t NodeHandle(void* node)
{
    uint32_t handle = 0;
    SafeReadU32(reinterpret_cast<uintptr_t>(node) + kNodeHandle, &handle);
    return handle;
}

uint32_t __fastcall HookConeTest(void* system, void* edx, void* node, uintptr_t query, uint32_t* flags, void* extra)
{
    uint32_t inside = g_origCone(system, edx, node, query, flags, extra);
    if ((inside & 0xFF) || !IsLocalQuery(query)) return inside;
    float dist = 0;
    Guid g{};
    if (!WouldTake(g_localComponent, NodeHandle(node), &dist, &g)) return inside;
    if (g_logCandidates) LogReach(g_localComponent, g, dist, 0, "direction (outside the cone)");
    return (inside & ~0xFFu) | 1;
}

// The game's RangeTest call, with the node's own range and the query radius raised to `radius` for that one call.
// In its own function because a __try block can't share one with C++ objects.
void PutBack(float* at, float value)
{
    __try {
        *at = value;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool RangeTestWithin(void* system, void* edx, void* node, uintptr_t query, uint32_t* flags, float radius,
                     float* savedOwn, uint32_t* inside)
{
    auto* own = reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(node) + 0x14);
    auto* limit = reinterpret_cast<float*>(query + 4);
    float savedLimit = 0;
    bool ownRead = false, limitRead = false;
    __try {
        *savedOwn = *own;
        ownRead = true;
        savedLimit = *limit;
        limitRead = true;
        if (*savedOwn > 0 && *savedOwn < radius * radius) *own = radius * radius;
        if (savedLimit > 0 && savedLimit < radius) *limit = radius;
        *inside = g_origRange(system, edx, node, query, flags);
        *own = *savedOwn;
        *limit = savedLimit;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Whatever faulted, both values go back to what they were.
        if (ownRead) PutBack(own, *savedOwn);
        if (limitRead) PutBack(limit, savedLimit);
        return false;
    }
}

// RangeTest compares the squared distance with the query radius (query +4, squared first; 0 = none) and with the
// node's own range (+0x14, compared with the squared distance as it is; 0 = none), then runs the node's own filter.
// For an item the auto-pickup takes within the Reach setting, both limits are raised for one more call, so that
// filter still decides.
uint32_t __fastcall HookRangeTest(void* system, void* edx, void* node, uintptr_t query, uint32_t* flags)
{
    uint32_t inside = g_origRange(system, edx, node, query, flags);
    if ((inside & 0xFF) || !(g_radius > 0) || !IsLocalQuery(query)) return inside;
    float dist = 0;
    Guid g{};
    if (!WouldTake(g_localComponent, NodeHandle(node), &dist, &g) || !(dist < g_radius)) return inside;
    float savedOwn = 0;
    if (!RangeTestWithin(system, edx, node, query, flags, g_radius, &savedOwn, &inside)) return 0;
    if ((inside & 0xFF) && g_logCandidates) LogReach(g_localComponent, g, dist, savedOwn, "radius");
    return inside;
}

// The sweeper check measures against component +0x140; with a Reach setting it is raised for that one call.
struct ReachOverride {
    float* range = nullptr;
    float saved = 0;
    explicit ReachOverride(void* component)
    {
        if (!g_allAround || !(g_radius > 0)) return;
        auto* r = reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(component) + kComponentRange);
        float v = 0;
        if (!SafeRead(reinterpret_cast<uintptr_t>(r), &v, sizeof(v)) || !(v < g_radius)) return;
        range = r;
        saved = v;
        *range = g_radius;
    }
    ~ReachOverride()
    {
        if (range) *range = saved;
    }
};

// Before the idle update: the refresh timer is set to its end so the query runs this frame.
void ScanNow(void* component)
{
    g_scanCarry = Carry{ComponentPlayer(component)};
    static void* logged = nullptr;
    if (g_logCandidates && logged != component) {
        logged = component;
        Logf("all around: sweeper reach %.2f m, radius %.0f m", SweeperRange(component), g_radius);
    }
    __try {
        *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(component) + kComponentScanTimer) = 1.0e6f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// ---- What was picked up: inventory and resource increases right after a pickup ----

constexpr int kMaxItems = 160;
constexpr ULONGLONG kPickupWindowMs = 2500;

struct Snapshot {
    uint8_t* player;
    uint32_t resources[DS3_RESOURCE_COUNT];
    int items;
    Guid guid[kMaxItems];
    uint32_t count[kMaxItems];
};
Snapshot g_before = {};
Snapshot g_now = {};
bool g_haveBefore = false;
ULONGLONG g_reportUntil = 0;

// This runs on every pickup action, so it reads the two inventories straight from memory. A mod that only looks now
// and then should use host->getResource / itemCount / getItem instead.
bool TakeSnapshot(uint8_t* player, Snapshot* s)
{
    s->player = player;
    s->items = 0;
    if (!SafeRead(ResourceBlock(player) + kBlockResources, s->resources, sizeof(s->resources))) return false;
    // Both inventories are summed per item, since one item can fill several stacks.
    const uintptr_t lists[] = {ResourceBlock(player) + kBlockInventoryEntries,
                               reinterpret_cast<uintptr_t>(player) + kPlayerInventoryEntries};
    for (uintptr_t list : lists) {
        uint32_t entries = 0, count = 0;
        if (!SafeReadU32(list, &entries) || !SafeReadU32(list + 4, &count) || (count && !entries) || count > kMaxItems)
            return false;
        for (uint32_t i = 0; i < count; ++i) {
            uintptr_t e = entries + i * kInventoryStride;
            Guid g{};
            uint32_t n = 0;
            if (!SafeRead(e, &g, sizeof(g)) || !SafeReadU32(e + kInventoryItemCount, &n)) return false;
            int k = 0;
            while (k < s->items && std::memcmp(&s->guid[k], &g, sizeof(g)) != 0) ++k;
            if (k == s->items) {
                if (s->items == kMaxItems) return false;
                s->guid[k] = g;
                s->count[k] = 0;
                ++s->items;
            }
            s->count[k] += n;
        }
    }
    return true;
}

// The game's item names are all capitals ("AMMO CLIP"); they are kept as "Ammo Clip", like the resource names. Words
// with anything but letters ("MK-V", "S.C.A.F.") are left as they are.
void TitleCase(char* s)
{
    while (*s) {
        char* end = s;
        bool letters = true;
        for (; *end && *end != ' '; ++end)
            letters = letters && ((*end >= 'A' && *end <= 'Z') || (*end >= 'a' && *end <= 'z'));
        if (letters) {
            for (char* c = s + 1; c < end; ++c) {
                if (*c >= 'A' && *c <= 'Z') *c = static_cast<char>(*c - 'A' + 'a');
            }
        }
        s = *end ? end + 1 : end;
    }
}

void Report(const char* name, unsigned amount)
{
    AddLine(name, amount);
    if (g_logCandidates) Logf("picked up %u %s", amount, name);
}

// Compares the inventory with the last look; increases are shown only inside the window after a pickup, so crafting,
// the store and bench storage are not reported.
void UpdateList(void* component)
{
    uint8_t* player = ComponentPlayer(component);
    if (!player || !TakeSnapshot(player, &g_now)) return;
    bool report = g_haveBefore && g_before.player == player && GetTickCount64() < g_reportUntil;
    if (report) {
        for (int r = 0; r < DS3_RESOURCE_COUNT; ++r) {
            if (g_now.resources[r] > g_before.resources[r])
                Report(g_host->resourceName(r), g_now.resources[r] - g_before.resources[r]);
        }
        for (int i = 0; i < g_now.items; ++i) {
            uint32_t old = 0;
            for (int j = 0; j < g_before.items; ++j) {
                if (std::memcmp(&g_before.guid[j], &g_now.guid[i], sizeof(Guid)) == 0) {
                    old = g_before.count[j];
                    break;
                }
            }
            if (g_now.count[i] <= old) continue;
            char name[64];
            if (g_host->itemName(&g_now.guid[i], name, sizeof(name)))
                TitleCase(name);
            else
                strcpy_s(name, "Item");
            Report(name, g_now.count[i] - old);
        }
    }
    g_before = g_now;
    g_haveBefore = true;
}

// ---- The other hooks ----

bool IsResourceData(uintptr_t data)
{
    uint32_t categories = 0;
    return SafeReadU32(data + kItemDataCategories, &categories) && (categories & kResourceCategory) &&
           !(categories & ~kResourceCategory);
}

uint32_t __fastcall HookRoomFor(void* block, void* edx, const Guid* guid)
{
    uint32_t room = g_origRoom(block, edx, guid);
    // The sweeper check passes item data +0x108; only that call is changed.
    if (room || !g_inBuiltInCheck || !IsResourceData(reinterpret_cast<uintptr_t>(guid) - kItemDataGuid)) return room;
    return 1;
}

// If the game's check faults while items are held back or the reach is raised, the skip bits and the reach are put
// back before the fault goes on to the game's own handling (nothing is swallowed here).
int PutBackOnFault(const Held* held, ReachOverride* reach)
{
    Release(*held);
    if (reach->range) PutBack(reach->range, reach->saved);
    reach->range = nullptr;
    g_inBuiltInCheck = false;
    return EXCEPTION_CONTINUE_SEARCH;
}

uint32_t CheckHolding(void* component, void* edx, const Held& held, ReachOverride* reach)
{
    __try {
        return g_origCheck(component, edx, &g_sweeper);
    } __except (PutBackOnFault(&held, reach)) {
        return 0;  // not reached: the filter passes the fault on
    }
}

uint32_t __fastcall HookAttachmentCheck(void* component, void* edx, void* sweeper)
{
    if (sweeper || !g_autoPickup || !IsLocal(component)) {
        if (component == g_builtInUsedBy) g_builtInUsedBy = nullptr;
        return g_origCheck(component, edx, sweeper);
    }
    Held held;
    HoldBack(component, &held);
    uint32_t found;
    {
        ReachOverride reach(component);
        g_inBuiltInCheck = true;
        found = CheckHolding(component, edx, held, &reach);
        g_inBuiltInCheck = false;
    }
    Release(held);
    g_builtInUsedBy = (found & 0xFF) ? component : nullptr;
    return found;
}

// What the game copies from the sweeper on entering attemptAutoPickup: the slots to keep open while the item is added.
// A resource takes no slot, so for one the limit is 0.
void SetMinOpenSlots(void* component)
{
    uint8_t* player = ComponentPlayer(component);
    if (!player) return;
    uint32_t handle = 0, slots = g_sweeperData.minOpenSlots;
    uintptr_t item = 0, data = 0;
    if (SafeReadU32(reinterpret_cast<uintptr_t>(component) + kComponentActiveItem, &handle) &&
        ItemFromHandle(handle, &item, &data) && IsResourceData(data))
        slots = 0;
    __try {
        *reinterpret_cast<uint32_t*>(ResourceBlock(player) + kBlockMinOpenSlots) = slots;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

char __fastcall HookPickupAction(void* component, void* edx, uint32_t a, uint32_t b, uint32_t c, int action, uint32_t e)
{
    if (action == kActionEnterAutoPickup && component == g_builtInUsedBy) {
        g_builtInUsedBy = nullptr;
        SetMinOpenSlots(component);
        return 1;
    }
    if (!IsLocal(component)) return g_origAction(component, edx, a, b, c, action, e);
    g_localComponent = component;
    if (action == kActionUpdateIdleScan && g_allAround && g_autoPickup) ScanNow(component);
    if (!g_pickupList) return g_origAction(component, edx, a, b, c, action, e);

    if (action == kActionEnterPickupItem || action == kActionEnterRemotePickup) {
        UpdateList(component);  // anything that changed before this pickup is not reported
        g_reportUntil = GetTickCount64() + kPickupWindowMs;
    } else if (action == kActionExitPickupItem) {
        g_reportUntil = GetTickCount64() + kPickupWindowMs;  // the item may land as the state ends
    }
    char result = g_origAction(component, edx, a, b, c, action, e);
    UpdateList(component);
    return result;
}

// ---- Start, and settings changes ----

bool Hook(const char* name, unsigned address, const unsigned char* bytes, unsigned len, void* detour, void** original)
{
    return g_host->hook(g_host, name, address, bytes, len, detour, original) != 0;
}

// Applies the settings to the running game. Each part is on only if its hooks went in; a part that is off passes
// every call straight through to the game.
void ApplySettings()
{
    const Settings s = ReadSettings();
    g_logCandidates = s.logCandidates;
    // Without the lookups nothing can be told apart, so it falls back to what the attachment itself collects: ammo.
    g_sweeperData.categories = g_lookupsOk ? s.categories : s.categories & 2;
    g_sweeperData.minOpenSlots = s.minOpenSlots;
    g_skipWeaponParts = s.skipWeaponParts;
    g_skipQuestItems = s.skipQuestItems;
    g_maxMedPacks = s.maxMedPacks;
    g_maxStasisPacks = s.maxStasisPacks;
    g_radius = g_rangeHook ? static_cast<float>(s.radius) : 0.0f;
    g_allAround = s.allAround && g_coneHook;
    g_autoPickup = s.collect && g_checkHook;
    g_pickupList = s.list && g_actionHook;

    g_listLines = s.listLines;
    g_listLifetimeMs = static_cast<ULONGLONG>(s.listSeconds) * 1000;
    g_listTextSize = s.listTextSize;
    g_listOn = g_pickupList;

    Logf("auto-pickup %s (categories 0x%08X, %s, keeps %u inventory slots open, weapon parts %s, quest items %s, med "
         "packs up to %u, stasis packs up to %u; 0 = no limit); pickup list %s",
         g_autoPickup ? "on" : (s.collect ? "UNAVAILABLE" : "off"), g_sweeperData.categories,
         g_allAround ? (g_radius > 0 ? "all around, reach set" : "all around, game's reach")
                     : (s.allAround && s.collect ? "all around UNAVAILABLE, in front" : "in front"),
         g_sweeperData.minOpenSlots, g_skipWeaponParts ? "left" : "collected",
         g_skipQuestItems ? "left" : "collected", g_maxMedPacks, g_maxStasisPacks,
         g_pickupList ? "on" : (s.list ? "UNAVAILABLE" : "off"));
}

}  // namespace

// ---- What the loader calls ----

extern "C" __declspec(dllexport) int __cdecl DS3ModInit(const DS3ModHost* host)
{
    // hook, localPlayer, itemName and onFrame came with API 2.
    if (!host || host->apiVersion < 2 || !DS3MOD_HAS(host, onFrame)) {
        if (host && host->log) host->log(host, "Auto Pickup needs a newer loader (API 2 or later)");
        return 1;
    }
    g_host = host;

    // The lookups are called by address, so their bytes are checked first, as hook does for the hooked functions.
    g_lookupsOk = BytesMatch(kPowerUpLookupVa, kLookupBytes, sizeof(kLookupBytes));
    for (uintptr_t fn : kWeaponPartLookups) g_lookupsOk = g_lookupsOk && BytesMatch(fn, kLookupBytes, sizeof(kLookupBytes));
    g_questLookupOk = BytesMatch(kQuestKeyLookupVa, kLookupBytes, sizeof(kLookupBytes));
    if (!g_lookupsOk) Logf("item lookups do not match this game version: collecting ammo only, no pack limits");

    // The action hook first: both parts need it, and our sweeper is never handed out without its action fix in
    // place. Each later hook only goes in if the one it builds on did. A refused hook is in ds3_mods.log.
    g_actionHook = Hook("PickupAction", kPickupActionVa, kPickupActionBytes, sizeof(kPickupActionBytes),
                        reinterpret_cast<void*>(&HookPickupAction), reinterpret_cast<void**>(&g_origAction));
    g_checkHook = g_actionHook &&
                  Hook("AttachmentCheck", kAttachmentCheckVa, kAttachmentCheckBytes, sizeof(kAttachmentCheckBytes),
                       reinterpret_cast<void*>(&HookAttachmentCheck), reinterpret_cast<void**>(&g_origCheck));
    g_coneHook = g_checkHook && Hook("ConeTest", kConeTestVa, kConeTestBytes, sizeof(kConeTestBytes),
                                     reinterpret_cast<void*>(&HookConeTest), reinterpret_cast<void**>(&g_origCone));
    g_rangeHook = g_coneHook && Hook("RangeTest", kRangeTestVa, kConeTestBytes, sizeof(kConeTestBytes),
                                     reinterpret_cast<void*>(&HookRangeTest), reinterpret_cast<void**>(&g_origRange));
    // Without it the game's rule stays: no resources either while only the kept-open slots are left.
    g_roomHook = g_checkHook && Hook("RoomFor", kRoomForVa, kRoomForBytes, sizeof(kRoomForBytes),
                                     reinterpret_cast<void*>(&HookRoomFor), reinterpret_cast<void**>(&g_origRoom));
    if (!g_actionHook) {
        Logf("the pickup action could not be hooked: Auto Pickup does nothing");
        return 1;
    }

    ApplySettings();
    host->onFrame(host, &OnFrame, nullptr);  // draws the pickup list
    return 0;
}

// The Mods panel changed one of our settings (or auto_pickup.ini was edited): apply it now.
extern "C" __declspec(dllexport) void __cdecl DS3ModSettingsChanged()
{
    ApplySettings();
}
