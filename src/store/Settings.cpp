#include "store/Settings.h"

#include "system/Files.h"
#include "system/Log.h"

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

namespace
{
const char kPath[] = "T:\\cxs_config.txt";

char s_proxies[Settings::kMaxProxies][32];
int s_proxyCount;
bool s_proxyOn;

void save()
{
    std::string text = "proxies=";
    for (int i = 0; i < s_proxyCount; ++i) {
        if (i) text += ",";
        text += s_proxies[i];
    }
    text += "\r\nproxy_activo=";
    text += s_proxyOn ? "1" : "0";
    text += "\r\n";
    Files::writeDurable(kPath, text.data(), text.size());
}

void addProxy(const std::string& hostPort)
{
    if (s_proxyCount >= Settings::kMaxProxies || hostPort.empty() || hostPort.size() >= sizeof(s_proxies[0])) return;
    _snprintf(s_proxies[s_proxyCount++], sizeof(s_proxies[0]), "%s", hostPort.c_str());
}
} // namespace

namespace Settings
{
void load()
{
    std::vector<unsigned char> bytes;
    if (!Files::readAll(kPath, bytes)) return;
    std::string text(bytes.begin(), bytes.end());
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find_first_of("\r\n", at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        const size_t equals = line.find('=');
        if (equals != std::string::npos) {
            const std::string key = line.substr(0, equals), value = line.substr(equals + 1);
            if (key == "proxies" || key == "proxy") {   // "proxy" = formato de la version anterior
                size_t from = 0;
                while (from <= value.size()) {
                    size_t comma = value.find(',', from);
                    if (comma == std::string::npos) comma = value.size();
                    addProxy(value.substr(from, comma - from));
                    from = comma + 1;
                }
            }
            if (key == "proxy_activo") s_proxyOn = value == "1";
        }
        at = end + 1;
    }
    Log::write("ajustes: %d proxies (%s)", s_proxyCount, proxyOn() ? "activos" : "apagados");
}

int proxyCount()
{
    return s_proxyCount;
}

const char* proxyAt(int index)
{
    return index >= 0 && index < s_proxyCount ? s_proxies[index] : "";
}

bool proxyOn()
{
    return s_proxyOn && s_proxyCount > 0;
}

void setProxies(const char* const* list, int count, bool on)
{
    s_proxyCount = 0;
    for (int i = 0; i < count; ++i) addProxy(list[i]);
    s_proxyOn = on && s_proxyCount > 0;
    save();
}

void setProxyOn(bool on)
{
    s_proxyOn = on && s_proxyCount > 0;
    save();
}
} // namespace Settings
