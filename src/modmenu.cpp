// The Mods panel. See modmenu.h.
//
// The pause screen ("SP Pause") is found in the game's list of open UI screens: 15 slots at
// 0x012DEEFC, sorted by priority (FUN_00b4fbb0 inserts a screen there). An entry whose vtable is 0x00D494B8 is the
// pause screen. The panel opens only while it is in that list and closes when it leaves.

#include "modmenu.h"

#include "config.h"
#include "game.h"
#include "input.h"
#include "log.h"
#include "mods.h"
#include "overlay.h"
#include "version.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace {

constexpr uintptr_t kScreenListVa = 0x012DEEFC;
constexpr int kScreenSlots = 15;
constexpr uintptr_t kPauseVtable = 0x00D494B8;
// Two of its slots, checked once so another build's layout is not trusted: 31 activate row, 46 row label.
constexpr uintptr_t kPauseActivate = 0x0053CCC0;
constexpr uintptr_t kPauseRowLabel = 0x0050FDD0;

// XInput buttons.
constexpr unsigned kPadUp = 0x1, kPadDown = 0x2, kPadLeft = 0x4, kPadRight = 0x8;
constexpr unsigned kPadLb = 0x100, kPadRb = 0x200, kPadA = 0x1000, kPadB = 0x2000;

constexpr ULONGLONG kRepeatDelayMs = 380;
constexpr ULONGLONG kRepeatMs = 75;
constexpr ULONGLONG kStatusMs = 2500;

// Colours (0xAARRGGBB).
constexpr unsigned kShade = 0x99000000;
constexpr unsigned kPanelBg = 0xF0111519;
constexpr unsigned kAccent = 0xFF7FD8FF;
constexpr unsigned kText = 0xFFF2F2F2;
constexpr unsigned kDim = 0xFF9AA4AE;
constexpr unsigned kHeading = 0xFF8FE0FF;
constexpr unsigned kWarn = 0xFFFFC46B;
constexpr unsigned kSelectBg = 0x5530708C;
constexpr unsigned kOnBg = 0xFF2E7FA0;
constexpr unsigned kOffBg = 0xFF3A4048;

// ---- What the panel lists ----

// Built-in settings come from kSettings (config.cpp); drop-in mods and their settings from their manifests (mods.h).

struct Help {
    const wchar_t* key;
    const char* text;
};
// Keyed by "section/key".
constexpr Help kHelp[] = {
    {L"mods_menu/text_size", "Size of the text in this panel."},
};

// Keys Left / Right step through on a key setting (Enter waits for a key press instead).
constexpr unsigned kKeyCycle[] = {0,     VK_F1,     VK_F2,    VK_F3,  VK_F4,   VK_F5,   VK_F6,    VK_F7,
                                  VK_F8, VK_F9,     VK_F10,   VK_F11, VK_F12,  VK_INSERT, VK_DELETE, VK_HOME,
                                  VK_END, VK_PRIOR, VK_NEXT};

enum class RowType { Heading, Setting, ModSwitch, ModSetting, Close };
struct Row {
    RowType type;
    int index;  // into kSettings, ModAt or ModSettingAt
    const char* heading;
};
constexpr int kMaxRows = 256;  // built-in rows + kMaxMods switches + kMaxModSettings settings + their headings
Row g_rows[kMaxRows];
int g_rowCount = 0;

// ---- State ----

Config g_config;   // what the ini holds now
Config g_startup;  // what the game started with, for "applies after restart"
bool g_pauseOk = false;
int g_capture = -1;            // the key setting row waiting for a key press, or -1
bool g_prevKeys[256] = {};     // keys held the frame before, for the key press it waits for
char g_helpText[256] = {};

bool g_open = false;
bool g_releasing = false;  // closed, but the game stays blocked until the keys that closed it are let go
ULONGLONG g_releaseSince = 0;           // when that wait began (0: not waiting)
constexpr ULONGLONG kReleaseWaitMs = 3000;  // a key that reads as held for longer is given up on
bool g_pauseSeen = false;
int g_selected = 0;
int g_scroll = 0;
char g_status[96] = {};
unsigned g_statusColor = kDim;
ULONGLONG g_statusAt = 0;

struct Buttons {
    bool up, down, left, right, accept, back, toggle, pageUp, pageDown, click;
};
Buttons g_prev = {};
ULONGLONG g_heldSince[4] = {};  // up, down, left, right
ULONGLONG g_lastRepeat[4] = {};

float g_cursorX = 0, g_cursorY = 0;
bool g_cursorShown = false;
POINT g_lastOsCursor = {};

// Layout from the last draw, for the mouse.
struct Box {
    float x, y, w, h;
    bool Contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};
Box g_rowBox[kMaxRows] = {};
bool g_rowVisible[kMaxRows] = {};
Box g_valueBox[kMaxRows] = {};

// The "mods" button under the pause menu's rows (see Pause button below).
bool g_buttonOn = false;     // [mods_menu] pause_button, read when the pause menu comes up
float g_buttonNudgeX = 0, g_buttonNudgeY = 0;
ULONGLONG g_pauseAt = 0;     // when the pause menu came up
bool g_buttonFocus = false;  // selected with the keyboard / pad: the game gets no input meanwhile
bool g_buttonHover = false;
Box g_buttonBox = {};
bool g_wasAtLast = false, g_wasAtFirst = false;  // the game's last / first row was selected in the previous frame

// ---- Pause screen ----

uintptr_t PauseScreen()
{
    if (!g_pauseOk) return 0;
    uint32_t slots[kScreenSlots] = {};
    if (!game::SafeRead(kScreenListVa, slots, sizeof(slots))) return 0;
    for (uint32_t s : slots) {
        uint32_t vt = 0;
        if (s && game::SafeReadU32(s, &vt) && vt == kPauseVtable) return s;
    }
    return 0;
}

bool GameInForeground(void* window)
{
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    if (fg == static_cast<HWND>(window)) return true;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// ---- Values ----

const char* HelpFor(const Setting& s)
{
    wchar_t key[64];
    swprintf_s(key, L"%s/%s", s.section, s.key);
    for (const Help& h : kHelp) {
        if (wcscmp(h.key, key) == 0) return h.text;
    }
    return "";
}

void SetStatus(const char* text, unsigned color)
{
    strcpy_s(g_status, text);
    g_statusColor = color;
    g_statusAt = GetTickCount64();
}

long long SettingValue(const Config& c, const Setting& s)
{
    switch (s.kind) {
    case SettingKind::Flag: return c.*s.flag ? 1 : 0;
    case SettingKind::Int: return c.*s.number;
    default: return c.*s.count;
    }
}

void FormatSetting(const Setting& s, char* out, size_t size)
{
    switch (s.kind) {
    case SettingKind::Flag:
        strcpy_s(out, size, g_config.*s.flag ? "On" : "Off");
        return;
    case SettingKind::Int:
        sprintf_s(out, size, "%d%s", g_config.*s.number, s.unit ? s.unit : "");
        return;
    case SettingKind::Count:
        if (g_config.*s.count == 0 && s.zeroText)
            strcpy_s(out, size, s.zeroText);
        else
            sprintf_s(out, size, "%u%s", g_config.*s.count, s.unit ? s.unit : "");
        return;
    case SettingKind::Bits:
        sprintf_s(out, size, "0x%X", g_config.*s.count);
        return;
    }
}

// dir: -1 / +1 (Left / Right), 0 = Enter / click (toggle, or step up and wrap around).
void ChangeSetting(const Setting& s, int dir)
{
    switch (s.kind) {
    case SettingKind::Flag:
        g_config.*s.flag = !(g_config.*s.flag);
        break;
    case SettingKind::Int:
    case SettingKind::Count: {
        long long v = SettingValue(g_config, s);
        long long next = v + static_cast<long long>(dir ? dir : 1) * s.step;
        if (dir == 0 && next > s.maxValue) next = s.minValue;
        next = std::clamp<long long>(next, s.minValue, s.maxValue);
        if (next == v) return;
        if (s.kind == SettingKind::Int)
            g_config.*s.number = static_cast<int>(next);
        else
            g_config.*s.count = static_cast<unsigned>(next);
        break;
    }
    case SettingKind::Bits:
        return;  // ini only (the panel's key and pad button): never a row
    }

    char value[48];
    FormatSetting(s, value, sizeof(value));
    bool saved = WriteSetting(g_config, s);
    Log("mods panel: [%ls] %ls -> %s (%s%s)", s.section, s.key, value, saved ? "saved" : "NOT saved",
        s.apply == SettingApply::Live ? ", applied now" : ", applies after restart");
    if (!saved)
        SetStatus("Could not write ds3_mods.ini", kWarn);
    else
        SetStatus(s.apply == SettingApply::Live ? "Saved. Applied now." : "Saved. Applies after you restart the game.",
                  s.apply == SettingApply::Live ? kDim : kWarn);
}

void ChangeModSwitch(Mod& m)
{
    m.enabled = !m.enabled;
    bool saved = WriteModSwitch(m);
    Log("mods panel: [modules] %s -> %d (%s, applies after restart)", m.id, m.enabled ? 1 : 0,
        saved ? "saved" : "NOT saved");
    if (saved && m.needsLoader) {
        char text[96];
        sprintf_s(text, "Saved, but this mod needs DS3 Mod Loader %s or later.", m.minLoader);
        SetStatus(text, kWarn);
        return;
    }
    SetStatus(saved ? "Saved. Applies after you restart the game." : "Could not write ds3_mods.ini", kWarn);
}

void SaveModSetting(ModSetting& s)
{
    const Mod& m = ModAt(s.mod);
    char value[48];
    FormatModSetting(s, value, sizeof(value));
    bool saved = WriteModSetting(s);
    const bool live = ModSettingAppliesLive(s);
    Log("mods panel: mod %s [%ls] %ls -> %s (%s%s)", m.id, s.section, s.key, value, saved ? "saved" : "NOT saved",
        live ? ", applied now" : "");
    if (!saved) {
        char text[96];
        const wchar_t* file = wcsrchr(m.ini, L'\\');
        _snprintf_s(text, _TRUNCATE, "Could not write %ls", file ? file + 1 : m.ini);
        SetStatus(text, kWarn);
    } else if (live) {
        SetStatus("Saved. Applied now.", kDim);
    } else if (m.loaded && m.reloadHint[0]) {
        char text[96];
        sprintf_s(text, "Saved. Press %s in game (or restart) to apply.", m.reloadHint);
        SetStatus(text, kWarn);
    } else {
        SetStatus("Saved. Applies after you restart the game.", kWarn);
    }
}

// dir: -1 / +1 (Left / Right), 0 = Enter / click (toggle, step up and wrap around, or wait for a key).
void ChangeModSetting(int row, ModSetting& s, int dir)
{
    double v = s.value;
    switch (s.kind) {
    case ModSettingKind::Bool:
        v = v != 0 ? 0 : 1;
        break;
    case ModSettingKind::Int:
    case ModSettingKind::Float: {
        const double eps = s.step * 0.001;
        double next = v + (dir ? dir : 1) * s.step;
        if (dir == 0 && next > s.maxValue + eps) next = s.minValue;
        next = std::clamp(std::round(next / s.step) * s.step, s.minValue, s.maxValue);
        if (std::fabs(next - v) < eps) return;
        v = next;
        break;
    }
    case ModSettingKind::Choice: {
        const int n = s.choiceCount, at = static_cast<int>(v);
        v = dir < 0 ? (at + n - 1) % n : (at + 1) % n;
        break;
    }
    case ModSettingKind::Key: {
        if (dir == 0) {
            g_capture = row;
            return;
        }
        constexpr int n = static_cast<int>(sizeof(kKeyCycle) / sizeof(kKeyCycle[0]));
        int at = -1;
        for (int i = 0; i < n; ++i) {
            if (kKeyCycle[i] == static_cast<unsigned>(v)) at = i;
        }
        at = at < 0 ? 0 : (at + (dir < 0 ? -1 : 1) + n) % n;
        v = kKeyCycle[at];
        break;
    }
    }
    s.value = v;
    SaveModSetting(s);
}

// While a key setting waits for a key: the first new key press (not a mouse button) is the key. Esc / B cancels,
// Backspace sets none, a click cancels.
void HandleCapture(const InputFrame& in)
{
    ModSetting& s = ModSettingAt(g_rows[g_capture].index);
    const bool* k = in.keys;
    auto pressed = [&](int vk) { return k[vk] && !g_prevKeys[vk]; };
    if (pressed(VK_ESCAPE) || ((in.pad & kPadB) && !(g_prev.back))) {
        g_capture = -1;
        return;
    }
    if (in.mouseLeft && !g_prev.click) {
        g_capture = -1;
        return;
    }
    int vk = 0;
    if (pressed(VK_BACK)) {
        vk = -1;
    } else {
        for (int i = 7; i < 255 && !vk; ++i) {
            if (i >= VK_LSHIFT && i <= VK_RMENU) continue;  // the plain Shift / Ctrl / Alt codes come too
            if (pressed(i)) vk = i;
        }
    }
    if (!vk) return;
    g_capture = -1;
    s.value = vk < 0 ? 0 : vk;
    SaveModSetting(s);
}

bool AddRow(RowType type, int index, const char* heading)
{
    if (g_rowCount >= kMaxRows - 1) return false;  // the last one is kept for Close
    g_rows[g_rowCount++] = {type, index, heading};
    return true;
}

// The first group: the loader's own switches, then one switch per drop-in mod.
constexpr const char* kSwitchGroup = "Mod parts";

bool IsSwitch(const Setting& s)
{
    return std::strcmp(s.group, kSwitchGroup) == 0;
}

void BuildRows()
{
    g_rowCount = 0;
    bool heading = false;
    for (int i = 0; i < kSettingCount; ++i) {
        const Setting& s = kSettings[i];
        if (s.apply == SettingApply::Hidden || !IsSwitch(s)) continue;
        if (!heading) heading = AddRow(RowType::Heading, -1, kSwitchGroup);
        AddRow(RowType::Setting, i, nullptr);
    }
    if (ModCount() && !heading) AddRow(RowType::Heading, -1, kSwitchGroup);
    for (int i = 0; i < ModCount(); ++i) AddRow(RowType::ModSwitch, i, nullptr);
    // The loader's other settings, under their groups.
    const char* group = nullptr;
    for (int i = 0; i < kSettingCount; ++i) {
        const Setting& s = kSettings[i];
        if (s.apply == SettingApply::Hidden || IsSwitch(s)) continue;
        if (!group || std::strcmp(group, s.group) != 0) {
            group = s.group;
            AddRow(RowType::Heading, -1, group);
        }
        AddRow(RowType::Setting, i, nullptr);
    }
    // Each mod's settings, under their groups (the mod's name unless the manifest names one).
    group = nullptr;
    for (int i = 0; i < ModSettingCount(); ++i) {
        const ModSetting& s = ModSettingAt(i);
        if (!group || std::strcmp(group, s.group) != 0) {
            group = s.group;
            if (g_rowCount >= kMaxRows - 2) break;
            AddRow(RowType::Heading, -1, group);
        }
        if (!AddRow(RowType::ModSetting, i, nullptr)) break;
    }
    if (g_rowCount >= kMaxRows - 1) Log("mods panel: more than %d rows; the last ones are not shown", kMaxRows - 1);
    g_rows[g_rowCount++] = {RowType::Close, -1, nullptr};
}

// Footer text of a drop-in mod's switch: its description, author and version.
const char* ModHelp(const Mod& m)
{
    g_helpText[0] = '\0';
    if (m.description[0]) strcpy_s(g_helpText, m.description);
    char by[96] = {};
    if (m.author[0] && m.version[0])
        sprintf_s(by, "By %s, version %s.", m.author, m.version);
    else if (m.author[0])
        sprintf_s(by, "By %s.", m.author);
    else if (m.version[0])
        sprintf_s(by, "Version %s.", m.version);
    if (by[0]) {
        if (g_helpText[0]) strcat_s(g_helpText, " ");
        strcat_s(g_helpText, by);
    }
    if (!g_helpText[0]) sprintf_s(g_helpText, "Drop-in mod from plugins\\%ls.", m.folder);
    if (m.needsLoader) {
        // In front, so it is seen: this mod is not loaded, whatever its switch says.
        char need[sizeof(g_helpText)];
        _snprintf_s(need, _TRUNCATE, "Not loaded: needs DS3 Mod Loader %s or later (this is " DS3_PROXY_VERSION "). %s",
                    m.minLoader, g_helpText);
        strcpy_s(g_helpText, need);
    }
    return g_helpText;
}

bool Selectable(int r)
{
    return r >= 0 && r < g_rowCount && g_rows[r].type != RowType::Heading;
}

void Select(int r)
{
    if (Selectable(r)) g_selected = r;
}

void Move(int dir)
{
    int r = g_selected;
    for (int n = 0; n < g_rowCount; ++n) {
        r = (r + dir + g_rowCount) % g_rowCount;
        if (Selectable(r)) {
            g_selected = r;
            return;
        }
    }
}

// To the first row of the next / previous group.
void JumpGroup(int dir)
{
    int own = g_selected;  // the current group's heading
    while (own > 0 && g_rows[own].type != RowType::Heading) --own;
    int r = own + dir;
    while (r > 0 && r < g_rowCount && g_rows[r].type != RowType::Heading) r += dir;
    if (r >= g_rowCount) r = g_rowCount - 1;  // past the last group: the Close row
    Select(r < 0 ? 1 : r + (g_rows[r].type == RowType::Heading ? 1 : 0));
}

void Activate(int r, int dir)
{
    if (!Selectable(r)) return;
    const Row& row = g_rows[r];
    if (row.type == RowType::Setting)
        ChangeSetting(kSettings[row.index], dir);
    else if (row.type == RowType::ModSwitch)
        ChangeModSwitch(ModAt(row.index));
    else if (row.type == RowType::ModSetting)
        ChangeModSetting(r, ModSettingAt(row.index), dir);
}

void Open()
{
    g_open = true;
    g_releasing = false;
    g_cursorShown = false;
    g_capture = -1;
    if (!Selectable(g_selected)) Move(1);
    g_status[0] = '\0';
    InputSetBlocked(true);
    Log("mods panel: opened");
}

void Close()
{
    g_open = false;
    g_releasing = true;  // unblocked once nothing is held
    Log("mods panel: closed");
}

// True for the first frame a button is held, then again every kRepeatMs after kRepeatDelayMs.
bool Repeat(int i, bool now, bool before, ULONGLONG t)
{
    if (!now) return false;
    if (!before) {
        g_heldSince[i] = t;
        g_lastRepeat[i] = t;
        return true;
    }
    if (t - g_heldSince[i] >= kRepeatDelayMs && t - g_lastRepeat[i] >= kRepeatMs) {
        g_lastRepeat[i] = t;
        return true;
    }
    return false;
}

void UpdateCursor(const InputFrame& in, float width, float height, void* window, bool* moved)
{
    *moved = false;
    POINT os{};
    if (GetCursorPos(&os) && (os.x != g_lastOsCursor.x || os.y != g_lastOsCursor.y)) {
        g_lastOsCursor = os;
        RECT client{};
        POINT p = os;
        if (window && ScreenToClient(static_cast<HWND>(window), &p) &&
            GetClientRect(static_cast<HWND>(window), &client) && client.right > 0 && client.bottom > 0) {
            g_cursorX = p.x * width / client.right;
            g_cursorY = p.y * height / client.bottom;
            *moved = true;
        }
    } else if (in.mouseDx || in.mouseDy) {
        float scale = std::max(1.0f, height / 1080.0f);
        g_cursorX += in.mouseDx * scale;
        g_cursorY += in.mouseDy * scale;
        *moved = true;
    }
    g_cursorX = std::clamp(g_cursorX, 0.0f, width - 1);
    g_cursorY = std::clamp(g_cursorY, 0.0f, height - 1);
    if (*moved) g_cursorShown = true;
}

void HandleOpen(const InputFrame& in, const Buttons& b, float width, float height, void* window)
{
    ULONGLONG t = GetTickCount64();
    if (g_capture >= 0) {
        HandleCapture(in);
        return;
    }
    if ((b.back && !g_prev.back) || (b.toggle && !g_prev.toggle)) {
        Close();
        return;
    }
    if (Repeat(0, b.up, g_prev.up, t)) Move(-1);
    if (Repeat(1, b.down, g_prev.down, t)) Move(1);
    if (Repeat(2, b.left, g_prev.left, t)) Activate(g_selected, -1);
    if (Repeat(3, b.right, g_prev.right, t)) Activate(g_selected, 1);
    if (b.pageUp && !g_prev.pageUp) JumpGroup(-1);
    if (b.pageDown && !g_prev.pageDown) JumpGroup(1);
    if (b.accept && !g_prev.accept) {
        if (g_rows[g_selected].type == RowType::Close) {
            Close();
            return;
        }
        Activate(g_selected, 0);
    }

    bool moved = false;
    UpdateCursor(in, width, height, window, &moved);
    if (in.wheel) Move(in.wheel > 0 ? -1 : 1);
    if (g_cursorShown) {
        int hover = -1;
        for (int r = 0; r < g_rowCount; ++r) {
            if (g_rowVisible[r] && Selectable(r) && g_rowBox[r].Contains(g_cursorX, g_cursorY)) hover = r;
        }
        if (moved && hover >= 0) g_selected = hover;
        if (b.click && !g_prev.click && hover >= 0) {
            g_selected = hover;
            if (g_rows[hover].type == RowType::Close) {
                Close();
                return;
            }
            // On a value with arrows, the left half steps down and the right half up.
            const Box& v = g_valueBox[hover];
            const Row& row = g_rows[hover];
            bool arrows = false;
            if (row.type == RowType::Setting) arrows = kSettings[row.index].kind != SettingKind::Flag;
            if (row.type == RowType::ModSetting) {
                ModSettingKind kind = ModSettingAt(row.index).kind;
                arrows = kind != ModSettingKind::Bool && kind != ModSettingKind::Key;
            }
            Activate(hover, arrows ? (g_cursorX < v.x + v.w * 0.5f ? -1 : 1) : 0);
        }
    }
}

// ---- Pause button ----
//
// A "mods" button drawn by us under the pause menu's last row ("save and quit"), left of the movie's "back" button;
// the movie itself is not changed (it has clips for six rows only). Placed from a 1920x1080 screenshot,
// relative to the screen centre and scaled like the movie (to fit 1920x1080); [mods_menu] pause_button_x / _y move it
// (in 1080p pixels). The button behaves like one more row between the last and the first: while the game shows its
// highlight on the last row, down is kept from the game (which would wrap to the top) and selects the button; up does
// the same from the first row. The game's highlight is hidden meanwhile and gets no input; up / down leave to the
// last / first row, Esc / B back to where it came from, a mouse move off the button leaves the selection as it is.

// The list control is at screen +0x5C (vtable 0xE39D44). Its +0x28 is the selected row and the byte at +0x2C hides the
// highlight (FUN_00b51620 gives a row the "selected" look only if it is the selected one and +0x2C is 0); the game sets
// it while the mouse is not over a row and clears it on the first key press. FUN_00b51bb0 (__thiscall(list, row,
// force), ret 8) redraws one row with its current look, as the game does for the old and new row when it moves the
// selection. (Do not touch list +0x70 / +0x74: the range of rows shown. With it broken the game stops redrawing the
// row it leaves, and every row it passes stays highlighted.)
constexpr uintptr_t kScreenList = 0x5C;
constexpr uintptr_t kRedrawRowVa = 0x00B51BB0;
constexpr unsigned char kRedrawRowBytes[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x53, 0x56,
                                             0x8B, 0xF1, 0x8B, 0x5E, 0x40, 0x57, 0x83, 0xFB};
using RedrawRowFn = void(__fastcall*)(void* list, void* edx, int row, int force);
bool g_redrawRowOk = false;
constexpr uintptr_t kScreenSelected = kScreenList + 0x28;
constexpr uintptr_t kScreenNoHighlight = kScreenList + 0x2C;
constexpr uintptr_t kScreenRowCount = 0x148;
// Greyed-out rows (e.g. "checkpoint restart" in some states) are skipped by the game's own navigation, which asks the
// screen's slot 33 (FUN_00523e40: the first enabled row at or after `from`) or slot 34 (FUN_00523ec0: at or before
// it), both wrapping and returning `from` when no row is enabled. Activation (FUN_00510ea0) does not check again, so
// the button must only ever select a row these give. Both fill each row they test through slot 46.
constexpr int kNextEnabledSlot = 33, kPrevEnabledSlot = 34;
constexpr uintptr_t kNextEnabled = 0x00523E40, kPrevEnabled = 0x00523EC0;
using StepRowFn = uint32_t(__fastcall*)(void* screen, void* edx, uint32_t from);
bool g_stepRowOk = false;
int g_firstEnabled = 0, g_lastEnabled = 0;
int64_t g_boundsFor = -1;  // the selected row the two above were worked out for; -1 = not yet on this pause
// Slot 42 (FUN_005064c0, __thiscall(screen, row), ret 4) is what the movie calls when its navigation reaches a row:
// it selects the row through the list (FUN_00b54bb0) and plays the move sound. The button uses it to keep a held
// key scrolling past "mods", while the game is kept from seeing that key.
constexpr int kSelectRowSlot = 42;
constexpr uintptr_t kSelectRow = 0x005064C0;
using SelectRowFn = void(__fastcall*)(void* screen, void* edx, int row);
bool g_selectRowOk = false;
// A held up / down repeats. The game's timing is measured while it scrolls by itself (first repeat after
// g_repeatDelay, then every g_repeatRate ms), so the button keeps the same pace. The defaults, used until then, are
// what it measured in game with the keyboard (about 340-380 ms, then 140-150 ms).
unsigned g_repeatDelay = 350, g_repeatRate = 150;
struct NavHold {
    int dir = 0;  // 1 down, -1 up, 0 none
    ULONGLONG start = 0, last = 0;  // when it was pressed; the last step the game made with it
    int steps = 0;
    uint32_t sel = 0;
} g_hold;
int g_driveDir = 0;  // the button holds the navigation (game input blocked) and steps rows itself while it is held
ULONGLONG g_driveNext = 0;
constexpr float kButtonLeft = -260, kButtonRight = 80, kButtonTop = 262, kButtonBottom = 306, kButtonText = -214;
constexpr ULONGLONG kButtonDelayMs = 450;  // the menu's opening animation
constexpr ULONGLONG kButtonFadeMs = 200;

float ButtonScale(float width, float height)
{
    return std::min(width / 1920.0f, height / 1080.0f);
}

Box ButtonBox(float width, float height)
{
    const float s = ButtonScale(width, height), cx = width / 2, cy = height / 2;
    const float x0 = std::round(cx + (kButtonLeft + g_buttonNudgeX) * s);
    const float y0 = std::round(cy + (kButtonTop + g_buttonNudgeY) * s);
    return {x0, y0, std::round((kButtonRight - kButtonLeft) * s), std::round((kButtonBottom - kButtonTop) * s)};
}

float ButtonAlpha()
{
    ULONGLONG t = GetTickCount64() - g_pauseAt;
    if (t < kButtonDelayMs) return 0;
    return std::min(1.0f, static_cast<float>(t - kButtonDelayMs) / kButtonFadeMs);
}

void LoadButtonPlace()
{
    g_buttonOn = g_stepRowOk && GetPrivateProfileIntW(L"mods_menu", L"pause_button", 1, g_config.iniPath) != 0;
    g_buttonNudgeX = static_cast<float>(static_cast<int>(
        GetPrivateProfileIntW(L"mods_menu", L"pause_button_x", 0, g_config.iniPath)));
    g_buttonNudgeY = static_cast<float>(static_cast<int>(
        GetPrivateProfileIntW(L"mods_menu", L"pause_button_y", 0, g_config.iniPath)));
}

template <class T>
void WriteField(uintptr_t addr, T value)
{
    __try {
        *reinterpret_cast<volatile T*>(addr) = value;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Has the list draw one row again with its current look (selected or not).
void RedrawRow(uintptr_t screen, int row)
{
    if (!g_redrawRowOk) return;
    __try {
        reinterpret_cast<RedrawRowFn>(kRedrawRowVa)(reinterpret_cast<void*>(screen + kScreenList), nullptr, row, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The enabled row nearest `from`, forwards or backwards (wrapping); `from` itself if the check is unavailable.
int EnabledRow(uintptr_t screen, int from, bool forward)
{
    if (!g_stepRowOk) return from;
    uint32_t row = static_cast<uint32_t>(from);
    __try {
        const uintptr_t fn = forward ? kNextEnabled : kPrevEnabled;
        row = reinterpret_cast<StepRowFn>(fn)(reinterpret_cast<void*>(screen), nullptr, static_cast<uint32_t>(from));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return static_cast<int>(row);
}

int g_focusFrom = 0;  // the row selected when the button took over, for Esc / B

void EnterButton(uintptr_t screen, int fromRow)
{
    g_buttonFocus = true;
    g_focusFrom = fromRow;
    InputSetBlocked(true);
    WriteField<uint8_t>(screen + kScreenNoHighlight, 1);  // the game's highlight off: only the button looks selected
    RedrawRow(screen, fromRow);
}

// row: the game's row to select, or -1 to leave its selection alone (the mouse moved off the button). drive: up /
// down is still held and the button goes on stepping rows itself; else the input is handed back once nothing is held.
void LeaveButton(uintptr_t screen, int row, bool drive = false)
{
    if (row >= 0) {
        // The row it came from was already redrawn unselected on the way in; drawing it unselected again makes the
        // movie replay its un-highlight, a visible flicker. So only the new row is drawn.
        WriteField<int32_t>(screen + kScreenSelected, row);
        WriteField<uint8_t>(screen + kScreenNoHighlight, 0);
        RedrawRow(screen, row);
    }
    g_buttonFocus = false;
    if (!drive) {
        g_driveDir = 0;
        g_releasing = true;  // unblocked once nothing is held, so the key that left is not seen by the game
    }
}

void SelectRow(uintptr_t screen, int row)
{
    __try {
        reinterpret_cast<SelectRowFn>(kSelectRow)(reinterpret_cast<void*>(screen), nullptr, row);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void LearnRepeat(unsigned* value, ULONGLONG sample, unsigned lo, unsigned hi)
{
    if (sample >= lo && sample <= hi) *value = static_cast<unsigned>((*value + sample) / 2);
}

// Follows a held up / down while the game scrolls by itself: measures its repeat timing, and tells when its next step
// would be due.
void TrackHold(int dir, uint32_t selected, ULONGLONG now)
{
    NavHold& h = g_hold;
    if (dir != h.dir) {
        h = {dir, now, now, 1, selected};  // the press itself is the first step
        return;
    }
    if (!dir || !h.steps || selected == h.sel) return;
    h.sel = selected;
    if (h.steps == 1 && now - h.start < 60) return;  // the press's own step, seen a frame late
    ++h.steps;
    if (h.steps == 2)
        LearnRepeat(&g_repeatDelay, now - h.start, 150, 1000);
    else
        LearnRepeat(&g_repeatRate, now - h.last, 30, 500);
    h.last = now;
}

// While the pause menu is up and the panel closed. Returns the presses to keep from the game.
unsigned UpdateButton(uintptr_t screen, const InputFrame& in, const Buttons& b, float width, float height,
                      void* window)
{
    if (!g_buttonOn || ButtonAlpha() <= 0) {
        g_wasAtLast = g_wasAtFirst = false;
        g_boundsFor = -1;
        return 0;
    }
    unsigned mask = 0;
    g_buttonBox = ButtonBox(width, height);
    bool moved = false;
    UpdateCursor(in, width, height, window, &moved);
    g_buttonHover = g_cursorShown && g_buttonBox.Contains(g_cursorX, g_cursorY);
    if (g_buttonHover) mask |= kMaskClick;
    if (g_buttonHover && b.click && !g_prev.click) {
        Open();
        return 0;
    }
    uint32_t selected = 0, rows = 0, flags = 0;
    if (!game::SafeReadU32(screen + kScreenSelected, &selected) || !game::SafeReadU32(screen + kScreenRowCount, &rows) ||
        !game::SafeReadU32(screen + kScreenNoHighlight, &flags) || !rows || rows > 16)
        return mask;
    const int last = static_cast<int>(rows) - 1;
    // The first and last rows that can be selected, worked out again whenever the selection moves (not every frame).
    if (static_cast<int64_t>(selected) != g_boundsFor) {
        g_firstEnabled = EnabledRow(screen, 0, true);
        g_lastEnabled = EnabledRow(screen, last, false);
        g_boundsFor = selected;
    }
    // Whether the last / first row was already selected (with the highlight shown) a frame ago, while the game was
    // kept from seeing down / up. Else a press the game used this frame to reach that row would also take the button.
    const bool atLast = !g_buttonFocus && (flags & 0xFF) == 0 && static_cast<int>(selected) == g_lastEnabled;
    const bool atFirst = !g_buttonFocus && (flags & 0xFF) == 0 && static_cast<int>(selected) == g_firstEnabled;
    const bool armedDown = g_wasAtLast && atLast, armedUp = g_wasAtFirst && atFirst;
    g_wasAtLast = atLast;
    g_wasAtFirst = atFirst;

    const ULONGLONG now = GetTickCount64();
    const int dir = b.down == b.up ? 0 : b.down ? 1 : -1;
    const int edge = b.down && !g_prev.down ? 1 : b.up && !g_prev.up ? -1 : 0;
    // The step a held key makes now while the button holds the navigation: a new press at once, then the game's pace.
    auto driveStep = [&]() {
        if (edge) {
            g_driveDir = edge;
            g_driveNext = now + g_repeatDelay;
            return edge;
        }
        if (g_driveDir && dir == g_driveDir && now >= g_driveNext) {
            g_driveNext = now + g_repeatRate;
            return dir;
        }
        return 0;
    };

    if (g_buttonFocus) {
        // The button sits between the last row and the first, as if the list had one more row.
        if (moved && !g_buttonHover) {
            LeaveButton(screen, -1);
        } else if (b.back && !g_prev.back) {
            LeaveButton(screen, EnabledRow(screen, g_focusFrom, true));  // in case it was greyed out meanwhile
        } else if (b.accept && !g_prev.accept) {
            Open();
        } else {
            if (!dir) g_driveDir = 0;
            if (int step = driveStep())
                LeaveButton(screen, step > 0 ? EnabledRow(screen, 0, true) : EnabledRow(screen, last, false),
                            g_selectRowOk);
        }
        return 0;
    }
    if (g_driveDir) {
        // Left the button with up / down still held: the game still gets no input (it would take the held key for a
        // new press), and the button steps the rows at the game's pace until "mods" or the key is let go.
        if (moved || !dir) {
            g_driveDir = 0;
            g_releasing = true;
        } else if (int step = driveStep()) {
            const int s = static_cast<int>(selected);
            if (step > 0 ? s == g_lastEnabled : s == g_firstEnabled)
                EnterButton(screen, s);
            else
                SelectRow(screen, step > 0 ? EnabledRow(screen, (s + 1) % static_cast<int>(rows), true)
                                           : EnabledRow(screen, (s + last) % static_cast<int>(rows), false));
        }
        return 0;
    }

    TrackHold(dir, selected, now);
    // Only while the game shows its highlight: otherwise the first press just shows it, without moving.
    if (atLast) mask |= kMaskDown;
    if (atFirst) mask |= kMaskUp;
    // A held key takes the button when the game's next repeat would have been due.
    const ULONGLONG due = g_hold.steps <= 1 ? g_hold.start + g_repeatDelay : g_hold.last + g_repeatRate;
    const int held = g_hold.dir == dir && g_hold.steps && now >= due ? dir : 0;
    if (armedDown && (edge > 0 || held > 0)) {
        EnterButton(screen, static_cast<int>(selected));
        g_driveDir = 1;
        g_driveNext = now + (edge ? g_repeatDelay : g_repeatRate);
    } else if (armedUp && (edge < 0 || held < 0)) {
        EnterButton(screen, static_cast<int>(selected));
        g_driveDir = -1;
        g_driveNext = now + (edge ? g_repeatDelay : g_repeatRate);
    }
    return g_buttonFocus ? 0 : mask;
}

bool Update(float width, float height, void* window, int* textPx)
{
    uintptr_t screen = PauseScreen();
    bool paused = screen != 0;
    if (!paused && (g_buttonFocus || g_driveDir)) {
        // The pause screen went away while the button held the input: hand it back once nothing is held.
        g_buttonFocus = false;
        g_driveDir = 0;
        g_releasing = true;
    }
    if (!paused && !g_open && !g_releasing) {
        g_pauseSeen = false;
        g_buttonHover = false;
        g_wasAtLast = g_wasAtFirst = false;
        InputSetMask(0);
        return false;
    }

    InputWatchWindow(window);
    InputFrame in;
    InputPoll(&in);
    bool focus = GameInForeground(window);
    Buttons b{};
    if (focus) {
        const bool* k = in.keys;
        b.up = k[VK_UP] || k['W'] || (in.pad & kPadUp);
        b.down = k[VK_DOWN] || k['S'] || (in.pad & kPadDown);
        b.left = k[VK_LEFT] || k['A'] || (in.pad & kPadLeft);
        b.right = k[VK_RIGHT] || k['D'] || (in.pad & kPadRight);
        b.accept = k[VK_RETURN] || k[VK_SPACE] || (in.pad & kPadA);
        b.back = k[VK_ESCAPE] || k[VK_BACK] || (in.pad & kPadB);
        b.toggle = (g_config.modsMenuKey && g_config.modsMenuKey < 256 && k[g_config.modsMenuKey]) ||
                   (in.pad & g_config.modsMenuPadButton);
        b.pageUp = k[VK_PRIOR] || (in.pad & kPadLb);
        b.pageDown = k[VK_NEXT] || (in.pad & kPadRb);
        b.click = in.mouseLeft;
    }

    if (!g_pauseSeen) {
        g_prev = b;  // a key held while the pause menu came up does not count as a press
        g_pauseSeen = true;
        g_pauseAt = GetTickCount64();
        LoadButtonPlace();
        g_driveDir = 0;
        // Not a press: a key held while the menu came up does not count as a hold either.
        g_hold = {b.down == b.up ? 0 : b.down ? 1 : -1, g_pauseAt, g_pauseAt, 0, 0};
    }

    unsigned mask = 0;
    if (!g_releasing) g_releaseSince = 0;  // also when the wait ended elsewhere
    if (g_open) {
        if (!paused) {
            Close();
        } else {
            HandleOpen(in, b, width, height, window);
        }
    } else if (g_releasing) {
        // Every key, mouse button, pad button and trigger, not only the menu ones: else a key held while the panel
        // closes (W, right mouse, a trigger) reaches the game as a new press.
        bool held = b.accept || b.back || b.toggle || b.click || b.left || b.right || b.up || b.down || in.pad ||
                    in.padTrigger || in.mouseLeft;
        for (int vk = 1; vk < 256 && !held; ++vk) held = in.keys[vk];
        ULONGLONG now = GetTickCount64();
        if (!g_releaseSince) g_releaseSince = now;
        bool givenUp = held && now - g_releaseSince >= kReleaseWaitMs;
        if (givenUp) Log("mods panel: a key still reads as held %llu ms after closing; game input handed back anyway",
                         now - g_releaseSince);
        if (!held || givenUp) {
            g_releasing = false;
            g_releaseSince = 0;
            if (!g_buttonFocus) InputSetBlocked(false);  // back from the panel to the button: stays blocked
        }
    } else if (paused && focus && b.toggle && !g_prev.toggle) {
        Open();
    } else if (paused) {
        mask = UpdateButton(screen, in, b, width, height, window);
    }
    InputSetMask(mask);
    g_prev = b;
    if (focus)
        memcpy(g_prevKeys, in.keys, sizeof(g_prevKeys));
    else
        memset(g_prevKeys, 0, sizeof(g_prevKeys));

    // The panel follows [mods_menu] text_size; the button's label follows its bar, so it fits at any text size. Both
    // are the same at the default size, so opening and closing the panel does not rebuild the font then.
    if (g_open)
        *textPx = std::min(160, static_cast<int>(height / 34.0f * g_config.modsMenuTextScale / 100.0f + 0.5f));
    else
        *textPx = static_cast<int>(1080.0f / 34.0f * ButtonScale(width, height) + 0.5f);
    return g_open || (paused && g_buttonOn && ButtonAlpha() > 0);
}

// ---- Drawing ----

// Draws text wrapped to maxW, at most maxLines lines.
void DrawWrapped(OverlayCanvas& c, float x, float y, float maxW, const char* text, unsigned color, int maxLines)
{
    const float lh = c.LineHeight();
    char line[256];
    int lines = 0;
    const char* p = text;
    while (*p && lines < maxLines) {
        size_t len = 0, lastBreak = 0;
        while (p[len]) {
            char test[256];
            size_t n = std::min(len + 1, sizeof(test) - 1);
            memcpy(test, p, n);
            test[n] = '\0';
            if (c.TextWidth(test) > maxW) break;
            if (p[len] == ' ') lastBreak = len;
            ++len;
        }
        size_t take = p[len] && lastBreak ? lastBreak : len;
        if (!take) take = 1;
        take = std::min(take, sizeof(line) - 1);
        memcpy(line, p, take);
        line[take] = '\0';
        c.Text(x, y + lines * lh, line, color);
        p += take;
        while (*p == ' ') ++p;
        ++lines;
    }
}

void DrawCursor(OverlayCanvas& c, float x, float y)
{
    const float h = std::round(c.LineHeight() * 0.9f);
    for (int pass = 0; pass < 2; ++pass) {
        float grow = pass == 0 ? 2.0f : 0.0f;
        unsigned color = pass == 0 ? 0xE0000000 : 0xFFFFFFFF;
        for (float i = 0; i < h; i += 1.0f) {
            float w = std::max(1.0f, (i < h * 0.75f ? i : (h - i) * 2.0f) * 0.6f);
            c.Rect(x - grow, y + i - grow, w + 2 * grow, 1 + (pass == 0 ? 2 * grow : 0), color);
        }
    }
}

// text cut to maxW, ending in "..." when it had to be cut.
void Fit(OverlayCanvas& c, const char* text, float maxW, char* out, size_t size)
{
    strcpy_s(out, size, text);
    if (c.TextWidth(out) <= maxW) return;
    size_t len = strlen(out);
    while (len > 0) {
        out[--len] = '\0';
        char test[72];
        sprintf_s(test, "%s...", out);
        if (c.TextWidth(test) <= maxW) {
            strcpy_s(out, size, test);
            return;
        }
    }
}

void DrawPanel(OverlayCanvas& c)
{
    const float W = c.Width(), H = c.Height(), lh = c.LineHeight();
    const float pad = std::round(lh * 0.9f);
    const float rowH = std::round(lh * 1.5f);
    const float panelW = std::min(std::round(lh * 34.0f), W - 2 * pad);
    const float panelH = std::min(std::round(H * 0.88f), std::round(rowH * (g_rowCount + 1) + lh * 7.5f));
    const float px = std::round((W - panelW) / 2), py = std::round((H - panelH) / 2);

    c.Rect(0, 0, W, H, kShade);
    c.Rect(px, py, panelW, panelH, kPanelBg);
    c.Rect(px, py, panelW, 3, kAccent);

    // Title
    float y = py + pad * 0.7f;
    c.Text(px + pad, y, "MODS", kAccent);
    const char* sub = "Dead Space 3 mod settings";
    c.Text(px + panelW - pad - c.TextWidth(sub), y, sub, kDim);
    y += lh * 1.6f;

    // Rows
    const float footerH = lh * 4.6f;
    const float listTop = y, listBottom = py + panelH - footerH;
    const int visibleRows = std::max(1, static_cast<int>((listBottom - listTop) / rowH));
    int maxScroll = std::max(0, g_rowCount - visibleRows);
    if (g_selected < g_scroll + 1 && g_selected > 0) g_scroll = std::min(g_scroll, g_selected - 1);
    if (g_selected >= g_scroll + visibleRows) g_scroll = g_selected - visibleRows + 1;
    g_scroll = std::clamp(g_scroll, 0, maxScroll);

    const float valueRight = px + panelW - pad;
    const float valueW = std::round(lh * 11.0f);
    for (int r = 0; r < g_rowCount; ++r) {
        g_rowVisible[r] = r >= g_scroll && r < g_scroll + visibleRows;
        if (!g_rowVisible[r]) continue;
        const Row& row = g_rows[r];
        float ry = listTop + (r - g_scroll) * rowH;
        float ty = ry + std::round((rowH - lh) / 2);
        g_rowBox[r] = {px + pad * 0.5f, ry, panelW - pad, rowH};
        g_valueBox[r] = {valueRight - valueW, ry, valueW, rowH};
        if (row.type == RowType::Heading) {
            char upper[64];
            strcpy_s(upper, row.heading);
            for (char* ch = upper; *ch; ++ch) *ch = static_cast<char>(toupper(static_cast<unsigned char>(*ch)));
            c.Text(px + pad, ty + lh * 0.15f, upper, kHeading);
            c.Rect(px + pad, ry + rowH - 2, panelW - 2 * pad, 1, 0x40FFFFFF);
            continue;
        }
        bool sel = r == g_selected;
        if (sel) {
            c.Rect(px + pad * 0.5f, ry + 1, panelW - pad, rowH - 2, kSelectBg);
            c.Rect(px + pad * 0.5f, ry + 1, 3, rowH - 2, kAccent);
        }
        if (row.type == RowType::Close) {
            const char* t = "Close";
            c.Text(px + (panelW - c.TextWidth(t)) / 2, ty, t, sel ? kText : kDim);
            continue;
        }

        const char* label = "";
        char value[48] = {};
        bool isFlag = false, on = false, changed = false;
        const char* tag = nullptr;
        char tagText[48];
        if (row.type == RowType::Setting) {
            const Setting& s = kSettings[row.index];
            label = s.label;
            FormatSetting(s, value, sizeof(value));
            isFlag = s.kind == SettingKind::Flag;
            on = isFlag && g_config.*s.flag;
            if (s.apply == SettingApply::Restart) {
                changed = SettingValue(g_config, s) != SettingValue(g_startup, s);
                tag = changed ? "applies after restart" : "restart";
            }
        } else if (row.type == RowType::ModSwitch) {
            const Mod& m = ModAt(row.index);
            label = m.name;
            isFlag = true;
            on = m.enabled;
            strcpy_s(value, on ? "On" : "Off");
            changed = m.enabled != m.startup;
            tag = changed ? "applies after restart" : (m.startup && !m.loaded) ? "did not load, see log" : "restart";
            if (!changed && m.startup && !m.loaded) changed = true;  // shown in the warning colour
            if (m.needsLoader) {
                sprintf_s(tagText, "needs loader %s", m.minLoader);
                tag = tagText;
                changed = true;
            }
        } else {
            const ModSetting& s = ModSettingAt(row.index);
            const Mod& m = ModAt(s.mod);
            label = s.label;
            if (g_capture == r)
                strcpy_s(value, "Press a key...");
            else
                FormatModSetting(s, value, sizeof(value));
            isFlag = s.kind == ModSettingKind::Bool;
            on = isFlag && s.value != 0;
            if (!ModSettingAppliesLive(s)) {
                changed = std::fabs(s.value - s.startup) > 1e-9;
                if (m.loaded && m.reloadHint[0]) {
                    sprintf_s(tagText, changed ? "press %s or restart" : "%s / restart", m.reloadHint);
                    tag = tagText;
                } else {
                    tag = changed ? "applies after restart" : "restart";
                }
            }
        }
        float labelRight = valueRight - valueW - pad * 0.6f;
        if (tag) {
            float tw = c.TextWidth(tag);
            c.Text(labelRight - tw, ty, tag, changed ? kWarn : 0xFF6E7781);
            labelRight -= tw + pad * 0.6f;
        }
        char fitted[64];
        Fit(c, label, labelRight - (px + pad * 1.2f), fitted, sizeof(fitted));
        c.Text(px + pad * 1.2f, ty, fitted, sel ? kText : 0xFFD8DDE2);
        if (isFlag) {
            float bw = std::round(lh * 3.2f), bh = std::round(lh * 1.05f);
            float bx = valueRight - bw, by = ry + std::round((rowH - bh) / 2);
            c.Rect(bx, by, bw, bh, on ? kOnBg : kOffBg);
            c.Text(bx + std::round((bw - c.TextWidth(value)) / 2), ty, value, on ? kText : kDim);
        } else {
            float vw = c.TextWidth(value);
            float mid = valueRight - valueW / 2;
            c.Text(mid - vw / 2, ty, value, kText);
            unsigned arrow = sel ? kAccent : 0xFF6E7781;
            c.Text(valueRight - valueW, ty, "<", arrow);
            c.Text(valueRight - c.TextWidth(">"), ty, ">", arrow);
        }
    }
    if (g_scroll > 0) c.Text(px + panelW - pad * 0.9f, listTop - lh * 0.9f, "^", kDim);
    if (g_scroll < maxScroll) c.Text(px + panelW - pad * 0.9f, listBottom - lh * 0.4f, "v", kDim);

    // Footer: what the selected row does, the last change, and the keys.
    float fy = listBottom + lh * 0.4f;
    c.Rect(px + pad, fy - lh * 0.25f, panelW - 2 * pad, 1, 0x40FFFFFF);
    const char* help = "";
    if (Selectable(g_selected)) {
        const Row& row = g_rows[g_selected];
        if (g_capture == g_selected)
            help = "Press the key to use. Esc / B: cancel. Backspace: none.";
        else if (row.type == RowType::Setting)
            help = HelpFor(kSettings[row.index]);
        else if (row.type == RowType::ModSwitch)
            help = ModHelp(ModAt(row.index));
        else if (row.type == RowType::ModSetting)
            help = ModSettingAt(row.index).help;
        else
            help = "Back to the pause menu.";
    }
    DrawWrapped(c, px + pad, fy, panelW - 2 * pad, help, 0xFFC8CED4, 2);
    fy += lh * 2.1f;
    if (g_status[0] && GetTickCount64() - g_statusAt < kStatusMs) c.Text(px + pad, fy, g_status, g_statusColor);
    fy += lh * 1.15f;
    const char* keys = "Up/Down: choose   Left/Right: change   Enter/A: toggle   Esc/B: close";
    if (c.TextWidth(keys) > panelW - 2 * pad) keys = "Arrows: choose/change   Enter/A: toggle   Esc/B: close";
    c.Text(px + pad, fy, keys, 0xFF7C8690);

    if (g_cursorShown) DrawCursor(c, g_cursorX, g_cursorY);
}

unsigned Faded(unsigned color, float alpha)
{
    unsigned a = static_cast<unsigned>((color >> 24) * alpha + 0.5f);
    return (a << 24) | (color & 0x00FFFFFF);
}

// Styled after the movie's rows: dark see-through bar, thin cyan edge, light blue lowercase text.
void DrawButton(OverlayCanvas& c)
{
    const float alpha = ButtonAlpha();
    const Box& r = g_buttonBox;
    const bool lit = g_buttonFocus || g_buttonHover;
    const float edge = std::max(1.0f, std::round(r.h / 22.0f));
    c.Rect(r.x, r.y, r.w, r.h, Faded(lit ? 0xD0185A70 : 0xB00C2630, alpha));
    const unsigned edgeColor = Faded(lit ? 0xFF8FE0FF : 0xFF2C7085, alpha);
    c.Rect(r.x, r.y, r.w, edge, edgeColor);
    c.Rect(r.x, r.y + r.h - edge, r.w, edge, edgeColor);
    c.Rect(r.x, r.y, edge, r.h, edgeColor);
    c.Rect(r.x + r.w - edge, r.y, edge, r.h, edgeColor);
    const float s = r.w / (kButtonRight - kButtonLeft);
    const float tx = std::round(r.x + (kButtonText - kButtonLeft) * s);
    const float ty = std::round(r.y + (r.h - c.LineHeight()) / 2);
    c.Text(tx, ty, "mods", Faded(lit ? 0xFFFFFFFF : 0xFF7CC4EC, alpha));
}

void Draw(OverlayCanvas& c)
{
    if (g_open)
        DrawPanel(c);
    else
        DrawButton(c);
}

}  // namespace

void InstallModMenu(const Config& config)
{
    g_config = config;
    g_startup = config;
    BuildRows();
    Move(1);

    uint32_t activate = 0, label = 0;
    g_pauseOk = game::SafeReadU32(kPauseVtable + 31 * 4, &activate) && activate == kPauseActivate &&
                game::SafeReadU32(kPauseVtable + 46 * 4, &label) && label == kPauseRowLabel;
    if (!g_pauseOk) {
        Log("mods panel: pause screen vtable does not match (different game build?); panel unavailable");
        return;
    }
    OverlaySetPanel({&Update, &Draw});
    g_redrawRowOk = game::BytesMatch(kRedrawRowVa, kRedrawRowBytes, sizeof(kRedrawRowBytes));
    if (!g_redrawRowOk) Log("pause button: row redraw function differs; the game's highlight may lag behind the button");
    uint32_t next = 0, prev = 0;
    g_stepRowOk = game::SafeReadU32(kPauseVtable + kNextEnabledSlot * 4, &next) && next == kNextEnabled &&
                  game::SafeReadU32(kPauseVtable + kPrevEnabledSlot * 4, &prev) && prev == kPrevEnabled;
    if (!g_stepRowOk) {
        // Without the game's enabled-row check the button could select a greyed-out row (activation does not check).
        Log("pause button: enabled-row functions differ; button off (F11 / Back still open the panel)");
    }
    uint32_t select = 0;
    g_selectRowOk = game::SafeReadU32(kPauseVtable + kSelectRowSlot * 4, &select) && select == kSelectRow;
    if (!g_selectRowOk) Log("pause button: select-row function differs; a held key stops after leaving the button");
    Log("mods panel: ready (%d rows, %d drop-in mod(s)); opens from the pause menu, key 0x%02X, pad button 0x%04X",
        g_rowCount, ModCount(), config.modsMenuKey, config.modsMenuPadButton);
}
