#include "system/Network.h"

#include "XboxXtl.h"

#include <stdio.h>
#include <string.h>

namespace
{
enum class State { NotStarted, Ready, Failed };

volatile LONG s_initLock = 0;
State s_state = State::NotStarted;

void lock()
{
    while (InterlockedCompareExchange(&s_initLock, 1, 0) != 0)
        Sleep(1);
}

void unlock()
{
    InterlockedExchange(&s_initLock, 0);
}

const DWORD kStallTimeoutMs = 30000;
bool s_blockingStreams = false;

bool shouldRetry()
{
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
}
} // namespace

namespace Network
{
bool initialize()
{
    lock();
    if (s_state == State::NotStarted)
    {
        s_state = State::Failed;
        XNetStartupParams params;
        memset(&params, 0, sizeof(params));
        params.cfgSizeOfStruct = sizeof(params);
        params.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;
        // Por defecto XNet recibe con 16 KB de ventana y 8 paquetes de cola:
        // con ~150 ms de ida y vuelta eso son ~0,1 MB/s. Para descargar juegos:
        // TCP de la Xbox no amplia la ventana: 64 KB es lo maximo que se anuncia.
        params.cfgSockDefaultRecvBufsizeInK = 64;    // ventana por socket (KB)
        params.cfgEnetReceiveQueueLength = 128;      // paquetes en la cola de la tarjeta
        params.cfgPrivatePoolSizeInPages = 255;      // memoria de XNet (~1 MB): 8 descargas a la vez
        WSADATA wsa;
        if (XNetStartup(&params) == 0 && WSAStartup(MAKEWORD(2, 2), &wsa) == 0)
        {
            // Hasta 10 s para la IP (DHCP incluido).
            XNADDR addr;
            for (int i = 0; i < 200 && XNetGetTitleXnAddr(&addr) == XNET_GET_XNADDR_PENDING; ++i)
                Sleep(50);
            s_state = State::Ready;
        }
    }
    const bool ready = s_state == State::Ready;
    unlock();
    return ready;
}

bool cableConnected()
{
    return (XNetGetEthernetLinkStatus() & XNET_ETHERNET_LINK_ACTIVE) != 0;
}

void localAddress(char* out, int size)
{
    out[0] = 0;
    XNADDR addr;
    const DWORD status = XNetGetTitleXnAddr(&addr);
    if (status == XNET_GET_XNADDR_PENDING || (status & XNET_GET_XNADDR_NONE) || addr.ina.s_addr == 0)
        return;
    formatIPv4(addr.ina.s_addr, out, size);
}

void formatIPv4(unsigned long address, char* out, int size)
{
    const unsigned char* b = reinterpret_cast<const unsigned char*>(&address);
    _snprintf(out, size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    out[size - 1] = 0;
}

unsigned long addressFlags()
{
    XNADDR addr;
    return XNetGetTitleXnAddr(&addr);
}

bool hasGateway()
{
    return (addressFlags() & XNET_GET_XNADDR_GATEWAY) != 0;
}

bool hasDnsServer()
{
    return (addressFlags() & XNET_GET_XNADDR_DNS) != 0;
}

namespace
{
bool xnetLookup(const char* host, unsigned long* address)
{
    WSAEVENT event = WSACreateEvent();
    if (event == WSA_INVALID_EVENT)
        return false;
    XNDNS* dns = nullptr;
    bool ok = false;
    if (XNetDnsLookup(host, event, &dns) == 0 && dns != nullptr)
    {
        for (int i = 0; i < 100 && dns->iStatus == WSAEINPROGRESS; ++i)
            WaitForSingleObject(event, 50);
        if (dns->iStatus == 0 && dns->cina > 0)
        {
            *address = dns->aina[0].s_addr;
            ok = true;
        }
        XNetDnsRelease(dns);
    }
    WSACloseEvent(event);
    return ok;
}

// Salta un nombre DNS (etiquetas o puntero comprimido). Devuelve -1 si se sale.
int skipName(const unsigned char* p, int pos, int len)
{
    while (pos < len)
    {
        const unsigned char b = p[pos];
        if (b == 0)
            return pos + 1;
        if ((b & 0xC0) == 0xC0)
            return pos + 2;
        pos += b + 1;
    }
    return -1;
}

// Pregunta tipo A por UDP al servidor `server` (orden de red), puerto 53.
bool udpLookup(unsigned long server, const char* host, unsigned long* address)
{
    unsigned char packet[300];
    const unsigned short id = static_cast<unsigned short>(GetTickCount());
    int n = 0;
    packet[n++] = id >> 8;
    packet[n++] = id & 0xFF;
    packet[n++] = 0x01;   // recursion deseada
    packet[n++] = 0x00;
    const unsigned char counts[8] = {0, 1, 0, 0, 0, 0, 0, 0};
    memcpy(packet + n, counts, 8);
    n += 8;
    for (const char* label = host; *label;)
    {
        const char* dot = strchr(label, '.');
        const int size = dot ? static_cast<int>(dot - label) : static_cast<int>(strlen(label));
        if (size == 0 || size > 63 || n + size + 6 > static_cast<int>(sizeof(packet)))
            return false;
        packet[n++] = static_cast<unsigned char>(size);
        memcpy(packet + n, label, size);
        n += size;
        label += size + (dot ? 1 : 0);
    }
    packet[n++] = 0;
    packet[n++] = 0; packet[n++] = 1;   // tipo A
    packet[n++] = 0; packet[n++] = 1;   // clase IN

    const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
        return false;
    sockaddr_in target;
    memset(&target, 0, sizeof(target));
    target.sin_family = AF_INET;
    target.sin_port = htons(53);
    target.sin_addr.s_addr = server;
    bool ok = false;
    for (int attempt = 0; attempt < 2 && !ok; ++attempt)
    {
        if (sendto(s, reinterpret_cast<const char*>(packet), n, 0, reinterpret_cast<const sockaddr*>(&target),
                   sizeof(target)) != n)
            break;
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(s, &readable);
        timeval timeout = {2, 0};
        if (select(0, &readable, nullptr, nullptr, &timeout) <= 0)
            continue;
        unsigned char reply[512];
        const int len = recv(s, reinterpret_cast<char*>(reply), sizeof(reply), 0);
        if (len < 12 || reply[0] != packet[0] || reply[1] != packet[1] || (reply[3] & 0x0F) != 0)
            continue;
        const int answers = (reply[6] << 8) | reply[7];
        int pos = skipName(reply, 12, len);
        if (pos < 0)
            continue;
        pos += 4;
        for (int i = 0; i < answers && pos > 0 && pos + 10 <= len; ++i)
        {
            pos = skipName(reply, pos, len);
            if (pos < 0 || pos + 10 > len)
                break;
            const int type = (reply[pos] << 8) | reply[pos + 1];
            const int size = (reply[pos + 8] << 8) | reply[pos + 9];
            pos += 10;
            if (type == 1 && size == 4 && pos + 4 <= len)
            {
                memcpy(address, reply + pos, 4);
                ok = true;
                break;
            }
            pos += size;
        }
    }
    closesocket(s);
    return ok;
}
} // namespace

namespace
{
// Direcciones ya resueltas: cada conexion (portadas, listas, descarga) volvia
// a preguntar, y con el DNS de la consola caido eso eran 5 s cada vez.
struct CachedHost
{
    char host[96];
    unsigned long address;
    const char* via;
};
CachedHost s_hosts[24];
int s_hostCount;
volatile LONG s_dnsLock;
// El DNS de la Xbox fallo y uno publico respondio: no volver a esperarlo.
bool s_xnetDnsBroken;

void dnsLock()
{
    while (InterlockedCompareExchange(&s_dnsLock, 1, 0) != 0)
        Sleep(1);
}

void dnsUnlock()
{
    InterlockedExchange(&s_dnsLock, 0);
}

bool cached(const char* host, unsigned long* address, const char** via)
{
    dnsLock();
    bool found = false;
    for (int i = 0; i < s_hostCount && !found; ++i)
        if (_stricmp(s_hosts[i].host, host) == 0)
        {
            *address = s_hosts[i].address;
            if (via) *via = s_hosts[i].via;
            found = true;
        }
    dnsUnlock();
    return found;
}

void remember(const char* host, unsigned long address, const char* via)
{
    if (strlen(host) >= sizeof(s_hosts[0].host)) return;
    dnsLock();
    CachedHost& h = s_hosts[s_hostCount < 24 ? s_hostCount++ : (GetTickCount() % 24)];
    strcpy(h.host, host);
    h.address = address;
    h.via = via;
    dnsUnlock();
}
} // namespace

bool resolveIPv4(const char* host, unsigned long* address, const char** via)
{
    if (address == nullptr || host == nullptr || host[0] == '\0' || !initialize())
        return false;

    const unsigned long literal = inet_addr(host);
    if (literal != INADDR_NONE)
    {
        *address = literal;
        if (via) *via = "IP";
        return true;
    }
    if (cached(host, address, via))
        return true;
    if (!s_xnetDnsBroken && xnetLookup(host, address))
    {
        if (via) *via = "Xbox";
        remember(host, *address, "Xbox");
        return true;
    }
    // El DNS de la Xbox no respondio (sin DNS configurado en el panel, o el
    // del router no contesta): preguntar a DNS publicos.
    static const char* const kPublic[] = {"1.1.1.1", "8.8.8.8"};
    for (const char* server : kPublic)
        if (udpLookup(inet_addr(server), host, address))
        {
            if (via) *via = server;
            s_xnetDnsBroken = true;
            remember(host, *address, server);
            return true;
        }
    return false;
}

namespace
{
intptr_t connectOnce(unsigned long address, int port, int timeoutMs, bool blocking);
}

void setBlockingStreams(bool blocking)
{
    s_blockingStreams = blocking;
}

intptr_t tcpConnect(unsigned long address, int port, int timeoutMs, bool blocking)
{
    if (blocking)
        return connectOnce(address, port, timeoutMs, true);
    // A veces el primer paquete de conexion se pierde y la respuesta tarda
    // segundos (medido: 0,2 s, 1,2 s, 5 s al mismo servidor). Mejor varios
    // intentos cortos que uno largo.
    const int attempts = 4;
    const int perAttempt = timeoutMs / attempts > 1500 ? timeoutMs / attempts : 1500;
    intptr_t s = kInvalidSocket;
    for (int i = 0; i < attempts && s == kInvalidSocket; ++i)
    {
        s = connectOnce(address, port, perAttempt, false);
        // Rechazada o sin ruta no se arregla reintentando.
        const int error = WSAGetLastError();
        if (s == kInvalidSocket && error != WSAETIMEDOUT)
            break;
    }
    return s;
}

namespace
{
intptr_t connectOnce(unsigned long address, int port, int timeoutMs, bool blocking)
{
    if (port < 1 || port > 65535 || !initialize())
        return kInvalidSocket;

    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return kInvalidSocket;

    sockaddr_in target;
    memset(&target, 0, sizeof(target));
    target.sin_family = AF_INET;
    target.sin_port = htons(static_cast<u_short>(port));
    target.sin_addr.s_addr = address;

    // Ventana de recepcion grande: con la de XNet por defecto (pocos KB) y
    // ~150 ms de ida y vuelta a archive.org, la descarga no pasa de 0,1 MB/s.
    // Se pide antes de conectar para que la ventana se anuncie desde el inicio.
    int receiveBuffer = 64 * 1024;   // sin ampliacion de ventana, mas no sirve
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&receiveBuffer), sizeof(receiveBuffer));
    int sendBuffer = 32 * 1024;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sendBuffer), sizeof(sendBuffer));

    if (blocking)
    {
        if (connect(s, reinterpret_cast<const sockaddr*>(&target), sizeof(target)) != 0)
        {
            const int error = WSAGetLastError();
            closesocket(s);
            WSASetLastError(error);
            return kInvalidSocket;
        }
        u_long nonBlocking = 1;
        ioctlsocket(s, FIONBIO, &nonBlocking);
        return static_cast<intptr_t>(s);
    }

    // Sin bloqueo, para poder limitar la espera de un servidor que no responde.
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    if (connect(s, reinterpret_cast<const sockaddr*>(&target), sizeof(target)) != 0)
    {
        if (!shouldRetry())
        {
            const int error = WSAGetLastError();
            closesocket(s);
            WSASetLastError(error);
            return kInvalidSocket;
        }
        fd_set writable, failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(s, &writable);
        FD_SET(s, &failed);
        timeval timeout;
        timeout.tv_sec = timeoutMs / 1000;
        timeout.tv_usec = (timeoutMs % 1000) * 1000;
        const int ready = select(0, nullptr, &writable, &failed, &timeout);
        if (ready <= 0 || FD_ISSET(s, &failed) || !FD_ISSET(s, &writable))
        {
            int error = WSAETIMEDOUT;
            if (ready < 0)
            {
                error = WSAGetLastError();
            }
            else if (ready > 0)
            {
                // XNet no tiene SO_ERROR: un segundo connect() devuelve el error del primero.
                if (connect(s, reinterpret_cast<const sockaddr*>(&target), sizeof(target)) != 0)
                    error = WSAGetLastError();
            }
            closesocket(s);
            WSASetLastError(error);
            return kInvalidSocket;
        }
    }
    if (s_blockingStreams)
    {
        u_long blockingMode = 0;
        ioctlsocket(s, FIONBIO, &blockingMode);
        DWORD timeout = kStallTimeoutMs;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    }
    return static_cast<intptr_t>(s);
}
} // namespace


int tcpRecv(intptr_t s, char* buffer, int length)
{
    const DWORD start = GetTickCount();
    for (;;)
    {
        const int count = recv(static_cast<SOCKET>(s), buffer, length, 0);
        if (count >= 0 || !shouldRetry())
            return count;
        if (GetTickCount() - start > kStallTimeoutMs)
        {
            WSASetLastError(WSAETIMEDOUT);
            return -1;
        }
        // Esperar a que llegue algo (despierta en cuanto llega, no tras un Sleep fijo).
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(static_cast<SOCKET>(s), &readable);
        timeval wait = {0, 100 * 1000};
        select(0, &readable, nullptr, nullptr, &wait);
    }
}

int tcpSend(intptr_t s, const char* buffer, int length)
{
    const DWORD start = GetTickCount();
    for (;;)
    {
        const int count = send(static_cast<SOCKET>(s), buffer, length, 0);
        if (count >= 0 || !shouldRetry())
            return count;
        if (GetTickCount() - start > kStallTimeoutMs)
        {
            WSASetLastError(WSAETIMEDOUT);
            return -1;
        }
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(static_cast<SOCKET>(s), &writable);
        timeval wait = {0, 100 * 1000};
        select(0, nullptr, &writable, nullptr, &wait);
    }
}

void tcpClose(intptr_t s)
{
    closesocket(static_cast<SOCKET>(s));
}

int lastError()
{
    return WSAGetLastError();
}
} // namespace Network
