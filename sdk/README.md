# Making a drop-in mod

The DS3 proxy (`dinput8.dll`) loads mods from the game's `plugins` folder. A mod is a folder with a `mod.ini` that
describes it, and either a DLL or a few patches written in the `mod.ini` itself. Drop the folder in, and the mod shows
up in the in-game Mods panel (pause, then **mods**, or F11). Its settings show up there too, with no change to the
proxy.

There are two kinds of mod:
- **A patch mod** is only a `mod.ini`, written in Notepad. It changes a few bytes in the game: a number, or an
  instruction. No programming and no compiler. Start here: [Patch mods](#patch-mods), and the sample
  [`mouse_slider_range/mod.ini`](mouse_slider_range/mod.ini) (in the game: the Mouse Slider Range add-on,
  `plugins\MouseSliderRange\mod.ini`).
- **A DLL mod** is a small program you write in C++ and build into a 32-bit Windows DLL. It can do what a patch
  can't: show things on screen, read your resources and items, react to what happens. The proxy does the hard parts
  for you (drawing, finding the player, changing the game's code safely), so **you need no reverse engineering for
  the common things**. Start here: [DLL mods: your first mod](#dll-mods-your-first-mod), from the sample
  [`resource_counter/`](resource_counter/).

```
Dead Space 3\
  dinput8.dll, ds3_mods.ini
  plugins\
    HelloMod\
      mod.ini          <- the manifest (this page)
      HelloMod.asi     <- your 32-bit DLL (any name; .asi or .dll)
      hello_mod.ini    <- the player's settings; the panel creates and edits it, you never ship it
```

What players get:
- **A new mod is off.** The first start after it is dropped in adds `hello_mod=0` under `[modules]` in
  `ds3_mods.ini`. The player switches it on in the panel (or in the file), and it loads on the next start. A DLL
  without a `mod.ini` is never loaded.
- **Problems are logged.** A manifest that can't be used is written to `ds3_mods.log` and the mod is skipped; a bad
  setting is logged and ignored (a value out of range is clamped to it). A crash in your `DS3ModInit` or your frame
  callback is caught and logged, but a crash in a thread or hook of your own can still stop the game. Check the log
  first when your mod doesn't show up.
- **Writes stay in your folder.** The panel only writes the `ini=` file named in your manifest, which must be inside
  your folder. It never writes `ds3_seals_state.ini` and never lists a `[debug]` section.

What is in this `sdk` folder:

| | |
|---|---|
| [`ds3mod.h`](ds3mod.h) | The one file a DLL mod includes. Every call the proxy offers, with a comment on each. |
| [`resource_counter/`](resource_counter/) | A DLL mod (C++), **the sample to copy.** Shows your six resources in a corner of the screen. |
| [`hello_mod/`](hello_mod/) | A DLL mod (C++), the smallest one: a hotkey that greets you on screen and in the log. |
| [`mouse_slider_range/`](mouse_slider_range/) | The no-code example: a patch mod, only a `mod.ini`. A wider range for the game's mouse sensitivity slider. |
| [`auto_pickup/`](auto_pickup/) | The Auto Pickup add-on: five hooks, settings that apply at once, a pickup list drawn on screen. |
| [`raw_mouse/`](raw_mouse/) | The Raw Input Mouse Fix add-on: raw mouse input and two hooks, an ASI mod moved to the SDK. |
| [`seal_store/`](seal_store/) | The Ration Seal Store add-on: its own store offers, sold for Ration Seals only. Large; read the others first. |
| [`GAME_NOTES.md`](GAME_NOTES.md), [`CIRCUITS.md`](CIRCUITS.md) | Known game addresses, and the game's circuit list. |
| `tools\ds3_mcp.py` | The read-only memory inspector: [Finding things in memory](#finding-things-in-memory). In the source repo it is `scripts\ds3_mcp.py`. |
| [`CMakeLists.txt`](CMakeLists.txt) | Builds the samples. Add your own mod here (one line). |
| `README.md` | This page. |

In a release this folder is in the source download, `ds3-mod-loader-source-v<version>.zip` (folder
`Source code\sdk\`). Players don't need it: the loader's download has no `sdk` folder.


## What you need

You don't need to have made a mod before. Everything here is free.

| For | You need |
|---|---|
| Playing with mods | Dead Space 3 on Steam (the current version), Windows 10 or 11, and the DS3 Mod Loader installed (its `README.txt`). |
| A patch mod | Notepad. That's all. |
| A DLL mod | **Build Tools for Visual Studio 2022** with "Desktop development with C++", and **CMake**. Setup: [Install the tools](#1-install-the-tools-once). |
| Finding numbers in the game | **Python 3** (<https://www.python.org/downloads/>, tick "Add python.exe to PATH") for the memory inspector (`tools\ds3_mcp.py`). Optional: a disassembler such as [Ghidra](https://ghidra-sre.org/) to read the game's code. |
| Making an existing ASI mod work here | Just a `mod.ini` next to it: [Moving an existing ASI here](#moving-an-existing-asi-here). |

Words this page uses:

- **Mod loader / proxy**: the loader is a file named `dinput8.dll` in the game folder. The game loads it like a
  Windows file it needs, and the loader then loads your mods.
- **Manifest**: your mod's `mod.ini`. It names the mod, its settings and, for a DLL mod, its file.
- **DLL / ASI**: a compiled program the game loads. `.asi` is the same thing with another name, common for game mods.
- **32-bit**: Dead Space 3 is a 32-bit program, so every DLL mod must be built as 32-bit (`-A Win32` below).
- **Address**: where something is in the game's memory, written in hex, e.g. `0x00D7AD58`.
- **Hook**: your function runs when the game calls one of its own functions. **Patch**: a few bytes of the game's
  code or data are changed while it runs. The loader does both for you. A mod that is switched off is not loaded at
  the next start, so none of its hooks or patches are in the game then.

## mod.ini

A plain Windows ini file in ASCII (or UTF-16 with a BOM). Put comments on their own lines, starting with `;`. A `;`
after a value is part of the value.

### `[mod]`

| Key | | |
|---|---|---|
| `file` | DLL mods | Your DLL, relative to the folder. Must end in `.asi` or `.dll`. No `..`, no drive. A patch mod leaves it out. |
| `name` | | Shown in the panel. Default: the folder name. |
| `id` | | Your `[modules]` key in `ds3_mods.ini`: `a-z`, `0-9` and `_`, up to 31. Default: the folder name in lower case, other characters turned into `_`. Always set it in a mod you share, and never change it afterwards, or players lose their switch. |
| `ini` | | The player's settings file, relative to the folder. Must end in `.ini` and can't be `mod.ini`. Default: `settings.ini`, if the mod has any `[setting]` sections (the panel creates it on the first change). Don't put this file in your download: [Releasing and updating your mod](#releasing-and-updating-your-mod). |
| `author`, `version` | | Shown in the panel's help line under your switch. `version` is three numbers, e.g. `1.2.0`. |
| `min_loader` | | The oldest DS3 Mod Loader your mod works with, e.g. `1.0.0`. An older loader doesn't load the mod: it lists it in the panel with "needs loader 1.0.0" and says so in the log. Set it to the loader you built and tested with. |
| `description` | | One or two sentences for the help line. |
| `reload` | | A key in game that makes your mod re-read its ini, e.g. `F6`. The panel then says "press F6 or restart" after a change. Leave it out if your mod uses `DS3ModSettingsChanged` (below), or reads its ini only at start. |

### `[setting.<name>]`

`<name>` is up to you; a patch's `value=` or `toggle=` uses it to pick the setting (below).

One section per setting, in the order you want them in the panel. Every section whose name starts with `setting`
counts.

| Key | | |
|---|---|---|
| `section`, `key` | required | Where the value lives in your ini (`[section]` `key=`). `section=debug` is not allowed. |
| `kind` | required | `bool`, `int`, `float`, `choice` or `key` (below). |
| `label` | | The row's text. Default: the key. Keep it short (it's cut with "..." if it doesn't fit). |
| `help` | | A line at the bottom of the panel while the row is selected. |
| `group` | | A heading. Rows with the same group in a row share one heading. Default: the mod's name. |
| `default` | | Used when your ini has no value, or one that isn't valid. |
| `unit` | | Shown after a number, e.g. `%` or ` s`. |

Kinds:
- **`bool`**: On / Off. `values=true,false` sets what is written for on and off (default `1,0`). Reading accepts those,
  and `1 / true / yes / on` and `0 / false / no / off` too.
- **`int`**: needs `min=` and `max=`. Optional `step=` (default 1). Left / Right step it, and Enter steps up and wraps.
- **`float`**: needs `min=` and `max=`. Optional `step=` (default 0.1) and `decimals=` (default: as many as `step` has).
- **`choice`**: `choices=value:Label|value:Label|...`, 2 to 8 of them. The value is written; the label is shown. A
  value with no `:Label` is its own label. `default=` names a value.
- **`key`**: a keyboard key. Enter waits for the player to press one (Esc cancels, Backspace sets none); Left / Right
  step through F1-F12, Insert, Delete, Home, End, Page Up and Page Down. It is written as a hex virtual-key code
  (`0x7B` = F12). `default=` takes `F12`, a letter or digit, `none`, or a code.

Limits, so the panel stays usable: 16 mods, 24 settings per mod, 128 settings over all mods. Labels are cut to 47
characters, help lines to 159, and text is shown in ASCII only (other characters become `?`).

## Patch mods

A patch mod changes bytes in `deadspace3.exe` while it runs (never the file on disk). Everything the game does is
bytes in memory: numbers it uses, and the instructions of its code. If you know *where* a number or an instruction
is, a patch can change it. Finding where is the hard part. Start from what is already known
([`GAME_NOTES.md`](GAME_NOTES.md), next to this guide), and watch the running game with the memory
inspector (see [Finding things in memory](#finding-things-in-memory)). Writing the patch afterwards is easy.

### Your first patch, step by step

This patch uses a number that is already known: how much the game's Mouse Sensitivity slider speeds up the mouse
([the walkthrough below](#walkthrough-the-sample) shows how it was found). So you can make it, see it work, and learn
the steps before you look for numbers of your own.

1. **Make the folder.** In the game folder, open `plugins` (make a folder named `plugins` there first if there
   isn't one yet) and make a new folder in it named `FirstPatch`.
2. **Write the file.** Open Notepad and paste this:
   ```ini
   [mod]
   name=First Patch
   id=first_patch

   [patch.range]
   ; where
   address=0x00D7AD58
   ; what is there now (the double 1.8)
   expect=00 00 00 C6 CC CC FC 3F
   ; what to put there (the double 3.6)
   write=00 00 00 C6 CC CC 0C 40
   ```
   (Comments go on their own line: a `;` after a value would be read as part of it.)
3. **Save it as `mod.ini`.** In Notepad: File, Save as. Go to `plugins\FirstPatch`, set **Save as type** to
   **All files (\*.\*)**, type the file name `mod.ini`, and save. With "Text documents", Notepad saves `mod.ini.txt`,
   which the proxy doesn't see. To check: in File Explorer, View, Show, tick **File name extensions**; the file must
   be called `mod.ini`, not `mod.ini.txt`.
4. **Switch it on.** Start the game. Open the Mods panel (pause, then **mods**, or F11), switch **First Patch** on,
   and restart the game. If the Mouse Slider Range add-on is on, switch it off: both change the same number, and only
   one mod may. The same goes for the Raw Input Mouse Fix add-on, which replaces the game's mouse speed (its
   UseGameSlider reads where the slider is, never this number).
5. **Check it.** Open `ds3_mods.log` in the game folder. It should say
   `mod first_patch: [patch.range] 8 byte(s) at 0x00D7AD58-0x00D7AD5F written`. In the game's Options, move the
   Mouse Sensitivity slider: the mouse is now almost twice as fast at the same slider position.
6. **To undo it,** switch First Patch off and restart, or delete the folder.

That is a whole patch mod. A patch is one `[patch.<name>]` section in `mod.ini`; a mod can have several, and then
the folder needs no DLL. The keys:

| Key | | |
|---|---|---|
| `address` | required | Where the bytes start. `0x00D7AD58` (as Ghidra shows it) or `deadspace3.exe+97AD58` (offset from the start of the exe; same place). |
| `expect` | required | The bytes there in an unchanged game, in hex, two digits each, spaces allowed: `55 8B EC`. Up to 64 bytes. |
| `write` | one of these | The new bytes, in hex. Written at `address` (plus `offset`). |
| `toggle` | | With `write`: names a `bool` setting. On = `write` is written; Off = the original bytes are put back. |
| `value` | | Instead of `write`: names an `int`, `float` or `bool` setting, whose value is written as `type`. |
| `type` | with `value` | How the value is stored: `int8`, `int16`, `int32`, `uint8`, `uint16`, `uint32` (or `byte`, `int`), `float` (4 bytes), `double` (8 bytes). Ghidra shows which: `float ptr` / `double ptr` / `dword ptr`. The setting's `min`..`max` must fit in it. |
| `offset` | | Write this many bytes after `address`. `expect` still starts at `address`, so it can check a few bytes around the change. |

How it keeps the game safe:
- **Off until switched on.** Like every drop-in mod, a patch mod is off when first found. Nothing is written until
  the player switches it on and restarts.
- **Only after the game is unpacked.** The game's code is encrypted on disk and unpacked by its DRM wrapper at start.
  Patches are written only once the proxy has confirmed the unpacked code; otherwise nothing is written that run.
- **Checked first.** When the game starts, every patch of the mod compares `expect` with the game's bytes. If even
  one differs (another game version, or another mod changed them), **nothing** of that mod is written, and
  `ds3_mods.log` shows the bytes that are there instead.
- **Logged.** Every write is logged with its exact byte range, e.g. `8 byte(s) at 0x00D7AD58-0x00D7AD5F written`.
- **One owner per byte.** Two mods can't patch the same bytes; the second one is skipped with a log line naming the
  first. The proxy's own hooks are protected the same way.
- **Only the game's own code and data** (`.text`, `.rdata`, `.data` of `deadspace3.exe`). Never the DRM wrapper
  (its code lives in another part of the exe, which is never patchable).
- **Never DLC ownership, entitlements, the store, login or Origin.** Patches there are refused in two ways: a list of
  address ranges known to hold that code, and a check that refuses any patch within 64 bytes of text about
  entitlements, DLC ownership or logins. **Neither is a complete map:** where all of the game's
  entitlement and DLC code lies is not known. So the rule stands on its own, whatever the checks catch: this project
  never changes paid DLC, entitlements, ownership or the DRM, and patch mods made with it must not either.
- **Live changes.** A `value` or `toggle` patch is rewritten at once when the setting changes in the panel
  ("applied now") if the bytes it writes fit inside one aligned block of 8: the first written byte (`address` plus
  `offset`) and the last one in the same block of 8 that starts at an address divisible by 8. When several patches
  use the same setting and one of them doesn't fit, the panel says the setting applies after a restart: the
  patches that fit are still rewritten at once, the others at the next start. So give patches that must change
  together a setting where all of them fit, or a setting each. Whether the *game* notices at once depends on when
  it reads those bytes: the sample's number is only read when you move the game's slider.

### Walkthrough: the sample

[`mouse_slider_range/mod.ini`](mouse_slider_range/mod.ini) is a complete patch mod,
with a comment on every line. How it was made:
1. **Find the code.** The game's Options menu has a Mouse Sensitivity slider. Earlier research had found the
   function that applies it (`FUN_00632d90`). In Ghidra its last lines are
   `FMUL double ptr [0x00d7ad58]` and `FADD double ptr [0x00d229a8]`: mouse speed = slider / 100 × (the number
   at `0x00D7AD58`, 1.8) + (the number at `0x00D229A8`, 0.1).
2. **Check that nothing else uses it.** Ghidra's "references to" `0x00D7AD58` lists only that one instruction, so
   changing it changes nothing else. (`0x00D229A8` is used in about 70 places: a patch there would change all of
   them, so leave it alone.)
3. **Read the bytes.** At `0x00D7AD58`: `00 00 00 C6 CC CC FC 3F`, which as a double is 1.8. That is `expect`.
4. **Write the patch.** A `float` setting from 0.3 to 5.4 (default 1.8), and a patch with `value=range` and
   `type=double`. The panel's value is then written as a double at that address.
5. **Test it.** Switch the mod on in the panel and restart the game. The log should say
   `mod mouse_slider_range: [patch.range] 8 byte(s) at 0x00D7AD58-0x00D7AD5F written`. If you have the Raw Input Mouse Fix
   add-on, switch it off: it replaces the game's mouse speed, and its UseGameSlider reads only where the
   slider is. Set the range to 3.6 and move the game's own slider:
   the mouse should be almost twice as fast. (Only the slider's part of the speed doubles: at 50, the speed goes from
   1.0 to 1.9.)

### Fixed bytes and on / off patches

To change an instruction, `write` the new bytes. A common first patch is to skip a jump: in Ghidra the instruction
`JZ` is the bytes `74 xx`; writing `EB` over the `74` makes it `JMP`, which always jumps. With `toggle=` the panel
gets an On / Off row for it:

```ini
[setting.skip]
section=Patch
key=Skip
kind=bool
default=0
label=Skip the check

[patch.skip]
address=0x00123456
expect=85 C0 74 0B
offset=2
write=EB
toggle=skip
```

Here `expect` checks the `TEST EAX,EAX` before the jump too (`85 C0`), and `offset=2` writes only over the `74`.
(The address is made up: this example is not a real place in the game.)

### When it doesn't work

Open `ds3_mods.log` in the game folder and look for lines starting with `mod <your id>`. Every problem has one, and
says what to fix: a missing `expect`, bytes that differ, a setting name that doesn't exist, a value that doesn't fit
the type, an address outside the game's code. Limits: 64 patches over all mods, 64 bytes per patch.

## DLL mods: your first mod

This part assumes you have never built a DLL. It takes about half an hour the first time, most of it installing
tools.

### 1. Install the tools (once)

1. **Build Tools for Visual Studio 2022** (free): <https://visualstudio.microsoft.com/downloads/>. The page shows
   the newest version first; the 2022 version is under **Older Downloads**. In the installer, tick **Desktop
   development with C++** and install. (The full Visual Studio Community works too; it includes the same thing.)
   The project is built and tested with the 2022 tools; the newer 2026 Build Tools should work too but haven't
   been tried. If you use them, every "VS 2022" below is "VS 2026" for you.
2. **CMake** (free): <https://cmake.org/download/>, the newest Windows x64 installer. Let it add CMake to the PATH.

To check: open **Developer PowerShell for VS 2022** from the Start menu and type `cmake --version`. It should print a
version number.

### 2. Build the samples

In that Developer PowerShell, go to this `sdk` folder and build:

```
cd "C:\path\to\sdk"
cmake -S . -B build -A Win32
cmake --build build --config Release
```

- `-A Win32` matters: Dead Space 3 is a 32-bit game, so its mods must be 32-bit too. Without it, CMake stops with
  "configure with -A Win32".
- The first command prepares a `build` folder; you only need it once. Run the second one after every change.
- At the end, each mod is ready in its own folder: `build\plugins\ResourceCounter\` and `build\plugins\HelloMod\`.

### 3. Try the sample in the game

1. Copy the folder `build\plugins\ResourceCounter` into the game's `plugins` folder (make it if it isn't there
   yet), so you have
   `Dead Space 3\plugins\ResourceCounter\ResourceCounter.asi` and `mod.ini`.
2. Start the game once. The proxy finds the mod and adds `resource_counter=0` to `ds3_mods.ini`: **new mods are
   off.** Switch it on in the Mods panel (pause, **mods**, or F11), under "Mod parts", and restart the game.
3. Load a save. Your resources show in the top-left corner. In the Mods panel, the Resource Counter's settings
   (corner, text size, which resources) change it at once.

If nothing shows, open `ds3_mods.log` in the game folder and look for lines with `resource_counter`. Every problem
writes one.

### 4. Make it yours: copy and rename

Say your mod is called **Seal Watch**. Do this once:

1. **Copy the folder.** Copy `sdk\resource_counter` to `sdk\seal_watch`.
2. **Rename the source.** In `sdk\seal_watch`, rename `resource_counter.cpp` to `seal_watch.cpp`.
3. **Add it to the build.** Open `sdk\CMakeLists.txt` and add this block at the end of the file (it is the
   `ds3_mod(ResourceCounter ...)` block with the names changed):
   ```cmake
   ds3_mod(SealWatch SealWatch
       SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/seal_watch/seal_watch.cpp
       FILES ${CMAKE_CURRENT_SOURCE_DIR}/seal_watch/mod.ini ${CMAKE_CURRENT_SOURCE_DIR}/seal_watch/CHANGELOG.txt)
   ```
   The first `SealWatch` is the DLL's name (`SealWatch.asi`), the second the folder it goes to.
4. **Replace `mod.ini`.** Open `sdk\seal_watch\mod.ini` in any editor (Visual Studio, VS Code, even Notepad),
   delete everything in it, and paste this:
   ```ini
   [mod]
   name=Seal Watch
   id=seal_watch
   author=Your name
   version=1.0.0
   min_loader=1.0.0
   description=Shows your Ration Seals in the top-left corner.
   file=SealWatch.asi
   ini=seal_watch.ini

   [setting.show]
   section=Watch
   key=Show
   kind=bool
   default=1
   label=Show the seal count
   ```
   `id` is lower case, letters, digits and `_`; never change it after you share the mod. `file` is the DLL the build
   makes, `ini` the file where the Mods panel keeps the player's settings. The one `[setting.show]` section is one
   On / Off row in the Mods panel ([mod.ini](#modini) has the whole format). In `CHANGELOG.txt`, change "Resource
   Counter" on the first line to "Seal Watch".
5. **Replace the code.** Open `sdk\seal_watch\seal_watch.cpp`, delete everything in it, and paste this. It is a
   whole mod: it builds as it is. [How a mod is put together](#how-a-mod-is-put-together) explains each part.
   ```cpp
   // Seal Watch: shows your Ration Seals in the top-left corner while you play.
   #include "../ds3mod.h"

   #include <windows.h>  // GetPrivateProfileIntW: reading our ini
   #include <cstdio>     // sprintf_s: turning a number into text

   namespace {

   const DS3ModHost* g_host = nullptr;  // the proxy's table of functions; DS3ModInit keeps it here
   bool g_show = true;                  // our one setting, [setting.show] in mod.ini

   // Reads our setting from seal_watch.ini (section [Watch], key Show). 1 if it isn't there yet.
   void ReadSettings()
   {
       g_show = GetPrivateProfileIntW(L"Watch", L"Show", 1, g_host->iniPath) != 0;
   }

   // Runs once per frame: read the game, then draw.
   void __cdecl OnFrame(const DS3Frame* frame, void*)
   {
       if (!g_show) return;                              // switched off in the Mods panel
       if (frame->state != DS3_STATE_PLAYING) return;    // only in a level, not paused
       unsigned seals = 0;
       if (!g_host->getResource(DS3_RATION_SEALS, &seals)) return;
       char text[32];
       sprintf_s(text, "Seals: %u", seals);
       frame->text(frame, 40, 40, text, DS3_WHITE);     // x = 40, y = 40 pixels from the top left
   }

   }  // namespace

   // Once, when the game starts and your mod is switched on.
   extern "C" __declspec(dllexport) int __cdecl DS3ModInit(const DS3ModHost* host)
   {
       if (!host || host->apiVersion < 2 || !DS3MOD_HAS(host, onFrame)) return 1;  // the proxy is too old
       g_host = host;                           // keep it: it is your way to the proxy
       ReadSettings();
       host->onFrame(host, &OnFrame, nullptr);  // run OnFrame once per frame from now on
       host->log(host, "Seal Watch started");   // into ds3_mods.log
       return 0;                                // 0 = fine; anything else is logged as a failure
   }

   // When the Mods panel changes one of our settings.
   extern "C" __declspec(dllexport) void __cdecl DS3ModSettingsChanged()
   {
       ReadSettings();
   }
   ```
6. **Build** (`cmake --build build --config Release`; run the `cmake -S` line again first, since you changed
   `CMakeLists.txt`), copy `build\plugins\SealWatch` into the game's `plugins` folder, start the game, switch
   **Seal Watch** on in the Mods panel, and restart.
7. **Try it.** Load a save: "Seals: " and your seal count show in the top-left corner. In the Mods panel, switch
   **Show the seal count** off: the text goes at once.

If the build fails, the error names the file and line. The most common first errors: a missing `;`, a name typed
with different capitals, or a `"` not closed.

From here, change what you like. [What a mod can do](#what-a-mod-can-do) lists every call, and
`resource_counter.cpp` shows more of them at work: a box behind the text, a corner and a text size from the
settings, all six resources.

### How a mod is put together

Look at `seal_watch.cpp` from step 5 again. The proxy calls three functions in it:

1. **`DS3ModInit`**, once, when the game starts and your mod is switched on. It checks that the proxy is new enough
   (API 2, with `onFrame`), keeps `host`, reads the settings, and asks the proxy to run `OnFrame` every frame. It
   returns 0 when all is well; anything else is logged as a failure. The DLL stays loaded, so a mod that fails
   its checks must not ask for `OnFrame` or hooks before it returns.
2. **`OnFrame`**, once per frame from then on. It reads the game (`getResource`) and draws (`frame->text`).
3. **`DS3ModSettingsChanged`** (optional), when the Mods panel changed one of your settings. Re-read your ini there;
   the panel then says "applied now" instead of "applies after restart". It runs on the same thread as `OnFrame`, so
   the two never run at the same time.

- `extern "C" __declspec(dllexport)` makes `DS3ModInit` and `DS3ModSettingsChanged` visible to the proxy under
  exactly those names. Copy them as they are.
- The `host` is a table of functions: `g_host->getResource(...)` calls the proxy's code. It stays valid until the
  game exits, so keep the pointer.
- `OnFrame` comes before `DS3ModInit` in the file, because C++ needs a name declared before it is used.
- **Nothing you draw stays on screen.** The game draws a new picture about 60 times a second, and your `OnFrame` draws
  on top of each one. To show something, draw it every frame; to hide it, stop drawing it.

### What a mod can do

Everything is in [`ds3mod.h`](ds3mod.h), with a comment on each line. In plain words:

**Basics (API 1)**

| Call | What it does |
|---|---|
| `host->log(host, "text")` | Writes a line to `ds3_mods.log`, starting with your mod's id. Your best friend when something doesn't work. |
| `host->inputBlocked()` | Nonzero while the Mods panel has the keyboard. Check it before acting on a hotkey (see [Hotkeys](#hotkeys)). |
| `host->iniPath`, `modDir`, `gameDir` | Your settings file, your folder, and the game folder, as full paths. |
| `host->modId`, `proxyVersion`, `apiVersion` | Your id, the proxy's version (e.g. "1.0.0"), and its API version (3). |

**Reading the game (API 2)**. Call these from `OnFrame`.

| Call | What it does |
|---|---|
| `host->gameState()` or `frame->state` | `DS3_STATE_NO_PLAYER` (main menu, loading), `DS3_STATE_PLAYING`, or `DS3_STATE_PAUSED` (pause menu open). |
| `host->getResource(DS3_SCRAP_METAL, &n)` | Puts how much scrap metal you have in `n`. Returns 0 if there is no player. The six: `DS3_TUNGSTEN`, `DS3_SEMICONDUCTOR`, `DS3_SCRAP_METAL`, `DS3_SOMATIC_GEL`, `DS3_TRANSDUCER`, `DS3_RATION_SEALS`. |
| `host->setResource(host, DS3_TUNGSTEN, 500)` | Sets it to 500, the same way the game adds and takes resources. Saved with your next save. Logged. |
| `host->resourceName(i)` | `"Tungsten"`, `"Scrap Metal"`, ... for showing. |
| `host->itemCount(list)`, `host->getItem(list, i, &item)` | The player's items: `DS3_ITEMS_CONSUMABLES` (ammo, med and stasis packs, some mission items) or `DS3_ITEMS_KEY_AND_PARTS` (suits, weapon parts, circuits, key items). `item.name` is the game's name ("AMMO CLIP"), `item.count` the stack size, `item.id` the kind of item. Read only. |
| `host->itemName(&id, buf, size)` | The game's name for an item id. |
| `host->localPlayer()` | The player object's address, for your own research (`GAME_NOTES.md` lists what is known). NULL with no player. |

**Drawing (API 2)**. Only inside `OnFrame`, through the `frame` it gets.

| Call | What it does |
|---|---|
| `frame->width`, `frame->height` | The screen size in pixels. (0, 0) is the top-left corner. Place things relative to these, so they fit every resolution. |
| `frame->text(frame, x, y, "text", color)` | Draws text with its top-left corner at (x, y). Returns where it ended, to draw more after it. ASCII only. |
| `frame->rect(frame, x, y, w, h, color)` | A filled box. A see-through background: `DS3_RGBA(0, 0, 0, 160)`. |
| `frame->setTextSize(frame, px)` | Letter height in pixels (8 to 160) for the text after it. Returns the line height. The default is about `height / 54`. |
| `frame->textWidth(frame, "text")`, `frame->lineHeight(frame)` | How big text will be, to centre it or size a box around it. |
| `frame->setFont(frame, "Consolas", px)` | Like `setTextSize`, and also picks the font: the name of any font installed in Windows (`NULL` = the usual one), in printable ASCII characters, at most 31 of them. If Windows doesn't have it, or the name is longer or has other characters, you get the usual one. |
| `frame->gradient(frame, x, y, w, h, from, to, vertical)` | A box whose colour runs from `from` to `to`, left to right (or top to bottom if `vertical` is 1). With alpha 0 at one end it fades out. |
| `frame->timeMs`, `frame->deltaSeconds` | A clock in milliseconds and the time since the last frame, for things that fade or move. |

Colours are `DS3_RGBA(red, green, blue, alpha)`, each 0 to 255; alpha 255 is solid, 0 invisible. `DS3_WHITE` and
`DS3_BLACK` are ready-made. Your drawing goes under the Mods panel. `setFont` and `gradient` came with DS3 Mod
Loader 1.0.0: check `DS3FRAME_HAS(frame, setFont)` if your mod should also run on an older test build. The proxy
keeps up to 8 fonts (a face at a size) per frame for all mods together; ask for more and you get the nearest one.
All mods together can draw 8192 boxes and letters per frame; anything drawn after that is left out of that frame.

**Changing the game's code (API 2)**. For when you know an address, from `GAME_NOTES.md` or your own research
work. Both return 1 when done and 0 when refused, and the log always says why.

| Call | What it does |
|---|---|
| `host->patch(host, "name", address, expect, write, len)` | Like a [patch mod](#patch-mods), from code: if the game has the bytes `expect` at `address`, write `write` there. To undo, call it again with the two swapped. |
| `host->hook(host, "name", address, expect, len, detour, &original)` | Makes the game call your function `detour` instead of its own function at `address`. `original` then runs the game's own code. `expect` is the function's first bytes (5 to 64). |

Both have the same safety as patch mods: nothing until the game's code is decrypted; only the game's own code and
data; the bytes must be the expected ones; nothing in or next to the store, DLC, entitlement, login or Origin code;
and no two mods (or a mod and the proxy) on the same bytes. Hook functions must match the game function's calling
convention exactly (a `__thiscall` function is replaced by a `__fastcall` one with an extra unused second argument).
Getting that wrong crashes the game, so start from a decompiled function and test carefully. Hooking a function the
proxy itself hooks, or one another mod has already hooked, is refused.

The Auto Pickup add-on (`auto_pickup/`) is the example to read: five hooks that change
how the game collects items, each part switching itself off if its hook is refused, settings that apply at once,
and a pickup list drawn with `setFont` and `gradient`. The Raw Input Mouse Fix add-on (`raw_mouse/`)
is the input example: it reads the mouse through Windows raw input and replaces what two game functions do with it,
one of them hooked with a detour in assembly (see [Going further](#going-further-rebuilding-an-asi-with-the-sdk)).

**Selling things for Ration Seals (API 3)**. The store's code is off limits to `hook` and `patch`;
`sealOffers` is the one way in, and it can only add offers that cost Ration Seals.

| Call | What it does |
|---|---|
| `host->sealOffers(host, fn, user)` | Each time the bench store opens, the proxy calls your `fn(out, max, user)`: fill `out[0 .. max - 1]` with `DS3SealOffer`s (`id`, `name`, `description`, `packType`) and return how many. The strings are copied. Returns 1, or 0 if refused. `fn = NULL` stops it. |

- **IDs**: `"DS3SEALS:"` plus `A-Z`, `0-9` or `_` (up to 47 characters), or the game's own Ultra / Epic seal packs
  (`"OFB-EAST:109546252"` / `"OFB-EAST:109546253"`), which the Steam store leaves out. Any other ID is refused, so a
  mod can never list a real offer.
- **Pack types**: `DS3_PACK_RESOURCE`, `_ULTRA` and `_EPIC` are the game's own packs: the game prices, rolls and gives
  them. Types 5 to 63 are yours, and are listed only while your mod holds hooks (made with `host->hook`) on both
  `DS3_ADDR_BUY_WITH_SEALS` and `DS3_ADDR_SEAL_PRICE`: the game knows no price or contents for them, so your hooks
  must give both.
- **Never money**: before it lists anything, the proxy hooks the game's money checkout and refuses every checkout
  with an offer from a mod in it. That refusal stays in until the game exits. If it can't be installed, nothing is
  listed.

The Ration Seal Store add-on (`seal_store/`) is the full example: its own pack types,
rolls, pity counts and pop-ups. It is large; read the other samples first.

### Hotkeys

The proxy keeps keys from the game while the Mods panel is open, but it can't keep them from a mod that reads the
keyboard itself (`GetAsyncKeyState`). So check `host->inputBlocked()` before acting on a key, as `hello_mod` does,
or a key pressed in the panel (including the one that picks your hotkey there) also triggers your mod. Keys others
use: the Raw Input Mouse Fix add-on has F6-F10, the panel opens on F11, Hello Mod uses F12.

### Rules of thumb

- **Keep `OnFrame` quick.** The game waits for it every frame. No `Sleep`, no file reading every frame (read your
  ini in `DS3ModInit` and `DS3ModSettingsChanged`).
- **Read the game from `OnFrame`.** It runs on the game's own thread, so the player can't disappear halfway through.
  A thread of your own (like Hello Mod's hotkey thread) should only set a flag that `OnFrame` acts on.
- **Crashes are caught, but don't rely on it.** If `OnFrame` crashes, the proxy switches it off for the rest of the
  game and logs it; the game keeps running. A crash in your own thread or in a hook can still take the game down.
- **Check `apiVersion`.** A mod that uses API 2 calls should check `host->apiVersion >= 2` in `DS3ModInit`, as the
  sample does, and say in the log that it needs a newer proxy. `DS3MOD_HAS(host, field)` checks one field.
- **Don't** read the process environment or memory you don't need: it holds EA login tokens. **Never** change paid
  DLC, entitlements, ownership or the DRM wrapper. The SDK refuses the code it knows about; the rule covers the rest.

### Rebuild while the game runs (hot reload)

With API 2.1 (`apiVersion` is still 2, nothing in `DS3ModHost` changed; the 1.0.0 loader has it) you don't have to quit
the game to try a change. Export one more function, and the proxy swaps in each new build of your DLL while you play:

```cpp
extern "C" __declspec(dllexport) void __cdecl DS3ModShutdown()
{
    // Stop your threads and wait for them, undo what you changed yourself.
}
```

Then, with the game running: change your code, build, and copy the new `.asi` over the one in the game's
`plugins\<YourMod>\` folder, for example in PowerShell:

```
cmake --build build --config Release
Copy-Item build\plugins\HelloMod\HelloMod.asi "C:\path\to\Dead Space 3\plugins\HelloMod\" -Force
```

About a second later the top-left corner says "Hello Mod reloaded (build 2)", and the log says what happened. Your
`DS3ModInit` runs again, so your settings are read again and your hooks are made again.

What the proxy does for you, before it calls `DS3ModShutdown`: it stops your frame callback, switches off every hook
you made with `host->hook`, and waits until none of them is running. After it: it removes those hooks and puts back
every byte you changed with `host->patch`. What **you** must do in `DS3ModShutdown`, because the old build is never
called again (the proxy leaves it in memory until the game exits; `[mods] hot_reload_free=1` frees it instead, which
crashes the game if you broke a rule below):

- **Stop every thread you started, and wait for it to end.** `hello_mod` shows how: an event the thread waits on
  instead of `Sleep`, then `SetEvent` and `WaitForSingleObject` on the thread.
- **Undo what you did without the host:** hooks of your own, memory you wrote yourself, window procedures, timers.
- **Take back anything of yours you gave the game:** a pointer to one of your functions, strings or objects.
  Pointers you keep to the game's own objects (the player, a bot) are fine.
- Your new build starts from scratch: globals are back to their start values. Read what you need from the game again.
- The DLL runs from a copy in `%TEMP%\ds3mods`, so use `host->modDir` and `host->iniPath` to find your files, not
  `GetModuleFileName`.

A mod without `DS3ModShutdown` loads as before and needs a restart for a new build. `[mods] hot_reload=0` in
`ds3_mods.ini` turns hot reload off. Settings don't even need a rebuild: edit your mod's ini in Notepad while the
game runs, save, and the proxy reads it again and calls your `DS3ModSettingsChanged` (any mod, with or without
`DS3ModShutdown`). Changes to `mod.ini` itself (new settings, patches) still need a restart.

### When it doesn't work

| What you see | What to check |
|---|---|
| The mod isn't in the Mods panel | `mod.ini` is in `plugins\<YourMod>\` (not deeper), and the log has no `mod.ini` error for it. |
| It's in the panel but does nothing | Switched on, and the game restarted since? The log says `mod <id>: loaded` or why not. |
| `DS3ModInit returned 1` | Your own check failed: usually `apiVersion < 2` with an old `dinput8.dll`. |
| The panel says `needs loader 1.2.0` | The mod's `min_loader=` is newer than the installed `dinput8.dll`. Install the newer loader. |
| `FAILED to load ... (error 193)` | The DLL is 64-bit: build with `-A Win32`. |
| `FAILED to load ... (error 126)` | Your DLL needs another DLL that isn't there. The `CMakeLists.txt` here builds with the C runtime inside, so this is usually a library you added. |
| Nothing drawn | `OnFrame` is registered with `host->onFrame` (the log says "frame callback registered"), and you draw in a state you are in (`DS3_STATE_PLAYING` is not the main menu). |
| `its frame callback crashed` | A bad pointer or array index in `OnFrame`. Add `host->log` lines to find where. |
| A new build isn't picked up | Does the log say `it can be rebuilt while the game runs` at start? If not, `DS3ModShutdown` isn't exported (`extern "C" __declspec(dllexport)`). Did you copy the `.asi` into the game's `plugins\<YourMod>\`? |
| `one of its hooks is still running` | A hook of yours didn't return for 5 s (or never returns, e.g. a game loop function). The old build stays loaded with its hooks off; restart the game. |
| The game crashes right after `build N unloaded` | Something still pointed into the old DLL: usually a thread that wasn't stopped in `DS3ModShutdown`. |

## Releasing and updating your mod

Players update a mod in one way: **close the game, unzip the new version into the game folder over the old one, let
Windows replace the files, start the game.** Their on/off switch and their settings must survive that. They do, if
your download follows these rules.

**1. The id and the folder name never change.** The player's switch hangs on `id=`, and the new version only lands on
top of the old one if the folder in your zip has the same name. A different mod, or a variant meant to sit beside the
original, takes a new id and a new folder. Two folders with the same id: only the first by folder name is loaded, and
the log names the other.

**2. `version=` is three numbers**, like `1.2.0`:

| Raise | When |
|---|---|
| the third (`1.2.0` to `1.2.1`) | A fix. Nothing new for the player. |
| the second (`1.2.1` to `1.3.0`) | Something new: a feature, a setting. Old settings still work. |
| the first (`1.3.0` to `2.0.0`) | The player has to do something: a setting was renamed, or it changes how saves behave. Start the changelog entry with "You need to:". |

A text tail after `-` is allowed for a build of someone else's mod (`1.1.2-slider`).

**3. Set `min_loader=`** to the loader version you built and tested with. A player with an older loader then reads
"needs loader 1.2.0" in the Mods panel, instead of meeting a mod that half works. In a DLL mod, keep the
`host->apiVersion` check in `DS3ModInit` as well.

**4. Three kinds of file. Only the first goes in your zip.**

| Kind | Examples | In the zip? |
|---|---|---|
| What you made | `mod.ini`, your DLL, `README.txt`, `CHANGELOG.txt`, a licence | Yes. Replaced by every update. |
| The player's settings | the file `ini=` names | **Never.** The defaults are the `default=` lines in `mod.ini` and the defaults in your code. The panel creates the file on the first change. |
| What your mod remembers | counters, progress, a log | **Never.** Your mod creates these files itself. |

A zip that holds the settings file resets every player's settings on every update.

**5. Settings across versions.**
- A new setting: add its `[setting.*]` section with `default=`, and use the same default in your code when the key
  is missing. Players who update get the default.
- A removed setting: delete its section. The old line stays in the player's file and does nothing.
- A renamed setting: don't. Add a new key and leave the old one alone. If it can't be avoided, raise the first
  version number, and let a DLL mod copy the old value to the new key once in `DS3ModInit` (when the new key is
  missing and the old one is there).
- Never give an old key a new meaning.

**6. One zip layout.** The zip holds one folder and nothing beside it:

```
YourMod-v1.2.0.zip
  plugins\
    YourMod\
      mod.ini
      YourMod.asi
      README.txt       what it does, how to install, update and remove it
      CHANGELOG.txt    one entry per version, newest first
      LICENSE.txt
```

Unzipped into the game folder, it installs or updates your mod and can't replace anyone else's files. (This
project's own add-ons download is different: a collection of mod folders that players copy into `plugins\` one
by one. For a single mod, the layout above is the one to use.) Name the zip
`<Folder>-v<version>.zip`. To make it: put a copy of the built folder `build\plugins\<YourMod>\` inside a new folder
named `plugins`, add your `README.txt` and `LICENSE.txt` to `plugins\<YourMod>\`, and zip the `plugins` folder
(right-click it; on Windows 11 **Compress to**, **ZIP File**; on Windows 10 **Send to**, **Compressed (zipped)
folder**). Zipping `<YourMod>\` on its own would put it straight
into the game folder instead of `plugins`. The build only copies what `FILES` in `CMakeLists.txt` lists: `mod.ini`
and `CHANGELOG.txt`, and not your settings ini.

**7. Before you publish an update**, try it the way a player will: install your previous version in a game folder,
change a setting, unzip the new zip over it, and check that the switch and the setting are still as you left them.

The loader itself checks the id, loads one mod per id, and respects `min_loader=`. The rest is this convention, and it
only works if mods keep to it. The loader never looks online for updates, and a mod shouldn't either: players get new
versions where they got the first one.

## Moving an existing ASI here

An ASI built for an ASI loader usually works unchanged: put it in its own folder with a `mod.ini` that names it in
`file=`. It loads after the game's code is decrypted, which is later than an ASI loader would load it. Most ASIs
wait for the game anyway.

Step by step, for an ASI called `CoolMod.asi` (and its own `coolmod.ini`, if it has one):

1. Make a folder `plugins\CoolMod\` in the game folder and put `CoolMod.asi` (and `coolmod.ini`) in it.
2. Make a text file `mod.ini` in that folder with Notepad:

   ```ini
   [mod]
   name=Cool Mod
   id=cool_mod
   author=Whoever made it
   version=1.0.0
   min_loader=1.0.0
   description=One line saying what it does.
   file=CoolMod.asi
   ini=coolmod.ini
   ```

   `id` is the mod's switch in `ds3_mods.ini`: lower-case letters, digits and `_` only. Leave out `ini=` if the
   mod has no settings file.
3. Start the game, pause, choose **mods**, switch **Cool Mod** on, and restart the game.
4. Optional: list its settings as `[setting.<name>]` sections (see [mod.ini](#modini)) so players can change them
   in the Mods panel instead of in Notepad.

If it doesn't load, `ds3_mods.log` in the game folder says why. Only share someone else's mod this way if its
licence allows it, and credit its author.

### Going further: rebuilding an ASI with the SDK

An ASI that only has a `mod.ini` runs as it is: the loader can't check its hooks, apply its settings while the game
runs, or swap in a new build. If you have its source (and its licence allows it), rebuild it with `ds3mod.h`. The
Raw Input Mouse Fix add-on (`raw_mouse/`) is slopkhayzo's standalone mouse fix moved
over this way, and its `CHANGELOG.txt` lists each change. What usually moves where:

| In the ASI | With the SDK |
|---|---|
| `DllMain` starts a thread that waits for the game's code, then works | `DS3ModInit`: the loader calls it once the code is decrypted, outside the loader lock |
| Its own hook writer (`VirtualProtect`, a `jmp`, suspending threads) | `host->hook` with the function's first bytes as `expect`: checked, logged, and taken out again on unload |
| A thread that polls hotkeys and settings in a loop | The frame callback (`host->onFrame`), on the game's own thread; check `host->inputBlocked()` for hotkeys |
| "Press a key to reload the ini" | `DS3ModSettingsChanged`, so the Mods panel's changes apply at once |
| Files found with `GetModuleFileName` | `host->modDir` and `host->iniPath` (with hot reload the DLL runs from a copy) |
| Nothing on unload | `DS3ModShutdown`: undo what the loader can't see (window procedures, handles, threads) |

A hook through the SDK keeps EAX, ECX and EDX on the way into your detour and EAX and EDX on the way back. A game
function that takes values in other registers (its compiler made up its own convention) needs a detour written in
assembly that leaves them alone: `HookChaseOrientationDamp` in `raw_mouse.cpp` is one, six lines long.

## Finding things in memory

To change a number with a patch, or read it from a DLL mod, you first need to know where it is. The loader comes
with a small, read-only **memory inspector**, `ds3_mcp.py`: in the source download it is in the `tools` folder next to this guide, in the source repo
in `scripts\`. It only reads the running game and never changes anything in it. It also hides login data as well as it
can: anything that looks like an account token is blanked, and searches for such things are refused. Your Windows user
name is blanked too. That filter is a best effort, so don't post raw memory dumps where others can see them (see
**Asking for help** below for a file you can share).

1. Install Python 3 (see [What you need](#what-you-need)).
2. Start Dead Space 3 and load a save.
3. Open a terminal in the inspector's folder: in File Explorer, open the folder, right-click an empty spot and choose
   **Open in Terminal** (on Windows 10: hold Shift while you right-click, then **Open PowerShell window here**). Then type `py -3 -I ds3_mcp.py` and press Enter. Type `help` for the commands.

A typical hunt, e.g. for where your Tungsten count lives:

```
ds3> status                         <- the player's address and your six resources
ds3> snapshot before                <- remember the player's memory now
     (pick up some Tungsten in the game)
ds3> diff before skip_floats=true   <- every whole number that changed since, with its address
ds3> read player+0x594 0x40 dwords  <- look at a place more closely
ds3> search u32 1500                <- find every place holding the number 1500
```

Leave out `skip_floats=true` to see decimal numbers too; the camera and your position change all the time, so the
list gets long. To search for words, put them in quotes: `search text "Plasma Cutter"`.

**Asking for help.** Put `save <file>` in front of a command to write its output to a text file next to the
inspector, e.g. `save tungsten.txt diff before skip_floats=true`. To ask for help, share this file, not a screenshot
or a pasted dump. It goes through the same filter: login data and your Windows user name are blanked (`*`), folder
paths show as `<game>` and `%USERPROFILE%`, and a line at the end says how many bytes were hidden. The filter is a
best effort, so read the file before you post it. `save` never replaces a file that is already there unless you add
`overwrite=true` after the name, and it writes only `.txt` files on your own computer.

[`GAME_NOTES.md`](GAME_NOTES.md), next to this guide, lists the addresses that are already known: the
player, the resources, the lists of items and circuits. Note what you find and share it with other modders.
The inspector can also be used by an AI assistant that supports MCP tools; the assistant starts the script itself.
