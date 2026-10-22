#include "net/Http.h"

#include "XboxXtl.h"
#include "system/Log.h"
#include "system/Network.h"

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

extern const unsigned char kRootCerts[];
extern const unsigned int kRootCerts_size;

// Entropia para mbedTLS (MBEDTLS_ENTROPY_HARDWARE_ALT): el generador de XNet
// mezclado con el contador de ciclos.
extern "C" int mbedtls_hardware_poll(void*, unsigned char* output, size_t length, size_t* produced)
{
    XNetRandom(output, static_cast<UINT>(length));
    for (size_t i = 0; i < length; ++i)
        output[i] ^= static_cast<unsigned char>(__rdtsc() >> ((i & 3) * 2));
    *produced = length;
    return 0;
}

namespace
{
// --- TLS: estado comun, se prepara una vez -------------------------------------------

volatile LONG s_tlsLock = 0;
bool s_tlsReady;
mbedtls_entropy_context s_entropy;
mbedtls_ctr_drbg_context s_drbg;
mbedtls_x509_crt s_roots;
mbedtls_ssl_config s_config;

bool tlsInit(char* error, int errorSize)
{
    while (InterlockedCompareExchange(&s_tlsLock, 1, 0) != 0)
        Sleep(1);
    if (!s_tlsReady)
    {
        mbedtls_entropy_init(&s_entropy);
        mbedtls_ctr_drbg_init(&s_drbg);
        mbedtls_x509_crt_init(&s_roots);
        mbedtls_ssl_config_init(&s_config);
        static const unsigned char kPersonal[] = "ClassicXboxStore";
        int ret = mbedtls_ctr_drbg_seed(&s_drbg, mbedtls_entropy_func, &s_entropy, kPersonal, sizeof(kPersonal));
        if (ret == 0)
            ret = mbedtls_x509_crt_parse(&s_roots, kRootCerts, kRootCerts_size + 1);   // + el 0 final
        if (ret == 0)
            ret = mbedtls_ssl_config_defaults(&s_config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                              MBEDTLS_SSL_PRESET_DEFAULT);
        if (ret == 0)
        {
            mbedtls_ssl_conf_authmode(&s_config, MBEDTLS_SSL_VERIFY_REQUIRED);
            mbedtls_ssl_conf_ca_chain(&s_config, &s_roots, nullptr);
            mbedtls_ssl_conf_rng(&s_config, mbedtls_ctr_drbg_random, &s_drbg);
            s_tlsReady = true;
        }
        else
        {
            char text[96];
            mbedtls_strerror(ret, text, sizeof(text));
            _snprintf(error, errorSize, "No arranca el cifrado: %s", text);
        }
    }
    const bool ready = s_tlsReady;
    InterlockedExchange(&s_tlsLock, 0);
    return ready;
}

int bioSend(void* context, const unsigned char* data, size_t length)
{
    const int sent = Network::tcpSend(*static_cast<intptr_t*>(context), reinterpret_cast<const char*>(data),
                                      static_cast<int>(length));
    return sent < 0 ? MBEDTLS_ERR_NET_SEND_FAILED : sent;
}

int bioRecv(void* context, unsigned char* data, size_t length)
{
    const int got = Network::tcpRecv(*static_cast<intptr_t*>(context), reinterpret_cast<char*>(data),
                                     static_cast<int>(length));
    return got < 0 ? MBEDTLS_ERR_NET_RECV_FAILED : got;
}

// --- una conexion, cifrada o no ------------------------------------------------------

struct Connection
{
    intptr_t socket = Network::kInvalidSocket;
    bool secure = false;
    mbedtls_ssl_context ssl;

    ~Connection()
    {
        if (secure)
        {
            mbedtls_ssl_close_notify(&ssl);
            mbedtls_ssl_free(&ssl);
        }
        if (socket != Network::kInvalidSocket)
            Network::tcpClose(socket);
    }

    // > 0 bytes, 0 fin, < 0 error
    int read(unsigned char* buffer, int length)
    {
        if (!secure)
            return Network::tcpRecv(socket, reinterpret_cast<char*>(buffer), length);
        for (;;)
        {
            const int ret = mbedtls_ssl_read(&ssl, buffer, length);
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
                continue;
            if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
                return 0;
            return ret;
        }
    }

    bool writeAll(const char* data, int length)
    {
        while (length > 0)
        {
            int ret;
            if (secure)
                ret = mbedtls_ssl_write(&ssl, reinterpret_cast<const unsigned char*>(data), length);
            else
                ret = Network::tcpSend(socket, data, length);
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
                continue;
            if (ret <= 0)
                return false;
            data += ret;
            length -= ret;
        }
        return true;
    }
};

struct Url
{
    bool secure;
    char host[128];
    int port;
    char path[512];
};

bool parseUrl(const char* url, Url* out)
{
    const char* p = url;
    if (_strnicmp(p, "https://", 8) == 0)
    {
        out->secure = true;
        out->port = 443;
        p += 8;
    }
    else if (_strnicmp(p, "http://", 7) == 0)
    {
        out->secure = false;
        out->port = 80;
        p += 7;
    }
    else
    {
        return false;
    }
    const char* hostEnd = p;
    while (*hostEnd && *hostEnd != '/' && *hostEnd != ':')
        ++hostEnd;
    const int hostLength = static_cast<int>(hostEnd - p);
    if (hostLength == 0 || hostLength >= static_cast<int>(sizeof(out->host)))
        return false;
    memcpy(out->host, p, hostLength);
    out->host[hostLength] = 0;
    p = hostEnd;
    if (*p == ':')
    {
        out->port = atoi(p + 1);
        while (*p && *p != '/')
            ++p;
    }
    _snprintf(out->path, sizeof(out->path), "%s", *p ? p : "/");
    out->path[sizeof(out->path) - 1] = 0;
    return true;
}

char s_proxyHost[64];
int s_proxyPort;

// "ip:puerto" -> host y puerto. Falso si no vale.
bool splitHostPort(const char* hostPort, char* host, int hostSize, int* port)
{
    host[0] = 0;
    *port = 0;
    if (!hostPort) return false;
    const char* colon = strchr(hostPort, ':');
    if (!colon || colon == hostPort || colon - hostPort >= hostSize) return false;
    memcpy(host, hostPort, colon - hostPort);
    host[colon - hostPort] = 0;
    *port = atoi(colon + 1);
    if (*port <= 0 || *port > 65535) {
        host[0] = 0;
        return false;
    }
    return true;
}

// Tunel por el proxy: CONNECT host:puerto y esperar "200". Devuelve el socket o kInvalidSocket.
intptr_t connectThroughProxy(const Url& url, const char* proxyHost, int proxyPort)
{
    unsigned long address = 0;
    if (!Network::resolveIPv4(proxyHost, &address)) return Network::kInvalidSocket;
    const intptr_t s = Network::tcpConnect(address, proxyPort, 6000);
    if (s == Network::kInvalidSocket) return s;
    char request[256];
    const int length = _snprintf(request, sizeof(request), "CONNECT %s:%d HTTP/1.1\r\nHost: %s:%d\r\n\r\n", url.host,
                                 url.port, url.host, url.port);
    char reply[512];
    int got = 0;
    if (length > 0 && Network::tcpSend(s, request, length) == length) {
        while (got < static_cast<int>(sizeof(reply)) - 1) {
            const int n = Network::tcpRecv(s, reply + got, 1);   // de uno en uno: no comerse el saludo TLS
            if (n <= 0) break;
            got += n;
            reply[got] = 0;
            if (got >= 4 && strcmp(reply + got - 4, "\r\n\r\n") == 0) break;
        }
    }
    reply[got] = 0;
    const char* space = strchr(reply, ' ');
    if (space && strncmp(space + 1, "200", 3) == 0) return s;
    Log::write("http: el proxy %s:%d no abrio el tunel a %s (%.40s)", proxyHost, proxyPort, url.host, reply);
    Network::tcpClose(s);
    return Network::kInvalidSocket;
}

// `proxyHost` vacio o nulo: directo. Con proxy, solo para conexiones cifradas.
bool open(Connection& c, const Url& url, char* error, int errorSize, const char* proxyHost = nullptr,
          int proxyPort = 0, bool* viaProxy = nullptr)
{
    const DWORD start = GetTickCount();
    if (viaProxy) *viaProxy = false;
    if (proxyHost && proxyHost[0] && url.secure) {
        c.socket = connectThroughProxy(url, proxyHost, proxyPort);
        if (c.socket != Network::kInvalidSocket) {
            if (viaProxy) *viaProxy = true;
            Log::write("http: tunel a %s por el proxy %s:%d en %lu ms", url.host, proxyHost, proxyPort,
                       GetTickCount() - start);
            goto connected;
        }
        // Si el proxy no responde, directo: la descarga no se corta por su culpa.
    }
    {
    unsigned long address = 0;
    if (!Network::resolveIPv4(url.host, &address))
    {
        _snprintf(error, errorSize, "No se encuentra %s (DNS)", url.host);
        return false;
    }
    char ip[24];
    Network::formatIPv4(address, ip, sizeof(ip));
    c.socket = Network::tcpConnect(address, url.port, 10000);
    if (c.socket == Network::kInvalidSocket)
    {
        _snprintf(error, errorSize, "%s no responde (error %d)", url.host, Network::lastError());
        Log::write("http: %s (%s) no conecta: %d", url.host, ip, Network::lastError());
        return false;
    }
    Log::write("http: conectado a %s (%s:%d) en %lu ms", url.host, ip, url.port, GetTickCount() - start);
    }
connected:
    if (!url.secure)
        return true;
    if (!tlsInit(error, errorSize))
        return false;

    mbedtls_ssl_init(&c.ssl);
    c.secure = true;
    int ret = mbedtls_ssl_setup(&c.ssl, &s_config);
    if (ret == 0)
        ret = mbedtls_ssl_set_hostname(&c.ssl, url.host);
    if (ret == 0)
    {
        mbedtls_ssl_set_bio(&c.ssl, &c.socket, bioSend, bioRecv, nullptr);
        do
            ret = mbedtls_ssl_handshake(&c.ssl);
        while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);
    }
    if (ret != 0)
    {
        char text[96];
        mbedtls_strerror(ret, text, sizeof(text));
        const uint32_t flags = mbedtls_ssl_get_verify_result(&c.ssl);
        if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED && flags)
            _snprintf(error, errorSize, "Certificado de %s no valido (0x%x)", url.host, flags);
        else
            _snprintf(error, errorSize, "Cifrado con %s: %s (-0x%04x)", url.host, text, -ret);
        Log::write("http: %s", error);
        return false;
    }
    Log::write("http: cifrado listo con %s en %lu ms (%s)", url.host, GetTickCount() - start,
               mbedtls_ssl_get_ciphersuite(&c.ssl));
    return true;
}

// Lee la cabecera HTTP. Lo que llegue del cuerpo queda en buffer[*bodyStart, *filled).
bool readHead(Connection& c, char* buffer, int size, int* filled, int* bodyStart)
{
    *filled = 0;
    for (;;)
    {
        if (*filled >= size - 1)
            return false;
        const int got = c.read(reinterpret_cast<unsigned char*>(buffer) + *filled, size - 1 - *filled);
        if (got <= 0)
            return false;
        *filled += got;
        buffer[*filled] = 0;
        const char* end = strstr(buffer, "\r\n\r\n");
        if (end)
        {
            *bodyStart = static_cast<int>(end - buffer) + 4;
            return true;
        }
    }
}

// Valor de una cabecera (sin distinguir mayusculas). Escribe "" si no esta.
void header(const char* head, const char* name, char* out, int size)
{
    out[0] = 0;
    const size_t nameLength = strlen(name);
    for (const char* line = strstr(head, "\r\n"); line && line[2]; line = strstr(line + 2, "\r\n"))
    {
        const char* p = line + 2;
        if (_strnicmp(p, name, nameLength) == 0 && p[nameLength] == ':')
        {
            p += nameLength + 1;
            while (*p == ' ')
                ++p;
            int n = 0;
            while (p[n] && p[n] != '\r' && n < size - 1)
            {
                out[n] = p[n];
                ++n;
            }
            out[n] = 0;
            return;
        }
    }
}

// Entrega el cuerpo: primero lo que ya vino con la cabecera, luego el resto.
struct BodyReader
{
    Connection& c;
    char* pending;
    int pendingLength;

    int read(unsigned char* buffer, int length)
    {
        if (pendingLength > 0)
        {
            const int n = pendingLength < length ? pendingLength : length;
            memcpy(buffer, pending, n);
            pending += n;
            pendingLength -= n;
            return n;
        }
        return c.read(buffer, length);
    }
};

bool readChunked(BodyReader& body, Http::Sink sink, void* user, char* error, int errorSize)
{
    static const int kBuffer = 16384;
    unsigned char* buffer = static_cast<unsigned char*>(malloc(kBuffer));
    bool ok = false;
    for (;;)
    {
        // Linea con el tamaño en hexadecimal.
        char line[32];
        int n = 0;
        unsigned char ch;
        while (n < static_cast<int>(sizeof(line)) - 1 && body.read(&ch, 1) == 1 && ch != '\n')
            if (ch != '\r')
                line[n++] = static_cast<char>(ch);
        line[n] = 0;
        const long size = strtol(line, nullptr, 16);
        if (n == 0 || size < 0)
        {
            _snprintf(error, errorSize, "Respuesta por trozos rota");
            break;
        }
        if (size == 0)
        {
            ok = true;
            break;
        }
        long left = size;
        while (left > 0)
        {
            const int got = body.read(buffer, left < kBuffer ? static_cast<int>(left) : kBuffer);
            if (got <= 0)
                break;
            if (!sink(buffer, got, user))
            {
                _snprintf(error, errorSize, "Descarga cancelada");
                free(buffer);
                return false;
            }
            left -= got;
        }
        if (left > 0)
        {
            _snprintf(error, errorSize, "Se corto la conexion");
            break;
        }
        unsigned char crlf[2];
        body.read(crlf, 1);
        body.read(crlf + 1, 1);
    }
    free(buffer);
    return ok;
}

bool readPlain(BodyReader& body, long long contentLength, Http::Sink sink, void* user, char* error, int errorSize)
{
    static const int kBuffer = 16384;
    unsigned char* buffer = static_cast<unsigned char*>(malloc(kBuffer));
    long long received = 0;
    bool ok = true;
    for (;;)
    {
        if (contentLength >= 0 && received >= contentLength)
            break;
        int want = kBuffer;
        if (contentLength >= 0 && contentLength - received < want)
            want = static_cast<int>(contentLength - received);
        const int got = body.read(buffer, want);
        if (got == 0 && contentLength < 0)
            break;   // sin tamaño: el final es el cierre
        if (got <= 0)
        {
            _snprintf(error, errorSize, "Se corto la conexion");
            ok = false;
            break;
        }
        received += got;
        if (!sink(buffer, got, user))
        {
            _snprintf(error, errorSize, "Descarga cancelada");
            ok = false;
            break;
        }
    }
    free(buffer);
    return ok;
}

struct MemorySink
{
    unsigned char* data;
    int length;
    int capacity;
    int maxBytes;
};

bool memorySink(const unsigned char* data, int length, void* user)
{
    MemorySink& m = *static_cast<MemorySink*>(user);
    if (m.length + length > m.maxBytes)
        return false;
    if (m.length + length > m.capacity)
    {
        int capacity = m.capacity ? m.capacity * 2 : 65536;
        while (capacity < m.length + length)
            capacity *= 2;
        unsigned char* grown = static_cast<unsigned char*>(realloc(m.data, capacity));
        if (!grown)
            return false;
        m.data = grown;
        m.capacity = capacity;
    }
    memcpy(m.data + m.length, data, length);
    m.length += length;
    return true;
}
} // namespace

namespace Http
{
void setProxy(const char* hostPort)
{
    splitHostPort(hostPort, s_proxyHost, sizeof(s_proxyHost), &s_proxyPort);
}

const char* proxy()
{
    static char text[80];
    if (!s_proxyHost[0]) return "";
    _snprintf(text, sizeof(text), "%s:%d", s_proxyHost, s_proxyPort);
    return text;
}

bool get(const char* url, const char* extraHeaders, Sink sink, void* user, Response* response, char* error,
         int errorSize)
{
    return request("GET", url, extraHeaders, nullptr, 0, sink, user, response, error, errorSize);
}

bool request(const char* method, const char* url, const char* extraHeaders, const void* body, int bodyLength,
             Sink sink, void* user, Response* response, char* error, int errorSize)
{
    response->status = 0;
    response->contentLength = -1;
    _snprintf(response->finalUrl, sizeof(response->finalUrl), "%s", url);
    response->finalUrl[sizeof(response->finalUrl) - 1] = 0;
    error[0] = 0;

    for (int redirect = 0; redirect < 8; ++redirect)
    {
        Url target;
        if (!parseUrl(response->finalUrl, &target))
        {
            _snprintf(error, errorSize, "Direccion no valida");
            return false;
        }
        Connection c;
        if (!open(c, target, error, errorSize))
            return false;

        std::string request = std::string(method) + " " + target.path + " HTTP/1.1\r\nHost: " + target.host +
                              "\r\nUser-Agent: ClassicXboxStore/0.1\r\nAccept: */*\r\nConnection: close\r\n";
        if (extraHeaders) request += extraHeaders;
        if (body) {
            char length[48];
            _snprintf(length, sizeof(length), "Content-Length: %d\r\n", bodyLength);
            request += length;
        }
        request += "\r\n";
        if (body) request.append(static_cast<const char*>(body), bodyLength);
        if (!c.writeAll(request.c_str(), static_cast<int>(request.size())))
        {
            _snprintf(error, errorSize, "No se pudo enviar la peticion a %s", target.host);
            return false;
        }

        static const int kHead = 8192;
        char* head = static_cast<char*>(malloc(kHead));
        int filled = 0, bodyStart = 0;
        Log::write("http: %s %s", method, target.path);
        if (!readHead(c, head, kHead, &filled, &bodyStart))
        {
            Log::write("http: %s no mando cabeceras (%d bytes recibidos)", target.host, filled);
            free(head);
            _snprintf(error, errorSize, "%s no contesto bien", target.host);
            return false;
        }
        head[bodyStart - 2] = 0;   // corta tras la ultima cabecera
        int status = 0;
        sscanf(head, "HTTP/%*d.%*d %d", &status);
        response->status = status;
        {
            char length[32], range[64];
            header(head, "Content-Length", length, sizeof(length));
            header(head, "Content-Range", range, sizeof(range));
            Log::write("http: respuesta %d, Content-Length %s, Content-Range %s", status, length, range);
        }

        char value[512];
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)
        {
            header(head, "Location", value, sizeof(value));
            free(head);
            if (!value[0])
            {
                _snprintf(error, errorSize, "Redireccion sin destino");
                return false;
            }
            if (value[0] == '/')
                _snprintf(response->finalUrl, sizeof(response->finalUrl), "%s://%s%s",
                          target.secure ? "https" : "http", target.host, value);
            else
                _snprintf(response->finalUrl, sizeof(response->finalUrl), "%s", value);
            response->finalUrl[sizeof(response->finalUrl) - 1] = 0;
            if (status != 307 && status != 308) {   // tras un POST, la redireccion se pide con GET
                method = "GET";
                body = nullptr;
                bodyLength = 0;
            }
            continue;
        }

        header(head, "Content-Length", value, sizeof(value));
        response->contentLength = value[0] ? _atoi64(value) : -1;
        header(head, "Transfer-Encoding", value, sizeof(value));
        const bool chunked = _stricmp(value, "chunked") == 0;

        BodyReader body = {c, head + bodyStart, filled - bodyStart};
        bool ok;
        if (status < 200 || status >= 300)
        {
            _snprintf(error, errorSize, "%s respondio %d", target.host, status);
            ok = false;
        }
        else if (chunked)
        {
            ok = readChunked(body, sink, user, error, errorSize);
        }
        else
        {
            ok = readPlain(body, response->contentLength, sink, user, error, errorSize);
        }
        free(head);
        return ok;
    }
    _snprintf(error, errorSize, "Demasiadas redirecciones");
    return false;
}

bool getToMemory(const char* url, const char* extraHeaders, int maxBytes, unsigned char** data, int* length,
                 Response* response, char* error, int errorSize)
{
    MemorySink m = {nullptr, 0, 0, maxBytes};
    const bool ok = get(url, extraHeaders, memorySink, &m, response, error, errorSize);
    if (!ok)
    {
        free(m.data);
        *data = nullptr;
        *length = 0;
        if (m.length + 1 > maxBytes && !error[0])
            _snprintf(error, errorSize, "Respuesta demasiado grande");
        return false;
    }
    *data = m.data;
    *length = m.length;
    return true;
}

bool postToMemory(const char* url, const char* extraHeaders, const void* body, int bodyLength, int maxBytes,
                  unsigned char** data, int* length, Response* response, char* error, int errorSize)
{
    MemorySink m = {nullptr, 0, 0, maxBytes};
    const bool ok = request("POST", url, extraHeaders, body, bodyLength, memorySink, &m, response, error, errorSize);
    if (!ok)
    {
        free(m.data);
        *data = nullptr;
        *length = 0;
        return false;
    }
    *data = m.data;
    *length = m.length;
    return true;
}

RangeClient::RangeClient()
{
    // Por defecto, el proxy forzado de proxy.txt (si lo hay).
    if (s_proxyHost[0]) {
        _snprintf(m_proxyHost, sizeof(m_proxyHost), "%s", s_proxyHost);
        m_proxyPort = s_proxyPort;
    }
}

void RangeClient::setProxy(const char* hostPort)
{
    close();
    splitHostPort(hostPort, m_proxyHost, sizeof(m_proxyHost), &m_proxyPort);
}

RangeClient::~RangeClient()
{
    close();
}

void RangeClient::close()
{
    delete static_cast<Connection*>(m_connection);
    m_connection = nullptr;
}

bool RangeClient::fetch(const char* url, const char* extraHeaders, long long start, int length, unsigned char* out,
                        char* error, int errorSize)
{
    error[0] = 0;
    Url target;
    if (!parseUrl(url, &target))
    {
        _snprintf(error, errorSize, "Direccion no valida");
        return false;
    }
    if (m_connection && (strcmp(m_host, target.host) != 0 || m_port != target.port || m_secure != target.secure))
        close();
    if (!m_connection)
    {
        Connection* c = new Connection;
        if (!open(*c, target, error, errorSize, m_proxyHost, m_proxyPort, &m_viaProxy))
        {
            delete c;
            return false;
        }
        m_connection = c;
        _snprintf(m_host, sizeof(m_host), "%s", target.host);
        m_port = target.port;
        m_secure = target.secure;
    }
    Connection& c = *static_cast<Connection*>(m_connection);

    char range[96];
    _snprintf(range, sizeof(range), "Range: bytes=%I64d-%I64d\r\n", start, start + length - 1);
    std::string request = std::string("GET ") + target.path + " HTTP/1.1\r\nHost: " + target.host +
                          "\r\nUser-Agent: ClassicXboxStore/0.1\r\nAccept: */*\r\nConnection: keep-alive\r\n";
    if (extraHeaders) request += extraHeaders;
    request += range;
    request += "\r\n";
    if (!c.writeAll(request.c_str(), static_cast<int>(request.size())))
    {
        _snprintf(error, errorSize, "No se pudo enviar la peticion a %s", target.host);
        close();
        return false;
    }

    static const int kHead = 8192;
    std::vector<char> head(kHead);
    int filled = 0, bodyStart = 0;
    if (!readHead(c, head.data(), kHead, &filled, &bodyStart))
    {
        _snprintf(error, errorSize, "%s no contesto", target.host);
        close();
        return false;
    }
    head[bodyStart - 2] = 0;
    int status = 0;
    sscanf(head.data(), "HTTP/%*d.%*d %d", &status);
    char value[64];
    header(head.data(), "Content-Length", value, sizeof(value));
    const long long contentLength = value[0] ? _atoi64(value) : -1;
    header(head.data(), "Connection", value, sizeof(value));
    const bool closeAfter = _stricmp(value, "close") == 0;
    if (status != 206 || contentLength != length)
    {
        _snprintf(error, errorSize, "%s respondio %d (%I64d bytes en vez de %d)", target.host, status, contentLength,
                  length);
        close();
        return false;
    }

    int got = filled - bodyStart;
    if (got > length) got = length;
    memcpy(out, head.data() + bodyStart, got);
    while (got < length)
    {
        const int n = c.read(out + got, length - got);
        if (n <= 0)
        {
            _snprintf(error, errorSize, "Se corto la conexion con %s", target.host);
            close();
            return false;
        }
        got += n;
    }
    if (closeAfter)
        close();
    return true;
}
} // namespace Http
