// Resource Counter: a small box on screen that lists your resources and updates as you pick them up.
//
// This is the sample to copy when you make your own mod. It shows, in about 250 lines:
//   - DS3ModInit: where a mod starts, and how to check that the proxy is new enough;
//   - reading settings from your own ini, and re-reading them when the Mods panel changes one;
//   - a frame callback: code the proxy runs once per frame, where you read the game and draw;
//   - reading the game (is there a player? is the game paused? how much scrap metal?) and drawing text and boxes.
// It never changes the game. sdk\README.md explains each part in plain words.
//
// Only one header is needed: ds3mod.h, which describes everything the proxy offers a mod.

#include "../ds3mod.h"

#include <windows.h>  // GetPrivateProfileIntW: reading our ini

#include <cmath>   // std::round
#include <cstdio>  // sprintf_s: turning numbers into text

// Everything below is "file-local" (inside an unnamed namespace), so its names can't clash with anything else.
namespace {

// The proxy's table of functions. We keep the pointer DS3ModInit gets; it stays valid until the game exits.
const DS3ModHost* g_host = nullptr;

// ---- Settings ----
// mod.ini lists these for the Mods panel (section [Counter] of resource_counter.ini). Here we only read them.

enum Corner { TopLeft = 0, TopRight = 1, BottomLeft = 2, BottomRight = 3 };

struct Settings {
    int corner = TopLeft;
    int sizePercent = 100;   // text size, % of the usual size
    int backgroundPercent = 60;  // how dark the box is: 0 = no box, 100 = solid
    bool showWhilePaused = false;
    bool showChanges = true;  // "+5" next to a resource for a few seconds after it changes
    bool show[DS3_RESOURCE_COUNT] = {true, true, true, true, true, true};
};
Settings g_settings;

// The ini keys for the six "show" switches, in the game's resource order (DS3_TUNGSTEN .. DS3_RATION_SEALS).
const wchar_t* const kShowKeys[DS3_RESOURCE_COUNT] = {L"ShowTungsten",   L"ShowSemiconductor", L"ShowScrapMetal",
                                                      L"ShowSomaticGel", L"ShowTransducer",    L"ShowRationSeals"};

// Reads one whole number from our ini, or `def` if it isn't there.
int ReadInt(const wchar_t* key, int def)
{
    return static_cast<int>(GetPrivateProfileIntW(L"Counter", key, def, g_host->iniPath));
}

void ReadSettings()
{
    Settings s;
    s.corner = ReadInt(L"Corner", TopLeft);
    s.sizePercent = ReadInt(L"Size", 100);
    s.backgroundPercent = ReadInt(L"Background", 60);
    s.showWhilePaused = ReadInt(L"ShowWhilePaused", 0) != 0;
    s.showChanges = ReadInt(L"ShowChanges", 1) != 0;
    for (int i = 0; i < DS3_RESOURCE_COUNT; ++i) s.show[i] = ReadInt(kShowKeys[i], 1) != 0;
    // Someone may edit the ini by hand: keep the numbers in a range that makes sense.
    if (s.corner < TopLeft || s.corner > BottomRight) s.corner = TopLeft;
    if (s.sizePercent < 50) s.sizePercent = 50;
    if (s.sizePercent > 300) s.sizePercent = 300;
    if (s.backgroundPercent < 0) s.backgroundPercent = 0;
    if (s.backgroundPercent > 100) s.backgroundPercent = 100;
    g_settings = s;
}

// ---- Remembering changes, for the "+5" ----

constexpr unsigned kChangeShownMs = 3000;  // how long a change stays next to its resource

struct Tracked {
    bool known = false;     // we have read it at least once since the player appeared
    unsigned value = 0;     // what it was last frame
    int change = 0;         // the change shown now (several pickups close together add up)
    unsigned changedAt = 0; // when it last changed (DS3Frame.timeMs)
};
Tracked g_tracked[DS3_RESOURCE_COUNT];

// ---- Drawing ----

// Colours are 0xAARRGGBB; DS3_RGBA(red, green, blue, alpha) builds one from 0-255 values.
const unsigned kNameColor = DS3_RGBA(170, 200, 215, 255);  // pale blue-grey, a bit like the game's RIG text
const unsigned kValueColor = DS3_WHITE;
const unsigned kUpColor = DS3_RGBA(110, 230, 140, 255);    // green
const unsigned kDownColor = DS3_RGBA(255, 120, 100, 255);  // red
const unsigned kAccentColor = DS3_RGBA(90, 190, 255, 255); // the thin bar on the box's left edge

// The proxy calls this once per frame, on the game's own thread. `frame` has the screen size, the time, the game's
// state, and the drawing functions. Anything drawn here shows for this one frame only, so it is drawn again every
// frame. Keep it quick: the game waits for it.
void __cdecl OnFrame(const DS3Frame* frame, void*)
{
    // 1. Is there anything to show? On the main menu and loading screens there is no player: forget what we knew,
    //    so loading another save doesn't show its difference as a "change".
    if (frame->state == DS3_STATE_NO_PLAYER) {
        for (Tracked& t : g_tracked) t = Tracked();
        return;
    }
    // The pause menu has text of its own; stay out of its way unless the player asked otherwise.
    const bool hidden = frame->state == DS3_STATE_PAUSED && !g_settings.showWhilePaused;

    // 2. Read the six resources, and note changes even while hidden (so a change made in the pause menu shows after).
    unsigned value[DS3_RESOURCE_COUNT] = {};
    bool have[DS3_RESOURCE_COUNT] = {};
    for (int i = 0; i < DS3_RESOURCE_COUNT; ++i) {
        have[i] = g_host->getResource(i, &value[i]) != 0;
        if (!have[i]) continue;
        Tracked& t = g_tracked[i];
        if (t.known && value[i] != t.value) {
            const int delta = static_cast<int>(value[i]) - static_cast<int>(t.value);
            const bool stillShown = frame->timeMs - t.changedAt < kChangeShownMs;
            t.change = stillShown ? t.change + delta : delta;
            t.changedAt = frame->timeMs;
        }
        t.known = true;
        t.value = value[i];
    }
    if (hidden) return;

    // 3. Work out the box's size. The text size scales with the screen (about height / 54) and our Size setting.
    const float lineH = frame->setTextSize(frame, frame->height / 54.0f * g_settings.sizePercent / 100.0f);
    const float pad = std::round(lineH * 0.45f);  // space inside the box
    const float gap = std::round(lineH * 0.8f);   // between the name and number columns

    // The widest name, number and change decide the columns. Numbers are drawn right-aligned in their column.
    char number[DS3_RESOURCE_COUNT][16] = {};
    char change[DS3_RESOURCE_COUNT][16] = {};
    float nameW = 0, numberW = 0, changeW = 0;
    int rows = 0;
    for (int i = 0; i < DS3_RESOURCE_COUNT; ++i) {
        if (!have[i] || !g_settings.show[i]) continue;
        ++rows;
        sprintf_s(number[i], "%u", value[i]);
        const Tracked& t = g_tracked[i];
        if (g_settings.showChanges && t.change != 0 && frame->timeMs - t.changedAt < kChangeShownMs)
            sprintf_s(change[i], "%+d", t.change);  // "%+d" writes the sign: +5 or -10
        const float w1 = frame->textWidth(frame, g_host->resourceName(i));
        const float w2 = frame->textWidth(frame, number[i]);
        const float w3 = frame->textWidth(frame, change[i]);
        if (w1 > nameW) nameW = w1;
        if (w2 > numberW) numberW = w2;
        if (w3 > changeW) changeW = w3;
    }
    if (rows == 0) return;  // every resource switched off in the settings
    // Room for "+999" always, so the box doesn't jump wider when a change appears.
    const float changeColumn = frame->textWidth(frame, "+999") + pad;
    if (changeW < changeColumn) changeW = changeColumn;

    const float boxW = pad + nameW + gap + numberW + pad + changeW;
    const float boxH = pad + rows * lineH + pad;

    // 4. Place it in the chosen corner, a little away from the screen's edge.
    const float margin = std::round(frame->height * 0.03f);
    const bool right = g_settings.corner == TopRight || g_settings.corner == BottomRight;
    const bool bottom = g_settings.corner == BottomLeft || g_settings.corner == BottomRight;
    const float x = right ? frame->width - margin - boxW : margin;
    const float y = bottom ? frame->height - margin - boxH : margin;

    // 5. Draw: the see-through box, a thin bar on its left edge, then one line per resource.
    if (g_settings.backgroundPercent > 0) {
        const unsigned alpha = static_cast<unsigned>(g_settings.backgroundPercent * 255 / 100);
        frame->rect(frame, x, y, boxW, boxH, DS3_RGBA(8, 12, 16, alpha));
        frame->rect(frame, x, y, std::round(lineH * 0.12f) + 1, boxH, kAccentColor);
    }
    float lineY = y + pad;
    for (int i = 0; i < DS3_RESOURCE_COUNT; ++i) {
        if (!have[i] || !g_settings.show[i]) continue;
        const float nameX = x + pad;
        const float numberRight = nameX + nameW + gap + numberW;  // right edge of the number column
        frame->text(frame, nameX, lineY, g_host->resourceName(i), kNameColor);
        frame->text(frame, numberRight - frame->textWidth(frame, number[i]), lineY, number[i], kValueColor);
        if (change[i][0]) {
            const unsigned color = g_tracked[i].change > 0 ? kUpColor : kDownColor;
            frame->text(frame, numberRight + pad, lineY, change[i], color);
        }
        lineY += lineH;
    }
}

}  // namespace

// ---- What the proxy calls ----

// Called once, when the game starts and the mod is switched on. `host` is the proxy's table of functions.
extern "C" __declspec(dllexport) int __cdecl DS3ModInit(const DS3ModHost* host)
{
    // This mod needs API 2 (onFrame, getResource...). An older proxy has a shorter table: say so and stop.
    if (!host || host->apiVersion < 2 || !DS3MOD_HAS(host, onFrame)) {
        if (host && host->log) host->log(host, "Resource Counter needs a newer loader (API 2 or later)");
        return 1;  // logged as a failure; the mod does nothing
    }
    g_host = host;
    ReadSettings();
    host->onFrame(host, &OnFrame, nullptr);  // from now on OnFrame runs once per frame
    host->log(host, "Resource Counter started");
    return 0;
}

// Called when the Mods panel changed one of our settings. It runs on the same thread as OnFrame, so the two never
// run at the same time and g_settings needs no lock.
extern "C" __declspec(dllexport) void __cdecl DS3ModSettingsChanged()
{
    ReadSettings();
}
