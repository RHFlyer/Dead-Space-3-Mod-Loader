# Dead Space 3: what modders already know

Addresses and layouts that the DS3 Mod Loader, its samples and the memory inspector use. They are for the current
Steam version of `deadspace3.exe` (32-bit, loaded at `0x00400000`). Another version of the game may differ: the
loader checks the expected bytes before every hook or patch, and refuses when they don't match.

**Use the SDK first.** Most of this is already wrapped by calls in `sdk/ds3mod.h`, which check the game state and
are safer than reading raw addresses yourself:

| Want | SDK call |
|---|---|
| Is a level loaded, is the game paused | `host->gameState()` |
| The player object | `host->localPlayer()` |
| Read or set the six resources (incl. Ration Seals) | `host->getResource()`, `host->setResource()`, `host->resourceName()` |
| The inventory and the list of discovered circuits and parts | `host->itemCount()`, `host->getItem()`, `host->itemName()` |
| Draw text and boxes on screen | `host->onFrame()` and the `frame` calls |
| Run code when the game calls one of its functions; change bytes | `host->hook()`, `host->patch()` |

## The player

| What | Where | Notes |
|---|---|---|
| Player manager | `[0x011D3B78]` | Holds the player slots (co-op: 2). `host->localPlayer()` does the walk for you. |
| Player flags | `player+0x594` | Bit 13 picks which resource block is active (below). |
| Resource block | `player+0xE3C`, or `player+0xEA4` when bit 13 is set | |
| Resources | `block+0x48` | Six `uint32`, in this order: Tungsten, Semiconductor, Scrap Metal, Somatic Gel, Transducer, Ration Seals. |
| Inventory list | `player+0xF10` | Pointer to the entries, count at `+0x4`. Each entry is `0x28` bytes: item GUID, flags, count. |
| Discovered list | `player+0xFBC` | Circuits and parts the save knows. Entries pointer at `+0x4`, count at `+0x8`; entry `0x9DC` bytes: GUID, then a "new" byte at `+0x10`. |

With the memory inspector: `status` shows all of these; `read player+0x594 4 dwords` reads the flags.

## Settings and the mouse

| What | Where | Notes |
|---|---|---|
| Mouse speed formula constant | `0x00D7AD58` (`double`, 1.8) | `speed = slider / 100 * 1.8 + 0.1`. Used only by the mouse slider, so changing it is safe. The **Mouse Slider Range** sample patches it. |
| Options object | `[0x011D191C]` | General settings at `+0x80`; the Mouse Sensitivity slider (0 to 1) at `+0x88`. Read by the Raw Input Mouse Fix's `UseGameSlider`. |

## Items

- Every circuit of the main set, with its stats and GUID: [`CIRCUITS.md`](CIRCUITS.md).
- Item GUIDs are 16 bytes; the inspector and `host->itemName()` turn them into names.

## Places the loader never lets a mod change

Patches and hooks are refused inside the game's login, store/checkout and DLC-ownership code, and next to text
about entitlements or ownership. This keeps mods away from accounts and paid content. Mods made with this SDK
must not try to unlock paid DLC or change what you own.

## Adding to this page

Found something? Note the address, the game version, how you found it (e.g. a `snapshot` and `diff` with the
inspector), and how sure you are. Share it with other modders.
