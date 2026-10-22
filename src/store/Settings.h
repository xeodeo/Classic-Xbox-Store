#pragma once

// Ajustes que se guardan en T:\cxs_config.txt (lineas "clave=valor").
namespace Settings
{
const int kMaxProxies = 6;

void load();

// Proxies para las descargas ("ip:puerto", el mejor primero) y si estan activos.
int proxyCount();
const char* proxyAt(int index);
bool proxyOn();
void setProxies(const char* const* list, int count, bool on);
void setProxyOn(bool on);
}
