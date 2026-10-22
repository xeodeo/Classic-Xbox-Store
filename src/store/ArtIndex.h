#pragma once

// Arte de cada juego en el repositorio de MobCat, por nombre redump
// (assets/data/artindex.tsv, va dentro del XBE; lo crea scripts/make_art_index.py).
namespace ArtIndex
{
struct Entry
{
    const char* name;        // nombre redump = nombre del ZIP sin .zip
    const char* xmid;
    const char* titleId;
    const char* thumbnail;   // rutas relativas a thumbnail/, background/, icon/; "" si no hay
    const char* background;
    const char* icon;
};

// nullptr si el juego no esta en la base.
const Entry* find(const char* redumpName);

// URL completa de un arte ("thumbnail", "background" o "icon") o "" si no hay.
void url(const char* folder, const char* path, char* out, int size);
}
