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

// La lista de proxies gratis cambia a diario: se vuelve a buscar sola (si estan
// activos) cuando la ultima busqueda tiene mas de unas horas. Reloj de la Xbox.
bool proxySearchDue();
void markProxySearched();

// Idioma de la interfaz (ingles hasta que se cambie; luego se queda).
void setLanguage(int language);
}
