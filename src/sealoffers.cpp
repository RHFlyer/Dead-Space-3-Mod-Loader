// The seal-offer core: the three store hooks that sit in the store's code, which no mod may touch (patches.cpp),
// driven by the SDK's sealOffers. The rest of the Ration Seal Store is the add-on in sdk/seal_store.
//  - Checkout (FUN_00a4c4e0): refuses a money checkout that includes any seal offer. Installed FIRST, before anything
//    can be listed, and never removed.
//  - GetOffers (FUN_00a4ef60): after the real call, appends the offers the mods' callbacks list, with every money
//    field filled here (no currency, price 0, not owned, purchasable).
//  - OfferStatus (FUN_00a29bc0): gives those offers their pack type and makes them seal-purchasable.
// Only DS3SEALS: IDs and the two stock seal packs (which the Steam catalogue lacks) are ever added; real offers and
// entitlements are never touched. Nothing here is installed until a mod calls sealOffers.

#include "sealoffers.h"

#include "game.h"
#include "log.h"
#include "mods.h"
#include "patches.h"

#include <windows.h>

#include <MinHook.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr const char* kUltraOfferId = "OFB-EAST:109546252";  // pack type 3; the game maps this ID itself
constexpr const char* kEpicOfferId = "OFB-EAST:109546253";   // pack type 4
constexpr const char* kModPrefix = "DS3SEALS:";               // can never match a real EA offer
constexpr size_t kMaxIdChars = 47;
constexpr size_t kMaxNameChars = 63;
constexpr size_t kMaxDescriptionChars = 199;
constexpr int kMaxOffersPerMod = 32;
constexpr int kMaxListed = 64;       // per store open, over all mods (the game's buffer holds 50 records in all)
constexpr int kMaxEverListed = 128;  // IDs the checkout refusal remembers

// FUN_00a4ef60: __thiscall int GetOffers(OfferRecord* buf, int max, int* count)
constexpr uintptr_t kGetOffersVa = 0x00A4EF60;
constexpr unsigned char kGetOffersBytes[] = {0x55, 0x8B, 0xEC, 0x8B, 0x89, 0x38, 0x01, 0x00, 0x00,
                                             0x33, 0xC0, 0x85, 0xC9, 0x74, 0x08};
// FUN_00a4c4e0: __stdcall int Checkout(const char** offerIds, int count): the money checkout
constexpr uintptr_t kCheckoutVa = 0x00A4C4E0;
constexpr unsigned char kCheckoutBytes[] = {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x0C, 0x8B, 0x4D,
                                            0x08, 0x50, 0x51, 0xE8, 0x10, 0xC9, 0x23, 0x00};
constexpr int kCheckoutRefused = -1;  // any non-zero makes the store manager report a failed purchase
// FUN_00a29bc0: __cdecl int OfferStatus(StringHolder* offerId). Only caller 0xA2A319 in the store manager's offer loop,
// where ESI = the store entry just added, whose type ([ESI+4]) was written by the instruction before the call.
constexpr uintptr_t kOfferStatusVa = 0x00A29BC0;
constexpr unsigned char kOfferStatusBytes[] = {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x8B, 0x00,
                                               0x56, 0xBE, 0x03, 0x00, 0x00, 0x00, 0x85, 0xC0};
constexpr int kStatusSealPurchasable = 4;  // the loop then sets [ESI+9]
constexpr uintptr_t kStoreManagerPtrVa = 0x011D3B80;  // entries at +0x10, stride 0x9B0, count +0x26C10
constexpr uintptr_t kStoreEntriesOffset = 0x10;
constexpr uintptr_t kStoreEntryStride = 0x9B0;
constexpr uintptr_t kStoreCountOffset = 0x26C10;
constexpr uintptr_t kEntryTypeOffset = 0x4;
// A mod's own pack types need its hooks on these (sealOffers in ds3mod.h).
constexpr uintptr_t kBuyWithSealsVa = DS3_ADDR_BUY_WITH_SEALS;
constexpr uintptr_t kSealPriceVa = DS3_ADDR_SEAL_PRICE;

using FreeFn = void(__cdecl*)(void*);
struct StringHolder {
    const char* ptr;
    uint32_t length;
    uint32_t capacity;  // equals length in every real record
    FreeFn free;
};
struct OfferRecord {
    StringHolder name;
    StringHolder offerId;
    StringHolder description;
    StringHolder currency;
    StringHolder unused;
    float price;
    int32_t type;  // 3 for every real offer
    uint8_t owned;
    uint8_t canPurchase;
    uint8_t pad[2];
};
static_assert(sizeof(OfferRecord) == 0x5C, "offer record layout");

using GetOffersFn = int(__fastcall*)(void* self, void* edx, OfferRecord* buf, int max, int* count);
using CheckoutFn = int(__stdcall*)(const char** offerIds, int count);
using OfferStatusFn = int(__cdecl*)(StringHolder* offerId);
GetOffersFn g_origGetOffers = nullptr;
CheckoutFn g_origCheckout = nullptr;
OfferStatusFn g_origOfferStatus = nullptr;

struct ModCallback {
    DS3SealOffersFn fn;
    void* user;
    char id[32];
};
ModCallback g_callbacks[kMaxMods] = {};

enum class CoreState { NotInstalled, Ready, Failed };
CoreState g_state = CoreState::NotInstalled;
bool g_refusalIn = false;  // the checkout hook is in (stays in, whatever happens later)

// What the last GetOffers listed: the strings the records point at, and each ID's pack type for OfferStatus. The
// store manager copies the records and frees them (with NoFree) before GetOffers can run again.
struct Listed {
    char id[kMaxIdChars + 1];
    char name[kMaxNameChars + 1];
    char description[kMaxDescriptionChars + 1];
    int packType;
};
Listed g_listed[kMaxListed] = {};
int g_listedCount = 0;

char g_everListed[kMaxEverListed][kMaxIdChars + 1] = {};
int g_everListedCount = 0;
LONG g_getOffersCalls = 0;

void __cdecl NoFree(void*) {}

StringHolder MakeString(const char* s)
{
    auto len = static_cast<uint32_t>(std::strlen(s));
    return {s, len, len, &NoFree};
}

// Copies a string from game or mod memory; false if unreadable or longer than `max` characters (nothing is cut, so an
// ID can never be shortened into another one). Bytes below 0x20 and 0x7F end up as '?'.
bool SafeCopy(const char* src, char* dst, size_t max, bool* tooLong = nullptr)
{
    if (tooLong) *tooLong = false;
    dst[0] = '\0';
    if (!src) return false;
    __try {
        size_t i = 0;
        for (; src[i]; ++i) {
            if (i >= max) {
                if (tooLong) *tooLong = true;
                dst[0] = '\0';
                return false;
            }
            unsigned char c = static_cast<unsigned char>(src[i]);
            dst[i] = (c < 0x20 || c == 0x7F) ? '?' : static_cast<char>(c);
        }
        dst[i] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        dst[0] = '\0';
        return false;
    }
}

// Like SafeCopy, but a long text is cut instead of refused (names and descriptions).
bool SafeCopyCut(const char* src, char* dst, size_t max)
{
    dst[0] = '\0';
    if (!src) return false;
    __try {
        size_t i = 0;
        for (; i < max && src[i]; ++i) {
            unsigned char c = static_cast<unsigned char>(src[i]);
            dst[i] = (c < 0x20 || c == 0x7F) ? '?' : static_cast<char>(c);
        }
        dst[i] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        dst[0] = '\0';
        return false;
    }
}

bool IsStockSealId(const char* id)
{
    return std::strcmp(id, kUltraOfferId) == 0 || std::strcmp(id, kEpicOfferId) == 0;
}

bool IsModSealId(const char* id)
{
    const size_t n = std::strlen(kModPrefix);
    if (std::strncmp(id, kModPrefix, n) != 0 || !id[n]) return false;
    for (const char* p = id + n; *p; ++p) {
        const char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

// The checkout refusal's test: any DS3SEALS: ID (well-formed or not), either stock seal pack, or anything listed.
bool IsSealOfferId(const char* rawId)
{
    char id[64];
    bool tooLong = false;
    if (!SafeCopy(rawId, id, sizeof(id) - 1, &tooLong)) {
        // An ID too long to copy that starts like ours is refused too.
        char head[16];
        return tooLong && SafeCopyCut(rawId, head, std::strlen(kModPrefix)) &&
               std::strcmp(head, kModPrefix) == 0;
    }
    if (std::strncmp(id, kModPrefix, std::strlen(kModPrefix)) == 0 || IsStockSealId(id)) return true;
    for (int i = 0; i < g_everListedCount; ++i) {
        if (std::strcmp(id, g_everListed[i]) == 0) return true;
    }
    return false;
}

void RememberListed(const char* id)
{
    for (int i = 0; i < g_everListedCount; ++i) {
        if (std::strcmp(id, g_everListed[i]) == 0) return;
    }
    if (g_everListedCount < kMaxEverListed) strcpy_s(g_everListed[g_everListedCount++], id);
}

bool InCatalogue(const OfferRecord* buf, int count, const char* id)
{
    char got[64];
    for (int i = 0; i < count; ++i) {
        if (SafeCopy(buf[i].offerId.ptr, got, sizeof(got) - 1) && std::strcmp(got, id) == 0) return true;
    }
    return false;
}

// Calls mod i's callback under __try. A crash switches its offers off.
int AskMod(int i, DS3SealOffer* out, int max)
{
    ModCallback& cb = g_callbacks[i];
    std::memset(out, 0, sizeof(DS3SealOffer) * max);
    int n = 0;
    DWORD code = 0;
    __try {
        n = cb.fn(out, max, cb.user);
    } __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        Log("seal offers: mod %s: its offer callback crashed (%08lX); its offers are switched off", cb.id, code);
        cb.fn = nullptr;
        return 0;
    }
    if (n < 0) n = 0;
    if (n > max) {
        Log("seal offers: mod %s: its callback said %d offers, room was %d; the first %d are used", cb.id, n, max, max);
        n = max;
    }
    return n;
}

// Checks one offer and, if it passes, appends it. Every refusal names the reason.
void Append(const ModCallback& cb, const DS3SealOffer& o, OfferRecord* buf, int max, int* count)
{
    char id[kMaxIdChars + 1];
    bool tooLong = false;
    if (!SafeCopy(o.id, id, kMaxIdChars, &tooLong)) {
        Log("seal offers: mod %s: an offer with %s ID skipped", cb.id, tooLong ? "a too long" : "no readable");
        return;
    }
    if (!IsModSealId(id) && !IsStockSealId(id)) {
        Log("seal offers: mod %s: %s refused: only DS3SEALS:<A-Z 0-9 _> IDs and the two stock seal packs can be "
            "listed",
            cb.id, id);
        return;
    }
    const int type = o.packType;
    if (type < DS3_PACK_RESOURCE || type > DS3_PACK_LAST_MOD) {
        Log("seal offers: mod %s: %s refused: pack type %d is not 2..%d", cb.id, id, type, DS3_PACK_LAST_MOD);
        return;
    }
    if (IsStockSealId(id) && type != (std::strcmp(id, kUltraOfferId) == 0 ? DS3_PACK_ULTRA : DS3_PACK_EPIC)) {
        Log("seal offers: mod %s: %s refused: the stock pack's type is fixed by the game", cb.id, id);
        return;
    }
    if (type >= DS3_PACK_FIRST_MOD && !(ClaimHeldBy(kBuyWithSealsVa, cb.id) && ClaimHeldBy(kSealPriceVa, cb.id))) {
        Log("seal offers: mod %s: %s (type %d) left out: the mod does not hold hooks on both the seal purchase "
            "(0x%08X) and the seal price (0x%08X), so the game would charge for it and give nothing",
            cb.id, id, type, static_cast<unsigned>(kBuyWithSealsVa), static_cast<unsigned>(kSealPriceVa));
        return;
    }
    if (InCatalogue(buf, *count, id)) {
        Log("seal offers: %s already in the catalogue; not adding", id);
        return;
    }
    if (*count >= max || g_listedCount >= kMaxListed) {
        Log("seal offers: no room for %s (count=%d max=%d)", id, *count, max);
        return;
    }
    Listed& l = g_listed[g_listedCount];
    strcpy_s(l.id, id);
    if (!SafeCopyCut(o.name, l.name, kMaxNameChars) || !l.name[0]) {
        Log("seal offers: mod %s: %s refused: no readable name", cb.id, id);
        return;
    }
    if (!SafeCopyCut(o.description, l.description, kMaxDescriptionChars)) l.description[0] = '\0';
    l.packType = type;
    ++g_listedCount;
    RememberListed(id);  // before it can be shown: the refusal knows it from now on

    OfferRecord& r = buf[*count];
    std::memset(&r, 0, sizeof(r));
    r.name = MakeString(l.name);
    r.offerId = MakeString(l.id);
    r.description = MakeString(l.description);
    r.currency = MakeString("");
    r.unused = {};
    r.price = 0.0f;
    r.type = 3;  // as every real record
    r.owned = 0;
    r.canPurchase = 1;
    Log("  added %s \"%s\" (type %d, mod %s) at index %d", l.id, l.name, type, cb.id, *count);
    ++*count;
}

int __fastcall HookGetOffers(void* self, void* edx, OfferRecord* buf, int max, int* count)
{
    int ret = g_origGetOffers(self, edx, buf, max, count);
    LONG call = InterlockedIncrement(&g_getOffersCalls);
    Log("GetOffers #%ld: ret=%d max=%d count=%d", call, ret, max, count ? *count : -1);
    g_listedCount = 0;
    if (ret != 0 || !buf || !count || *count < 0 || max <= 0) return ret;
    static DS3SealOffer offers[kMaxOffersPerMod];
    for (int i = 0; i < kMaxMods; ++i) {
        if (!g_callbacks[i].fn) continue;
        const int n = AskMod(i, offers, kMaxOffersPerMod);
        for (int k = 0; k < n; ++k) Append(g_callbacks[i], offers[k], buf, max, count);
    }
    return ret;
}

int __stdcall HookCheckout(const char** offerIds, int count)
{
    bool hasSealOffer = false;
    for (int i = 0; offerIds && i < count; ++i) {
        char id[64];
        if (!SafeCopyCut(offerIds[i], id, sizeof(id) - 1)) strcpy_s(id, "(unreadable)");
        Log("Checkout offer[%d] = %s", i, id);
        if (IsSealOfferId(offerIds[i])) hasSealOffer = true;
    }
    if (hasSealOffer) {
        Log("Checkout REFUSED: seal offers are bought with Ration Seals only, never money");
        return kCheckoutRefused;
    }
    int ret = g_origCheckout(offerIds, count);
    Log("Checkout passed through -> %d", ret);
    return ret;
}

// Called from the stub below with the caller's ESI (the store entry just added). For offers the last GetOffers listed
// only: their pack type and seal-purchasable, the same as the game does for Ultra (3) and Epic (4). Everything else
// gets the game's answer.
int __cdecl OfferStatus(StringHolder* offerId, uint8_t* entry)
{
    int status = g_origOfferStatus(offerId);
    char id[64];
    if (!offerId || !SafeCopy(offerId->ptr, id, sizeof(id) - 1)) return status;
    int packType = 0;
    for (int i = 0; i < g_listedCount; ++i) {
        if (std::strcmp(g_listed[i].id, id) == 0) packType = g_listed[i].packType;
    }
    if (!packType || IsStockSealId(id)) return status;  // the game maps Ultra and Epic itself

    // Only touch the entry if it really is the newest one in the store manager and still has no type.
    uint32_t manager = 0, n = 0;
    uintptr_t newest = 0;
    if (game::SafeReadU32(kStoreManagerPtrVa, &manager) && manager &&
        game::SafeReadU32(manager + kStoreCountOffset, &n) && n > 0) {
        newest = manager + kStoreEntriesOffset + (n - 1) * kStoreEntryStride;
    }
    uint32_t type = 0xFFFFFFFF;
    if (reinterpret_cast<uintptr_t>(entry) != newest || !game::SafeReadU32(newest + kEntryTypeOffset, &type) ||
        type != 0) {
        Log("%s: entry check failed (entry=%p newest=%08X type=%08X); left as the game set it", id, entry,
            static_cast<unsigned>(newest), type);
        return status;
    }
    *reinterpret_cast<uint32_t*>(entry + kEntryTypeOffset) = static_cast<uint32_t>(packType);
    return kStatusSealPurchasable;
}

// Detour for FUN_00a29bc0: hands the caller's ESI to OfferStatus. [esp+4] is the offerId argument on entry.
__declspec(naked) void HookOfferStatusStub()
{
    __asm {
        push esi                 // entry
        push dword ptr [esp + 8] // offerId (was [esp+4] before the push)
        call OfferStatus
        add esp, 8
        ret
    }
}

// Claim, byte check, create and enable one hook. False (logged) if any step fails; a failed hook is removed again.
bool InstallOne(const char* name, uintptr_t va, const unsigned char* bytes, size_t len, void* detour, void** orig)
{
    if (!game::BytesMatch(va, bytes, len)) {
        Log("seal offers: %s does not match the expected bytes (different game build?)", name);
        return false;
    }
    if (!ClaimHook(name, va)) return false;
    void* target = reinterpret_cast<void*>(va);
    MH_STATUS st = MH_CreateHook(target, detour, orig);
    if (st == MH_OK) {
        st = MH_EnableHook(target);
        if (st != MH_OK) MH_RemoveHook(target);
    }
    if (st != MH_OK) {
        ReleaseRange(va, 16, "the proxy's own hooks");
        Log("seal offers: hooking %s failed: %s", name, MH_StatusToString(st));
        return false;
    }
    return true;
}

// Once, on the first sealOffers call. The refusal first; the catalogue hooks only with it in.
void InstallCore()
{
    g_state = CoreState::Failed;
    if (!PatchesAllowed()) {
        Log("seal offers: the game's code is not decrypted; nothing installed");
        return;
    }
    if (!game::InitMinHook()) return;
    g_refusalIn = InstallOne("Checkout", kCheckoutVa, kCheckoutBytes, sizeof(kCheckoutBytes),
                             reinterpret_cast<void*>(&HookCheckout), reinterpret_cast<void**>(&g_origCheckout));
    if (!g_refusalIn) {
        Log("seal offers: the money-checkout refusal could not be installed, so no seal offer will ever be listed");
        return;
    }
    Log("seal offers: money checkout hooked: refuses any checkout with a seal offer in it");
    // OfferStatus before GetOffers: an offer listed without its status would show but could not be bought.
    if (!InstallOne("OfferStatus", kOfferStatusVa, kOfferStatusBytes, sizeof(kOfferStatusBytes),
                    reinterpret_cast<void*>(&HookOfferStatusStub), reinterpret_cast<void**>(&g_origOfferStatus)) ||
        !InstallOne("GetOffers", kGetOffersVa, kGetOffersBytes, sizeof(kGetOffersBytes),
                    reinterpret_cast<void*>(&HookGetOffers), reinterpret_cast<void**>(&g_origGetOffers))) {
        Log("seal offers: the catalogue hooks could not be installed; no seal offer will be listed (the checkout "
            "refusal stays)");
        return;
    }
    Log("seal offers: catalogue and offer status hooked (lists the mods' seal offers)");
    g_state = CoreState::Ready;
}

}  // namespace

int SealOffersSet(int modIndex, const char* modId, DS3SealOffersFn fn, void* user)
{
    if (modIndex < 0 || modIndex >= kMaxMods) return 0;
    ModCallback& cb = g_callbacks[modIndex];
    strncpy_s(cb.id, modId ? modId : "?", _TRUNCATE);
    if (!fn) {
        cb.fn = nullptr;
        cb.user = nullptr;
        Log("seal offers: mod %s: callback removed", cb.id);
        return 1;
    }
    if (g_state == CoreState::NotInstalled) InstallCore();
    if (g_state != CoreState::Ready) {
        Log("seal offers: mod %s: refused: the store hooks are not in (see above)", cb.id);
        return 0;
    }
    cb.user = user;
    cb.fn = fn;
    Log("seal offers: mod %s: offer callback registered", cb.id);
    return 1;
}

void SealOffersDrop(int modIndex)
{
    if (modIndex < 0 || modIndex >= kMaxMods || !g_callbacks[modIndex].fn) return;
    g_callbacks[modIndex].fn = nullptr;
    g_callbacks[modIndex].user = nullptr;
    Log("seal offers: mod %s: offers no longer listed", g_callbacks[modIndex].id);
}
