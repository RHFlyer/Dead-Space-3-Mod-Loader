#pragma once

#include <windows.h>

// Drop-in mods: plugins\<folder>\mod.ini (format in sdk\README.md). Each mod found gets a [modules] switch in
// ds3_mods.ini (written as 0 when missing, so a new mod is off until switched on); only switched-on mods are loaded.
// A manifest that cannot be used is logged and skipped. A mod whose [mod] min_loader= is newer than this loader is
// listed (so the panel can say what it needs) but its settings, patches and DLL are left alone. Everything here runs on one thread at a time: discovery and
// loading on the game's first DirectInput8Create, the panel's reads and writes on the render thread afterwards.
//
// Basic mods: a plain .asi with no mod.ini, loose in plugins\ or directly inside a folder there that has no mod.ini.
// Each gets a switch under [basic_mods], keyed by its path inside plugins\ (new ones off), and is loaded as it is:
// no host, no settings, no reload, and none of the SDK's checks. A .dll is never listed.
//
// Safe start (startguard.h): after a start that did not finish, the next start loads no mod and writes no patch.

constexpr int kMaxMods = 32;
constexpr int kMaxModSettings = 128;  // over all mods
constexpr int kMaxSettingsPerMod = 24;
constexpr int kMaxModChoices = 8;

enum class ModSettingKind { Bool, Int, Float, Choice, Key };

struct ModChoice {
    wchar_t value[32];  // written to the ini
    char label[32];     // shown in the panel
};

struct ModSetting {
    int mod;  // index of its mod
    wchar_t section[48];
    wchar_t key[48];
    ModSettingKind kind;
    double minValue, maxValue, step, def;
    int decimals;       // Float: digits written and shown
    wchar_t onText[16];  // Bool: what is written for on / off
    wchar_t offText[16];
    ModChoice choices[kMaxModChoices];  // Choice: the value is the index
    int choiceCount;
    char label[48];
    char group[40];  // panel heading; the mod's name when not given
    char help[160];
    char unit[8];
    double value;    // what the ini holds now (Choice: index; Key: virtual-key code, 0 = none)
    double startup;  // what it held when the game started
    char name[32];   // from its section, [setting.<name>]: what a patch's value= / toggle= names
    bool patchBound;  // a patch takes its bytes from it (patches.h)
    bool patchLive;   // ... and every such patch can be rewritten while the game runs
};

struct Mod {
    char id[32];  // the [modules] key
    wchar_t idW[32];
    char name[48];
    char author[40];
    char version[16];
    char description[160];
    char reloadHint[16];  // [mod] reload, e.g. "F6": a key in game that makes the mod re-read its ini
    char minLoader[16];   // [mod] min_loader, e.g. "1.2.0": the oldest loader the mod works with
    bool needsLoader;     // ... and this loader is older: listed in the panel, never loaded
    wchar_t folder[64];
    wchar_t dir[MAX_PATH];   // full path of its folder
    wchar_t file[MAX_PATH];  // full path of the DLL, or empty for a patch-only mod
    wchar_t ini[MAX_PATH];   // full path of its settings ini, or empty
    bool enabled;    // the switch as the ini holds it now
    bool startup;    // the switch when the game started
    bool loaded;
    void(__cdecl* settingsChanged)();  // DS3ModSettingsChanged, if the loaded DLL exports it
    int settingCount;  // how many of the settings in ModSettingAt are this mod's
    bool basic;              // a plain .asi with no mod.ini
    wchar_t switchKey[128];  // basic: its key under [basic_mods]: the file's path inside the plugins folder
    char problem[40];       // basic: why this file can never be loaded; the panel's tag
    char notLoaded[40];      // basic: why it was left alone this time although on (another loader, no journal)
    bool changed;            // in a safe start: new or changed since the last start that finished
};

// Finds every mod in <moduleDir>\plugins\*\mod.ini, reads its switch from sealsIni (adding `<id>=0` when missing) and
// its settings from its own ini. Folders are taken in name order. reserved: [modules] keys of built-in parts, which
// a mod may not use (nullptr-terminated).
void DiscoverMods(const wchar_t* moduleDir, const wchar_t* sealsIni, const char* const* reserved);
// Loads every switched-on mod and calls its DS3ModInit.
void LoadMods();
// Whether a discovered mod uses this [modules] key.
bool ModIdTaken(const char* id);
// Whether this start is a safe start (no mod was loaded), and whether the player asked for it with safe_start=1.
bool ModsSafeStart();
bool ModsSafeStartAsked();
// The file name of another ASI loader found in the game (Ultimate ASI Loader), or "" if there is none. While one is
// there, basic mods are left to it.
const char* ModsOtherLoader();

int ModCount();
Mod& ModAt(int i);
int ModSettingCount();
ModSetting& ModSettingAt(int i);

// The panel's edits: write the new value to the file and, for a setting, tell the mod if it is loaded and listens.
bool WriteModSwitch(Mod& m);
bool WriteModSetting(ModSetting& s);
// Whether the running mod picks a setting change up at once (it is loaded and exports DS3ModSettingsChanged).
bool ModAppliesLive(const Mod& m);
// Whether a change of this setting applies at once: its patches are rewritten live, or the mod's DLL is told.
bool ModSettingAppliesLive(const ModSetting& s);
// Display text of a setting's current value.
void FormatModSetting(const ModSetting& s, char* out, size_t size);
// Display name of a virtual-key code ("F6", "Num 5", "None").
void KeyName(unsigned vk, char* out, size_t size);
