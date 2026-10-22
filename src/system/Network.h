#pragma once

#include <stdint.h>

// XNet (pila de red del XDK) y sockets TCP. Basado en XboxNetwork de OptiCraft.
namespace Network
{
// XNetStartup sin seguridad (la unica forma de llegar a servidores normales)
// + WSAStartup, y espera a que DHCP de una IP. Se puede llamar varias veces.
bool initialize();

bool cableConnected();

// IP de la consola ("192.168.0.3"), o "" si aun no tiene.
void localAddress(char* out, int size);

// Banderas XNET_GET_XNADDR_* (DHCP, puerta de enlace, DNS configurado...).
unsigned long addressFlags();
bool hasGateway();
bool hasDnsServer();

// host: IPv4 en texto o nombre. Prueba el DNS de la Xbox (XNetDnsLookup) y,
// si no responde, pregunta por UDP a 1.1.1.1 y 8.8.8.8. `via` dice quien
// respondio ("Xbox", "1.1.1.1", ...). Direccion en orden de red.
bool resolveIPv4(const char* host, unsigned long* address, const char** via = nullptr);
void formatIPv4(unsigned long address, char* out, int size);

const intptr_t kInvalidSocket = -1;

// Si falla, lastError() dice por que: WSAETIMEDOUT (10060) si nadie contesto en
// `timeoutMs`, o el error real del socket (10061 rechazada, 10065 sin ruta...).
// `blocking`: connect() normal, que espera lo que XNet decida (para comparar).
intptr_t tcpConnect(unsigned long address, int port, int timeoutMs, bool blocking = false);

// Tras conectar, deja el socket en modo bloqueante (recv despierta justo al
// llegar datos; tiempo limite con SO_RCVTIMEO) en vez de sin bloqueo + espera.
void setBlockingStreams(bool blocking);
int tcpRecv(intptr_t socket, char* buffer, int length);          // > 0 bytes, <= 0 cerrado/error
int tcpSend(intptr_t socket, const char* buffer, int length);    // bytes enviados, <= 0 error
void tcpClose(intptr_t socket);
int lastError();
}
