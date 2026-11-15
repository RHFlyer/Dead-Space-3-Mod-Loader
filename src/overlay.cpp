// Overlay: what drop-in mods draw, and the Mods panel. See overlay.h.

#include "overlay.h"

#include "game.h"
#include "log.h"

#include <windows.h>

#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

constexpr int kPresentSlot = 17;  // IDirect3DDevice9::Present
OverlayPanel g_panel = {};

using PresentFn = HRESULT(__stdcall*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
PresentFn g_origPresent = nullptr;
bool g_presentSeen = false;

struct Glyph {
    short x, y, w;   // cell in the atlas
    short offset;    // from the pen position to the cell's left edge
    short advance;
};

struct Font {
    const char* name = "";
    const wchar_t* face = L"Segoe UI";  // falls back to Segoe UI Semibold if Windows does not have it
    int weight = FW_SEMIBOLD;
    IDirect3DDevice9* device = nullptr;
    IDirect3DTexture9* texture = nullptr;
    int atlas = 512;
    int px = 0;
    int lineHeight = 0;
    Glyph glyphs[128] = {};
    bool failed = false;  // building it failed for this size; not retried every frame
    wchar_t asked[LF_FACESIZE] = L"";  // a mod's font: the face it asked for ("" = the default face)
};
Font g_panelFont = {"panel"};

struct Vertex {
    float x, y, z, rhw;
    D3DCOLOR color;
    float u, v;
};
constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

void ReleaseFont(Font& font)
{
    if (font.texture) font.texture->Release();
    font.texture = nullptr;
    font.device = nullptr;
}

HFONT CreateFace(int px, const wchar_t* face, int weight)
{
    return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
}

// Renders printable ASCII with GDI (the font's face, greyscale anti-aliasing) and copies the coverage into an
// A8R8G8B8 texture as white with alpha. A 4x4 white block at (0,0) is used for solid quads.
bool BuildFont(Font& f, IDirect3DDevice9* device, int px)
{
    ReleaseFont(f);
    f.px = px;
    f.failed = true;
    // 95 glyphs: 1024 holds up to ~150 px (packing failed at ~159 px, 4K at text size 250%).
    f.atlas = px > 110 ? 2048 : px > 26 ? 1024 : 512;
    const int size = f.atlas;

    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    HFONT font = CreateFace(px, f.face, f.weight);
    wchar_t used[LF_FACESIZE] = L"";
    bool ok = dc && bmp && font;
    if (ok) {
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        HGDIOBJ oldFont = SelectObject(dc, font);
        GetTextFaceW(dc, LF_FACESIZE, used);
        if (_wcsicmp(used, f.face) != 0) {  // GDI substituted another face: use ours
            SelectObject(dc, oldFont);
            DeleteObject(font);
            font = CreateFace(px, L"Segoe UI", FW_SEMIBOLD);
            ok = font != nullptr;
            oldFont = SelectObject(dc, font ? static_cast<HGDIOBJ>(font) : GetStockObject(DEFAULT_GUI_FONT));
            GetTextFaceW(dc, LF_FACESIZE, used);
        }
        SetTextColor(dc, RGB(255, 255, 255));
        SetBkMode(dc, TRANSPARENT);
        TEXTMETRICW tm{};
        GetTextMetricsW(dc, &tm);
        f.lineHeight = tm.tmHeight;

        int x = 6, y = 0;
        for (int c = 32; c < 127 && ok; ++c) {
            ABC abc{};
            if (!GetCharABCWidthsW(dc, c, c, &abc)) {
                SIZE s{};
                wchar_t ch = static_cast<wchar_t>(c);
                GetTextExtentPoint32W(dc, &ch, 1, &s);
                abc.abcA = 0;
                abc.abcB = static_cast<UINT>(s.cx);
                abc.abcC = 0;
            }
            int w = static_cast<int>(abc.abcB) + 2;
            if (x + w > size) {
                x = 0;
                y += f.lineHeight + 1;
            }
            if (y + f.lineHeight > size) {
                ok = false;
                break;
            }
            wchar_t ch = static_cast<wchar_t>(c);
            TextOutW(dc, x + 1 - abc.abcA, y, &ch, 1);
            f.glyphs[c] = {static_cast<short>(x), static_cast<short>(y), static_cast<short>(w),
                           static_cast<short>(abc.abcA - 1),
                           static_cast<short>(abc.abcA + static_cast<int>(abc.abcB) + abc.abcC)};
            x += w + 1;
        }
        GdiFlush();
        SelectObject(dc, oldFont);
        SelectObject(dc, oldBmp);
    }

    IDirect3DTexture9* tex = nullptr;
    if (ok &&
        SUCCEEDED(device->CreateTexture(size, size, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr))) {
        D3DLOCKED_RECT lr{};
        if (SUCCEEDED(tex->LockRect(0, &lr, nullptr, 0))) {
            const auto* src = static_cast<const uint32_t*>(bits);
            for (int row = 0; row < size; ++row) {
                auto* dst = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + row * lr.Pitch);
                for (int col = 0; col < size; ++col) {
                    uint32_t a = (row < 4 && col < 4) ? 255 : (src[row * size + col] >> 16 & 0xFF);
                    dst[col] = a << 24 | 0x00FFFFFF;
                }
            }
            tex->UnlockRect(0);
            f.texture = tex;
            f.device = device;
            f.failed = false;
        } else {
            tex->Release();
        }
    }

    if (font) DeleteObject(font);
    if (bmp) DeleteObject(bmp);
    if (dc) DeleteDC(dc);
    Log("overlay: %s font %ls %dpx (line %d) %s", f.name, used, px, f.lineHeight, f.failed ? "FAILED" : "built");
    return !f.failed;
}

// Builds the font for this device and size if needed; false if there is none to draw with.
bool EnsureFont(Font& f, IDirect3DDevice9* device, int px)
{
    if (f.device != device || f.px != px) {
        if (!(f.failed && f.px == px)) BuildFont(f, device, px);
    }
    return f.texture != nullptr;
}

int TextWidth(const Font& f, const char* s)
{
    int w = 0;
    for (; *s; ++s) {
        unsigned char c = static_cast<unsigned char>(*s);
        w += f.glyphs[(c >= 32 && c < 127) ? c : '?'].advance;
    }
    return w;
}

class Batch {
public:
    void Begin(const Font& font)
    {
        font_ = &font;
        count_ = 0;
        segments_ = 0;
        segmentStart_ = 0;
    }

    // Draws what follows with another font (drop-in mods mix text sizes). Each font is one draw call.
    void SetFont(const Font& font)
    {
        if (&font == font_) return;
        if (count_ > segmentStart_) {
            if (segments_ == kMaxSegments) return;  // out of draw calls: keep the font
            segment_[segments_++] = {font_, segmentStart_, count_ - segmentStart_};
            segmentStart_ = count_;
        }
        font_ = &font;
    }

    bool Empty() const { return count_ == 0; }
    const Font* CurrentFont() const { return font_; }

    void Quad(float x, float y, float w, float h, float u0, float v0, float u1, float v1, D3DCOLOR color,
              D3DCOLOR right = 0, bool gradient = false)
    {
        if (count_ + 6 > kMax) return;
        if (!(std::isfinite(x) && std::isfinite(y) && std::isfinite(w) && std::isfinite(h))) return;  // a mod's NaN
        x -= 0.5f;
        y -= 0.5f;
        const D3DCOLOR r = gradient ? right : color;
        const Vertex a{x, y, 0, 1, color, u0, v0}, b{x + w, y, 0, 1, r, u1, v0};
        const Vertex c{x, y + h, 0, 1, color, u0, v1}, d{x + w, y + h, 0, 1, r, u1, v1};
        Vertex* v = verts_ + count_;
        v[0] = a, v[1] = b, v[2] = c, v[3] = b, v[4] = d, v[5] = c;
        count_ += 6;
    }

    void Rect(float x, float y, float w, float h, D3DCOLOR color)
    {
        const float t = 2.0f / font_->atlas;  // middle of the white block
        Quad(x, y, w, h, t, t, t, t, color);
    }

    // A solid quad whose colour runs from left to right.
    void RectH(float x, float y, float w, float h, D3DCOLOR left, D3DCOLOR right)
    {
        const float t = 2.0f / font_->atlas;
        Quad(x, y, w, h, t, t, t, t, left, right, true);
    }

    // ... or from top to bottom.
    void RectV(float x, float y, float w, float h, D3DCOLOR top, D3DCOLOR bottom)
    {
        if (count_ + 6 > kMax) return;
        if (!(std::isfinite(x) && std::isfinite(y) && std::isfinite(w) && std::isfinite(h))) return;  // a mod's NaN
        const float t = 2.0f / font_->atlas;
        x -= 0.5f;
        y -= 0.5f;
        const Vertex a{x, y, 0, 1, top, t, t}, b{x + w, y, 0, 1, top, t, t};
        const Vertex c{x, y + h, 0, 1, bottom, t, t}, d{x + w, y + h, 0, 1, bottom, t, t};
        Vertex* v = verts_ + count_;
        v[0] = a, v[1] = b, v[2] = c, v[3] = b, v[4] = d, v[5] = c;
        count_ += 6;
    }

    // Draws s with its left edge at x; returns the pen position after it.
    float Text(float x, float y, const char* s, D3DCOLOR color)
    {
        const float inv = 1.0f / font_->atlas;
        const float lh = static_cast<float>(font_->lineHeight);
        for (; *s; ++s) {
            unsigned char c = static_cast<unsigned char>(*s);
            const Glyph& g = font_->glyphs[(c >= 32 && c < 127) ? c : '?'];
            if (c != ' ') {
                Quad(x + g.offset, y, g.w, lh, g.x * inv, g.y * inv, (g.x + g.w) * inv, (g.y + font_->lineHeight) * inv,
                     color);
            }
            x += g.advance;
        }
        return x;
    }

    void Draw(IDirect3DDevice9* device)
    {
        for (int i = 0; i < segments_; ++i) DrawRange(device, segment_[i].font, segment_[i].first, segment_[i].count);
        DrawRange(device, font_, segmentStart_, count_ - segmentStart_);
    }

private:
    struct Segment {
        const Font* font;
        int first, count;
    };
    static constexpr int kMax = 6 * 8192;  // 8192 boxes and letters per frame, over all mods
    static constexpr int kMaxSegments = 32;

    void DrawRange(IDirect3DDevice9* device, const Font* font, int first, int count)
    {
        if (count <= 0 || !font || !font->texture) return;
        device->SetTexture(0, font->texture);
        device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, count / 3, verts_ + first, sizeof(Vertex));
    }

    Vertex verts_[kMax];
    int count_ = 0;
    const Font* font_ = nullptr;
    Segment segment_[kMaxSegments] = {};
    int segments_ = 0;
    int segmentStart_ = 0;  // first vertex drawn with font_
};

Batch g_batch;  // only used on the render thread

class Canvas final : public OverlayCanvas {
public:
    Canvas(Batch& batch, const Font& font, float width, float height)
        : batch_(batch), font_(font), width_(width), height_(height)
    {
    }
    float Width() const override { return width_; }
    float Height() const override { return height_; }
    float LineHeight() const override { return static_cast<float>(font_.lineHeight); }
    void Rect(float x, float y, float w, float h, unsigned color) override { batch_.Rect(x, y, w, h, color); }
    float Text(float x, float y, const char* s, unsigned color) override { return batch_.Text(x, y, s, color); }
    float TextWidth(const char* s) const override { return static_cast<float>(::TextWidth(font_, s)); }

private:
    Batch& batch_;
    const Font& font_;
    float width_, height_;
};

// ---- Drop-in mods' drawing (sdk.cpp) ----

// Up to eight fonts (a face at a size), kept between frames so the mods' usual ones are built once.
constexpr int kModFonts = 8;
Font g_modFonts[kModFonts] = {{"mod"}, {"mod"}, {"mod"}, {"mod"}, {"mod"}, {"mod"}, {"mod"}, {"mod"}};
unsigned g_modFontFrame[kModFonts] = {};  // the frame each was last used in
unsigned g_frameNo = 0;
void (*g_modFrame)(OverlayModCanvas& canvas) = nullptr;
void (*g_builtinFrame)(OverlayModCanvas& canvas) = nullptr;  // the loader's own notices

// Records the mods' text and boxes into the batch; DrawOverlay draws them under the panel.
class ModCanvas final : public OverlayModCanvas {
public:
    ModCanvas(Batch& batch, IDirect3DDevice9* device, float width, float height)
        : batch_(batch), device_(device), width_(width), height_(height)
    {
        defaultPx_ = std::max(12.0f, std::round(height / 54.0f));
    }
    void ResetTextSize() override
    {
        font_ = nullptr;
        face_[0] = L'\0';
    }

    float Width() const override { return width_; }
    float Height() const override { return height_; }
    float LineHeight() const override { return font_ ? static_cast<float>(font_->lineHeight) : 0.0f; }
    void Rect(float x, float y, float w, float h, unsigned color) override
    {
        if (Ready()) batch_.Rect(x, y, w, h, color);
    }
    float Text(float x, float y, const char* s, unsigned color) override
    {
        return Ready() && s ? batch_.Text(x, y, s, color) : x;
    }
    float TextWidth(const char* s) const override
    {
        // Measuring before drawing anything picks the default size, as drawing would.
        if (!const_cast<ModCanvas*>(this)->Ready() || !s) return 0.0f;
        return static_cast<float>(::TextWidth(*font_, s));
    }
    void Gradient(float x, float y, float w, float h, unsigned from, unsigned to, bool vertical) override
    {
        if (!Ready()) return;
        if (vertical)
            batch_.RectV(x, y, w, h, from, to);
        else
            batch_.RectH(x, y, w, h, from, to);
    }
    float SetTextSize(float px) override
    {
        if (!std::isfinite(px)) px = defaultPx_;
        font_ = Pick(static_cast<int>(std::clamp(std::round(px), 8.0f, 160.0f)));
        // Measure with the font that is really drawn: after kMaxSegments switches the batch keeps its current font.
        if (font_) {
            batch_.SetFont(*font_);
            font_ = const_cast<Font*>(batch_.CurrentFont());
        }
        return LineHeight();
    }
    float SetFont(const char* face, float px) override
    {
        // A face name is short printable ASCII; anything else means the default face.
        size_t n = 0;
        bool ok = face != nullptr;
        for (; ok && face[n]; ++n) ok = n + 1 < LF_FACESIZE && face[n] >= 0x20 && face[n] < 0x7F;
        if (!ok) n = 0;
        for (size_t i = 0; i < n; ++i) face_[i] = static_cast<wchar_t>(face[i]);
        face_[n] = L'\0';
        return SetTextSize(px);
    }

private:
    bool Ready()
    {
        if (!font_) SetTextSize(defaultPx_);
        return font_ != nullptr;
    }
    // The current face at this size: a font already built, else a slot not used this frame (oldest first), else the
    // nearest one built this frame (the mods asked for more fonts than there are slots), the same face if there is one.
    Font* Pick(int px)
    {
        int free = -1, nearest = -1;
        for (int i = 0; i < kModFonts; ++i) {
            Font& f = g_modFonts[i];
            const bool sameFace = _wcsicmp(f.asked, face_) == 0;
            if (sameFace && f.px == px && ((f.texture && f.device == device_) || (!f.texture && f.failed))) {
                g_modFontFrame[i] = g_frameNo;
                return f.texture ? &f : nullptr;
            }
            if (g_modFontFrame[i] != g_frameNo || !f.texture) {
                if (free < 0 || g_modFontFrame[i] < g_modFontFrame[free]) free = i;
            } else if (nearest < 0 || Distance(f, px) < Distance(g_modFonts[nearest], px)) {
                nearest = i;
            }
        }
        if (free >= 0) {
            Font& f = g_modFonts[free];
            g_modFontFrame[free] = g_frameNo;
            if (_wcsicmp(f.asked, face_) != 0) {
                // Another face in this slot: built anew. A named face is drawn at normal weight; if Windows does not
                // have it, BuildFont falls back to the default face.
                ReleaseFont(f);
                wcscpy_s(f.asked, face_);
                f.face = f.asked[0] ? f.asked : L"Segoe UI";
                f.weight = f.asked[0] ? FW_NORMAL : FW_SEMIBOLD;
                f.px = 0;
                f.failed = false;
            }
            return EnsureFont(f, device_, px) ? &f : nullptr;
        }
        return nearest >= 0 ? &g_modFonts[nearest] : nullptr;
    }
    // How far a built font is from the one asked for: another face counts as far away.
    int Distance(const Font& f, int px) const
    {
        return std::abs(f.px - px) + (_wcsicmp(f.asked, face_) == 0 ? 0 : 1000);
    }

    Batch& batch_;
    IDirect3DDevice9* device_;
    float width_, height_;
    float defaultPx_;
    Font* font_ = nullptr;
    wchar_t face_[LF_FACESIZE] = L"";  // the face SetFont chose; "" = the default
};

void SetDrawState(IDirect3DDevice9* d)
{
    d->SetPixelShader(nullptr);
    d->SetVertexShader(nullptr);
    d->SetFVF(kFvf);
    for (DWORD i = 1; i < 8; ++i) d->SetTexture(i, nullptr);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_FOGENABLE, FALSE);
    d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    d->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    d->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
}

void DrawOverlay(IDirect3DDevice9* device)
{
    if (device->TestCooperativeLevel() != D3D_OK) return;
    IDirect3DSurface9* backBuffer = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer))) return;
    D3DSURFACE_DESC desc{};
    backBuffer->GetDesc(&desc);
    const float width = static_cast<float>(desc.Width), height = static_cast<float>(desc.Height);

    bool panel = false;
    int panelPx = 0;
    if (g_panel.update) {
        D3DDEVICE_CREATION_PARAMETERS cp{};
        device->GetCreationParameters(&cp);
        panel = g_panel.update(width, height, cp.hFocusWindow, &panelPx);
        panel = panel && EnsureFont(g_panelFont, device, std::max(12, panelPx));
    }

    // Drop-in mods' frame callbacks run every frame (they may do more than draw); what they draw is recorded here and
    // drawn first, so the panel stays on top.
    bool mods = false;
    if (g_modFrame || g_builtinFrame) {
        ++g_frameNo;
        g_batch.Begin(g_modFonts[0]);
        ModCanvas canvas(g_batch, device, width, height);
        if (g_modFrame) g_modFrame(canvas);
        if (g_builtinFrame) {
            canvas.ResetTextSize();
            g_builtinFrame(canvas);  // after the mods, so a mod can't cover it
        }
        mods = !g_batch.Empty();
    }

    if (!panel && !mods) {
        backBuffer->Release();
        return;
    }

    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &saved))) {
        backBuffer->Release();
        return;
    }
    IDirect3DSurface9* oldTarget = nullptr;
    IDirect3DSurface9* oldDepth = nullptr;
    device->GetRenderTarget(0, &oldTarget);
    device->GetDepthStencilSurface(&oldDepth);
    device->SetRenderTarget(0, backBuffer);  // also resets the viewport to the whole back buffer
    device->SetDepthStencilSurface(nullptr);
    SetDrawState(device);

    if (SUCCEEDED(device->BeginScene())) {
        if (mods) g_batch.Draw(device);
        if (panel) {
            g_batch.Begin(g_panelFont);
            Canvas canvas(g_batch, g_panelFont, width, height);
            g_panel.draw(canvas);
            g_batch.Draw(device);
        }
        device->EndScene();
    }

    device->SetRenderTarget(0, oldTarget);
    device->SetDepthStencilSurface(oldDepth);
    saved->Apply();  // after the render target, which would otherwise reset the viewport again
    saved->Release();
    if (oldTarget) oldTarget->Release();
    if (oldDepth) oldDepth->Release();
    backBuffer->Release();
}

HRESULT __stdcall HookPresent(IDirect3DDevice9* device, const RECT* src, const RECT* dst, HWND window,
                              const RGNDATA* dirty)
{
    if (!g_presentSeen) {
        g_presentSeen = true;
        Log("overlay: first Present from the game (device %p)", device);
    }
    DrawOverlay(device);
    return g_origPresent(device, src, dst, window, dirty);
}

// The address of IDirect3DDevice9::Present, read from a throwaway NULLREF device on a hidden window.
uintptr_t FindPresent()
{
    HMODULE d3d9 = LoadLibraryW(L"d3d9.dll");
    auto create = d3d9 ? reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(d3d9, "Direct3DCreate9"))
                       : nullptr;
    IDirect3D9* d3d = create ? create(D3D_SDK_VERSION) : nullptr;
    if (!d3d) {
        Log("overlay: Direct3DCreate9 unavailable");
        return 0;
    }
    HWND window = CreateWindowExW(0, L"STATIC", L"ds3 overlay probe", WS_POPUP, 0, 0, 8, 8, nullptr, nullptr,
                                  nullptr, nullptr);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow = window;
    IDirect3DDevice9* device = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, window,
                                   D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_DISABLE_DRIVER_MANAGEMENT, &pp,
                                   &device);
    uintptr_t present = 0;
    if (SUCCEEDED(hr) && device) {
        present = reinterpret_cast<uintptr_t>((*reinterpret_cast<void***>(device))[kPresentSlot]);
        device->Release();
    } else {
        Log("overlay: probe device failed (0x%08lX)", static_cast<unsigned long>(hr));
    }
    d3d->Release();
    if (window) DestroyWindow(window);
    return present;
}

bool g_installed = false;

}  // namespace

void InstallOverlay()
{
    uintptr_t present = FindPresent();
    if (!present || !game::InitMinHook()) return;
    // No byte check: d3d9.dll differs per Windows build, and the Steam and EA overlays may already have hooked it.
    g_installed = game::HookChecked("Present", present, nullptr, 0, reinterpret_cast<void*>(&HookPresent),
                                    reinterpret_cast<void**>(&g_origPresent));
    if (g_installed) Log("overlay: hooked IDirect3DDevice9::Present @ %08X", static_cast<unsigned>(present));
}

void OverlaySetPanel(const OverlayPanel& panel)
{
    g_panel = panel;
}

void OverlaySetModFrame(void (*frame)(OverlayModCanvas& canvas))
{
    g_modFrame = frame;
}

void OverlaySetBuiltinFrame(void (*frame)(OverlayModCanvas& canvas))
{
    g_builtinFrame = frame;
}

bool OverlayInstalled()
{
    return g_installed;
}
