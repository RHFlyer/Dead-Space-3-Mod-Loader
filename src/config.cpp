#include "config.h"

#include <windows.h>

#include <algorithm>
#include <cwchar>

namespace {

constexpr Setting Flag(const wchar_t* section, const wchar_t* key, bool Config::*member, bool def, SettingApply apply,
                       const char* group, const char* label)
{
    return {section, key, SettingKind::Flag, member, nullptr, nullptr, def ? 1u : 0u, 0, 1, 1, apply, group, label,
            nullptr, nullptr};
}

constexpr Setting Int(const wchar_t* section, const wchar_t* key, int Config::*member, int def, int lo, int hi, int step,
                      SettingApply apply, const char* group, const char* label, const char* unit = nullptr)
{
    return {section, key, SettingKind::Int, nullptr, member, nullptr, static_cast<unsigned>(def), lo, hi, step, apply,
            group, label, unit, nullptr};
}

constexpr Setting Count(const wchar_t* section, const wchar_t* key, unsigned Config::*member, unsigned def, int hi,
                        SettingApply apply, const char* group, const char* label, const char* zeroText = nullptr)
{
    return {section, key, SettingKind::Count, nullptr, nullptr, member, def, 0, hi, 1, apply, group, label, nullptr,
            zeroText};
}

constexpr Setting Bits(const wchar_t* section, const wchar_t* key, unsigned Config::*member, unsigned def,
                       SettingApply apply, const char* group, const char* label)
{
    return {section, key, SettingKind::Bits, nullptr, nullptr, member, def, 0, 0, 0, apply, group, label, nullptr,
            nullptr};
}

constexpr SettingApply kLive = SettingApply::Live;
constexpr SettingApply kHidden = SettingApply::Hidden;

}  // namespace

// Panel order. Defaults here are the ones used when a key is missing (see Config for what each does).
const Setting kSettings[] = {
    Flag(L"modules", L"mods_menu", &Config::modsMenu, true, kHidden, "Mod parts", "Mods menu"),

    Int(L"mods_menu", L"text_size", &Config::modsMenuTextScale, 100, 50, 250, 10, kLive, "Mods menu", "Text size",
        "%"),
    Bits(L"mods_menu", L"key", &Config::modsMenuKey, 0x7A, kHidden, "Mods menu", "Key"),
    Bits(L"mods_menu", L"pad_button", &Config::modsMenuPadButton, 0x20, kHidden, "Mods menu", "Pad button"),
};
const int kSettingCount = static_cast<int>(sizeof(kSettings) / sizeof(kSettings[0]));

Config LoadConfig(const wchar_t* moduleDir)
{
    Config c;
    swprintf_s(c.iniPath, L"%s\\ds3_mods.ini", moduleDir);
    const wchar_t* path = c.iniPath;

    for (const Setting& s : kSettings) {
        switch (s.kind) {
        case SettingKind::Flag:
            c.*s.flag = GetPrivateProfileIntW(s.section, s.key, static_cast<INT>(s.defaultValue), path) != 0;
            break;
        case SettingKind::Int: {
            int v = static_cast<int>(GetPrivateProfileIntW(s.section, s.key, static_cast<INT>(s.defaultValue), path));
            c.*s.number = std::clamp(v, s.minValue, s.maxValue);
            break;
        }
        case SettingKind::Count: {
            unsigned v = GetPrivateProfileIntW(s.section, s.key, static_cast<INT>(s.defaultValue), path);
            c.*s.count = std::min(v, static_cast<unsigned>(s.maxValue));
            break;
        }
        case SettingKind::Bits: {
            wchar_t def[16], text[32];
            swprintf_s(def, L"0x%X", s.defaultValue);
            GetPrivateProfileStringW(s.section, s.key, def, text, 32, path);
            c.*s.count = static_cast<unsigned>(wcstoul(text, nullptr, 0));  // decimal or 0x hex
            break;
        }
        }
    }
    return c;
}

bool WriteSetting(const Config& c, const Setting& s)
{
    wchar_t text[32];
    switch (s.kind) {
    case SettingKind::Flag:
        swprintf_s(text, L"%d", c.*s.flag ? 1 : 0);
        break;
    case SettingKind::Int:
        swprintf_s(text, L"%d", c.*s.number);
        break;
    case SettingKind::Count:
        swprintf_s(text, L"%u", c.*s.count);
        break;
    case SettingKind::Bits:
        swprintf_s(text, L"0x%X", c.*s.count);
        break;
    }
    return WritePrivateProfileStringW(s.section, s.key, text, c.iniPath) != FALSE;
}
