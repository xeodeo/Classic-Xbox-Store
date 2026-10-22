#pragma once

#include "store/Catalog.h"

// Descarga e instalacion de un juego, en un hilo propio:
//   1. baja la ISO de dentro del ZIP (archive.org la saca del ZIP por su lado);
//   2. se salta la particion de video del principio (redump: 0x18300000) y
//      guarda la del juego en trozos de menos de 4 GB (limite de FATX) en
//      <unidad>:\Games\_cxs_tmp;
//   3. en cuanto llega el arbol de directorios (XDVDFS) sabe donde acaba el
//      ultimo archivo y corta la descarga ahi: el relleno del final no se baja;
//   4. copia los archivos a <unidad>:\Games\<juego> y borra los trozos.
namespace Installer
{
enum class Phase { Idle, Starting, Downloading, Installing, Done, Failed, Cancelled };

struct Progress
{
    Phase phase = Phase::Idle;
    long long done = 0;        // bytes de la fase actual
    long long total = 0;       // -1 si aun no se sabe
    float speed = 0;           // bytes/s (descarga)
    char text[160] = "";       // que esta haciendo, en español
    char folder[96] = "";      // carpeta final, p. ej. "F:\Games\Halo 2"
    const ArtIndex::Entry* art = nullptr;
    char title[96] = "";
    char detail[96] = "";      // medidas: en que se va el tiempo
    char route[64] = "";       // por donde baja: "directo" o "proxy ip:puerto + directo"
};

// Empieza a instalar en la unidad `drive` ('E', 'F', 'G'). Falso si ya hay una en marcha.
bool start(const Catalog::Game& game, char drive);
void cancel();
Progress progress();
// Vuelve a Idle tras Done/Failed/Cancelled.
void dismiss();

// Descarga a medias que se puede seguir (quedo un punto de control al cerrar la
// tienda, apagar la consola o fallar la red).
struct Pending
{
    char title[96] = "";
    char folder[96] = "";
    long long written = 0;       // bytes guardados en disco
    long long isoPosition = 0;   // por donde iba la ISO
    bool installing = false;     // ya estaba descargado: solo falta copiar
    const ArtIndex::Entry* art = nullptr;
};
bool pending(Pending& out);
bool resume();            // sigue la descarga a medias
void discardPending();    // borra lo descargado y el punto de control

// Borra los trozos que dejo una descarga interrumpida (por ejemplo, salir con
// la combinacion de botones a mitad): <unidad>:\Games\_cxs_tmp. Al arrancar.
void cleanupLeftovers();
}
