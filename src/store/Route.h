#pragma once

// Buscar un proxy gratis cercano que vaya mas rapido que la conexion directa.
// Se lanza desde la vista Red (no en cada descarga): baja la lista de
// ProxyScrape y prueba hasta 8, todos a la vez, con un archivo publico de
// archive.org (la sesion no pasa por los proxies). Si uno gana con claridad, se
// guarda en los ajustes y queda activo.
namespace Route
{
const int kMaxResults = 9;   // directo + 8 proxies

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
