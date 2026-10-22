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

char s_proxy[64];
bool s_proxyOn;

void save()
{
    char text[160];
    const int length = _snprintf(text, sizeof(text), "proxy=%s\r\nproxy_activo=%d\r\n", s_proxy, s_proxyOn ? 1 : 0);
    if (length > 0) Files::writeDurable(kPath, text, length);
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
            if (key == "proxy") _snprintf(s_proxy, sizeof(s_proxy) - 1, "%s", value.c_str());
            if (key == "proxy_activo") s_proxyOn = value == "1";
        }
        at = end + 1;
    }
    if (!s_proxy[0]) s_proxyOn = false;
    Log::write("ajustes: proxy %s (%s)", s_proxy[0] ? s_proxy : "ninguno", s_proxyOn ? "activo" : "apagado");
}

const char* proxy()
{
    return s_proxy;
}

bool proxyOn()
{
    return s_proxyOn && s_proxy[0];
}

void setProxy(const char* hostPort, bool on)
{
    _snprintf(s_proxy, sizeof(s_proxy) - 1, "%s", hostPort ? hostPort : "");
    s_proxy[sizeof(s_proxy) - 1] = 0;
    s_proxyOn = on && s_proxy[0];
    save();
}

void setProxyOn(bool on)
{
    s_proxyOn = on && s_proxy[0];
    save();
}
} // namespace Settings
