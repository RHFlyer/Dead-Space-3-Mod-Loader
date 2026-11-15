#pragma once

// Draws the mod's own text over the game: hooks IDirect3DDevice9::Present (the device vtable is read from a throwaway
// NULLREF device, which never touches the GPU) and draws with GDI-built glyph textures, saving and restoring the
// game's render state around it. It shows what drop-in mods draw, and the Mods panel.
void InstallOverlay();

// What the panel draws with, valid only inside its draw callback. Colours are 0xAARRGGBB; text is printable ASCII.
class OverlayCanvas {
public:
    virtual float Width() const = 0;  // back buffer, in pixels
    virtual float Height() const = 0;
    virtual float LineHeight() const = 0;
    virtual void Rect(float x, float y, float w, float h, unsigned color) = 0;
    virtual float Text(float x, float y, const char* s, unsigned color) = 0;  // returns the pen position after s
    virtual float TextWidth(const char* s) const = 0;

protected:
    ~OverlayCanvas() = default;
};

// Called on the render thread every frame. update(width, height, window, &textPx) runs first, with the back buffer
// size and the device's window, and returns whether to draw; then draw(canvas) with a font of textPx pixels.
struct OverlayPanel {
    bool (*update)(float width, float height, void* window, int* textPx);
    void (*draw)(OverlayCanvas& canvas);
};
void OverlaySetPanel(const OverlayPanel& panel);

// What drop-in mods draw with (sdk.cpp): the panel's canvas plus fonts and gradients. It records; the overlay draws
// everything after the frame callbacks, under the panel. Valid only inside the frame callback below.
class OverlayModCanvas : public OverlayCanvas {
public:
    // Sets the text size for what follows (8 to 160 px), keeping the face, and returns the line height.
    virtual float SetTextSize(float px) = 0;
    // Sets the face (a Windows font family name; null or "" = the default face) and the size. Up to 8 fonts (a face at
    // a size) per frame, over all mods. Returns the line height.
    virtual float SetFont(const char* face, float px) = 0;
    // A box whose colour runs from `from` (left, or top if vertical) to `to`.
    virtual void Gradient(float x, float y, float w, float h, unsigned from, unsigned to, bool vertical) = 0;
    // Back to the default face and size (about height / 54), for the next mod.
    virtual void ResetTextSize() = 0;

protected:
    ~OverlayModCanvas() = default;
};
// Called on the render thread at every Present while the overlay is installed, before the panel.
void OverlaySetModFrame(void (*frame)(OverlayModCanvas& canvas));
// The loader's own notices, drawn on the same canvas right after the mods' callbacks.
void OverlaySetBuiltinFrame(void (*frame)(OverlayModCanvas& canvas));
bool OverlayInstalled();
