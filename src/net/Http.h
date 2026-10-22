#pragma once

// Cliente HTTP/HTTPS minimo: GET con redirecciones, cuerpo por trozos
// (chunked) o con Content-Length. HTTPS con mbedTLS (TLS 1.2), comprobando la
// cadena de certificados contra las raices de assets/certs/roots.pem.
// Bloquea: llamalo desde un hilo de trabajo, nunca desde el de la pantalla.
namespace Http
{
struct Response
{
    int status;                 // 200, 404... (0 si no llego a responder)
    long long contentLength;    // -1 si el servidor no lo dice
    char finalUrl[512];         // donde acabo tras las redirecciones
};

// Recibe el cuerpo a trozos. Devuelve false para cortar la descarga.
typedef bool (*Sink)(const unsigned char* data, int length, void* user);

// `url` ya codificada (%20...). `extraHeaders`: lineas "Nombre: valor\r\n" o
// nullptr (por ejemplo la cookie de archive.org). Devuelve false si fallo la
// red, el cifrado o el sink corto; `error` dice por que, en español.
bool get(const char* url, const char* extraHeaders, Sink sink, void* user, Response* response, char* error,
         int errorSize);

// Proxy HTTP (CONNECT) forzado para las descargas, "ip:puerto" (proxy.txt);
// "" = elegirlo solo (store/Route). El cifrado sigue de punta a punta: el proxy
// solo ve a que servidor se conecta.
void setProxy(const char* hostPort);
const char* proxy();

// Conexion que se reutiliza (keep-alive) para pedir trozos de un mismo archivo.
// Sin redirecciones: se le da la URL final. Varias a la vez reparten la descarga:
// la pila TCP de la Xbox no amplia la ventana (max. 64 KB en vuelo), y con
// ~0,2 s de ida y vuelta una sola conexion no pasa de ~0,3 MB/s.
class RangeClient
{
public:
    RangeClient();
    ~RangeClient();
    // Pide [start, start + length) y lo escribe en `out`. Falso si fallo (la
    // conexion se cierra y la siguiente llamada vuelve a conectar).
    bool fetch(const char* url, const char* extraHeaders, long long start, int length, unsigned char* out,
               char* error, int errorSize);
    void close();
    // Por donde van sus peticiones: "ip:puerto" de un proxy o "" (directo).
    // Si el proxy no abre el tunel, esa conexion va directa.
    void setProxy(const char* hostPort);
    bool usingProxy() const { return m_proxyHost[0] != 0; }      // tiene proxy puesto
    bool connectedViaProxy() const { return m_viaProxy; }        // la conexion actual va por el

private:
    bool m_viaProxy = false;
    void* m_connection = nullptr;
    char m_proxyHost[64] = "";
    int m_proxyPort = 0;
    char m_host[128] = "";
    int m_port = 0;
    bool m_secure = false;
};

// Peticion con metodo y cuerpo (POST de un formulario). `extraHeaders` debe llevar
// el Content-Type; el Content-Length lo pone esta funcion.
bool request(const char* method, const char* url, const char* extraHeaders, const void* body, int bodyLength,
             Sink sink, void* user, Response* response, char* error, int errorSize);

// Atajo: el cuerpo entero a memoria (malloc; liberar con free). Para JSON e
// imagenes pequeñas. Falla si pasa de `maxBytes`.
bool getToMemory(const char* url, const char* extraHeaders, int maxBytes, unsigned char** data, int* length,
                 Response* response, char* error, int errorSize);
bool postToMemory(const char* url, const char* extraHeaders, const void* body, int bodyLength, int maxBytes,
                  unsigned char** data, int* length, Response* response, char* error, int errorSize);
}
