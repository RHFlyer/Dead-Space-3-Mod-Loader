#pragma once

#include <cstddef>
#include <cstdint>

// Patch mods: [patch*] sections in a drop-in mod's mod.ini (format in sdk\README.md) that change bytes in
// deadspace3.exe without any code. Each patch names an address, the bytes expected there and what to write: fixed
// bytes, fixed bytes switched by a bool setting, or a setting's value. A mod's patches are all checked against the
// game before any is written, and the mod is skipped if one does not fit. Only the exe's .text / .rdata / .data are
// patchable, never the store, entitlement, login or Origin SDK code, and never bytes another patch or one of the
// proxy's own hooks has taken.

constexpr int kMaxPatches = 64;      // over all mods
constexpr int kMaxPatchBytes = 64;   // per patch, expected and written

// Code ranges taken by the proxy's hooks and by applied patches, so two never write the same bytes. Returns false if
// [va, va + size) overlaps a range someone else holds (*holder says who); a range already held by `owner` is fine.
// *added: whether a new claim was recorded (false when `owner` already held the whole range).
bool ClaimRange(uintptr_t va, size_t size, const char* owner, const char** holder = nullptr, bool* added = nullptr);
// The first 16 bytes of a function the proxy hooks (MinHook's jump plus the instructions its trampoline moves). False,
// logged, if a patch mod already holds them: the hook is then not installed.
bool ClaimHook(const char* name, uintptr_t va);

// Gives back one range `owner` claimed, exactly as claimed (an SDK hook that could not be installed after all).
void ReleaseRange(uintptr_t va, size_t size, const char* owner);
// Whether `owner` holds a claim that covers the byte at `va` (sealOffers: does the mod hook the seal purchase?).
bool ClaimHeldBy(uintptr_t va, const char* owner);

// For the SDK's hook and patch calls (sdk.cpp), which get the same checks as patch mods:
// whether the decryption gate below is open;
bool PatchesAllowed();
// whether a mod may change [va, va + size): 1 to kMaxPatchBytes bytes, inside the exe's .text / .rdata / .data (only
// .text if codeOnly), not in a refused range, not next to protected text. If not, `why` says so in a sentence, and
// *isProtected says whether it was one of the last two (accounts, store or paid content: the panel shows those);
bool ModRangeAllowed(uintptr_t va, size_t size, bool codeOnly, char* why, size_t whySize,
                     bool* isProtected = nullptr);
// and crash-proof reads and writes of the game's bytes (writes: page made writable for the moment, atomic when the
// bytes lie in one aligned 8-byte block, instruction cache flushed).
bool ReadGameBytes(uintptr_t va, void* out, size_t size);
bool WriteGameBytes(uintptr_t va, const uint8_t* bytes, size_t size);

// The proxy calls this with true once the game's .text is known to be decrypted (TextState in proxy.cpp). Until then
// ApplyModPatches writes nothing.
void AllowPatches(bool decrypted);

// Reads every [patch*] section of mod `modIndex`'s manifest (its settings must be parsed already). False if any
// patch cannot be used; the reason is logged and none of the mod's patches are kept.
bool ParseModPatches(int modIndex, const wchar_t* manifest);
// Forgets the patches of the last mod parsed (when that mod is skipped after all).
void DropModPatches(int modIndex);
int ModPatchCount(int modIndex);
// Checks every patch of the mod against the game's bytes, then writes them all. False (logged) if any does not fit;
// nothing is written then.
bool ApplyModPatches(int modIndex);
// After the panel changed setting `settingIndex`: rewrites the applied patches that take their bytes from it.
void ReapplyPatchesFor(int settingIndex);
