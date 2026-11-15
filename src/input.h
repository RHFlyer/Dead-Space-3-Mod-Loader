#pragma once

#include <cstdint>

// Input for the Mods panel, and keeping it from the game while the panel is open.
//
// The game reads the keyboard and mouse through DirectInput 8 (our own DirectInput8Create) and the pad through
// xinput1_3!XInputGetState. InputWatchDirectInput hooks CreateDevice on the interface the game gets, and through it
// GetDeviceState / GetDeviceData on its devices; InstallInput hooks XInputGetState. While blocked, the game gets no
// presses, movement or pad state; key and button releases still go through, so nothing stays held.

void InstallInput();
// Call with each interface the real DirectInput8Create returns.
void InputWatchDirectInput(void* directInput);

// Also keeps window messages (key presses, mouse moves and clicks) from the game's window while blocked. Call with
// the device window; subclassed once.
void InputWatchWindow(void* window);

void InputSetBlocked(bool blocked);
// Whether the game is kept from all input now (the panel or the pause button has it). Any thread.
bool InputBlocked();

// Narrower than blocking: keeps only some presses from the game, by every route above. Down: the Down arrow, S,
// numpad 2, the d-pad and the left stick downwards; Up likewise (Up arrow, W, numpad 8). Click: the left mouse button.
enum InputMask : unsigned { kMaskDown = 1, kMaskClick = 2, kMaskUp = 4 };
void InputSetMask(unsigned mask);

struct InputFrame {
    // Held this frame. Keyboard from DirectInput and GetAsyncKeyState, so it works whichever the game left usable.
    bool keys[256];  // by virtual-key code
    uint16_t pad;    // XInput wButtons, sticks folded into the d-pad bits
    bool padTrigger; // either trigger pulled (not in wButtons)
    bool mouseLeft;
    long mouseDx, mouseDy, wheel;  // DirectInput counts since the last frame (wheel: 120 per notch)
};
// Collects the state since the last call. Render thread only.
void InputPoll(InputFrame* out);
