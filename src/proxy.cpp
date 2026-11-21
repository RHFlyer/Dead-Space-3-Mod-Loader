// dinput8.dll proxy. Forwards every export to the real system dinput8.dll and logs.
//
// DllMain only sets up logging: hooks are never installed under the loader lock. The first DirectInput8Create call
// comes from real game code (the EA wrapper has finished decrypting .text by then), so hooks go in there.

#include "config.h"
#include "game.h"
#include "input.h"
#include "log.h"
#include "modmenu.h"
#include "mods.h"
#include "overlay.h"
#include "patches.h"
#include "sdk.h"
#include "startguard.h"
#include "version.h"

#include <windows.h>
#include <unknwn.h>

#include <cstring>
#include <cwchar>

namespace {

wchar_t g_moduleDir[MAX_PATH] = {};
HMODULE g_real = nullptr;
INIT_ONCE g_realOnce = INIT_ONCE_STATIC_INIT;
LONG g_createCalls = 0;
bool g_watchInput = false;  // the Mods panel is on: watch the DirectInput devices the game creates

// The overlay finds Present with a small throwaway Direct3D device of its own (overlay.cpp). Made while the game's
// main thread is still creating its real device, that has crashed the game inside ntdll: 2 of 8 starts with no mod
// to load, where this step came 0.3 to 0.8 s after the first DirectInput8Create, and none of 28 starts with mods,
// whose loading happened to push it to between 0.6 and 3.4 s. So the overlay is never installed sooner than this
// after the first DirectInput8Create. The game's input thread waits meanwhile; its main thread goes on.
constexpr ULONGLONG kOverlayNotBeforeMs = 2500;

// First 16 bytes of a function in the game's code (FUN_00a2a000) once the exe is unpacked in memory. On disk they
// are encrypted noise, so this tells whether the game's code is readable yet.
constexpr uintptr_t kProbeVa = 0x00A2A000;
constexpr unsigned char kProbeBytes[16] = {0x55, 0x8B, 0xEC, 0xB8, 0x84, 0x1C, 0x00, 0x00,
                                           0xE8, 0x53, 0x74, 0x15, 0x00, 0x53, 0x56, 0x8B};

const char* TextState()
{
    const auto* p = reinterpret_cast<const unsigned char*>(kProbeVa);
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return "unreadable";
    return std::memcmp(p, kProbeBytes, sizeof(kProbeBytes)) == 0 ? "decrypted" : "still encrypted";
}

BOOL CALLBACK LoadReal(PINIT_ONCE, PVOID, PVOID*)
{
    wchar_t path[MAX_PATH];
    UINT len = GetSystemDirectoryW(path, MAX_PATH);  // System32; WOW64 redirects a 32-bit process to SysWOW64.
    wcscpy_s(path + len, MAX_PATH - len, L"\\dinput8.dll");
    g_real = LoadLibraryW(path);
    if (g_real)
        Log("loaded real dinput8.dll at %p", g_real);
    else
        Log("FAILED to load real dinput8.dll (error %lu)", GetLastError());
    return TRUE;
}

FARPROC Real(const char* name)
{
    InitOnceExecuteOnce(&g_realOnce, LoadReal, nullptr, nullptr);
    FARPROC fn = g_real ? GetProcAddress(g_real, name) : nullptr;
    if (!fn) Log("real export %s missing", name);
    return fn;
}

// The built-in parts' [modules] keys, which a drop-in mod may not take.
void ReservedIds(const char** out, int max)
{
    static char ids[16][32];
    int n = 0;
    for (int i = 0; i < kSettingCount && n < max - 1 && n < 16; ++i) {
        if (wcscmp(kSettings[i].section, L"modules") != 0) continue;
        size_t k = 0;
        for (const wchar_t* p = kSettings[i].key; *p && k < 31; ++p) ids[n][k++] = static_cast<char>(*p);
        ids[n][k] = '\0';
        out[n] = ids[n];
        ++n;
    }
    out[n] = nullptr;
}

// ds3_seals.ini was the settings file of the test builds before 1.0. It is never read, written or deleted now; this
// only says so.
void NoteOldIni()
{
    wchar_t old[MAX_PATH];
    swprintf_s(old, L"%s\\ds3_seals.ini", g_moduleDir);
    if (GetFileAttributesW(old) != INVALID_FILE_ATTRIBUTES)
        Log("ds3_seals.ini is from a test build before 1.0 and is not read any more; settings are in ds3_mods.ini");
}

}  // namespace

extern "C" {

HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer)
{
    LONG call = InterlockedIncrement(&g_createCalls);
    if (call == 1) {
        const ULONGLONG began = GetTickCount64();
        Log("first DirectInput8Create (version 0x%04lX); game .text is %s", version, TextState());
        Config config = LoadConfig(g_moduleDir);
        NoteOldIni();
        // No store hooks here: the Ration Seal store is a drop-in add-on (plugins\SealStore), and the
        // store's catalogue and checkout hooks go in only when a mod asks through sealOffers (sealoffers.cpp).
        const char* reserved[17];
        ReservedIds(reserved, 17);
        DiscoverMods(g_moduleDir, config.iniPath, reserved);
        AllowPatches(std::strcmp(TextState(), "decrypted") == 0);
        game::CheckGameReads();  // before any mod can hook what they call
        LoadMods();
        // The Mods panel and DLL mods' frame callbacks draw through the overlay, so it is installed for any of them.
        bool overlay = config.modsMenu || SdkWanted();
        if (overlay) {
            const ULONGLONG spent = GetTickCount64() - began;
            if (spent < kOverlayNotBeforeMs) {
                Log("overlay: waiting %llu ms, until the game's own Direct3D start-up is over", kOverlayNotBeforeMs - spent);
                Sleep(static_cast<DWORD>(kOverlayNotBeforeMs - spent));
            }
            InstallOverlay();
        }
        InstallSdk();
        if (config.modsMenu) {
            InstallInput();
            InstallModMenu(config);
            g_watchInput = true;
        } else {
            Log("module mods_menu: off");
        }
    }

    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto fn = reinterpret_cast<Fn>(Real("DirectInput8Create"));
    HRESULT hr = fn ? fn(inst, version, riid, out, outer) : E_FAIL;
    Log("DirectInput8Create #%ld -> 0x%08lX", call, static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr) && out && g_watchInput) InputWatchDirectInput(*out);
    return hr;
}

HRESULT WINAPI Proxy_DllCanUnloadNow()
{
    auto fn = reinterpret_cast<HRESULT(WINAPI*)()>(Real("DllCanUnloadNow"));
    return fn ? fn() : S_FALSE;
}

HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out)
{
    auto fn = reinterpret_cast<HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*)>(Real("DllGetClassObject"));
    return fn ? fn(clsid, riid, out) : CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI Proxy_DllRegisterServer()
{
    auto fn = reinterpret_cast<HRESULT(WINAPI*)()>(Real("DllRegisterServer"));
    return fn ? fn() : E_FAIL;
}

HRESULT WINAPI Proxy_DllUnregisterServer()
{
    auto fn = reinterpret_cast<HRESULT(WINAPI*)()>(Real("DllUnregisterServer"));
    return fn ? fn() : E_FAIL;
}

LPCVOID WINAPI Proxy_GetdfDIJoystick()
{
    auto fn = reinterpret_cast<LPCVOID(WINAPI*)()>(Real("GetdfDIJoystick"));
    return fn ? fn() : nullptr;
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);

        DWORD len = GetModuleFileNameW(inst, g_moduleDir, MAX_PATH);
        while (len && g_moduleDir[len - 1] != L'\\') --len;
        g_moduleDir[len ? len - 1 : 0] = L'\0';
        LogInit(g_moduleDir);

        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        Log("DS3 Mod Loader v" DS3_PROXY_VERSION " attached to %ls", exe);
        Log("at load, game .text is %s", TextState());
    } else if (reason == DLL_PROCESS_DETACH) {
        StartGuardExit();  // a clean exit before the start counted as finished is not an unfinished start
        Log("detach; DirectInput8Create called %ld time(s)", g_createCalls);
    }
    return TRUE;
}
