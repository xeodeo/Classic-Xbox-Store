#pragma once

// JPEG y PNG a RGBA (stb_image). Se puede llamar desde cualquier hilo.
namespace ImageDecode
{
// Devuelve RGBA (4 bytes por pixel) o nullptr; liberar con release().
unsigned char* decode(const unsigned char* data, int length, int* width, int* height);
void release(unsigned char* pixels);
}
