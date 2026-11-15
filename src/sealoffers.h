#pragma once

#include "../sdk/ds3mod.h"

// The seal-offer core behind the SDK's sealOffers (API 3). The bench store's catalogue and checkout code lies in a
// range no mod may hook or patch (patches.cpp), so this is the only way a mod can list an offer there, and it can only
// list offers that cost Ration Seals. Nothing is installed until a mod asks. Then, in this order: the hook on the
// money checkout (refuses any checkout that includes a seal offer), then the catalogue hooks. If the checkout hook
// can't be installed, nothing else is. The checkout hook is never removed.

// Registers (fn) or drops (fn = NULL) mod `modIndex`'s offer callback. 1 if done, 0 if refused (logged).
int SealOffersSet(int modIndex, const char* modId, DS3SealOffersFn fn, void* user);
// Hot reload, unload step 1: the mod's callback is dropped; its offers are no longer listed. The refusal stays.
void SealOffersDrop(int modIndex);
