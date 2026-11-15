// Hello Mod: the smallest useful mod. It writes to ds3_mods.log and, on its hotkey, greets you on screen.
//
// It shows: DS3ModInit (start-up), reading its own ini, a hotkey that respects the Mods panel, DS3ModSettingsChanged
// (the panel changed a setting), and a frame callback that draws a line of text. It never changes the game.
//
// And DS3ModShutdown: with it, the loader swaps in a new build of HelloMod.asi while the game runs.
// Rebuild the mod, and the game picks it up within two seconds ("Hello Mod reloaded" top left).
//
// It also shows how a mod stays working with an older loader: an API 1 loader has no onFrame, so
// there the greeting only goes to the log. For a mod that reads the game and draws more, see resource_counter.

#include "../ds3mod.h"

#include <windows.h>

#include <cstdio>

namespace {

const DS3ModHost* g_host = nullptr;

// The settings, as mod.ini describes them. Written by DS3ModSettingsChanged, read by the hotkey thread.
// (volatile + Interlocked: two threads use them; this keeps each read and write whole.)
volatile LONG g_greet = 1;
volatile LONG g_repeat = 1;
volatile LONG g_hotkey = VK_F12;
float g_volume = 0.5f;
wchar_t g_greeting[32] = L"hello";

// The hotkey thread, and the event that tells it to stop (DS3ModShutdown).
HANDLE g_thread = nullptr;
HANDLE g_stop = nullptr;

// When the hotkey was last pressed (GetTickCount), for the on-screen greeting. 0 = not yet.
volatile LONG g_greetedAt = 0;
constexpr unsigned kGreetingShownMs = 3000;

void Say(const char* text)
{
    if (g_host) g_host->log(g_host, text);
}

void ReadSettings()
{
    const wchar_t* ini = g_host->iniPath;
    InterlockedExchange(&g_greet, GetPrivateProfileIntW(L"Hello", L"Greet", 1, ini) != 0);
    InterlockedExchange(&g_repeat, static_cast<LONG>(GetPrivateProfileIntW(L"Hello", L"Repeat", 1, ini)));
    // Keys are written as 0x hex; GetPrivateProfileInt reads hex too.
    InterlockedExchange(&g_hotkey, static_cast<LONG>(GetPrivateProfileIntW(L"Hello", L"Hotkey", VK_F12, ini)));
    wchar_t text[32];
    GetPrivateProfileStringW(L"Hello", L"Volume", L"0.50", text, 32, ini);
    g_volume = static_cast<float>(_wtof(text));
    GetPrivateProfileStringW(L"Hello", L"Greeting", L"hello", g_greeting, 32, ini);

    char line[160];
    sprintf_s(line, "settings: greet=%ld greeting=%ls repeat=%ld volume=%.2f hotkey=0x%02lX", g_greet, g_greeting,
              g_repeat, g_volume, g_hotkey);
    Say(line);
}

bool GameInForeground()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// Checks the hotkey 20 times a second. A thread of its own, because DS3ModInit must return quickly.
DWORD WINAPI HotkeyThread(void*)
{
    bool was = false;
    // Wait 50 ms between looks, or less if DS3ModShutdown asks us to stop.
    while (WaitForSingleObject(g_stop, 50) == WAIT_TIMEOUT) {
        const int vk = static_cast<int>(g_hotkey);
        const bool down = vk > 0 && vk < 256 && GameInForeground() && (GetAsyncKeyState(vk) & 0x8000);
        // A press while the Mods panel has the input (e.g. the one that just chose this key) is not for us. `was`
        // still follows the key, so a key held from then is not taken for a new press later either.
        const bool blocked = g_host->inputBlocked && g_host->inputBlocked();
        if (down && !was && !blocked && g_greet) {
            for (LONG i = 0; i < g_repeat; ++i) {
                char line[64];
                sprintf_s(line, "%ls from the hotkey (%ld of %ld)", g_greeting, i + 1, g_repeat);
                Say(line);
            }
            InterlockedExchange(&g_greetedAt, static_cast<LONG>(GetTickCount() | 1));  // never 0
        }
        was = down;
    }
    return 0;
}

// API 2: runs once per frame. For 3 seconds after the hotkey it draws the greeting near the top of the screen.
void __cdecl OnFrame(const DS3Frame* frame, void*)
{
    const unsigned at = static_cast<unsigned>(g_greetedAt);
    if (!at || frame->timeMs - at > kGreetingShownMs) return;

    char text[64];
    sprintf_s(text, "%ls from Hello Mod!", g_greeting);  // %ls: our greeting is a wide (wchar_t) string
    const float lineH = frame->setTextSize(frame, frame->height / 30.0f);  // bigger than usual
    const float w = frame->textWidth(frame, text);
    const float x = (frame->width - w) / 2;  // centred
    const float y = frame->height * 0.2f;
    frame->rect(frame, x - lineH * 0.5f, y - lineH * 0.2f, w + lineH, lineH * 1.4f, DS3_RGBA(0, 0, 0, 150));
    frame->text(frame, x, y, text, DS3_RGBA(255, 220, 120, 255));
}

}  // namespace

extern "C" __declspec(dllexport) int __cdecl DS3ModInit(const DS3ModHost* host)
{
    // Fields up to inputBlocked are API 1; every proxy that loads mods has them.
    if (!host || !DS3MOD_HAS(host, inputBlocked)) return 1;
    g_host = host;
    char line[128];
    sprintf_s(line, "Hello Mod started (proxy %s, API %u, mod id %s)", host->proxyVersion, host->apiVersion,
              host->modId);
    Say(line);
    ReadSettings();
    // API 2 has onFrame. Check before using it: on an older proxy the field isn't there at all.
    if (DS3MOD_HAS(host, onFrame))
        host->onFrame(host, &OnFrame, nullptr);
    else
        Say("this proxy has no on-screen drawing (API 1): greetings go to the log only");
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = g_stop ? CreateThread(nullptr, 0, &HotkeyThread, nullptr, 0, nullptr) : nullptr;
    return 0;
}

// Called before the proxy unloads this DLL to load a new build (hot reload). Afterwards nothing may run our code:
// stop the hotkey thread and wait until it has really ended. The proxy itself removes our frame callback (and any
// hooks and patches made through the host).
extern "C" __declspec(dllexport) void __cdecl DS3ModShutdown()
{
    if (g_stop) SetEvent(g_stop);
    if (g_thread) {
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_stop) {
        CloseHandle(g_stop);
        g_stop = nullptr;
    }
    Say("Hello Mod shut down (a new build is being loaded)");
}

extern "C" __declspec(dllexport) void __cdecl DS3ModSettingsChanged()
{
    ReadSettings();  // quick: a few ini reads
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
    return TRUE;
}
