#pragma once

// Settings from ds3_mods.ini next to the loader DLL. Missing file or keys = defaults.
struct Config {
    // (Drop-in mods, the mouse fix and the Seal Store among them, have their [modules] switches read by mods.cpp.)
    // [modules] mods_menu=1: the in-game Mods panel (opened from the pause menu). On when missing.
    bool modsMenu = true;
    // [mods_menu] text_size in percent (100 = 1/34 of the screen height). key: virtual-key code that opens the panel
    // while the pause menu is up (0x7A = F11, 0 = none); pad_button: XInput button mask that does the same (0x20 =
    // Back, 0 = none).
    int modsMenuTextScale = 100;
    unsigned modsMenuKey = 0x7A;
    unsigned modsMenuPadButton = 0x20;
    // ds3_mods.ini next to the DLL, the file the Mods panel writes back to.
    wchar_t iniPath[260] = {};
};

// One ini key. LoadConfig reads every entry of kSettings; the Mods panel lists those that are not Hidden, in table
// order under their group heading, and writes changes back with WriteSetting.
enum class SettingKind {
    Flag,   // bool, written 1 / 0
    Int,    // int, clamped to min..max
    Count,  // unsigned, clamped to min..max
    Bits,   // unsigned, written as 0x hex, read as decimal or 0x hex (ini only: the panel has no row for it)
};
enum class SettingApply {
    Live,     // the running game picks the change up at once
    Restart,  // read once when the game starts
    Hidden,   // in the ini only, not shown in the panel
};
struct Setting {
    const wchar_t* section;
    const wchar_t* key;
    SettingKind kind;
    bool Config::*flag;
    int Config::*number;
    unsigned Config::*count;  // Count and Bits
    unsigned defaultValue;
    int minValue;
    int maxValue;
    int step;
    SettingApply apply;
    const char* group;     // panel heading
    const char* label;     // panel row text
    const char* unit;      // after a number ("%", " s"), or nullptr
    const char* zeroText;  // shown instead of 0 ("no limit"), or nullptr
};
extern const Setting kSettings[];
extern const int kSettingCount;

Config LoadConfig(const wchar_t* moduleDir);
// Writes one setting's value in c to c.iniPath. Comments and the other keys stay as they are.
bool WriteSetting(const Config& c, const Setting& s);
