#pragma once

#include "../sdk/ds3mod.h"

// The SDK's API 2 calls for drop-in DLL mods (sdk/ds3mod.h): hook and patch with the patch mods' checks, game reads,
// and frame callbacks that draw through the overlay. mods.cpp fills API 1 of each mod's host and calls SdkFillHost
// for the rest, before the mod's DS3ModInit.

// Fills the API 2 fields of mod `modIndex`'s host (its modId must already be set).
void SdkFillHost(DS3ModHost& host, int modIndex);
// Whether any DLL mod got a host: the overlay is then installed for their frame callbacks.
bool SdkWanted();
// After InstallOverlay: runs the mods' frame callbacks from the overlay. Logs if the overlay is not there.
void InstallSdk();

// Hot reload (mods.cpp). All on the game's main thread, from the frame tick below.
// Before a mod's DS3ModInit: whether it can be unloaded (its hooks then go through counting stubs; other mods' hooks
// point the game straight at their detours, as before).
void SdkSetReloadable(int modIndex, bool reloadable);
// Step 1 of an unload: drops the mod's frame callback and disables its hooks (the game's bytes come back at once).
void SdkModStop(int modIndex);
// Whether none of the mod's hooks is mid-call. *unsafe: a call or a patch went uncounted, so it must never be freed.
bool SdkModIdle(int modIndex, bool* unsafe);
// Removes the mod's hooks and gives back their claims; puts back the bytes of its patches (where it still holds them).
void SdkModRelease(int modIndex);
// Shows a short line top-left for a few seconds (bad: in a warning colour).
void SdkNotice(const char* text, bool bad);
// Called once per frame before the mods' frame callbacks.
void SdkSetFrameTick(void (*tick)());
