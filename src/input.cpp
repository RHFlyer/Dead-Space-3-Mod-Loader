// Input for the Mods panel. See input.h.

#include "input.h"

#include "game.h"
#include "log.h"

#include <windows.h>
#include <unknwn.h>

#include <cstring>

namespace {

// IDirectInput8 vtable: 3 CreateDevice. IDirectInputDevice8: 9 GetDeviceState, 10 GetDeviceData. Same slots for the
// A and W interfaces.
constexpr int kCreateDeviceSlot = 3;
constexpr int kGetDeviceStateSlot = 9;
constexpr int kGetDeviceDataSlot = 10;
// GUID_SysMouse / GUID_SysKeyboard, written out so no dxguid.lib is needed.
constexpr GUID kSysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr GUID kSysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
// DIMOUSESTATE offsets: lX 0, lY 4, lZ 8, buttons from 12.
constexpr DWORD kMouseX = 0, kMouseY = 4, kMouseZ = 8, kMouseButton0 = 12;
constexpr DWORD kPeek = 1;  // DIGDD_PEEK

struct DiObjectData {  // DIDEVICEOBJECTDATA (DirectInput 8, 32-bit)
    DWORD ofs;
    DWORD data;
    DWORD timeStamp;
    DWORD sequence;
    UINT_PTR appData;
};

struct XInputGamepad {
    WORD buttons;
    BYTE leftTrigger, rightTrigger;
    SHORT thumbLX, thumbLY, thumbRX, thumbRY;
};
struct XInputState {
    DWORD packetNumber;
    XInputGamepad gamepad;
};
constexpr WORD kPadUp = 0x1, kPadDown = 0x2, kPadLeft = 0x4, kPadRight = 0x8;
constexpr SHORT kStickThreshold = 16000;
constexpr BYTE kTriggerThreshold = 30;  // XINPUT_GAMEPAD_TRIGGER_THRESHOLD

using CreateDeviceFn = HRESULT(__stdcall*)(void* self, const GUID& guid, void** device, IUnknown* outer);
using GetDeviceStateFn = HRESULT(__stdcall*)(void* self, DWORD size, void* data);
using GetDeviceDataFn = HRESULT(__stdcall*)(void* self, DWORD objectSize, DiObjectData* data, DWORD* count,
                                            DWORD flags);
using XInputGetStateFn = DWORD(WINAPI*)(DWORD user, XInputState* state);

enum class Kind { Other, Keyboard, Mouse };

// Hooked once per distinct function: the A and W interfaces may or may not share code.
constexpr int kMaxHooks = 4;
struct HookSet {
    void* target[kMaxHooks];
    void* orig[kMaxHooks];
    int count;
};
HookSet g_createHooks = {}, g_stateHooks = {}, g_dataHooks = {};
SRWLOCK g_hookLock = SRWLOCK_INIT;

constexpr int kMaxDevices = 16;
void* g_devices[kMaxDevices] = {};  // all three under g_deviceLock (shared to read, exclusive to write)
Kind g_kinds[kMaxDevices] = {};
LONG g_deviceCount = 0;
LONG g_nextEvict = 0;  // when the table is full, the slot a new device replaces (round robin)
SRWLOCK g_deviceLock = SRWLOCK_INIT;
volatile LONG g_unseenLogs = 0;  // "not seen at creation" lines so far; capped so a busy table can't flood the log
constexpr LONG kMaxUnseenLogs = 16;
constexpr int kGetCapabilitiesSlot = 3;
struct DiDevCaps {  // DIDEVCAPS
    DWORD size, flags, devType, axes, buttons, povs, ffSamplePeriod, ffMinTimeResolution, firmwareRevision,
        hardwareRevision, ffDriverVersion;
};
using GetCapabilitiesFn = HRESULT(__stdcall*)(void* self, DiDevCaps* caps);

XInputGetStateFn g_xinput = nullptr;  // the original, for our own reads too

volatile LONG g_blocked = 0;
volatile LONG g_mask = 0;  // InputMask bits
// The keys kMaskDown / kMaskUp keep from the game: DIK_DOWN, DIK_S, DIK_NUMPAD2 / DIK_UP, DIK_W, DIK_NUMPAD8.
constexpr BYTE kDownKeys[] = {0xD0, 0x1F, 0x50};
constexpr BYTE kUpKeys[] = {0xC8, 0x11, 0x48};
volatile LONG g_mouseDx = 0, g_mouseDy = 0, g_wheel = 0;
volatile LONG g_mouseLeft = 0, g_mouseLeftLatch = 0;
volatile BYTE g_diKeys[256] = {};   // DirectInput key held, by DIK code
volatile BYTE g_diLatch[256] = {};  // went down since the last poll, so a tap between two frames is not lost
BYTE g_dikToVk[256] = {};

void* OrigFor(const HookSet& set, void* self, int slot)
{
    void* fn = (*reinterpret_cast<void***>(self))[slot];
    AcquireSRWLockShared(&g_hookLock);
    void* orig = nullptr;
    for (int i = 0; i < set.count; ++i) {
        if (set.target[i] == fn) orig = set.orig[i];
    }
    ReleaseSRWLockShared(&g_hookLock);
    return orig ? orig : fn;  // only reached through our detour, so fn is hooked; the fallback is never expected
}

void HookSlot(HookSet& set, void* object, int slot, void* detour, const char* name)
{
    void* fn = (*reinterpret_cast<void***>(object))[slot];
    AcquireSRWLockExclusive(&g_hookLock);
    bool known = false;
    for (int i = 0; i < set.count; ++i) known = known || set.target[i] == fn;
    if (!known && set.count < kMaxHooks) {
        void* orig = nullptr;
        if (game::HookChecked(name, reinterpret_cast<uintptr_t>(fn), nullptr, 0, detour, &orig)) {
            set.target[set.count] = fn;
            set.orig[set.count] = orig;
            ++set.count;
            Log("input: hooked %s @ %p", name, fn);
        }
    }
    ReleaseSRWLockExclusive(&g_hookLock);
}

// Sets the kind of a device address, adding it if new. A known address takes the new kind: the game may release a
// device and get the next one at the same address (seen: a keyboard, then the mouse there).
void Remember(void* device, Kind kind)
{
    AcquireSRWLockExclusive(&g_deviceLock);
    LONG i = 0;
    while (i < g_deviceCount && g_devices[i] != device) ++i;
    if (i == g_deviceCount) {
        if (g_deviceCount < kMaxDevices) {
            ++g_deviceCount;
        } else {
            // Full: replace an old entry. If that device is still in use it is simply asked its type again.
            i = g_nextEvict;
            g_nextEvict = (g_nextEvict + 1) % kMaxDevices;
        }
        g_devices[i] = device;
    }
    g_kinds[i] = kind;
    ReleaseSRWLockExclusive(&g_deviceLock);
}

Kind KindOf(void* device)
{
    AcquireSRWLockShared(&g_deviceLock);
    for (LONG i = 0; i < g_deviceCount; ++i) {
        if (g_devices[i] == device) {
            Kind kind = g_kinds[i];
            ReleaseSRWLockShared(&g_deviceLock);
            return kind;
        }
    }
    ReleaseSRWLockShared(&g_deviceLock);  // not held across GetCapabilities
    // Not made through a hooked CreateDevice: e.g. the mouse was created before the hook went in, and reaches the
    // (shared) GetDeviceState hook only because a later device installed it. Ask the device what it is, once.
    DiDevCaps caps{};
    caps.size = sizeof(caps);
    auto vtable = *static_cast<void***>(device);
    auto getCaps = reinterpret_cast<GetCapabilitiesFn>(vtable[kGetCapabilitiesSlot]);
    Kind kind = Kind::Other;
    if (SUCCEEDED(getCaps(device, &caps))) {
        BYTE type = caps.devType & 0xFF;  // DI8DEVTYPE_MOUSE 0x12, DI8DEVTYPE_KEYBOARD 0x13
        kind = type == 0x12 ? Kind::Mouse : type == 0x13 ? Kind::Keyboard : Kind::Other;
    }
    Remember(device, kind);
    LONG logs = InterlockedIncrement(&g_unseenLogs);
    if (logs <= kMaxUnseenLogs)
        Log("input: DirectInput device %p not seen at creation; it says it is a %s%s", device,
            kind == Kind::Keyboard ? "keyboard" : kind == Kind::Mouse ? "mouse" : "other device",
            logs == kMaxUnseenLogs ? " (no more of these lines)" : "");
    return kind;
}

void SeeKey(DWORD dik, bool down)
{
    dik &= 0xFF;
    g_diKeys[dik] = down ? 1 : 0;
    if (down) g_diLatch[dik] = 1;
}

void SeeMouseButton(bool down)
{
    InterlockedExchange(&g_mouseLeft, down ? 1 : 0);
    if (down) InterlockedExchange(&g_mouseLeftLatch, 1);
}

template <size_t N>
bool IsIn(const BYTE (&keys)[N], DWORD dik)
{
    for (BYTE k : keys) {
        if (dik == k) return true;
    }
    return false;
}

// Whether a press (DirectInput offset on a device of this kind) is kept from the game by the mask.
bool Masked(Kind kind, DWORD ofs)
{
    LONG mask = g_mask;
    if (kind == Kind::Keyboard)
        return ((mask & kMaskDown) && IsIn(kDownKeys, ofs)) || ((mask & kMaskUp) && IsIn(kUpKeys, ofs));
    if (kind == Kind::Mouse) return (mask & kMaskClick) && ofs == kMouseButton0;
    return false;
}

HRESULT __stdcall HookGetDeviceState(void* self, DWORD size, void* data)
{
    auto orig = reinterpret_cast<GetDeviceStateFn>(OrigFor(g_stateHooks, self, kGetDeviceStateSlot));
    HRESULT hr = orig(self, size, data);
    if (FAILED(hr) || !data) return hr;
    Kind kind = KindOf(self);
    if (kind == Kind::Keyboard && size == 256) {
        const auto* keys = static_cast<const BYTE*>(data);
        for (DWORD i = 0; i < 256; ++i) SeeKey(i, (keys[i] & 0x80) != 0);
    } else if (kind == Kind::Mouse && size >= 16) {
        const auto* m = static_cast<const LONG*>(data);
        InterlockedExchangeAdd(&g_mouseDx, m[0]);
        InterlockedExchangeAdd(&g_mouseDy, m[1]);
        InterlockedExchangeAdd(&g_wheel, m[2]);
        SeeMouseButton((static_cast<const BYTE*>(data)[kMouseButton0] & 0x80) != 0);
    }
    if (g_blocked) {
        std::memset(data, 0, size);  // all released, no movement
    } else if (kind == Kind::Keyboard && size == 256) {
        for (BYTE k : kDownKeys) {
            if (Masked(kind, k)) static_cast<BYTE*>(data)[k] = 0;
        }
        for (BYTE k : kUpKeys) {
            if (Masked(kind, k)) static_cast<BYTE*>(data)[k] = 0;
        }
    } else if (kind == Kind::Mouse && size >= 16 && Masked(kind, kMouseButton0)) {
        static_cast<BYTE*>(data)[kMouseButton0] = 0;
    }
    return hr;
}

HRESULT __stdcall HookGetDeviceData(void* self, DWORD objectSize, DiObjectData* data, DWORD* count, DWORD flags)
{
    auto orig = reinterpret_cast<GetDeviceDataFn>(OrigFor(g_dataHooks, self, kGetDeviceDataSlot));
    HRESULT hr = orig(self, objectSize, data, count, flags);
    if (FAILED(hr) || !data || !count || objectSize < sizeof(DWORD) * 2) return hr;
    if (flags & kPeek) {  // left in the buffer: seen when it is read for real
        // Not filtered: a filtered peek would disagree with the queue the game then consumes, and could
        // make it handle a press twice. Logged once, to learn whether DS3 peeks at all.
        static volatile LONG peekLogged = 0;
        if (!InterlockedExchange(&peekLogged, 1)) Log("input: the game peeks DirectInput events (DIGDD_PEEK)");
        if (g_blocked) *count = 0;
        return hr;
    }
    Kind kind = KindOf(self);
    auto* bytes = reinterpret_cast<BYTE*>(data);
    DWORD kept = 0;
    for (DWORD i = 0; i < *count; ++i) {
        auto* e = reinterpret_cast<DiObjectData*>(bytes + i * objectSize);
        bool press = (e->data & 0x80) != 0;
        bool keep = !press;  // releases always reach the game
        if (kind == Kind::Keyboard) {
            SeeKey(e->ofs, press);
        } else if (kind == Kind::Mouse) {
            LONG v = static_cast<LONG>(e->data);
            if (e->ofs == kMouseX) InterlockedExchangeAdd(&g_mouseDx, v), keep = false;
            if (e->ofs == kMouseY) InterlockedExchangeAdd(&g_mouseDy, v), keep = false;
            if (e->ofs == kMouseZ) InterlockedExchangeAdd(&g_wheel, v), keep = false;
            if (e->ofs == kMouseButton0) SeeMouseButton(press);
        } else {
            keep = false;
        }
        if ((!g_blocked && !(press && Masked(kind, e->ofs))) || keep) {
            if (kept != i) std::memmove(bytes + kept * objectSize, e, objectSize);
            ++kept;
        }
    }
    *count = kept;
    return hr;
}

HRESULT __stdcall HookCreateDevice(void* self, const GUID& guid, void** device, IUnknown* outer)
{
    auto orig = reinterpret_cast<CreateDeviceFn>(OrigFor(g_createHooks, self, kCreateDeviceSlot));
    HRESULT hr = orig(self, guid, device, outer);
    if (FAILED(hr) || !device || !*device) return hr;
    Kind kind = guid == kSysKeyboard ? Kind::Keyboard : guid == kSysMouse ? Kind::Mouse : Kind::Other;
    // A reused address takes the new kind, else the mouse would be read as a keyboard: no wheel, no click.
    Remember(*device, kind);
    Log("input: game created DirectInput device %p (%s)", *device,
        kind == Kind::Keyboard ? "keyboard" : kind == Kind::Mouse ? "mouse" : "other");
    HookSlot(g_stateHooks, *device, kGetDeviceStateSlot, reinterpret_cast<void*>(&HookGetDeviceState),
             "GetDeviceState");
    HookSlot(g_dataHooks, *device, kGetDeviceDataSlot, reinterpret_cast<void*>(&HookGetDeviceData), "GetDeviceData");
    return hr;
}

HWND g_window = nullptr;
WNDPROC g_windowProc = nullptr;

// Presses, moves and clicks are dropped while blocked; releases and everything else (WM_INPUT included, which the
// mouse fix reads) go through.
LRESULT CALLBACK FilterWindowProc(HWND window, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_blocked) {
        switch (msg) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_CHAR:
        case WM_SYSCHAR:
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            return 0;
        default:
            break;
        }
    } else if (LONG mask = g_mask) {
        if ((mask & kMaskDown) && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) &&
            (wp == VK_DOWN || wp == 'S' || wp == VK_NUMPAD2))
            return 0;
        if ((mask & kMaskUp) && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) &&
            (wp == VK_UP || wp == 'W' || wp == VK_NUMPAD8))
            return 0;
        if ((mask & kMaskClick) && (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK)) return 0;
    }
    return CallWindowProcW(g_windowProc, window, msg, wp, lp);
}

DWORD WINAPI HookXInputGetState(DWORD user, XInputState* state)
{
    DWORD r = g_xinput(user, state);
    if (r != ERROR_SUCCESS || !state) return r;
    if (g_blocked) {
        std::memset(&state->gamepad, 0, sizeof(state->gamepad));
    } else if (LONG mask = g_mask) {
        if (mask & kMaskDown) {
            state->gamepad.buttons &= ~kPadDown;
            if (state->gamepad.thumbLY < 0) state->gamepad.thumbLY = 0;
        }
        if (mask & kMaskUp) {
            state->gamepad.buttons &= ~kPadUp;
            if (state->gamepad.thumbLY > 0) state->gamepad.thumbLY = 0;
        }
    }
    return r;
}

}  // namespace

void InstallInput()
{
    for (int dik = 0; dik < 256; ++dik) {
        UINT scan = (dik & 0x80) ? (0xE000u | (dik & 0x7F)) : static_cast<UINT>(dik);
        g_dikToVk[dik] = static_cast<BYTE>(MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX));
    }
    if (!game::InitMinHook()) return;
    HMODULE xinput = GetModuleHandleW(L"xinput1_3.dll");
    if (!xinput) xinput = LoadLibraryW(L"xinput1_3.dll");
    auto fn = xinput ? reinterpret_cast<uintptr_t>(GetProcAddress(xinput, "XInputGetState")) : 0;
    if (!fn) {
        Log("input: xinput1_3!XInputGetState not found; the pad cannot be used in the panel");
        return;
    }
    if (game::HookChecked("XInputGetState", fn, nullptr, 0, reinterpret_cast<void*>(&HookXInputGetState),
                          reinterpret_cast<void**>(&g_xinput)))
        Log("input: hooked XInputGetState @ %08X", static_cast<unsigned>(fn));
    else
        g_xinput = reinterpret_cast<XInputGetStateFn>(fn);  // still readable for the panel, just not blockable
}

void InputWatchDirectInput(void* directInput)
{
    if (directInput)
        HookSlot(g_createHooks, directInput, kCreateDeviceSlot, reinterpret_cast<void*>(&HookCreateDevice),
                 "CreateDevice");
}

void InputWatchWindow(void* window)
{
    auto hwnd = static_cast<HWND>(window);
    if (!hwnd || hwnd == g_window) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return;
    if (g_window) return;  // one window only; a second device window is not expected
    g_window = hwnd;
    // Set before the swap: the game thread may deliver a message as soon as the new proc is in.
    g_windowProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
    auto prior = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&FilterWindowProc)));
    if (prior) g_windowProc = prior;
    Log("input: filtering window %p messages (previous proc %p)", hwnd, g_windowProc);
}

void InputSetBlocked(bool blocked)
{
    InterlockedExchange(&g_blocked, blocked ? 1 : 0);
}

bool InputBlocked()
{
    return g_blocked != 0;
}

void InputSetMask(unsigned mask)
{
    InterlockedExchange(&g_mask, static_cast<LONG>(mask));
}

void InputPoll(InputFrame* out)
{
    std::memset(out, 0, sizeof(*out));
    for (int vk = 1; vk < 256; ++vk) out->keys[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
    for (int dik = 0; dik < 256; ++dik) {
        bool down = g_diKeys[dik] || g_diLatch[dik];
        g_diLatch[dik] = 0;
        if (down && g_dikToVk[dik]) out->keys[g_dikToVk[dik]] = true;
    }
    out->mouseLeft = out->keys[VK_LBUTTON] || InterlockedExchange(&g_mouseLeftLatch, 0) || g_mouseLeft;
    out->mouseDx = InterlockedExchange(&g_mouseDx, 0);
    out->mouseDy = InterlockedExchange(&g_mouseDy, 0);
    out->wheel = InterlockedExchange(&g_wheel, 0);
    if (g_xinput) {
        // Asking about an unplugged pad is slow, so those are only asked again once a second.
        static ULONGLONG retryAt[4] = {};
        ULONGLONG now = GetTickCount64();
        for (DWORD user = 0; user < 4; ++user) {
            if (now < retryAt[user]) continue;
            XInputState s{};
            if (g_xinput(user, &s) != ERROR_SUCCESS) {
                retryAt[user] = now + 1000;
                continue;
            }
            WORD b = s.gamepad.buttons;
            if (s.gamepad.thumbLY > kStickThreshold) b |= kPadUp;
            if (s.gamepad.thumbLY < -kStickThreshold) b |= kPadDown;
            if (s.gamepad.thumbLX < -kStickThreshold) b |= kPadLeft;
            if (s.gamepad.thumbLX > kStickThreshold) b |= kPadRight;
            out->pad |= b;
            if (s.gamepad.leftTrigger > kTriggerThreshold || s.gamepad.rightTrigger > kTriggerThreshold)
                out->padTrigger = true;
        }
    }
}
