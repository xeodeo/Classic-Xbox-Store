#pragma once

// Dibujo 2D sobre D3D8: degradados, esquinas redondeadas, brillos y texto
// Open Sans (UTF-8, ASCII + Latin-1: tildes y eñes). Colores en 0xAARRGGBB.
namespace Gfx
{
const int kWidth = 640;
const int kHeight = 480;

enum Font
{
    kSmall,      // 14 px
    kBody,       // 16 px
    kLabel,      // 16 px seminegrita
    kTitle,      // 22 px seminegrita
    kHuge,       // 32 px seminegrita
    kFontCount
};

void init();
void beginFrame(unsigned long clearColor);
void endFrame();
// Deja la ultima imagen en pantalla mientras arranca otro XBE.
void persist();

// Rectangulo con degradado vertical (arriba -> abajo).
void rect(float x, float y, float w, float h, unsigned long top, unsigned long bottom);
void roundRect(float x, float y, float w, float h, float radius, unsigned long top, unsigned long bottom);
// Brillo radial: `color` en el centro, transparente en el borde.
void glow(float cx, float cy, float rx, float ry, unsigned long color);

// `y` es la parte de arriba de la linea. Devuelven el ancho dibujado.
float text(Font font, const char* utf8, float x, float y, unsigned long color);
float textRight(Font font, const char* utf8, float right, float y, unsigned long color);
float textCenter(Font font, const char* utf8, float cx, float y, unsigned long color);
float textWidth(Font font, const char* utf8);
float lineHeight(Font font);

// Imagenes (portadas, fondos, iconos). Se crean y dibujan SOLO desde el hilo
// de la pantalla: D3D no se puede usar desde dos hilos.
struct Image
{
    void* texture = nullptr;
    int width = 0, height = 0;          // la imagen
    int texWidth = 0, texHeight = 0;    // su textura (potencias de 2)
};
// RGBA de 8 bits por canal. Si pasa de `maxSide`, se reduce a la mitad las veces necesarias.
bool createImage(const unsigned char* rgba, int width, int height, Image* out, int maxSide = 1024);
void freeImage(Image* image);
// u0..v1: el trozo de la imagen que se dibuja (0..1), para recortar.
void image(const Image& image, float x, float y, float w, float h, unsigned long color = 0xFFFFFFFF,
           float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1);

// Mezcla dos colores (t = 0 -> a, t = 1 -> b) y cambia la opacidad.
unsigned long mix(unsigned long a, unsigned long b, float t);
unsigned long alpha(unsigned long color, float opacity);
}
