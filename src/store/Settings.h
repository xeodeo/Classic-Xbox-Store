#pragma once

// Ajustes que se guardan en T:\cxs_config.txt (lineas "clave=valor").
namespace Settings
{
void load();

// Proxy para las descargas: "ip:puerto" (o "") y si esta activo.
const char* proxy();
bool proxyOn();
void setProxy(const char* hostPort, bool on);
void setProxyOn(bool on);
}
