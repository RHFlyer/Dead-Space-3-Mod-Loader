// The SDK's API 2 for drop-in DLL mods. See sdk.h, and sdk/ds3mod.h for what each call promises.
//
// Everything a mod asks for is checked the way patch mods are (patches.h) and logged with the mod's id. Game reads go
// through game.cpp's byte-checked, crash-proof helpers. Frame callbacks run inside the overlay's Present hook, on the
// game's main thread, each under __try so a crashing mod is switched off instead of taking the game down.

#include "sdk.h"

#include "game.h"
#include "log.h"
#include "mods.h"
#include "overlay.h"
#include "patches.h"
#include "sealoffers.h"

#include <windows.h>

#include <MinHook.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

constexpr int kHookBytes = 16;        // what the proxy holds for a hook, as ClaimHook does
constexpr int kLoggedResourceSets = 10;  // per mod; then one "no longer logged" line

const DS3ModHost* g_hostOf[kMaxMods] = {};
bool g_anyHost = false;

struct FrameCallback {
    DS3FrameFn volatile fn;
    void* volatile user;
};
FrameCallback g_frames[kMaxMods] = {};
volatile LONG g_resourceSets[kMaxMods] = {};

// The frame being drawn, and what it draws with; only set while the callbacks run.
const DS3Frame* g_frame = nullptr;
OverlayModCanvas* g_canvas = nullptr;
LONGLONG g_lastFrameAt = 0;  // performance counter at the previous frame
void (*g_tick)() = nullptr;   // mods.cpp's hot-reload step, once per frame

int ModIndexOf(const DS3ModHost* host)
{
    for (int i = 0; i < kMaxMods; ++i) {
        if (host && g_hostOf[i] == host) return i;
    }
    return -1;
}

const char* IdOf(const DS3ModHost* host)
{
    return host && host->modId ? host->modId : "?";
}

// "55 8B EC ..." (up to 16 bytes).
void Hex(const uint8_t* bytes, size_t len, char* out, size_t size)
{
    out[0] = '\0';
    size_t at = 0;
    for (size_t i = 0; i < len && i < 16 && at + 4 < size; ++i)
        at += sprintf_s(out + at, size - at, i ? " %02X" : "%02X", bytes[i]);
    if (len > 16 && at + 4 < size) strcat_s(out, size, " ...");
}

// ---- Hook stubs: counting the calls in flight, so a mod can be unloaded (hot reload) ----
//
// host->hook does not point the game straight at the mod's detour. The game jumps to a small stub of ours,
// `push record; jmp HotEnterAsm`, which counts the call, keeps the real return address on a per-thread shadow stack,
// swaps it for an exit stub and goes on to the detour. There is one exit stub per shadow-stack depth
// (`push depth; jmp HotExitAsm`), so a return always knows its own entry; HotExitAsm counts the call back down and
// returns to the game. Arguments, ECX / EDX on entry and EAX:EDX / ST0 on return are untouched, so any calling convention works.
// Stubs and records are never freed: a thread that is already in a stub when its hook goes away runs the game's own
// function instead (record->active is 0 by then). A record is reused only for the same game address.

constexpr int kMaxHookRecords = 256;  // over all mods and all reloads (same address = same record)
constexpr int kStubBytes = 16;
constexpr int kShadowDepth = 64;

struct HookRecord {
    void* volatile detour;  // the mod's
    void* target;           // the game function
    volatile LONG busy;     // calls inside the detour now
    volatile LONG active;   // 0: send calls to the game's own code
    volatile LONG lost;     // a call was not counted (shadow stack full): never unload this mod
    int mod;                // -1 when no mod holds it
    bool installed;         // a MinHook hook exists for it
    bool claimAdded;        // its 16-byte claim is new, so it is given back on removal
    unsigned address;
    uint8_t* stub;
};
HookRecord g_records[kMaxHookRecords] = {};
int g_recordCount = 0;
uint8_t* g_stubPage = nullptr;  // hook stubs, then the exit stubs
uint8_t* g_exitStubs = nullptr;

struct ShadowEntry {
    void* ret;       // the game's return address
    uintptr_t slot;  // where it was on the stack (ESP at the stub)
    HookRecord* rec;
};
struct ShadowStack {
    int n;
    ShadowEntry e[kShadowDepth];
};
__declspec(thread) ShadowStack t_shadow;

// ---- Byte patches a mod made through host->patch, to put back on unload ----

constexpr int kMaxPatchRecords = 128;
struct PatchRecord {
    int owner;  // mod index + 1; 0 = free
    unsigned address;
    unsigned len;
    bool claimAdded;
    uint8_t original[kMaxPatchBytes];
    uint8_t written[kMaxPatchBytes];
};
PatchRecord g_patchRecords[kMaxPatchRecords] = {};
bool g_untracked[kMaxMods] = {};
bool g_closing[kMaxMods] = {};
bool g_reloadable[kMaxMods] = {};  // runs from a temp copy and can be unloaded: its hooks go through counting stubs  // being unloaded: no new hooks or frame callbacks  // a patch could not be recorded: this mod can't be unloaded safely

// A notice the proxy draws for a few seconds (hot reload results).
char g_notice[128] = {};
DWORD g_noticeAt = 0;
bool g_noticeBad = false;
constexpr DWORD kNoticeMs = 4000;

struct HookRecord;
void* __stdcall HotEnterC(HookRecord* r, void** slot);
void* __stdcall HotLeaveC(int depth);
void HotExitAsm();

// [esp] = record (pushed by the stub), [esp + 4] = the game's return address, then the game's arguments.
__declspec(naked) void HotEnterAsm()
{
    __asm {
        push eax
        push ecx
        push edx
        lea eax, [esp + 16]        // the return address's slot
        push eax
        push dword ptr [esp + 16]  // the record (moved up by the push above)
        call HotEnterC             // __stdcall: pops both; EAX = where to go
        mov [esp + 12], eax        // the record's slot now holds it
        pop edx
        pop ecx
        pop eax
        ret                        // to the detour (or the game's own code), the game's stack as it was
    }
}

// Reached through exit stub k from the detour's `ret` / `ret n`: [esp] = k, and below it the stack as the game's caller
// expects it after the call.
__declspec(naked) void HotExitAsm()
{
    __asm {
        push eax
        push edx
        push dword ptr [esp + 8]   // k
        call HotLeaveC             // __stdcall; EAX = the game's return address
        mov [esp + 8], eax         // in k's place
        pop edx
        pop eax
        ret
    }
}

void* __stdcall HotEnterC(HookRecord* r, void** slot)
{
    // Count first, then look at `active`: an unload sets active = 0 and only then waits for busy to reach 0.
    InterlockedIncrement(&r->busy);
    if (!r->active) {
        InterlockedDecrement(&r->busy);
        return r->target;  // the hook is gone: MinHook has put the game's bytes back
    }
    ShadowStack& t = t_shadow;
    // Entries at or below this call's return slot belong to detours an exception (or longjmp) unwound past: a call
    // still running would have its slot above ours. Give their counts back.
    const uintptr_t at = reinterpret_cast<uintptr_t>(slot);
    while (t.n > 0 && t.e[t.n - 1].slot <= at) {
        --t.n;
        InterlockedDecrement(&t.e[t.n].rec->busy);
    }
    if (t.n >= kShadowDepth) {
        InterlockedDecrement(&r->busy);
        r->lost = 1;
        return r->detour;  // not counted; the mod can't be unloaded any more
    }
    t.e[t.n].ret = *slot;
    t.e[t.n].slot = reinterpret_cast<uintptr_t>(slot);
    t.e[t.n].rec = r;
    ++t.n;
    *slot = g_exitStubs + (t.n - 1) * 8;
    return r->detour;
}

void* __stdcall HotLeaveC(int depth)
{
    ShadowStack& t = t_shadow;
    // Our entry is `depth`. Any above it were pushed inside our detour by calls an exception unwound past.
    if (depth < 0 || depth >= t.n) __fastfail(FAST_FAIL_INVALID_ARG);  // no return address: cannot go on safely
    while (t.n > depth + 1) {
        --t.n;
        InterlockedDecrement(&t.e[t.n].rec->busy);
    }
    t.n = depth;
    InterlockedDecrement(&t.e[depth].rec->busy);
    return t.e[depth].ret;
}

// A record for a hook at `target`: the free one for that address, or a new one with its stub. Null when full.
HookRecord* RecordFor(void* target)
{
    for (int i = 0; i < g_recordCount; ++i) {
        if (g_records[i].target == target && !g_records[i].installed) return &g_records[i];
    }
    if (g_recordCount >= kMaxHookRecords) return nullptr;
    if (!g_stubPage) {
        g_stubPage = static_cast<uint8_t*>(VirtualAlloc(nullptr, kMaxHookRecords * kStubBytes + kShadowDepth * 8,
                                                        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!g_stubPage) return nullptr;
        g_exitStubs = g_stubPage + kMaxHookRecords * kStubBytes;
        for (int k = 0; k < kShadowDepth; ++k) {
            uint8_t* e = g_exitStubs + k * 8;
            e[0] = 0x6A;  // push imm8
            e[1] = static_cast<uint8_t>(k);
            e[2] = 0xE9;  // jmp rel32
            const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&HotExitAsm) -
                                                     reinterpret_cast<uintptr_t>(e + 7));
            std::memcpy(e + 3, &rel, 4);
            e[7] = 0xCC;
        }
        FlushInstructionCache(GetCurrentProcess(), g_exitStubs, kShadowDepth * 8);
    }
    HookRecord& r = g_records[g_recordCount];
    r.target = target;
    r.mod = -1;
    r.stub = g_stubPage + g_recordCount * kStubBytes;
    uint8_t* p = r.stub;
    p[0] = 0x68;  // push imm32
    const uint32_t rec = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&r));
    std::memcpy(p + 1, &rec, 4);
    p[5] = 0xE9;  // jmp rel32
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&HotEnterAsm) -
                                             reinterpret_cast<uintptr_t>(p + 10));
    std::memcpy(p + 6, &rel, 4);
    for (int i = 10; i < kStubBytes; ++i) p[i] = 0xCC;
    FlushInstructionCache(GetCurrentProcess(), p, kStubBytes);
    ++g_recordCount;
    return &r;
}

// Remembers what a mod's patch replaced, so an unload can put it back. A second call for the same bytes (an undo, or
// a rewrite) only updates what is there now.
void RecordPatch(int mi, unsigned address, unsigned len, const uint8_t* before, const uint8_t* written, bool added)
{
    PatchRecord* free = nullptr;
    for (PatchRecord& p : g_patchRecords) {
        if (p.owner == mi + 1 && p.address == address && p.len == len) {
            std::memcpy(p.written, written, len);
            p.claimAdded = p.claimAdded || added;
            return;
        }
        if (!free && !p.owner) free = &p;
    }
    if (!free) {
        if (!g_untracked[mi])
            Log("mod %s: too many patches to undo on a hot reload; it now updates only on a restart",
                g_hostOf[mi] ? g_hostOf[mi]->modId : "?");
        g_untracked[mi] = true;
        return;
    }
    free->owner = mi + 1;
    free->address = address;
    free->len = len;
    free->claimAdded = added;
    std::memcpy(free->original, before, len);
    std::memcpy(free->written, written, len);
}

// ---- hook / patch ----

int __cdecl HostHook(const DS3ModHost* host, const char* name, unsigned address, const void* expect,
                     unsigned expectLen, void* detour, void** original)
{
    const int mi = ModIndexOf(host);
    if (mi < 0) return 0;
    const char* id = IdOf(host);
    if (!name || !*name) name = "?";
    if (!expect || !detour || !original || expectLen < 5 || expectLen > static_cast<unsigned>(kMaxPatchBytes)) {
        Log("mod %s: hook %s refused: it needs the expected bytes (5 to %d), a detour and a place for the original",
            id, name, kMaxPatchBytes);
        return 0;
    }
    *original = nullptr;
    if (g_closing[mi]) {
        Log("mod %s: hook %s refused: the mod is being unloaded", id, name);
        return 0;
    }
    if (!PatchesAllowed()) {
        Log("mod %s: hook %s refused: the game's code is not decrypted; nothing was hooked", id, name);
        return 0;
    }
    char why[200];
    const size_t span = std::max<size_t>(expectLen, kHookBytes);
    if (!ModRangeAllowed(address, span, true, why, sizeof(why))) {
        Log("mod %s: hook %s refused: %s", id, name, why);
        return 0;
    }
    uint8_t now[kMaxPatchBytes];
    if (!ReadGameBytes(address, now, expectLen)) {
        Log("mod %s: hook %s refused: 0x%08X cannot be read", id, name, address);
        return 0;
    }
    if (memcmp(now, expect, expectLen) != 0) {
        char got[64];
        Hex(now, expectLen, got, sizeof(got));
        Log("mod %s: hook %s refused: the game's bytes at 0x%08X are not the expected ones (they are %s): a different "
            "game build, or another mod changed them",
            id, name, address, got);
        return 0;
    }
    const char* holder = nullptr;
    bool added = false;  // false: the mod already held these bytes (its own patch or hook), so keep its claim
    if (!ClaimRange(address, kHookBytes, id, &holder, &added)) {
        Log("mod %s: hook %s refused: 0x%08X is already taken by %s", id, name, address, holder ? holder : "?");
        return 0;
    }
    if (!game::InitMinHook()) {
        if (added) ReleaseRange(address, kHookBytes, id);
        return 0;
    }
    void* target = reinterpret_cast<void*>(static_cast<uintptr_t>(address));
    if (!g_reloadable[mi]) {
        // A mod that can't be unloaded: the game goes straight to its detour, as before hot reload existed.
        MH_STATUS st = MH_CreateHook(target, detour, original);
        if (st == MH_OK) {
            st = MH_EnableHook(target);
            if (st != MH_OK) MH_RemoveHook(target);
        }
        if (st != MH_OK) {
            if (added) ReleaseRange(address, kHookBytes, id);
            *original = nullptr;
            Log("mod %s: hook %s at 0x%08X FAILED: %s", id, name, address, MH_StatusToString(st));
            return 0;
        }
        Log("mod %s: hook %s at 0x%08X-0x%08X installed", id, name, address, address + kHookBytes - 1);
        return 1;
    }
    HookRecord* rec = RecordFor(target);
    if (!rec) {
        if (added) ReleaseRange(address, kHookBytes, id);
        Log("mod %s: hook %s refused: the proxy has no room for more hooks (%d; a hot reload reuses its hooks' room "
            "only for the same addresses); restart the game",
            id, name, kMaxHookRecords);
        return 0;
    }
    rec->detour = detour;
    rec->busy = 0;
    rec->lost = 0;
    rec->active = 1;  // before the hook is enabled: the stub then always goes to the detour
    MH_STATUS st = MH_CreateHook(target, rec->stub, original);
    if (st == MH_OK) {
        st = MH_EnableHook(target);
        if (st != MH_OK) MH_RemoveHook(target);
    }
    if (st != MH_OK) {
        rec->active = 0;
        if (added) ReleaseRange(address, kHookBytes, id);
        *original = nullptr;
        Log("mod %s: hook %s at 0x%08X FAILED: %s", id, name, address, MH_StatusToString(st));
        return 0;
    }
    rec->mod = mi;
    rec->installed = true;
    rec->claimAdded = added;
    rec->address = address;
    Log("mod %s: hook %s at 0x%08X-0x%08X installed", id, name, address, address + kHookBytes - 1);
    return 1;
}

int __cdecl HostPatch(const DS3ModHost* host, const char* name, unsigned address, const void* expect,
                      const void* write, unsigned len)
{
    const int mi = ModIndexOf(host);
    if (mi < 0) return 0;
    const char* id = IdOf(host);
    if (!name || !*name) name = "?";
    if (!expect || !write || !len || len > static_cast<unsigned>(kMaxPatchBytes)) {
        Log("mod %s: patch %s refused: it needs the expected and the new bytes, 1 to %d of each", id, name,
            kMaxPatchBytes);
        return 0;
    }
    if (!PatchesAllowed()) {
        Log("mod %s: patch %s refused: the game's code is not decrypted; nothing was written", id, name);
        return 0;
    }
    char why[200];
    if (!ModRangeAllowed(address, len, false, why, sizeof(why))) {
        Log("mod %s: patch %s refused: %s", id, name, why);
        return 0;
    }
    uint8_t now[kMaxPatchBytes];
    if (!ReadGameBytes(address, now, len)) {
        Log("mod %s: patch %s refused: 0x%08X cannot be read", id, name, address);
        return 0;
    }
    const bool already = memcmp(now, write, len) == 0;
    if (!already && memcmp(now, expect, len) != 0) {
        char got[64];
        Hex(now, len, got, sizeof(got));
        Log("mod %s: patch %s refused: the game's bytes at 0x%08X are not the expected ones (they are %s): a different "
            "game build, or another mod changed them",
            id, name, address, got);
        return 0;
    }
    const char* holder = nullptr;
    bool added = false;
    if (!ClaimRange(address, len, id, &holder, &added)) {
        Log("mod %s: patch %s refused: 0x%08X is already taken by %s", id, name, address, holder ? holder : "?");
        return 0;
    }
    if (!already && !WriteGameBytes(address, static_cast<const uint8_t*>(write), len)) {
        if (added) ReleaseRange(address, len, id);
        Log("mod %s: patch %s: writing 0x%08X FAILED", id, name, address);
        return 0;
    }
    RecordPatch(mi, address, len, already ? static_cast<const uint8_t*>(expect) : now,
                static_cast<const uint8_t*>(write), added);
    if (already) return 1;  // nothing to change (and not logged again)
    Log("mod %s: patch %s: %u byte(s) at 0x%08X-0x%08X written", id, name, len, address, address + len - 1);
    return 1;
}

// ---- Game reads ----

int CurrentState()
{
    if (!game::LocalPlayer()) return DS3_STATE_NO_PLAYER;
    return game::PauseMenuOpen() ? DS3_STATE_PAUSED : DS3_STATE_PLAYING;
}

int __cdecl HostGameState()
{
    return CurrentState();
}

void* __cdecl HostLocalPlayer()
{
    return game::LocalPlayer();
}

uintptr_t ResourceAddress(int which)
{
    if (which < 0 || which >= game::kResourceCount) return 0;
    const uint8_t* player = game::LocalPlayer();
    if (!player) return 0;
    return game::ResourceBlock(player) + 0x48 + static_cast<uintptr_t>(which) * 4;
}

int __cdecl HostGetResource(int which, unsigned* value)
{
    if (!value) return 0;
    const uintptr_t at = ResourceAddress(which);
    uint32_t v = 0;
    if (!at || !game::SafeReadU32(at, &v)) return 0;
    *value = v;
    return 1;
}

// Kept apart from C++ objects so __try is allowed. The game's own add / take (FUN_006ced30 / FUN_006ced70) are plain
// stores to the same field.
bool SafeWriteU32(uintptr_t at, uint32_t v)
{
    __try {
        *reinterpret_cast<volatile uint32_t*>(at) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int __cdecl HostSetResource(const DS3ModHost* host, int which, unsigned value)
{
    const int mi = ModIndexOf(host);
    if (mi < 0) return 0;
    const uintptr_t at = ResourceAddress(which);
    uint32_t old = 0;
    if (!at || !game::SafeReadU32(at, &old)) return 0;
    if (old == value) return 1;
    if (!SafeWriteU32(at, value)) return 0;
    const LONG n = InterlockedIncrement(&g_resourceSets[mi]);
    if (n <= kLoggedResourceSets)
        Log("mod %s: %s %u -> %u", IdOf(host), game::kResourceNames[which], old, value);
    if (n == kLoggedResourceSets)
        Log("mod %s: %d resource changes logged; further ones are not", IdOf(host), kLoggedResourceSets);
    return 1;
}

const char* __cdecl HostResourceName(int which)
{
    return which >= 0 && which < game::kResourceCount ? game::kResourceNames[which] : "";
}

int __cdecl HostItemCount(int list)
{
    return game::ItemListCount(game::LocalPlayer(), list);
}

int __cdecl HostGetItem(int list, int index, DS3Item* out)
{
    if (!out) return 0;
    game::Guid g{};
    uint32_t count = 0;
    if (!game::ItemListEntry(game::LocalPlayer(), list, index, &g, &count)) return 0;
    std::memcpy(&out->id, &g, sizeof(out->id));
    out->count = count;
    game::ItemName(g, out->name, sizeof(out->name));
    return 1;
}

int __cdecl HostItemName(const DS3Guid* id, char* buf, unsigned size)
{
    if (!buf || !size) return 0;
    buf[0] = '\0';
    if (!id) return 0;
    game::Guid g{};
    std::memcpy(&g, id, sizeof(g));
    return game::ItemName(g, buf, size) ? 1 : 0;
}

// ---- Seal offers (API 3) ----

int __cdecl HostSealOffers(const DS3ModHost* host, DS3SealOffersFn fn, void* user)
{
    const int mi = ModIndexOf(host);
    if (mi < 0) return 0;
    if (g_closing[mi] && fn) {
        Log("mod %s: seal offers refused: the mod is being unloaded", IdOf(host));
        return 0;
    }
    return SealOffersSet(mi, IdOf(host), fn, user);
}

// ---- Frames ----

int __cdecl HostOnFrame(const DS3ModHost* host, DS3FrameFn fn, void* user)
{
    const int mi = ModIndexOf(host);
    if (mi < 0) return 0;
    if (g_closing[mi] && fn) {
        Log("mod %s: frame callback refused: the mod is being unloaded", IdOf(host));
        return 0;
    }
    // The user value first: the render thread reads fn, then user.
    g_frames[mi].user = user;
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&g_frames[mi].fn), reinterpret_cast<void*>(fn));
    Log("mod %s: frame callback %s", IdOf(host), fn ? "registered" : "removed");
    return 1;
}

bool Drawing(const DS3Frame* f)
{
    return f && f == g_frame && g_canvas;
}

float __cdecl FrameText(const DS3Frame* f, float x, float y, const char* text, unsigned color)
{
    return Drawing(f) && text ? g_canvas->Text(x, y, text, color) : x;
}

void __cdecl FrameRect(const DS3Frame* f, float x, float y, float w, float h, unsigned color)
{
    if (Drawing(f) && w > 0 && h > 0) g_canvas->Rect(x, y, w, h, color);
}

float __cdecl FrameTextWidth(const DS3Frame* f, const char* text)
{
    return Drawing(f) && text ? g_canvas->TextWidth(text) : 0.0f;
}

float __cdecl FrameSetTextSize(const DS3Frame* f, float px)
{
    return Drawing(f) ? g_canvas->SetTextSize(px) : 0.0f;
}

float __cdecl FrameSetFont(const DS3Frame* f, const char* face, float px)
{
    return Drawing(f) ? g_canvas->SetFont(face, px) : 0.0f;
}

void __cdecl FrameGradient(const DS3Frame* f, float x, float y, float w, float h, unsigned from, unsigned to,
                           int vertical)
{
    if (Drawing(f) && w > 0 && h > 0) g_canvas->Gradient(x, y, w, h, from, to, vertical != 0);
}

float __cdecl FrameLineHeight(const DS3Frame* f)
{
    if (!Drawing(f)) return 0.0f;
    const float h = g_canvas->LineHeight();
    // No text drawn yet: pick the default size (as the canvas does) to measure it.
    return h > 0 ? h : g_canvas->SetTextSize(std::max(12.0f, static_cast<float>(static_cast<int>(f->height / 54.0f + 0.5f))));
}

bool CallFrame(DS3FrameFn fn, const DS3Frame* f, void* user)
{
    __try {
        fn(f, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void RunFrames(OverlayModCanvas& canvas)
{
    // Hot reload works here, between frames, on the game's main thread (mods.cpp).
    if (g_tick) g_tick();
    // deltaSeconds from the performance counter: GetTickCount only moves every ~16 ms, so at 60 fps it often gave 0
    // (seen in game as "0 fps" in the self-test).
    static LARGE_INTEGER freq = {};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    DS3Frame f{};
    f.size = sizeof(DS3Frame);
    f.width = canvas.Width();
    f.height = canvas.Height();
    f.timeMs = GetTickCount();
    f.deltaSeconds = g_lastFrameAt && freq.QuadPart
                         ? static_cast<float>(static_cast<double>(now.QuadPart - g_lastFrameAt) / freq.QuadPart)
                         : 0.0f;
    f.state = -1;  // read below, only if a mod is listening
    f.text = &FrameText;
    f.rect = &FrameRect;
    f.textWidth = &FrameTextWidth;
    f.setTextSize = &FrameSetTextSize;
    f.lineHeight = &FrameLineHeight;
    f.setFont = &FrameSetFont;
    f.gradient = &FrameGradient;
    g_lastFrameAt = now.QuadPart;

    g_frame = &f;
    g_canvas = &canvas;
    for (int i = 0; i < kMaxMods; ++i) {
        DS3FrameFn fn = g_frames[i].fn;
        if (!fn) continue;
        if (f.state < 0) f.state = CurrentState();
        canvas.ResetTextSize();
        if (!CallFrame(fn, &f, g_frames[i].user)) {
            g_frames[i].fn = nullptr;
            Log("mod %s: its frame callback crashed (caught); switched off until the game restarts",
                IdOf(g_hostOf[i]));
        }
    }
    g_frame = nullptr;
    g_canvas = nullptr;

    // The hot-reload notice, top left, over the mods.
    if (g_noticeAt && GetTickCount() - g_noticeAt < kNoticeMs) {
        canvas.ResetTextSize();
        const float lh = canvas.LineHeight() > 0 ? canvas.LineHeight() : canvas.SetTextSize(f.height / 54.0f);
        const float x = lh, y = lh;
        const float w = canvas.TextWidth(g_notice);
        canvas.Rect(x - lh * 0.4f, y - lh * 0.15f, w + lh * 0.8f, lh * 1.3f, 0xA0000000u);
        canvas.Text(x, y, g_notice, g_noticeBad ? 0xFFFF8060u : 0xFF90E0FFu);
    }
}

}  // namespace

void SdkFillHost(DS3ModHost& host, int modIndex)
{
    if (modIndex < 0 || modIndex >= kMaxMods) return;
    host.hook = &HostHook;
    host.patch = &HostPatch;
    host.gameState = &HostGameState;
    host.localPlayer = &HostLocalPlayer;
    host.getResource = &HostGetResource;
    host.setResource = &HostSetResource;
    host.resourceName = &HostResourceName;
    host.itemCount = &HostItemCount;
    host.getItem = &HostGetItem;
    host.itemName = &HostItemName;
    host.onFrame = &HostOnFrame;
    host.sealOffers = &HostSealOffers;
    g_hostOf[modIndex] = &host;
    g_closing[modIndex] = false;  // a (re)load
    g_anyHost = true;
}

bool SdkWanted()
{
    return g_anyHost;
}

void SdkSetReloadable(int mi, bool reloadable)
{
    if (mi >= 0 && mi < kMaxMods) g_reloadable[mi] = reloadable;
}

void SdkModStop(int mi)
{
    if (mi < 0 || mi >= kMaxMods) return;
    g_closing[mi] = true;
    g_frames[mi].fn = nullptr;
    g_frames[mi].user = nullptr;
    SealOffersDrop(mi);  // its offers stop being listed; the checkout refusal stays
    bool queued = false;
    for (int i = 0; i < g_recordCount; ++i) {
        HookRecord& r = g_records[i];
        if (r.installed && r.mod == mi && MH_QueueDisableHook(r.target) == MH_OK) queued = true;
    }
    // One pass with every other thread frozen; MinHook moves any thread that is inside the bytes it puts back.
    if (queued) {
        const MH_STATUS st = MH_ApplyQueued();
        if (st != MH_OK) Log("mod %s: disabling its hooks: %s", IdOf(g_hostOf[mi]), MH_StatusToString(st));
    }
    // Only now: before, a call reaching a stub with active = 0 would jump to the game's hooked bytes, i.e. the stub.
    for (int i = 0; i < g_recordCount; ++i) {
        HookRecord& r = g_records[i];
        if (r.installed && r.mod == mi) InterlockedExchange(&r.active, 0);
    }
}

bool SdkModIdle(int mi, bool* unsafe)
{
    *unsafe = mi >= 0 && mi < kMaxMods && g_untracked[mi];
    bool idle = true;
    for (int i = 0; i < g_recordCount; ++i) {
        const HookRecord& r = g_records[i];
        if (!r.installed || r.mod != mi) continue;
        if (r.lost) *unsafe = true;
        if (r.busy) idle = false;
    }
    return idle;
}

void SdkModRelease(int mi)
{
    if (mi < 0 || mi >= kMaxMods) return;
    const char* id = IdOf(g_hostOf[mi]);
    g_frames[mi].fn = nullptr;  // again: a thread of the mod may have set one meanwhile
    g_frames[mi].user = nullptr;
    int hooks = 0, patches = 0;
    for (int i = 0; i < g_recordCount; ++i) {
        HookRecord& r = g_records[i];
        if (!r.installed || r.mod != mi) continue;
        const MH_STATUS st = MH_RemoveHook(r.target);
        if (st != MH_OK) Log("mod %s: removing its hook at 0x%08X: %s", id, r.address, MH_StatusToString(st));
        if (r.claimAdded) ReleaseRange(r.address, kHookBytes, id);
        r.installed = false;
        r.mod = -1;
        r.detour = nullptr;
        ++hooks;
    }
    for (PatchRecord& p : g_patchRecords) {
        if (p.owner != mi + 1) continue;
        uint8_t now[kMaxPatchBytes];
        if (memcmp(p.original, p.written, p.len) != 0 && ReadGameBytes(p.address, now, p.len)) {
            if (memcmp(now, p.written, p.len) == 0) {
                if (WriteGameBytes(p.address, p.original, p.len))
                    ++patches;
                else
                    Log("mod %s: putting back 0x%08X FAILED", id, p.address);
            } else {
                Log("mod %s: 0x%08X is not what the mod wrote any more; left as it is", id, p.address);
            }
        }
        if (p.claimAdded) ReleaseRange(p.address, p.len, id);
        p.owner = 0;
    }
    g_untracked[mi] = false;
    Log("mod %s: %d hook(s) removed, %d patch(es) put back", id, hooks, patches);
}

void SdkNotice(const char* text, bool bad)
{
    strncpy_s(g_notice, text, _TRUNCATE);
    g_noticeBad = bad;
    g_noticeAt = GetTickCount() | 1;
}

void SdkSetFrameTick(void (*tick)())
{
    g_tick = tick;
}

void InstallSdk()
{
    if (!g_anyHost) return;
    if (!OverlayInstalled()) {
        Log("mods: the overlay is not installed, so frame callbacks will not run (see the overlay lines above)");
        return;
    }
    OverlaySetModFrame(&RunFrames);
}
