#pragma once

// Buscar un proxy gratis cercano que vaya mas rapido que la conexion directa.
// Se lanza desde la vista Red (no en cada descarga): baja la lista de
// ProxyScrape y prueba hasta 12, todos a la vez, con un archivo publico de
// archive.org (la sesion no pasa por los proxies). Los que funcionen y vayan al
// menos al 60 % de la directa se guardan (el mejor primero) y quedan activos.
namespace Route
{
const int kMaxResults = 13;   // directo + 12 proxies

struct Result
{
    char label[32];   // "directo" o "ip:puerto"
    float speed;      // MB/s (0 = no funciono)
    bool done;
};

enum class State { Idle, Searching, Done };

void startSearch();
State state();
// Copia los resultados; devuelve cuantos hay. `message` dice lo que se decidio.
int results(Result* out, char* message, int messageSize);
}
