#include "store/Route.h"

#include "XboxXtl.h"
#include "net/Http.h"
#include "store/Settings.h"
#include "system/Files.h"
#include "system/Log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

namespace
{
// Medido desde el PC con la ventana de la Xbox (64 KB): directo 3,7 MB/s con 8
// conexiones, por un proxy cercano 5,2 MB/s. Pero los gratis cambian a diario.
const char kProxyList[] = "https://api.proxyscrape.com/v2/?request=displayproxies&protocol=http&timeout=2000"
                          "&country=CO,BR,MX,PE,EC,PA,CL,AR,VE,CR,GT,DO,US&ssl=yes&anonymity=all";
// Archivo publico de archive.org (300 MB) para medir: no hace falta sesion.
const char kTestFile[] = "https://archive.org/download/pmovie-20hin_20221006/%28B%29PMovie20.mkv";
const int kProbeBytes = 512 * 1024;

Lock s_lock;
Route::State s_state = Route::State::Idle;
Route::Result s_results[Route::kMaxResults];
int s_count;
char s_message[160];

struct Probe
{
    std::string url;
    int index;
};

DWORD WINAPI probeThread(void* param)
{
    Probe* probe = static_cast<Probe*>(param);
    char proxy[32];
    {
        Guard guard(s_lock);
        memcpy(proxy, s_results[probe->index].label, sizeof(proxy));
    }
    const bool direct = probe->index == 0;
    Http::RangeClient client;
    client.setProxy(direct ? "" : proxy);
    unsigned char* buffer = static_cast<unsigned char*>(malloc(kProbeBytes));
    char error[160];
    float speed = 0;
    // Primero conectar y cifrar (64 KB); se mide la segunda: las descargas
    // reutilizan la conexion, asi que cuenta la velocidad, no el saludo.
    if (buffer && client.fetch(probe->url.c_str(), nullptr, 0, 64 * 1024, buffer, error, sizeof(error)) &&
        (direct || client.connectedViaProxy())) {
        const DWORD begin = GetTickCount();
        if (client.fetch(probe->url.c_str(), nullptr, 64 * 1024, kProbeBytes, buffer, error, sizeof(error))) {
            const DWORD ms = GetTickCount() - begin;
            speed = kProbeBytes / 1048576.0f / ((ms ? ms : 1) / 1000.0f);
        }
    }
    free(buffer);
    Guard guard(s_lock);
    s_results[probe->index].speed = speed;
    s_results[probe->index].done = true;
    delete probe;
    return 0;
}

DWORD WINAPI searchThread(void*)
{
    // La URL final del archivo de prueba (tras la redireccion de archive.org).
    unsigned char* data = nullptr;
    int length = 0;
    Http::Response response;
    char error[160];
    std::string url;
    if (Http::getToMemory(kTestFile, "Range: bytes=0-1023\r\n", 4096, &data, &length, &response, error, sizeof(error))) {
        url = response.finalUrl;
        free(data);
    }
    unsigned char* list = nullptr;
    if (url.empty() ||
        !Http::getToMemory(kProxyList, nullptr, 64 * 1024, &list, &length, &response, error, sizeof(error))) {
        Guard guard(s_lock);
        _snprintf(s_message, sizeof(s_message), "No se pudo buscar (%s)", error);
        s_state = Route::State::Done;
        return 0;
    }
    {
        Guard guard(s_lock);
        s_count = 1;
        _snprintf(s_results[0].label, sizeof(s_results[0].label), "directo");
        std::string text(reinterpret_cast<char*>(list), length);
        size_t at = 0;
        while (at < text.size() && s_count < Route::kMaxResults) {
            size_t end = text.find_first_of("\r\n", at);
            if (end == std::string::npos) end = text.size();
            const std::string line = text.substr(at, end - at);
            if (line.find(':') != std::string::npos && line.size() < sizeof(s_results[0].label))
                _snprintf(s_results[s_count++].label, sizeof(s_results[0].label), "%s", line.c_str());
            at = end + 1;
        }
    }
    free(list);
    const int count = s_count;
    for (int i = 0; i < count; ++i) {
        HANDLE h = CreateThread(NULL, 128 * 1024, probeThread, new Probe{url, i}, 0, NULL);
        if (h) CloseHandle(h);
        else {
            Guard guard(s_lock);
            s_results[i].done = true;
        }
    }
    // Como mucho 20 s: los que no contesten quedan con 0.
    for (int waited = 0; waited < 200; ++waited) {
        bool all = true;
        {
            Guard guard(s_lock);
            for (int i = 0; i < count; ++i) all = all && s_results[i].done;
        }
        if (all) break;
        Sleep(100);
    }

    Guard guard(s_lock);
    const float direct = s_results[0].speed;
    int best = -1;
    for (int i = 1; i < count; ++i) {
        Log::write("ruta: %s %.2f MB/s", s_results[i].label, s_results[i].speed);
        if (s_results[i].speed > (best < 0 ? direct : s_results[best].speed)) best = i;
    }
    // Solo si gana con claridad: un proxy gratis puede caerse en cualquier momento.
    if (best > 0 && s_results[best].speed >= direct * 1.2f) {
        Settings::setProxy(s_results[best].label, true);
        _snprintf(s_message, sizeof(s_message), "Proxy %s activo: %.2f MB/s por conexion (directo %.2f)",
                  s_results[best].label, s_results[best].speed, direct);
    } else {
        _snprintf(s_message, sizeof(s_message), "Ninguno gana a la conexion directa (%.2f MB/s): se queda como estaba",
                  direct);
    }
    Log::write("ruta: %s", s_message);
    s_state = Route::State::Done;
    return 0;
}
} // namespace

namespace Route
{
void startSearch()
{
    {
        Guard guard(s_lock);
        if (s_state == State::Searching) return;
        s_state = State::Searching;
        s_count = 0;
        memset(s_results, 0, sizeof(s_results));
        _snprintf(s_message, sizeof(s_message), "Buscando proxies cercanos y probandolos...");
    }
    HANDLE h = CreateThread(NULL, 128 * 1024, searchThread, NULL, 0, NULL);
    if (h) CloseHandle(h);
    else {
        Guard guard(s_lock);
        s_state = State::Done;
    }
}

State state()
{
    Guard guard(s_lock);
    return s_state;
}

int results(Result* out, char* message, int messageSize)
{
    Guard guard(s_lock);
    memcpy(out, s_results, sizeof(s_results));
    _snprintf(message, messageSize, "%s", s_message);
    return s_count;
}
} // namespace Route
