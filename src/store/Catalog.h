#pragma once

#include <string>
#include <vector>

#include "store/ArtIndex.h"

// Catalogo de archive.org por letras (las mismas colecciones que Classic Xbox
// Loader, core/catalog.py). Cada lista se guarda en T:\cache\lists, asi que
// se ve al instante y sigue disponible aunque archive.org este caido.
namespace Catalog
{
struct Game
{
    std::string file;          // "Halo 2 (USA, Europe) (En,Ja,...).zip"
    std::string title;         // "Halo 2"
    std::string region;        // "USA, Europe"
    std::string collection;    // "microsoft_xbox_h"
    long long size = 0;        // bytes del ZIP
    const ArtIndex::Entry* art = nullptr;
};

const int kLetterCount = 27;
const char* letterLabel(int index);      // "#", "A" ... "Z"

enum class State { Empty, Loading, Ready, Failed };

// Pide una letra; se carga en un hilo de trabajo (primero la cache, luego la red).
void request(int letter);

// Sube cada vez que cambia algo de cualquier letra: la pantalla solo copia
// (snapshot) cuando cambia.
long version();

// Copia segura de lo cargado. `message` explica un fallo o "de la cache".
State snapshot(int letter, std::vector<Game>& games, std::string& message);

// URL de descarga del ZIP (codificada).
std::string downloadUrl(const Game& game);
// Codifica un nombre para ponerlo en una URL ("Halo 2 (USA)" -> "Halo%202%20%28USA%29").
std::string urlEncode(const std::string& text);
}
