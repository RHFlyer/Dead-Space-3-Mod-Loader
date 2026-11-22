// Drop-in mods. See mods.h, and sdk\README.md for the manifest.

#include "mods.h"

#include "input.h"
#include "log.h"
#include "patches.h"
#include "pefile.h"
#include "sdk.h"
#include "startguard.h"
#include "version.h"

#include "../sdk/ds3mod.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>

#include <tlhelp32.h>

namespace {

constexpr DWORD kMaxManifestBytes = 64 * 1024;
constexpr int kMaxFolders = 64;

Mod g_mods[kMaxMods] = {};
int g_modCount = 0;
ModSetting g_settings[kMaxModSettings] = {};
int g_settingCount = 0;
DS3ModHost g_hosts[kMaxMods] = {};
wchar_t g_gameDir[MAX_PATH] = {};
wchar_t g_sealsIni[MAX_PATH] = {};
wchar_t g_pluginsDir[MAX_PATH] = {};
// What the file reader found for a basic mod, by mod index.
char g_sha[kMaxMods][65] = {};
unsigned long long g_fileSize[kMaxMods] = {};
bool g_safeStart = false;      // this start loads no mod (startguard.h)
char g_otherLoader[48] = {};   // file name of another ASI loader in the game, or empty

// ---- Text ----

// Printable ASCII only: the overlay's font has nothing else.
void ToAscii(const wchar_t* in, char* out, size_t size)
{
    size_t n = 0;
    for (; *in && n + 1 < size; ++in) {
        wchar_t ch = *in;
        out[n++] = ch == L'\t' ? ' ' : (ch >= 0x20 && ch < 0x7F) ? static_cast<char>(ch) : '?';
    }
    out[n] = '\0';
}

void Trim(wchar_t* s)
{
    size_t len = wcslen(s);
    while (len && iswspace(s[len - 1])) s[--len] = L'\0';
    size_t lead = 0;
    while (s[lead] && iswspace(s[lead])) ++lead;
    if (lead) wmemmove(s, s + lead, len - lead + 1);
}

void Read(const wchar_t* path, const wchar_t* section, const wchar_t* key, wchar_t* out, DWORD size)
{
    GetPrivateProfileStringW(section, key, L"", out, size, path);
    Trim(out);
}

void ReadAscii(const wchar_t* path, const wchar_t* section, const wchar_t* key, char* out, size_t size,
               const char* fallback = "")
{
    wchar_t text[256];
    Read(path, section, key, text, 256);
    if (text[0])
        ToAscii(text, out, size);
    else if (fallback != out)
        strncpy_s(out, size, fallback, _TRUNCATE);  // a fallback may be longer than out (e.g. the mod's name)
}

// "dir\name" into out (MAX_PATH); false if it does not fit.
bool JoinPath(wchar_t* out, const wchar_t* dir, const wchar_t* name)
{
    return _snwprintf_s(out, MAX_PATH, _TRUNCATE, L"%s\\%s", dir, name) >= 0;
}

// Numbers stay within this, so every value prints in a few characters.
constexpr double kMaxMagnitude = 1e9;

bool ParseNumber(const wchar_t* text, double* out)
{
    if (!text[0]) return false;
    wchar_t* end = nullptr;
    double v = wcstod(text, &end);
    if (end == text || *end || !std::isfinite(v) || std::fabs(v) > kMaxMagnitude) return false;
    *out = v;
    return true;
}

// A path inside the mod's folder: relative, no drive, no "..".
bool SafeRelative(const wchar_t* rel)
{
    if (!rel[0] || rel[0] == L'\\' || rel[0] == L'/') return false;
    return !wcschr(rel, L':') && !wcsstr(rel, L"..");
}

bool EndsWith(const wchar_t* s, const wchar_t* tail)
{
    size_t a = wcslen(s), b = wcslen(tail);
    return a >= b && _wcsicmp(s + a - b, tail) == 0;
}

// ---- Values ----

bool IsAny(const wchar_t* text, const wchar_t* const* words)
{
    for (; *words; ++words) {
        if (_wcsicmp(text, *words) == 0) return true;
    }
    return false;
}

bool ParseKey(const wchar_t* text, double* out)
{
    if (_wcsicmp(text, L"none") == 0) {
        *out = 0;
        return true;
    }
    if ((text[0] == L'F' || text[0] == L'f') && iswdigit(text[1])) {
        int n = _wtoi(text + 1);
        if (n >= 1 && n <= 24 && (!text[2] || (iswdigit(text[2]) && !text[3]))) {
            *out = 0x6F + n;  // VK_F1 = 0x70
            return true;
        }
    }
    if (text[0] && !text[1] && ((text[0] >= L'A' && text[0] <= L'Z') || (text[0] >= L'0' && text[0] <= L'9'))) {
        *out = text[0];
        return true;
    }
    if ((text[0] >= L'a' && text[0] <= L'z') && !text[1]) {
        *out = towupper(text[0]);
        return true;
    }
    wchar_t* end = nullptr;
    unsigned long v = wcstoul(text, &end, 0);  // decimal or 0x hex
    if (end == text || *end || v > 0xFF) return false;
    *out = static_cast<double>(v);
    return true;
}

bool ParseValue(const ModSetting& s, const wchar_t* raw, double* out)
{
    wchar_t text[64];
    wcsncpy_s(text, raw, _TRUNCATE);
    Trim(text);
    if (!text[0]) return false;
    switch (s.kind) {
    case ModSettingKind::Bool: {
        static const wchar_t* const kOn[] = {L"1", L"true", L"yes", L"on", nullptr};
        static const wchar_t* const kOff[] = {L"0", L"false", L"no", L"off", nullptr};
        if (_wcsicmp(text, s.onText) == 0 || IsAny(text, kOn)) {
            *out = 1;
            return true;
        }
        if (_wcsicmp(text, s.offText) == 0 || IsAny(text, kOff)) {
            *out = 0;
            return true;
        }
        return false;
    }
    case ModSettingKind::Int: {
        wchar_t* end = nullptr;
        long v = wcstol(text, &end, 0);
        if (end == text || *end) return false;
        *out = std::clamp(static_cast<double>(v), s.minValue, s.maxValue);
        return true;
    }
    case ModSettingKind::Float: {
        double v = 0;
        if (!ParseNumber(text, &v)) return false;
        *out = std::clamp(v, s.minValue, s.maxValue);
        return true;
    }
    case ModSettingKind::Choice:
        for (int i = 0; i < s.choiceCount; ++i) {
            if (_wcsicmp(text, s.choices[i].value) == 0) {
                *out = i;
                return true;
            }
        }
        return false;
    case ModSettingKind::Key:
        return ParseKey(text, out);
    }
    return false;
}

void ValueText(const ModSetting& s, double v, wchar_t* out, size_t size)
{
    switch (s.kind) {
    case ModSettingKind::Bool:
        wcscpy_s(out, size, v != 0 ? s.onText : s.offText);
        return;
    case ModSettingKind::Int:
        swprintf_s(out, size, L"%d", static_cast<int>(v));
        return;
    case ModSettingKind::Float:
        swprintf_s(out, size, L"%.*f", s.decimals, v);
        return;
    case ModSettingKind::Choice:
        wcscpy_s(out, size, s.choices[std::clamp(static_cast<int>(v), 0, s.choiceCount - 1)].value);
        return;
    case ModSettingKind::Key:
        swprintf_s(out, size, L"0x%02X", static_cast<unsigned>(v));
        return;
    }
}

double ReadValue(const ModSetting& s, const wchar_t* ini)
{
    wchar_t text[64];
    Read(ini, s.section, s.key, text, 64);
    double v = s.def;
    if (text[0] && !ParseValue(s, text, &v)) {
        Log("mod %s: [%ls] %ls = \"%ls\" in its ini is not a valid value; the default is used",
            g_mods[s.mod].id, s.section, s.key, text);
        v = s.def;
    }
    return v;
}

// ---- Manifest ----

bool ParseKind(const wchar_t* text, ModSettingKind* out)
{
    struct {
        const wchar_t* name;
        ModSettingKind kind;
    } constexpr kKinds[] = {{L"bool", ModSettingKind::Bool},     {L"int", ModSettingKind::Int},
                            {L"float", ModSettingKind::Float},   {L"choice", ModSettingKind::Choice},
                            {L"key", ModSettingKind::Key}};
    for (const auto& k : kKinds) {
        if (_wcsicmp(text, k.name) == 0) {
            *out = k.kind;
            return true;
        }
    }
    return false;
}

int DecimalsOf(const wchar_t* number)
{
    const wchar_t* dot = wcschr(number, L'.');
    if (!dot) return 0;
    int n = 0;
    for (const wchar_t* p = dot + 1; iswdigit(*p); ++p) ++n;
    return std::min(n, 6);
}

// "a,b" into two strings.
bool SplitPair(const wchar_t* text, wchar_t* a, size_t aSize, wchar_t* b, size_t bSize)
{
    const wchar_t* comma = wcschr(text, L',');
    if (!comma) return false;
    wcsncpy_s(a, aSize, text, std::min(static_cast<size_t>(comma - text), aSize - 1));
    wcsncpy_s(b, bSize, comma + 1, _TRUNCATE);
    Trim(a);
    Trim(b);
    return a[0] && b[0] && _wcsicmp(a, b) != 0;
}

// "value:Label|value:Label|...", a value alone being its own label.
int ParseChoices(const wchar_t* text, ModChoice* out)
{
    int n = 0;
    const wchar_t* p = text;
    while (*p && n < kMaxModChoices) {
        const wchar_t* bar = wcschr(p, L'|');
        size_t len = bar ? static_cast<size_t>(bar - p) : wcslen(p);
        wchar_t item[96] = {};
        wcsncpy_s(item, p, std::min(len, static_cast<size_t>(95)));
        wchar_t* colon = wcschr(item, L':');
        wchar_t label[64] = {};
        if (colon) {
            *colon = L'\0';
            wcsncpy_s(label, colon + 1, _TRUNCATE);
        }
        Trim(item);
        Trim(label);
        if (item[0]) {
            wcsncpy_s(out[n].value, item, _TRUNCATE);
            ToAscii(label[0] ? label : item, out[n].label, sizeof(out[n].label));
            ++n;
        }
        if (!bar) break;
        p = bar + 1;
    }
    return n;
}

// One [setting...] section. False (logged) if it cannot be used.
bool ParseSetting(int modIndex, const wchar_t* manifest, const wchar_t* sec, ModSetting& s)
{
    const Mod& m = g_mods[modIndex];
    s = {};
    s.mod = modIndex;
    wchar_t kind[16], text[512];
    Read(manifest, sec, L"section", s.section, 48);
    Read(manifest, sec, L"key", s.key, 48);
    Read(manifest, sec, L"kind", kind, 16);
    if (!s.section[0] || !s.key[0]) {
        Log("mod %s: [%ls] needs section= and key=; skipped", m.id, sec);
        return false;
    }
    if (_wcsicmp(s.section, L"debug") == 0) {
        Log("mod %s: [%ls] is in a [debug] section, which the panel never lists; skipped", m.id, sec);
        return false;
    }
    if (!ParseKind(kind, &s.kind)) {
        Log("mod %s: [%ls] kind=\"%ls\" is not bool, int, float, choice or key; skipped", m.id, sec, kind);
        return false;
    }
    // The name a patch refers to it by: [setting.speed] is "speed".
    const wchar_t* name = sec + 7;
    if (*name == L'.' || *name == L' ' || *name == L':' || *name == L'_') ++name;
    ToAscii(name, s.name, sizeof(s.name));
    ToAscii(s.key, s.label, sizeof(s.label));
    ReadAscii(manifest, sec, L"label", s.label, sizeof(s.label), s.label);
    ReadAscii(manifest, sec, L"group", s.group, sizeof(s.group), m.name);
    ReadAscii(manifest, sec, L"help", s.help, sizeof(s.help));
    ReadAscii(manifest, sec, L"unit", s.unit, sizeof(s.unit));

    switch (s.kind) {
    case ModSettingKind::Bool:
        wcscpy_s(s.onText, L"1");
        wcscpy_s(s.offText, L"0");
        Read(manifest, sec, L"values", text, 512);
        if (text[0] && !SplitPair(text, s.onText, 16, s.offText, 16)) {
            Log("mod %s: [%ls] values=\"%ls\" should be \"on,off\" (e.g. true,false); 1,0 is used", m.id, sec, text);
            wcscpy_s(s.onText, L"1");
            wcscpy_s(s.offText, L"0");
        }
        s.minValue = 0, s.maxValue = 1, s.step = 1;
        break;
    case ModSettingKind::Int:
    case ModSettingKind::Float: {
        wchar_t lo[32], hi[32], step[32];
        Read(manifest, sec, L"min", lo, 32);
        Read(manifest, sec, L"max", hi, 32);
        Read(manifest, sec, L"step", step, 32);
        if (!ParseNumber(lo, &s.minValue) || !ParseNumber(hi, &s.maxValue) || s.minValue >= s.maxValue) {
            Log("mod %s: [%ls] needs min= and max= numbers with min below max; skipped", m.id, sec);
            return false;
        }
        const bool isInt = s.kind == ModSettingKind::Int;
        if (!ParseNumber(step, &s.step) || s.step <= 0) s.step = isInt ? 1 : 0.1;
        if (isInt) {
            s.minValue = std::ceil(s.minValue), s.maxValue = std::floor(s.maxValue);
            s.step = std::max(1.0, std::round(s.step));
        } else {
            wchar_t dec[8];
            Read(manifest, sec, L"decimals", dec, 8);
            s.decimals = dec[0] ? std::clamp(_wtoi(dec), 0, 6) : std::max(DecimalsOf(step[0] ? step : L"0.1"), 1);
        }
        break;
    }
    case ModSettingKind::Choice:
        Read(manifest, sec, L"choices", text, 512);
        s.choiceCount = ParseChoices(text, s.choices);
        if (s.choiceCount < 2) {
            Log("mod %s: [%ls] needs choices=value:Label|value:Label (2 to %d); skipped", m.id, sec, kMaxModChoices);
            return false;
        }
        s.minValue = 0, s.maxValue = s.choiceCount - 1, s.step = 1;
        break;
    case ModSettingKind::Key:
        s.minValue = 0, s.maxValue = 255, s.step = 1;
        break;
    }

    s.def = s.kind == ModSettingKind::Int || s.kind == ModSettingKind::Float ? s.minValue : 0;
    Read(manifest, sec, L"default", text, 64);
    if (text[0] && !ParseValue(s, text, &s.def))
        Log("mod %s: [%ls] default=\"%ls\" is not a valid value; using the lowest", m.id, sec, text);
    return true;
}

// The [modules] key from the folder name: lower case, anything but a-z 0-9 _ becomes _.
void IdFromFolder(const wchar_t* folder, char* out, size_t size)
{
    size_t n = 0;
    for (; *folder && n + 1 < size; ++folder) {
        wchar_t ch = towlower(*folder);
        out[n++] = (ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9') ? static_cast<char>(ch) : '_';
    }
    out[n] = '\0';
}

bool ValidId(const char* id)
{
    if (!id[0]) return false;
    for (const char* p = id; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
    }
    return true;
}

// "1.2.0" into three numbers. One or two numbers count too ("1.2" is 1.2.0), and a tail after '-' or '+' is ignored
// ("1.1.2-slider"). False if it does not start with a number, or holds anything else.
bool ParseVersion(const char* text, unsigned out[3])
{
    out[0] = out[1] = out[2] = 0;
    const char* p = text;
    for (int part = 0; part < 3; ++part) {
        if (*p < '0' || *p > '9') return false;
        unsigned v = 0;
        for (int digits = 0; *p >= '0' && *p <= '9'; ++p) {
            if (++digits > 6) return false;
            v = v * 10 + static_cast<unsigned>(*p - '0');
        }
        out[part] = v;
        if (*p != '.' || part == 2) break;
        ++p;
    }
    return !*p || *p == '-' || *p == '+';
}

// Whether version a is older than version b.
bool VersionBelow(const unsigned a[3], const unsigned b[3])
{
    for (int i = 0; i < 3; ++i) {
        if (a[i] != b[i]) return a[i] < b[i];
    }
    return false;
}

// The mod's [modules] switch: read, and added as off when the mod is new.
void ReadSwitch(Mod& m)
{
    wchar_t text[16];
    Read(g_sealsIni, L"modules", m.idW, text, 16);
    if (!text[0]) {
        WritePrivateProfileStringW(L"modules", m.idW, L"0", g_sealsIni);
        Log("mods: new mod \"%s\" (plugins\\%ls) found; added [modules] %s=0, switch it on in the Mods panel", m.name,
            m.folder, m.id);
    }
    static const wchar_t* const kOn[] = {L"1", L"true", L"yes", L"on", nullptr};
    m.enabled = m.startup = IsAny(text, kOn);
}

bool ParseMod(const wchar_t* pluginsDir, const wchar_t* folder, const char* const* reserved)
{
    Mod& m = g_mods[g_modCount];
    m = {};
    wcsncpy_s(m.folder, folder, _TRUNCATE);
    wchar_t manifest[MAX_PATH];
    if (!JoinPath(m.dir, pluginsDir, folder) || !JoinPath(manifest, m.dir, L"mod.ini")) {
        Log("mods: plugins\\%ls: path too long; skipped", folder);
        return false;
    }

    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(manifest, GetFileExInfoStandard, &fa)) {
        Log("mods: plugins\\%ls has no mod.ini; not a mod, skipped", folder);
        return false;
    }
    if (fa.nFileSizeHigh || fa.nFileSizeLow > kMaxManifestBytes) {
        Log("mods: plugins\\%ls\\mod.ini is larger than %lu KB; skipped", folder, kMaxManifestBytes / 1024);
        return false;
    }

    wchar_t text[MAX_PATH];
    Read(manifest, L"mod", L"id", text, 32);
    if (text[0]) {
        ToAscii(text, m.id, sizeof(m.id));
        if (!ValidId(m.id)) {
            Log("mods: plugins\\%ls\\mod.ini: id=\"%s\" may only hold a-z, 0-9 and _; skipped", folder, m.id);
            return false;
        }
    } else {
        IdFromFolder(folder, m.id, sizeof(m.id));
    }
    for (const char* const* r = reserved; *r; ++r) {
        if (strcmp(*r, m.id) == 0) {
            Log("mods: plugins\\%ls: id \"%s\" is a built-in part of the proxy; give the mod another id", folder, m.id);
            return false;
        }
    }
    if (ModIdTaken(m.id)) {
        Log("mods: plugins\\%ls: id \"%s\" is already used by another mod; skipped", folder, m.id);
        return false;
    }
    for (int i = 0; m.id[i]; ++i) m.idW[i] = static_cast<wchar_t>(m.id[i]);

    char folderAscii[64];
    ToAscii(folder, folderAscii, sizeof(folderAscii));
    ReadAscii(manifest, L"mod", L"name", m.name, sizeof(m.name), folderAscii);
    ReadAscii(manifest, L"mod", L"author", m.author, sizeof(m.author));
    ReadAscii(manifest, L"mod", L"version", m.version, sizeof(m.version));
    ReadAscii(manifest, L"mod", L"description", m.description, sizeof(m.description));
    ReadAscii(manifest, L"mod", L"reload", m.reloadHint, sizeof(m.reloadHint));

    // min_loader= is the oldest loader the mod works with. With an older loader the mod is listed, so the panel can
    // say what it needs, and nothing else of its manifest is read: it may use keys this loader does not know.
    ReadAscii(manifest, L"mod", L"min_loader", m.minLoader, sizeof(m.minLoader));
    if (m.minLoader[0]) {
        unsigned need[3], have[3];
        if (!ParseVersion(m.minLoader, need)) {
            Log("mods: plugins\\%ls\\mod.ini: min_loader=\"%s\" is not a version like 1.2.0; ignored", folder,
                m.minLoader);
            m.minLoader[0] = '\0';
        } else if (ParseVersion(DS3_PROXY_VERSION, have) && VersionBelow(have, need)) {
            m.needsLoader = true;
            Log("mods: \"%s\" (plugins\\%ls) needs DS3 Mod Loader %s or later, and this is " DS3_PROXY_VERSION
                "; it is listed in the Mods panel but not loaded. Get the newer loader.", m.name, folder, m.minLoader);
            ReadSwitch(m);
            ++g_modCount;
            return true;
        }
    }

    // file= is the mod's DLL; a patch-only mod has none.
    Read(manifest, L"mod", L"file", text, MAX_PATH);
    if (text[0] && (!SafeRelative(text) || !(EndsWith(text, L".asi") || EndsWith(text, L".dll")) ||
                    !JoinPath(m.file, m.dir, text))) {
        Log("mods: plugins\\%ls\\mod.ini: file=\"%ls\" must be a .asi or .dll inside the mod's folder; skipped", folder,
            text);
        return false;
    }

    static wchar_t names[8192];
    DWORD len = GetPrivateProfileSectionNamesW(names, 8192, manifest);
    bool anySetting = false;
    for (const wchar_t* sec = names; len && *sec; sec += wcslen(sec) + 1) anySetting |= _wcsnicmp(sec, L"setting", 7) == 0;

    // ini= holds the player's values; settings.ini in the mod's folder when not given.
    Read(manifest, L"mod", L"ini", text, MAX_PATH);
    if (!text[0] && anySetting) wcscpy_s(text, L"settings.ini");
    if (text[0]) {
        if (!SafeRelative(text) || !EndsWith(text, L".ini") || _wcsicmp(text, L"mod.ini") == 0 ||
            !JoinPath(m.ini, m.dir, text)) {
            m.ini[0] = L'\0';
            Log("mods: plugins\\%ls\\mod.ini: ini=\"%ls\" must be an .ini inside the mod's folder, not mod.ini; the "
                "mod's settings are not shown", folder, text);
        }
    }

    // Settings: every section whose name starts with "setting", in file order.
    const int firstSetting = g_settingCount;
    int skipped = 0;
    for (const wchar_t* sec = names; len && *sec; sec += wcslen(sec) + 1) {
        if (_wcsnicmp(sec, L"setting", 7) != 0) continue;
        if (!m.ini[0]) {
            ++skipped;
            continue;
        }
        if (m.settingCount >= kMaxSettingsPerMod || g_settingCount >= kMaxModSettings) {
            Log("mod %s: more than %d settings (or %d over all mods); [%ls] and later ones are not shown", m.id,
                kMaxSettingsPerMod, kMaxModSettings, sec);
            break;
        }
        ModSetting& s = g_settings[g_settingCount];
        if (!ParseSetting(g_modCount, manifest, sec, s)) continue;
        s.value = s.startup = ReadValue(s, m.ini);
        ++g_settingCount;
        ++m.settingCount;
    }
    if (skipped) Log("mod %s: %d setting(s) ignored: [mod] has no usable ini=", m.id, skipped);

    // Patches, which may take their bytes from the settings above. A mod with one unusable patch is skipped whole.
    if (!ParseModPatches(g_modCount, manifest)) {
        g_settingCount = firstSetting;
        if (!m.touchedProtected) return false;
        // Refused as always (no patch of it is kept and it is never loaded), but listed, so the panel can say why.
        m.settingCount = 0;
        strcpy_s(m.problem, "protected code refused");
        ReadSwitch(m);
        ++g_modCount;
        return true;
    }
    if (!m.file[0] && !ModPatchCount(g_modCount)) {
        Log("mods: plugins\\%ls\\mod.ini has neither file= under [mod] nor any [patch] section; skipped", folder);
        g_settingCount = firstSetting;
        return false;
    }

    ReadSwitch(m);
    ++g_modCount;
    return true;
}

// ---- Loading ----

int __cdecl HostInputBlocked()
{
    return InputBlocked() ? 1 : 0;
}

void __cdecl HostLog(const DS3ModHost* host, const char* text)
{
    if (!host || !text) return;
    Log("[%s] %.900s", host->modId ? host->modId : "?", text);
}

// Kept apart from the C++ code around it so __try is allowed.
int CallInit(DS3ModInitFn init, const DS3ModHost* host, bool* crashed)
{
    __try {
        return init(host);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *crashed = true;
        return -1;
    }
}

bool CallChanged(void(__cdecl* fn)())
{
    __try {
        fn();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- Hot reload ----
//
// A DLL mod that exports DS3ModShutdown is loaded from a copy in %TEMP%\ds3mods, so its own file stays free for the
// next build. Twice a second (on the game's main thread, before the mods' frame callbacks) the proxy looks at that file
// and at every loaded mod's settings ini. A new build that has stopped changing for a second is swapped in over a few
// frames: callbacks and hooks off, wait until no hook is mid-call, DS3ModShutdown, hooks and patches undone,
// FreeLibrary, then the new copy is loaded and initialised like at start.

constexpr ULONGLONG kPollMs = 500;
constexpr ULONGLONG kSettleMs = 1000;    // a changed file must look the same this long before it is loaded
constexpr ULONGLONG kIdleMs = 100;       // hooks idle at two checks this far apart
constexpr ULONGLONG kGiveUpMs = 5000;    // a hook still mid-call after this: keep the old build, say restart

enum class HotPhase { Idle, Draining };

struct HotState {
    bool reloadable;  // the loaded build exports DS3ModShutdown and runs from a temp copy
    bool stuck;       // an unload could not finish: never try again this game
    HMODULE dll;
    void(__cdecl* shutdown)();
    wchar_t temp[MAX_PATH];  // the copy it runs from
    FILETIME fileTime;       // of the mod's own file when this build was copied
    ULONGLONG fileSize;
    FILETIME seenTime;  // a different file seen, waiting for it to settle
    ULONGLONG seenSize;
    ULONGLONG seenAt;
    FILETIME iniTime;  // of its settings ini, as last read
    bool iniKnown;
    HotPhase phase;
    ULONGLONG since;   // the unload started
    ULONGLONG idleAt;  // first idle check, 0 = none yet
    int build;         // builds loaded (1 = the one from the start)
};
HotState g_hot[kMaxMods] = {};
bool g_hotOn = true;
// [mods] hot_reload_free=1: FreeLibrary an old build once its hooks are idle. Off by default: the old copy stays mapped
// (never called again), so a thread or pointer the mod forgot to let go of can't run into freed memory. Costs the
// image size per reload until the game exits; the copies are deleted at the next start.
bool g_hotFree = false;
int g_keptBuilds = 0;
wchar_t g_tempDir[MAX_PATH] = {};
unsigned g_tempSerial = 0;

bool FileStamp(const wchar_t* path, FILETIME* time, ULONGLONG* size)
{
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a)) return false;
    *time = a.ftLastWriteTime;
    if (size) *size = (static_cast<ULONGLONG>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    return true;
}

bool SameTime(const FILETIME& a, const FILETIME& b)
{
    return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime;
}

// Nobody has the file open for writing (the linker is done with it).
bool FileSettled(const wchar_t* path)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

// Whether the DLL file exports `name`, read from the file's own bytes without mapping or running it.
bool FileExports(const wchar_t* file, const char* name)
{
    PeFile pe(file);
    return pe.Exports(name);
}

// %TEMP%\ds3mods, made on first use; copies left from earlier games are deleted then (a locked one is skipped).
bool TempDir()
{
    if (g_tempDir[0]) return true;
    wchar_t tmp[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n && n < MAX_PATH && tmp[n - 1] == L'\\') tmp[n - 1] = L'\0';  // JoinPath adds the backslash
    if (!n || n >= MAX_PATH || !JoinPath(g_tempDir, tmp, L"ds3mods")) {
        g_tempDir[0] = L'\0';
        return false;
    }
    CreateDirectoryW(g_tempDir, nullptr);
    wchar_t pattern[MAX_PATH], old[MAX_PATH];
    if (JoinPath(pattern, g_tempDir, L"*.asi")) {
        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && JoinPath(old, g_tempDir, fd.cFileName))
                    DeleteFileW(old);
            } while (FindNextFileW(find, &fd));
            FindClose(find);
        }
    }
    return true;
}

void ReadIniStamp(int i)
{
    const Mod& m = g_mods[i];
    g_hot[i].iniKnown = m.ini[0] && FileStamp(m.ini, &g_hot[i].iniTime, nullptr);
}

// ---- Basic mods: discovery ----

struct FileName {
    wchar_t name[64];
};

bool ByName(const FileName& a, const FileName& b)
{
    return _wcsicmp(a.name, b.name) < 0;
}

bool HasManifest(const wchar_t* folder)
{
    wchar_t dir[MAX_PATH], manifest[MAX_PATH];
    return JoinPath(dir, g_pluginsDir, folder) && JoinPath(manifest, dir, L"mod.ini") &&
           GetFileAttributesW(manifest) != INVALID_FILE_ATTRIBUTES;
}

// A path that can be an ini key and be shown in the panel's ASCII font.
bool PlainName(const wchar_t* s)
{
    if (!s[0] || s[0] == L';' || s[0] == L'[' || s[0] == L' ') return false;
    const wchar_t* last = s;
    for (; *s; ++s) {
        if (*s < 0x20 || *s > 0x7E || *s == L'=') return false;
        last = s;
    }
    return *last != L' ';
}

const wchar_t* BaseName(const wchar_t* path)
{
    const wchar_t* slash = wcsrchr(path, L'\\');
    return slash ? slash + 1 : path;
}

bool IsReserved(const char* id, const char* const* reserved)
{
    for (const char* const* r = reserved; *r; ++r) {
        if (strcmp(*r, id) == 0) return true;
    }
    return false;
}

// The basic mod's [basic_mods] switch: read, and added as off when the file is new.
void ReadBasicSwitch(Mod& m)
{
    wchar_t text[16];
    Read(g_sealsIni, L"basic_mods", m.switchKey, text, 16);
    if (!text[0]) {
        WritePrivateProfileStringW(L"basic_mods", m.switchKey, L"0", g_sealsIni);
        Log("mods: new basic mod \"%s\" (plugins\\%ls) found; added [basic_mods] %ls=0, switch it on in the Mods panel",
            m.name, m.switchKey, m.switchKey);
    }
    static const wchar_t* const kOn[] = {L"1", L"true", L"yes", L"on", nullptr};
    m.enabled = m.startup = IsAny(text, kOn);
}

// One .asi with no mod.ini: `folder` is its folder inside plugins\, or empty for a loose file.
void AddBasicMod(const wchar_t* folder, const wchar_t* fileName, const char* const* reserved)
{
    wchar_t rel[MAX_PATH];
    if (folder[0]) {
        if (!JoinPath(rel, folder, fileName)) return;
    } else {
        wcscpy_s(rel, fileName);
    }
    if (g_modCount >= kMaxMods) {
        Log("mods: more than %d mods; plugins\\%ls is not listed", kMaxMods, rel);
        return;
    }
    if (wcslen(rel) >= 128 || !PlainName(rel)) {
        Log("mods: plugins\\%ls can't be listed: its name (and its folder's) must be plain letters, digits and simple "
            "punctuation without '=', under 128 characters in all. Rename it", rel);
        return;
    }
    Mod& m = g_mods[g_modCount];
    m = {};
    m.basic = true;
    wcsncpy_s(m.folder, folder, _TRUNCATE);
    wcscpy_s(m.switchKey, rel);
    const bool placed = JoinPath(m.file, g_pluginsDir, rel) &&
                        (folder[0] ? JoinPath(m.dir, g_pluginsDir, folder) : wcscpy_s(m.dir, g_pluginsDir) == 0);
    if (!placed) {
        Log("mods: plugins\\%ls: path too long; skipped", rel);
        return;
    }

    wchar_t base[64] = {};
    wcsncpy_s(base, fileName, wcslen(fileName) - 4);  // without ".asi"
    ToAscii(base, m.name, sizeof(m.name));
    if (!m.name[0]) strcpy_s(m.name, "(no name)");
    // A name for log lines, made unique. It is never a key: the switch is keyed by the path.
    char stem[24];
    IdFromFolder(base, stem, sizeof(stem));
    if (!stem[0]) strcpy_s(stem, "basic");
    strcpy_s(m.id, stem);
    for (int n = 2; n < 100 && (ModIdTaken(m.id) || IsReserved(m.id, reserved)); ++n) sprintf_s(m.id, "%s_%d", stem, n);
    for (int i = 0; m.id[i]; ++i) m.idW[i] = static_cast<wchar_t>(m.id[i]);

    PeFile pe(m.file);
    g_fileSize[g_modCount] = pe.Size();
    pe.Sha256(g_sha[g_modCount]);
    if (!pe.Read()) {
        strcpy_s(m.problem, "can't be read");
        Log("mods: basic mod \"%s\" (plugins\\%ls) can't be read (missing, empty, in use, or over %llu MB); not loaded",
            m.name, rel, PeFile::kMaxBytes >> 20);
    } else if (!pe.X86Dll()) {
        strcpy_s(m.problem, "not a 32-bit DLL: can't load");
        Log("mods: basic mod \"%s\" (plugins\\%ls) is not a 32-bit DLL (a 64-bit build, or not a DLL at all); Dead "
            "Space 3 is a 32-bit game, so it can't be loaded", m.name, rel);
    } else if (pe.Exports("DS3ModInit")) {
        strcpy_s(m.problem, "needs its mod.ini");
        Log("mods: basic mod \"%s\" (plugins\\%ls) is a DS3 Mod Loader mod without its mod.ini; copy the mod's whole "
            "folder into plugins\\. Not loaded", m.name, rel);
    }

    // The same file name as a mod with a mod.ini: maybe a second copy of it. Only a suspicion, so it is still listed.
    for (int i = 0; i < g_modCount; ++i) {
        const Mod& other = g_mods[i];
        if (other.basic || !other.file[0] || _wcsicmp(BaseName(other.file), fileName) != 0) continue;
        _snprintf_s(m.description, _TRUNCATE, "May be a second copy of %s: use one of the two.", other.name);
        Log("mods: basic mod \"%s\" (plugins\\%ls) has the same file name as the mod \"%s\" (plugins\\%ls): it may be "
            "a second copy. Use one of the two", m.name, rel, other.name, other.folder);
        break;
    }
    ReadBasicSwitch(m);
    ++g_modCount;
}

// Every .asi directly inside a folder that has no mod.ini.
void AddBasicModsIn(const wchar_t* folder, const char* const* reserved)
{
    constexpr int kMaxPerFolder = 8;
    FileName files[kMaxPerFolder];
    int count = 0;
    wchar_t dir[MAX_PATH], pattern[MAX_PATH];
    if (!JoinPath(dir, g_pluginsDir, folder) || !JoinPath(pattern, dir, L"*.asi")) {
        Log("mods: plugins\\%ls: path too long; skipped", folder);
        return;
    }
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (!EndsWith(fd.cFileName, L".asi")) continue;  // "*.asi" also matches longer endings (8.3 names)
            if (wcslen(fd.cFileName) >= 64)
                Log("mods: plugins\\%ls\\%ls: file name too long; skipped", folder, fd.cFileName);
            else if (count < kMaxPerFolder)
                wcscpy_s(files[count++].name, fd.cFileName);
            else
                Log("mods: plugins\\%ls: more than %d .asi files; %ls is not listed", folder, kMaxPerFolder, fd.cFileName);
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    if (!count) {
        Log("mods: plugins\\%ls has no mod.ini and no .asi file; nothing to load there", folder);
        return;
    }
    std::sort(files, files + count, ByName);
    for (int i = 0; i < count; ++i) AddBasicMod(folder, files[i].name, reserved);
}

// Consent follows the file: a [basic_mods] line whose file is gone (or now belongs to a mod with a mod.ini) is
// removed, so a different file put there later starts switched off.
void PruneBasicSwitches()
{
    static wchar_t block[16384];
    const DWORD n = GetPrivateProfileSectionW(L"basic_mods", block, _countof(block), g_sealsIni);
    if (n == 0 || n >= _countof(block) - 2) return;
    for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
        const wchar_t* eq = wcschr(p, L'=');
        if (!eq || eq == p || eq - p >= 128) continue;
        wchar_t key[128], path[MAX_PATH], folder[128];
        wcsncpy_s(key, p, static_cast<size_t>(eq - p));
        Trim(key);
        bool listed = false;
        for (int i = 0; i < g_modCount && !listed; ++i)
            listed = g_mods[i].basic && _wcsicmp(g_mods[i].switchKey, key) == 0;
        if (listed) continue;
        if (!SafeRelative(key) || !JoinPath(path, g_pluginsDir, key)) continue;
        wcscpy_s(folder, key);
        wchar_t* slash = wcsrchr(folder, L'\\');
        if (slash) *slash = L'\0';
        const bool there = GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
        if (there && !(slash && HasManifest(folder))) continue;  // there, but over a limit this time: keep its switch
        WritePrivateProfileStringW(L"basic_mods", key, nullptr, g_sealsIni);
        Log("mods: [basic_mods] %ls removed: %s", key,
            there ? "its folder now has a mod.ini, which decides" : "that file is gone (a file put there later starts off)");
    }
}

}  // namespace

void DiscoverMods(const wchar_t* moduleDir, const wchar_t* sealsIni, const char* const* reserved)
{
    wcscpy_s(g_gameDir, moduleDir);
    wcscpy_s(g_sealsIni, sealsIni);
    wchar_t pattern[MAX_PATH];
    if (!JoinPath(g_pluginsDir, moduleDir, L"plugins") || !JoinPath(pattern, g_pluginsDir, L"*")) {
        Log("mods: the game folder's path is too long; no drop-in mods");
        return;
    }

    static FileName folders[kMaxFolders], loose[kMaxFolders];
    static bool bare[kMaxFolders];  // a folder without a mod.ini
    int count = 0, looseCount = 0;
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_PATH_NOT_FOUND || error == ERROR_FILE_NOT_FOUND) {
            Log("mods: no plugins folder; no drop-in mods");
            PruneBasicSwitches();  // the folder is gone, and with it every file a [basic_mods] line stood for
        } else {
            Log("mods: the plugins folder could not be read (error %lu); no drop-in mods this time", error);
        }
        return;
    }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            if (wcslen(fd.cFileName) >= 64) {
                Log("mods: plugins\\%ls: folder name too long; skipped", fd.cFileName);
            } else if (count < kMaxFolders) {
                wcscpy_s(folders[count++].name, fd.cFileName);
            }
        } else if (EndsWith(fd.cFileName, L".asi")) {
            if (wcslen(fd.cFileName) >= 64)
                Log("mods: plugins\\%ls: file name too long; skipped", fd.cFileName);
            else if (looseCount < kMaxFolders)
                wcscpy_s(loose[looseCount++].name, fd.cFileName);
        } else if (EndsWith(fd.cFileName, L".dll")) {
            Log("mods: plugins\\%ls is a .dll, so it is not listed. If its maker says it is an ASI mod, rename it to end "
                "in .asi, or give it a folder with a mod.ini (see the README)", fd.cFileName);
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);

    std::sort(folders, folders + count, ByName);
    std::sort(loose, loose + looseCount, ByName);
    // Mods with a mod.ini first, so they keep their place if the limit is reached.
    for (int i = 0; i < count; ++i) {
        bare[i] = !HasManifest(folders[i].name);
        if (bare[i]) continue;
        if (g_modCount >= kMaxMods) {
            Log("mods: more than %d mods; plugins\\%ls and later folders are skipped", kMaxMods, folders[i].name);
            break;
        }
        ParseMod(g_pluginsDir, folders[i].name, reserved);
    }
    const int withManifest = g_modCount;
    // Basic mods: loose .asi files, then the .asi files of folders without a mod.ini.
    for (int i = 0; i < looseCount; ++i) AddBasicMod(L"", loose[i].name, reserved);
    for (int i = 0; i < count; ++i) {
        if (bare[i]) AddBasicModsIn(folders[i].name, reserved);
    }
    PruneBasicSwitches();
    Log("mods: %d drop-in mod(s) and %d basic mod(s) found, %d setting(s) for the panel", withManifest,
        g_modCount - withManifest, g_settingCount);
}

namespace {

// Loads mod i's DLL (for a reloadable mod: a fresh temp copy of it) and runs DS3ModInit. First: the load at start.
bool LoadModDll(int i, bool first)
{
    Mod& m = g_mods[i];
    HotState& hot = g_hot[i];
    FILETIME stamp{};
    ULONGLONG size = 0;
    const bool stamped = FileStamp(m.file, &stamp, &size);
    // Reloadable if it can let go of everything: it exports DS3ModShutdown. Read from the file, without running it.
    const bool reloadable = g_hotOn && FileExports(m.file, "DS3ModShutdown") && TempDir();
    const wchar_t* from = m.file;
    wchar_t temp[MAX_PATH] = {};
    if (reloadable) {
        wchar_t name[96];
        swprintf_s(name, L"%ls-%lu-%u.asi", m.idW, GetCurrentProcessId(), ++g_tempSerial);
        if (!JoinPath(temp, g_tempDir, name) || !CopyFileW(m.file, temp, FALSE)) {
            Log("mod %s: could not copy %ls to %ls (error %lu); not loaded", m.id, m.file, temp, GetLastError());
            return false;
        }
        from = temp;
    } else if (!first) {
        Log("mod %s: the new build has no DS3ModShutdown, so it can't be swapped in while the game runs; restart the "
            "game to load it", m.id);
        return false;
    }
    HMODULE dll = nullptr;
    if (reloadable) {
        // From the temp folder, so add the mod's own folder to the search path: DLLs next to it are still found.
        DLL_DIRECTORY_COOKIE cookie = AddDllDirectory(m.dir);
        dll = LoadLibraryExW(from, nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        const DWORD err = GetLastError();
        if (cookie) RemoveDllDirectory(cookie);
        SetLastError(err);
    } else {
        // Altered search path: the mod's own DLLs are found in its folder.
        dll = LoadLibraryExW(from, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
    if (!dll) {
        Log("mod %s: FAILED to load %ls (error %lu)", m.id, from, GetLastError());
        if (reloadable) DeleteFileW(temp);
        return false;
    }
    m.loaded = true;
    hot.dll = dll;
    hot.reloadable = reloadable;
    hot.shutdown = reinterpret_cast<void(__cdecl*)()>(GetProcAddress(dll, "DS3ModShutdown"));
    wcscpy_s(hot.temp, temp);
    if (stamped) {
        hot.fileTime = hot.seenTime = stamp;
        hot.fileSize = hot.seenSize = size;
    }
    ++hot.build;
    if (first)
        Log("mod %s: loaded %ls at %p (%s%s%s)%s", m.id, from, dll, m.name, m.version[0] ? " " : "", m.version,
            reloadable ? "; it can be rebuilt while the game runs" : "");
    else
        Log("mod %s: build %d loaded from %ls at %p", m.id, hot.build, from, dll);

    DS3ModHost& h = g_hosts[i];
    h = {sizeof(DS3ModHost), DS3MOD_API_VERSION, DS3_PROXY_VERSION, m.id, m.dir, m.ini, g_gameDir, &HostLog,
         &HostInputBlocked};
    SdkFillHost(h, i);  // API 2: hook, patch, game reads, frame callbacks
    SdkSetReloadable(i, reloadable);
    if (auto init = reinterpret_cast<DS3ModInitFn>(GetProcAddress(dll, "DS3ModInit"))) {
        bool crashed = false;
        int r = CallInit(init, &h, &crashed);
        if (crashed)
            Log("mod %s: DS3ModInit crashed (caught); the mod may not work", m.id);
        else if (r != 0)
            Log("mod %s: DS3ModInit returned %d (failure)", m.id, r);
    }
    m.settingsChanged = reinterpret_cast<DS3ModSettingsChangedFn>(GetProcAddress(dll, "DS3ModSettingsChanged"));
    return true;
}

// The last steps of an unload, once no hook of the mod is mid-call; then the new build.
void FinishReload(int i)
{
    Mod& m = g_mods[i];
    HotState& hot = g_hot[i];
    if (hot.shutdown && !CallChanged(hot.shutdown))
        Log("mod %s: DS3ModShutdown crashed (caught); unloading anyway", m.id);
    SdkModRelease(i);
    m.loaded = false;
    m.settingsChanged = nullptr;
    hot.shutdown = nullptr;
    if (g_hotFree) {
        FreeLibrary(hot.dll);
        if (hot.temp[0]) DeleteFileW(hot.temp);
        Log("mod %s: build %d unloaded and freed", m.id, hot.build);
    } else {
        ++g_keptBuilds;
        Log("mod %s: build %d let go (left mapped, never called again; %d old build(s) kept until the game exits)",
            m.id, hot.build, g_keptBuilds);
    }
    hot.dll = nullptr;
    hot.temp[0] = L'\0';
    hot.phase = HotPhase::Idle;

    char note[128];
    if (LoadModDll(i, false))
        sprintf_s(note, "%s reloaded (build %d)", m.name, hot.build);
    else
        sprintf_s(note, "%s: new build NOT loaded, see ds3_mods.log", m.name);
    SdkNotice(note, !m.loaded);
}

void StartReload(int i)
{
    Mod& m = g_mods[i];
    HotState& hot = g_hot[i];
    if (!m.loaded) {  // the last build did not load: just try this one
        char note[128];
        if (LoadModDll(i, false))
            sprintf_s(note, "%s loaded (build %d)", m.name, hot.build);
        else
            sprintf_s(note, "%s: new build NOT loaded, see ds3_mods.log", m.name);
        SdkNotice(note, !m.loaded);
        return;
    }
    bool unsafe = false;
    SdkModIdle(i, &unsafe);
    if (unsafe) {
        Log("mod %s: a new build is there, but the proxy lost track of one of its hooks or patches; restart the game "
            "to load it",
            m.id);
        SdkNotice("New mod build: restart the game to load it (see ds3_mods.log)", true);
        hot.stuck = true;
        return;
    }
    Log("mod %s: new build seen; unloading build %d", m.id, hot.build);
    m.settingsChanged = nullptr;
    SdkModStop(i);
    hot.phase = HotPhase::Draining;
    hot.since = GetTickCount64();
    hot.idleAt = 0;
}

void StepDrain(int i, ULONGLONG now)
{
    HotState& hot = g_hot[i];
    bool unsafe = false;
    if (SdkModIdle(i, &unsafe) && !unsafe) {
        if (!hot.idleAt) {
            hot.idleAt = now;
        } else if (now - hot.idleAt >= kIdleMs) {
            FinishReload(i);
        }
        return;
    }
    hot.idleAt = 0;
    if (unsafe || now - hot.since >= kGiveUpMs) {
        Log("mod %s: one of its hooks is still running after %llu ms (or went uncounted); build %d stays loaded "
            "with its hooks off. Restart the game to load the new build",
            g_mods[i].id, now - hot.since, hot.build);
        SdkNotice("Mod reload stopped: a hook is still busy. Restart the game.", true);
        hot.phase = HotPhase::Idle;
        hot.stuck = true;
    }
}

// The settings ini changed outside the panel (an editor, a script): read every setting back and tell the mod.
void ReloadIni(int i)
{
    Mod& m = g_mods[i];
    int changed = 0;
    for (int k = 0; k < g_settingCount; ++k) {
        ModSetting& st = g_settings[k];
        if (st.mod != i) continue;
        const double v = ReadValue(st, m.ini);
        if (v == st.value) continue;
        st.value = v;
        ++changed;
        if (st.patchBound) ReapplyPatchesFor(k);
    }
    if (m.settingsChanged && !CallChanged(m.settingsChanged)) {
        Log("mod %s: DS3ModSettingsChanged crashed (caught); not called again", m.id);
        m.settingsChanged = nullptr;
    }
    Log("mod %s: its ini changed on disk; %d panel setting(s) read again%s", m.id, changed,
        m.settingsChanged ? ", the mod was told" : "");
}

// A notice from the moment the mods load, kept up for the first while of drawn frames. Timed from when it is set it
// would be gone before the first menu: the game has not drawn anything yet then (seen in game: the safe-start notice
// was never visible).
char g_startNotice[128] = {};
bool g_startNoticeBad = false;
ULONGLONG g_startNoticeMs = 0;

void SetStartNotice(const char* text, bool bad, ULONGLONG ms)
{
    strncpy_s(g_startNotice, text, _TRUNCATE);
    g_startNoticeBad = bad;
    g_startNoticeMs = ms;
    SdkWantFrames();  // it is shown from the frame tick, which is only installed when something asks for it
}

void StartNoticeTick(ULONGLONG now)
{
    if (!g_startNotice[0]) return;
    static ULONGLONG first = 0, last = 0;
    if (!first) first = now;
    if (now - first >= g_startNoticeMs) {
        g_startNotice[0] = '\0';
        return;
    }
    if (now - last < 1000) return;  // a notice stays a few seconds: showing it again each second keeps it up
    last = now;
    SdkNotice(g_startNotice, g_startNoticeBad);
}

void HotReloadTick()
{
    StartGuardFrame();
    const ULONGLONG now = GetTickCount64();
    StartNoticeTick(now);
    for (int i = 0; i < g_modCount; ++i) {
        if (g_hot[i].phase == HotPhase::Draining) StepDrain(i, now);
    }
    static ULONGLONG lastPoll = 0;
    if (now - lastPoll < kPollMs) return;
    lastPoll = now;
    for (int i = 0; i < g_modCount; ++i) {
        Mod& m = g_mods[i];
        HotState& hot = g_hot[i];
        if (hot.phase != HotPhase::Idle) continue;

        // The settings ini (any loaded mod, reloadable or not).
        if (m.loaded && m.ini[0]) {
            FILETIME t{};
            const bool has = FileStamp(m.ini, &t, nullptr);
            if (has && (!hot.iniKnown || !SameTime(t, hot.iniTime))) {
                hot.iniTime = t;
                hot.iniKnown = true;
                ReloadIni(i);
            }
        }

        // A new build of a reloadable mod (or of one whose last new build failed to load).
        if (hot.stuck || !hot.build || !m.startup || !(hot.reloadable || !m.loaded)) continue;
        FILETIME t{};
        ULONGLONG size = 0;
        if (!FileStamp(m.file, &t, &size)) continue;
        if (SameTime(t, hot.fileTime) && size == hot.fileSize) continue;
        if (!SameTime(t, hot.seenTime) || size != hot.seenSize) {
            hot.seenTime = t;
            hot.seenSize = size;
            hot.seenAt = now;
            continue;
        }
        if (now - hot.seenAt < kSettleMs || !FileSettled(m.file)) continue;
        hot.fileTime = t;  // this one is handled, whatever happens next
        hot.fileSize = size;
        StartReload(i);
    }
}

// ---- Basic mods: loading ----

unsigned long long MsSinceGameStart()
{
    FILETIME created{}, exited{}, kernel{}, user{}, now{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    GetSystemTimeAsFileTime(&now);
    const unsigned long long a = (static_cast<unsigned long long>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    const unsigned long long b = (static_cast<unsigned long long>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    return b > a ? (b - a) / 10000 : 0;
}

// Kept apart so __try is allowed. An exception caught here is diagnosis, not containment: after one the game may
// not be stable, and what protects the player is the safe start.
HMODULE LoadRaw(const wchar_t* file, DWORD* error)
{
    __try {
        HMODULE dll = LoadLibraryExW(file, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        *error = dll ? 0 : GetLastError();
        return dll;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *error = GetExceptionCode();
        return nullptr;
    }
}

struct Missing {
    const wchar_t* dir;
    char text[200];
};

// An import of a file that did not load: noted if it is neither loaded, beside the mod, nor found by Windows.
bool NoteMissing(const char* name, void* user)
{
    auto* miss = static_cast<Missing*>(user);
    if (_strnicmp(name, "api-ms-", 7) == 0 || _strnicmp(name, "ext-ms-", 7) == 0) return true;
    wchar_t wide[128], path[MAX_PATH], found[MAX_PATH];
    size_t k = 0;
    for (; name[k] && k + 1 < 128; ++k) wide[k] = static_cast<unsigned char>(name[k]);
    wide[k] = L'\0';
    if (GetModuleHandleW(wide)) return true;
    if (JoinPath(path, miss->dir, wide) && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return true;
    if (SearchPathW(nullptr, wide, nullptr, MAX_PATH, found, nullptr)) return true;
    if (strlen(miss->text) + strlen(name) + 3 < sizeof(miss->text)) {
        if (miss->text[0]) strcat_s(miss->text, ", ");
        strcat_s(miss->text, name);
    }
    return true;
}

void LoadBasicMod(int i)
{
    Mod& m = g_mods[i];
    if (GetModuleHandleW(m.file)) {
        strcpy_s(m.notLoaded, "already loaded");
        Log("basic mod %s: plugins\\%ls is already in the game (something else loaded it); left as it is and not "
            "started again", m.id, m.switchKey);
        return;
    }
    DWORD error = 0;
    HMODULE dll = LoadRaw(m.file, &error);
    if (!dll) {
        if (error == ERROR_BAD_EXE_FORMAT) {
            Log("basic mod %s: FAILED to load plugins\\%ls: not a valid 32-bit DLL (error 193)", m.id, m.switchKey);
        } else if (error == ERROR_MOD_NOT_FOUND) {
            Missing miss{m.dir, {}};
            PeFile pe(m.file);
            pe.Imports(&NoteMissing, &miss);
            Log("basic mod %s: FAILED to load plugins\\%ls: this file, or a DLL it needs, was not found (error 126)%s%s",
                m.id, m.switchKey, miss.text[0] ? ". Not beside it or in Windows: " : "", miss.text);
        } else if (error == ERROR_DLL_INIT_FAILED) {
            Log("basic mod %s: FAILED to load plugins\\%ls: its own start-up code failed or crashed (error 1114)", m.id,
                m.switchKey);
        } else if (error >= 0xC0000000u) {
            Log("basic mod %s: plugins\\%ls crashed while starting (exception 0x%08lX, caught). The game may not be "
                "stable until it is restarted", m.id, m.switchKey, error);
        } else {
            Log("basic mod %s: FAILED to load plugins\\%ls (error %lu)", m.id, m.switchKey, error);
        }
        return;
    }
    m.loaded = true;
    g_hot[i].dll = dll;
    Log("basic mod %s: loaded plugins\\%ls at %p (%llu bytes, SHA-256 %s), %llu ms after the game started. A plain "
        "ASI: the loader's hook and patch checks don't cover it", m.id, m.switchKey, dll, g_fileSize[i], g_sha[i],
        MsSinceGameStart());
    // Ultimate ASI Loader's convention: an exported InitializeASI is called once after the load.
    if (auto init = reinterpret_cast<void(__cdecl*)()>(GetProcAddress(dll, "InitializeASI"))) {
        if (CallChanged(init))
            Log("basic mod %s: InitializeASI called", m.id);
        else
            Log("basic mod %s: InitializeASI crashed (caught); the mod may not work", m.id);
    }
}

// Other DLLs in the game that load mods or stand in for a system DLL. An ASI loader is known by its own marker
// export, whatever its file is called.
void FindOtherLoaders()
{
    static const wchar_t* const kProxyNames[] = {
        L"d3d8.dll",   L"d3d9.dll",    L"d3d10.dll",  L"d3d11.dll",     L"d3d12.dll",     L"dxgi.dll",
        L"ddraw.dll",  L"dinput.dll",  L"dsound.dll", L"version.dll",   L"winmm.dll",     L"wininet.dll",
        L"winhttp.dll", L"msacm32.dll", L"msvfw32.dll", L"xinput1_1.dll", L"xinput1_2.dll", L"xinput1_3.dll",
        L"xinput1_4.dll", L"xinput9_1_0.dll", L"binkw32.dll", L"bink2w32.dll", L"vorbisfile.dll", L"xlive.dll",
        nullptr};
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        Log("mods: the game's loaded DLLs could not be listed (error %lu), so another ASI loader would go unnoticed",
            GetLastError());
        return;
    }
    int looked = 0;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    const size_t dirLen = wcslen(g_gameDir);
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        ++looked;
        if (GetProcAddress(me.hModule, "IsUltimateASILoader")) {
            ToAscii(me.szModule, g_otherLoader, sizeof(g_otherLoader));
            Log("mods: Ultimate ASI Loader is also in the game (%ls). It loads .asi files by itself, switched on in "
                "the Mods panel or not, so basic mods are left to it and hot reload is off. Using both loaders "
                "together is not supported: remove one", me.szExePath);
            continue;
        }
        const bool inGameDir = _wcsnicmp(me.szExePath, g_gameDir, dirLen) == 0 && me.szExePath[dirLen] == L'\\' &&
                               !wcschr(me.szExePath + dirLen + 1, L'\\');
        if (inGameDir && _wcsicmp(me.szModule, L"dinput8.dll") != 0 && IsAny(me.szModule, kProxyNames))
            Log("mods: another DLL with a system DLL's name is loaded from the game folder: %ls (not an ASI loader, "
                "as far as this loader can tell)", me.szModule);
    }
    CloseHandle(snap);
    if (!g_otherLoader[0]) Log("mods: no other ASI loader among the %d DLLs loaded in the game", looked);
}

int CountAsi(const wchar_t* dir)
{
    wchar_t pattern[MAX_PATH];
    if (!JoinPath(pattern, dir, L"*.asi")) return 0;
    int n = 0;
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && EndsWith(fd.cFileName, L".asi")) ++n;
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return n;
}

// .asi files where another ASI loader would look, but this one does not.
void NoteOtherPlaces()
{
    if (g_otherLoader[0]) return;
    wchar_t scripts[MAX_PATH];
    const int inScripts = JoinPath(scripts, g_gameDir, L"scripts") ? CountAsi(scripts) : 0;
    const int inRoot = CountAsi(g_gameDir);
    if (!inScripts && !inRoot) return;
    Log("mods: %d .asi file(s) in scripts\\ and %d in the game folder itself are not loaded: this loader only looks in "
        "plugins\\. Move them there to use them", inScripts, inRoot);
    SetStartNotice(".asi files in scripts\\ or the game folder are not loaded: move them into plugins\\", true, 30000);
}

// Whether mod i would be loaded on a normal start.
bool Wanted(int i)
{
    const Mod& m = g_mods[i];
    return m.enabled && !m.needsLoader && !m.problem[0] && !(m.basic && g_otherLoader[0]);
}

// The mod's line in the start journal: its key and its file's SHA-256 (empty for a patch-only mod).
void GuardEntry(int i, wchar_t* key, size_t keySize, char sha[65])
{
    const Mod& m = g_mods[i];
    sha[0] = '\0';
    if (m.basic) {
        swprintf_s(key, keySize, L"basic:%s", m.switchKey);
        strcpy_s(sha, 65, g_sha[i]);
    } else {
        swprintf_s(key, keySize, L"mod:%s", m.idW);
        if (m.file[0]) {
            PeFile pe(m.file);
            pe.Sha256(sha);
        }
    }
}

}  // namespace

void LoadMods()
{
    g_hotOn = GetPrivateProfileIntW(L"mods", L"hot_reload", 1, g_sealsIni) != 0;
    g_hotFree = GetPrivateProfileIntW(L"mods", L"hot_reload_free", 0, g_sealsIni) != 0;
    SdkSetFrameTick(&HotReloadTick);  // it also counts frames for the safe start

    FindOtherLoaders();
    const bool hotWasOn = g_hotOn;
    if (g_otherLoader[0]) g_hotOn = false;  // no mod runs from a temp copy while another loader may load the original

    wchar_t key[160];
    char sha[65];
    const StartKind start = StartGuardBegin(g_gameDir, g_sealsIni);
    if (start == StartKind::Safe) {
        // No mod, no patch. The panel shows which switched-on mods are new or changed since the last good start.
        g_safeStart = true;
        for (int i = 0; i < g_modCount; ++i) {
            Mod& m = g_mods[i];
            if (!Wanted(i)) continue;
            GuardEntry(i, key, _countof(key), sha);
            m.changed = !StartGuardKnown(key, sha);
            Log("safe start: \"%s\" is on and was not loaded%s", m.name,
                m.changed ? "; it is new or changed since the last start that finished" : "");
        }
        if (StartGuardStuck())
            SetStartNotice("Safe start, and the next start will be one too: a settings file is read-only (see ds3_mods.log).",
                           true, 30000);
        else
            SetStartNotice(StartGuardAsked() ? "Safe start: no mods were loaded this time."
                                             : "The last start did not finish: no mods were loaded this time. See the Mods panel.",
                           !StartGuardAsked(), 30000);
        return;
    }

    // The journal: what this start is about to load. Nothing to load, nothing to guard.
    int wanted = 0;
    bool journal = false;
    if (start == StartKind::Normal) {
        for (int i = 0; i < g_modCount; ++i) {
            if (!Wanted(i)) continue;
            GuardEntry(i, key, _countof(key), sha);
            StartGuardAdd(key, sha);
            ++wanted;
        }
        if (wanted > 0)
            journal = StartGuardCommit();
        else
            StartGuardNoMods();
    }

    // Basic mods first: a mod that goes through the SDK then sees what they changed and is refused cleanly.
    int basicLoaded = 0;
    char names[200] = {};
    for (int i = 0; i < g_modCount; ++i) {
        Mod& m = g_mods[i];
        if (!m.basic || m.problem[0]) continue;  // a problem was logged when the file was found
        if (!m.enabled) {
            Log("basic mod %s: off", m.id);
            continue;
        }
        if (g_otherLoader[0]) {
            strcpy_s(m.notLoaded, "left to the other loader");
            Log("basic mod %s: on, but left to %s (plugins\\%ls is %s)", m.id, g_otherLoader, m.switchKey,
                GetModuleHandleW(m.file) ? "in the game, loaded by it" : "not in the game now");
            continue;
        }
        if (start == StartKind::OtherGame) {
            strcpy_s(m.notLoaded, "another game is starting");
            Log("basic mod %s: on, but not loaded: another Dead Space 3 is still starting", m.id);
            continue;
        }
        if (!journal) {
            strcpy_s(m.notLoaded, "start journal not saved");
            Log("basic mod %s: on, but not loaded: without ds3_mods_start.ini a start that does not finish could not "
                "be noticed. Make the game folder writable", m.id);
            continue;
        }
        LoadBasicMod(i);
        if (!m.loaded) continue;
        ++basicLoaded;
        if (strlen(names) + strlen(m.name) + 3 < sizeof(names)) {
            if (names[0]) strcat_s(names, ", ");
            strcat_s(names, m.name);
        }
    }
    if (basicLoaded) Log("mods: %d basic mod(s) loaded: %s", basicLoaded, names);

    bool anyReloadable = false;
    for (int i = 0; i < g_modCount; ++i) {
        Mod& m = g_mods[i];
        if (m.basic) continue;
        if (m.needsLoader) continue;  // said so when it was found
        if (m.problem[0]) {  // a patch in a protected area: refused when it was found, never loaded
            if (m.enabled) Log("mod %s: on, but not loaded: %s", m.id, m.problem);
            continue;
        }
        if (!m.enabled) {
            Log("mod %s: off", m.id);
            continue;
        }
        if (m.file[0] && GetFileAttributesW(m.file) == INVALID_FILE_ATTRIBUTES) {
            Log("mod %s: on, but %ls is missing", m.id, m.file);
            continue;
        }
        // Patches first, all or nothing; a mod whose patches do not fit is not loaded at all (its DLL may rely on
        // them).
        if (ModPatchCount(i) && !ApplyModPatches(i)) continue;
        if (!m.file[0]) {
            m.loaded = true;
            Log("mod %s: %d patch(es) applied (%s%s%s)", m.id, ModPatchCount(i), m.name, m.version[0] ? " " : "",
                m.version);
            continue;
        }
        LoadModDll(i, true);
        ReadIniStamp(i);
        anyReloadable = anyReloadable || g_hot[i].reloadable;
    }
    if (journal) SdkWantFrames();  // the safe start counts drawn frames, with or without an SDK mod
    NoteOtherPlaces();
    if (g_otherLoader[0] && hotWasOn) Log("mods: hot reload off while another ASI loader is in the game");
    else if (!g_hotOn) Log("mods: hot reload off ([mods] hot_reload=0)");
    else if (anyReloadable) Log("mods: hot reload on: a new build of a mod marked so above is loaded while the game runs");
}

bool ModIdTaken(const char* id)
{
    for (int i = 0; i < g_modCount; ++i) {
        if (strcmp(g_mods[i].id, id) == 0) return true;
    }
    return false;
}

bool ModsSafeStart() { return g_safeStart; }
bool ModsSafeStartAsked() { return g_safeStart && StartGuardAsked(); }
const char* ModsOtherLoader() { return g_otherLoader; }

int ModCount() { return g_modCount; }
Mod& ModAt(int i) { return g_mods[i]; }
int ModSettingCount() { return g_settingCount; }
ModSetting& ModSettingAt(int i) { return g_settings[i]; }

bool ModAppliesLive(const Mod& m)
{
    return m.loaded && m.settingsChanged;
}

bool ModSettingAppliesLive(const ModSetting& s)
{
    const Mod& m = g_mods[s.mod];
    return m.loaded && (s.patchBound ? s.patchLive : m.settingsChanged != nullptr);
}

bool WriteModSwitch(Mod& m)
{
    return WritePrivateProfileStringW(m.basic ? L"basic_mods" : L"modules", m.basic ? m.switchKey : m.idW,
                                      m.enabled ? L"1" : L"0", g_sealsIni) != FALSE;
}

bool WriteModSetting(ModSetting& s)
{
    Mod& m = g_mods[s.mod];
    wchar_t text[64];
    ValueText(s, s.value, text, 64);
    bool saved = WritePrivateProfileStringW(s.section, s.key, text, m.ini) != FALSE;
    if (saved) ReadIniStamp(s.mod);  // the panel's own write: not an outside edit for the hot-reload watch
    if (m.loaded && s.patchBound) ReapplyPatchesFor(static_cast<int>(&s - g_settings));
    if (saved && ModAppliesLive(m) && !CallChanged(m.settingsChanged)) {
        Log("mod %s: DS3ModSettingsChanged crashed (caught); not called again", m.id);
        m.settingsChanged = nullptr;
    }
    return saved;
}

void KeyName(unsigned vk, char* out, size_t size)
{
    struct {
        unsigned vk;
        const char* name;
    } constexpr kNames[] = {
        {0, "None"},          {VK_BACK, "Backspace"}, {VK_TAB, "Tab"},       {VK_RETURN, "Enter"},
        {VK_SHIFT, "Shift"},  {VK_CONTROL, "Ctrl"},   {VK_MENU, "Alt"},      {VK_PAUSE, "Pause"},
        {VK_CAPITAL, "Caps Lock"}, {VK_ESCAPE, "Esc"}, {VK_SPACE, "Space"},  {VK_PRIOR, "Page Up"},
        {VK_NEXT, "Page Down"}, {VK_END, "End"},      {VK_HOME, "Home"},     {VK_LEFT, "Left"},
        {VK_UP, "Up"},        {VK_RIGHT, "Right"},    {VK_DOWN, "Down"},     {VK_INSERT, "Insert"},
        {VK_DELETE, "Delete"}, {VK_MULTIPLY, "Num *"}, {VK_ADD, "Num +"},    {VK_SUBTRACT, "Num -"},
        {VK_DECIMAL, "Num ."}, {VK_DIVIDE, "Num /"},  {VK_NUMLOCK, "Num Lock"}, {VK_SCROLL, "Scroll Lock"},
        {VK_XBUTTON1, "Mouse 4"}, {VK_XBUTTON2, "Mouse 5"}, {VK_MBUTTON, "Middle mouse"},
    };
    for (const auto& k : kNames) {
        if (k.vk == vk) {
            strcpy_s(out, size, k.name);
            return;
        }
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        sprintf_s(out, size, "F%u", vk - VK_F1 + 1);
    } else if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        sprintf_s(out, size, "%c", static_cast<char>(vk));
    } else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        sprintf_s(out, size, "Num %u", vk - VK_NUMPAD0);
    } else {
        UINT ch = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7FFF;
        if (ch > 0x20 && ch < 0x7F)
            sprintf_s(out, size, "%c", static_cast<char>(ch));
        else
            sprintf_s(out, size, "Key 0x%02X", vk);
    }
}

void FormatModSetting(const ModSetting& s, char* out, size_t size)
{
    switch (s.kind) {
    case ModSettingKind::Bool:
        strcpy_s(out, size, s.value != 0 ? "On" : "Off");
        return;
    case ModSettingKind::Int:
        sprintf_s(out, size, "%d%s", static_cast<int>(s.value), s.unit);
        return;
    case ModSettingKind::Float:
        sprintf_s(out, size, "%.*f%s", s.decimals, s.value, s.unit);
        return;
    case ModSettingKind::Choice:
        strcpy_s(out, size, s.choices[std::clamp(static_cast<int>(s.value), 0, s.choiceCount - 1)].label);
        return;
    case ModSettingKind::Key:
        KeyName(static_cast<unsigned>(s.value), out, size);
        return;
    }
}
