#pragma once

#include "store/ArtIndex.h"

// Portadas y fondos del juego elegido. Un hilo los baja de GitHub (una vez:
// quedan en T:\cache\art) y los decodifica; la pantalla los recoge con take()
// y crea las texturas (D3D solo desde su hilo).
namespace Art
{
enum Kind { kCover, kBackground };

struct Pixels
{
    unsigned char* rgba = nullptr;   // liberar con ImageDecode::release
    int width = 0, height = 0;
    Kind kind = kCover;
    const ArtIndex::Entry* entry = nullptr;
};

// El juego cuyo arte se quiere ahora (nullptr: ninguno). Lo anterior que no
// haya terminado se abandona.
void want(const ArtIndex::Entry* entry);

// Saca una imagen terminada, si hay. Puede ser de un juego anterior: compara `entry`.
bool take(Pixels& out);
}
