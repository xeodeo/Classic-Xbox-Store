#include "ui/Gfx.h"

#include "XboxXtl.h"

#include <xgraphics.h>
#include <mmintrin.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

extern const unsigned char kFontRegular[];
extern const unsigned char kFontSemibold[];

namespace
{
LPDIRECT3D8 g_d3d;
LPDIRECT3DDEVICE8 g_dev;

struct Vertex
{
    float x, y, z, rhw;
    D3DCOLOR color;
    float u, v;
};
const DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

// --- fuentes ---------------------------------------------------------------------

const int kAtlasSize = 1024;
const int kFirstChar = 32;
const int kCharCount = 224;          // 32..255: ASCII + Latin-1

struct FontInfo
{
    const unsigned char* ttf;
    float size;
    int oversample;
    stbtt_packedchar chars[kCharCount];
    float ascent;
    float lineHeight;
};

FontInfo g_fonts[Gfx::kFontCount] = {
    {kFontRegular, 14, 2},
    {kFontRegular, 16, 2},
    {kFontSemibold, 16, 2},
    {kFontSemibold, 22, 2},
    {kFontSemibold, 32, 1},
};
LPDIRECT3DTEXTURE8 g_atlas;

void bakeFonts()
{
    unsigned char* pixels = static_cast<unsigned char*>(calloc(kAtlasSize, kAtlasSize));
    stbtt_pack_context pc;
    stbtt_PackBegin(&pc, pixels, kAtlasSize, kAtlasSize, 0, 1, NULL);
    for (int i = 0; i < Gfx::kFontCount; ++i)
    {
        FontInfo& f = g_fonts[i];
        stbtt_PackSetOversampling(&pc, f.oversample, f.oversample);
        stbtt_PackFontRange(&pc, f.ttf, 0, f.size, kFirstChar, kCharCount, f.chars);
        stbtt_fontinfo info;
        stbtt_InitFont(&info, f.ttf, stbtt_GetFontOffsetForIndex(f.ttf, 0));
        int ascent, descent, gap;
        stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
        const float scale = stbtt_ScaleForPixelHeight(&info, f.size);
        f.ascent = ascent * scale;
        f.lineHeight = (ascent - descent + gap) * scale;
    }
    stbtt_PackEnd(&pc);

    // Textura A8 (solo opacidad): el color sale del vertice.
    g_dev->CreateTexture(kAtlasSize, kAtlasSize, 1, 0, D3DFMT_A8, 0, &g_atlas);
    D3DLOCKED_RECT locked;
    if (g_atlas && SUCCEEDED(g_atlas->LockRect(0, &locked, NULL, 0)))
    {
        XGSwizzleRect(pixels, kAtlasSize, NULL, locked.pBits, kAtlasSize, kAtlasSize, NULL, 1);
        // El swizzle del XDK usa MMX: deja la pila x87 limpia para los floats.
        _mm_empty();
        g_atlas->UnlockRect(0);
    }
    free(pixels);
}

// Decodifica un caracter UTF-8 y avanza `s`.
unsigned decodeUtf8(const unsigned char*& s)
{
    const unsigned c = *s++;
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0 && (*s & 0xC0) == 0x80) return ((c & 0x1F) << 6) | (*s++ & 0x3F);
    if ((c & 0xF0) == 0xE0 && (s[0] & 0xC0) == 0x80 && (s[1] & 0xC0) == 0x80)
    {
        const unsigned r = ((c & 0x0F) << 12) | ((s[0] & 0x3F) << 6) | (s[1] & 0x3F);
        s += 2;
        return r;
    }
    return '?';
}

int glyphIndex(unsigned codepoint)
{
    if (codepoint == 0x2026) codepoint = '.';      // ...
    if (codepoint == 0x2022 || codepoint == 0x00B7) return 0xB7 - kFirstChar;
    if (codepoint < kFirstChar || codepoint >= kFirstChar + kCharCount) codepoint = '?';
    return static_cast<int>(codepoint) - kFirstChar;
}

// --- dibujo ----------------------------------------------------------------------

// Textura puesta y de donde sale el color: nada (solo el vertice), la letra
// (A8: solo opacidad) o una imagen (color de la textura por el del vertice).
enum class Bound { None, Font, Image };
Bound g_bound = Bound::None;
LPDIRECT3DTEXTURE8 g_boundTexture;

void bind(Bound mode, LPDIRECT3DTEXTURE8 texture)
{
    if (mode == g_bound && texture == g_boundTexture) return;
    g_dev->SetTexture(0, texture);
    if (mode != g_bound)
    {
        g_dev->SetTextureStageState(0, D3DTSS_COLOROP, mode == Bound::Image ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
        g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, mode == Bound::None ? D3DTOP_SELECTARG2 : D3DTOP_MODULATE);
    }
    g_bound = mode;
    g_boundTexture = texture;
}

void useTexture(bool textured)
{
    bind(textured ? Bound::Font : Bound::None, textured ? g_atlas : NULL);
}

const int kCircleSteps = 40;
float g_cos[kCircleSteps + 1], g_sin[kCircleSteps + 1];

D3DCOLOR lerpColor(D3DCOLOR a, D3DCOLOR b, float t)
{
    if (t <= 0) return a;
    if (t >= 1) return b;
    D3DCOLOR out = 0;
    for (int shift = 0; shift < 32; shift += 8)
    {
        const float ca = float((a >> shift) & 0xFF), cb = float((b >> shift) & 0xFF);
        out |= D3DCOLOR(ca + (cb - ca) * t + 0.5f) << shift;
    }
    return out;
}

// Contorno de un rectangulo redondeado (sentido horario), como abanico desde el centro.
int roundRectFan(Vertex* v, float x, float y, float w, float h, float r, D3DCOLOR top, D3DCOLOR bottom)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    const int steps = 8;
    const float cx[4] = {x + w - r, x + w - r, x + r, x + r};
    const float cy[4] = {y + r, y + h - r, y + h - r, y + r};
    const float start[4] = {-1.5707963f, 0.0f, 1.5707963f, 3.1415926f};
    int n = 0;
    v[n++] = {x + w / 2, y + h / 2, 0, 1, lerpColor(top, bottom, 0.5f), 0, 0};
    for (int c = 0; c < 4; ++c)
        for (int i = 0; i <= steps; ++i)
        {
            const float a = start[c] + 1.5707963f * i / steps;
            const float px = cx[c] + cosf(a) * r, py = cy[c] + sinf(a) * r;
            v[n++] = {px, py, 0, 1, lerpColor(top, bottom, (py - y) / h), 0, 0};
        }
    v[n] = v[1];
    return n + 1;
}
} // namespace

namespace Gfx
{
void init()
{
    g_d3d = Direct3DCreate8(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp;
    ZeroMemory(&pp, sizeof(pp));
    pp.BackBufferWidth = kWidth;
    pp.BackBufferHeight = kHeight;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    // Bordes suaves en las curvas.
    pp.MultiSampleType = D3DMULTISAMPLE_4_SAMPLES_MULTISAMPLE_LINEAR;
    if (FAILED(g_d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_dev)))
    {
        pp.MultiSampleType = D3DMULTISAMPLE_NONE;
        g_d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_dev);
    }

    g_dev->SetVertexShader(kFvf);
    g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    g_dev->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    g_dev->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    g_dev->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    g_dev->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    g_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    for (int i = 0; i <= kCircleSteps; ++i)
    {
        const float a = 6.2831853f * i / kCircleSteps;
        g_cos[i] = cosf(a);
        g_sin[i] = sinf(a);
    }
    bakeFonts();
}

void beginFrame(unsigned long clearColor)
{
    g_dev->Clear(0, NULL, D3DCLEAR_TARGET, clearColor, 1.0f, 0);
    g_dev->BeginScene();
}

void persist()
{
    g_dev->PersistDisplay();
}

void endFrame()
{
    g_dev->EndScene();
    g_dev->Present(NULL, NULL, NULL, NULL);
}

void rect(float x, float y, float w, float h, unsigned long top, unsigned long bottom)
{
    useTexture(false);
    Vertex v[4] = {
        {x, y, 0, 1, top, 0, 0},
        {x + w, y, 0, 1, top, 0, 0},
        {x + w, y + h, 0, 1, bottom, 0, 0},
        {x, y + h, 0, 1, bottom, 0, 0},
    };
    g_dev->DrawPrimitiveUP(D3DPT_QUADLIST, 1, v, sizeof(Vertex));
}

void roundRect(float x, float y, float w, float h, float radius, unsigned long top, unsigned long bottom)
{
    if (w <= 0 || h <= 0) return;
    useTexture(false);
    Vertex v[48];
    const int n = roundRectFan(v, x, y, w, h, radius, top, bottom);
    g_dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, n - 2, v, sizeof(Vertex));
}

void glow(float cx, float cy, float rx, float ry, unsigned long color)
{
    useTexture(false);
    Vertex v[kCircleSteps + 2];
    v[0] = {cx, cy, 0, 1, color, 0, 0};
    const D3DCOLOR edge = color & 0x00FFFFFF;
    for (int i = 0; i <= kCircleSteps; ++i)
        v[i + 1] = {cx + g_cos[i] * rx, cy + g_sin[i] * ry, 0, 1, edge, 0, 0};
    g_dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, kCircleSteps, v, sizeof(Vertex));
}

float textWidth(Font font, const char* utf8)
{
    const FontInfo& f = g_fonts[font];
    float x = 0, y = 0;
    const unsigned char* s = reinterpret_cast<const unsigned char*>(utf8);
    while (*s)
    {
        stbtt_aligned_quad q;
        stbtt_GetPackedQuad(f.chars, kAtlasSize, kAtlasSize, glyphIndex(decodeUtf8(s)), &x, &y, &q, 0);
    }
    return x;
}

float text(Font font, const char* utf8, float x, float y, unsigned long color)
{
    if (!g_atlas) return 0;
    useTexture(true);
    const FontInfo& f = g_fonts[font];
    // Posiciones enteras: la letra pequeÃ±a queda nitida.
    float penX = floorf(x + 0.5f), penY = floorf(y + f.ascent + 0.5f);
    const float startX = penX;
    Vertex v[256];
    int n = 0;
    const unsigned char* s = reinterpret_cast<const unsigned char*>(utf8);
    while (*s)
    {
        stbtt_aligned_quad q;
        stbtt_GetPackedQuad(f.chars, kAtlasSize, kAtlasSize, glyphIndex(decodeUtf8(s)), &penX, &penY, &q, 0);
        const float l = q.x0 - 0.5f, r = q.x1 - 0.5f, t = q.y0 - 0.5f, b = q.y1 - 0.5f;
        v[n++] = {l, t, 0, 1, color, q.s0, q.t0};
        v[n++] = {r, t, 0, 1, color, q.s1, q.t0};
        v[n++] = {r, b, 0, 1, color, q.s1, q.t1};
        v[n++] = {l, b, 0, 1, color, q.s0, q.t1};
        if (n == 256)
        {
            g_dev->DrawPrimitiveUP(D3DPT_QUADLIST, n / 4, v, sizeof(Vertex));
            n = 0;
        }
    }
    if (n) g_dev->DrawPrimitiveUP(D3DPT_QUADLIST, n / 4, v, sizeof(Vertex));
    return penX - startX;
}

float textRight(Font font, const char* utf8, float right, float y, unsigned long color)
{
    return text(font, utf8, right - textWidth(font, utf8), y, color);
}

float textCenter(Font font, const char* utf8, float cx, float y, unsigned long color)
{
    return text(font, utf8, cx - textWidth(font, utf8) / 2, y, color);
}

float lineHeight(Font font)
{
    return g_fonts[font].lineHeight;
}

bool createImage(const unsigned char* rgba, int width, int height, Image* out, int maxSide)
{
    *out = Image();
    if (!rgba || width <= 0 || height <= 0) return false;
    // Reducir a la mitad (media de 2x2) hasta que quepa: un fondo de 1280x720
    // ocuparia 8 MB en textura y solo se ve a 640.
    int shift = 0;
    while ((width >> shift) > maxSide || (height >> shift) > maxSide) ++shift;
    const int w = width >> shift, h = height >> shift;
    int tw = 1, th = 1;
    while (tw < w) tw <<= 1;
    while (th < h) th <<= 1;
    DWORD* argb = static_cast<DWORD*>(calloc(static_cast<size_t>(tw) * th, sizeof(DWORD)));
    if (!argb) return false;
    const int step = 1 << shift, area = step * step;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            unsigned sum[4] = {0, 0, 0, 0};
            for (int sy = 0; sy < step; ++sy)
                for (int sx = 0; sx < step; ++sx)
                {
                    const unsigned char* p = rgba + ((static_cast<size_t>(y) * step + sy) * width + x * step + sx) * 4;
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                    sum[3] += p[3];
                }
            argb[static_cast<size_t>(y) * tw + x] = (DWORD(sum[3] / area) << 24) | (DWORD(sum[0] / area) << 16) |
                                                    (DWORD(sum[1] / area) << 8) | DWORD(sum[2] / area);
        }
    LPDIRECT3DTEXTURE8 texture = NULL;
    if (FAILED(g_dev->CreateTexture(tw, th, 1, 0, D3DFMT_A8R8G8B8, 0, &texture)))
    {
        free(argb);
        return false;
    }
    D3DLOCKED_RECT locked;
    if (SUCCEEDED(texture->LockRect(0, &locked, NULL, 0)))
    {
        XGSwizzleRect(argb, tw * 4, NULL, locked.pBits, tw, th, NULL, 4);
        _mm_empty();
        texture->UnlockRect(0);
    }
    free(argb);
    out->texture = texture;
    out->width = w;
    out->height = h;
    out->texWidth = tw;
    out->texHeight = th;
    return true;
}

void freeImage(Image* img)
{
    if (img->texture)
    {
        LPDIRECT3DTEXTURE8 texture = static_cast<LPDIRECT3DTEXTURE8>(img->texture);
        if (g_boundTexture == texture) bind(Bound::None, NULL);
        texture->Release();
    }
    *img = Image();
}

void image(const Image& img, float x, float y, float w, float h, unsigned long color, float u0, float v0, float u1,
           float v1)
{
    if (!img.texture) return;
    bind(Bound::Image, static_cast<LPDIRECT3DTEXTURE8>(img.texture));
    const float su = float(img.width) / img.texWidth, sv = float(img.height) / img.texHeight;
    const float l = x - 0.5f, t = y - 0.5f, r = x + w - 0.5f, b = y + h - 0.5f;
    Vertex v[4] = {
        {l, t, 0, 1, color, u0 * su, v0 * sv},
        {r, t, 0, 1, color, u1 * su, v0 * sv},
        {r, b, 0, 1, color, u1 * su, v1 * sv},
        {l, b, 0, 1, color, u0 * su, v1 * sv},
    };
    g_dev->DrawPrimitiveUP(D3DPT_QUADLIST, 1, v, sizeof(Vertex));
}

unsigned long mix(unsigned long a, unsigned long b, float t)
{
    return lerpColor(a, b, t);
}

unsigned long alpha(unsigned long color, float opacity)
{
    if (opacity < 0) opacity = 0;
    if (opacity > 1) opacity = 1;
    return (color & 0x00FFFFFF) | (D3DCOLOR(((color >> 24) & 0xFF) * opacity + 0.5f) << 24);
}
} // namespace Gfx
