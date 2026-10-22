#include "store/Catalog.h"

#include "XboxXtl.h"
#include "net/Http.h"
#include "system/Files.h"

#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{
// Igual que COLLECTION_MAP en Classic Xbox Loader (core/catalog.py).
const char* const kCollections[Catalog::kLetterCount][3] = {
    {"microsoft_xbox_numberssymbols"},
    {"microsoft_xbox_a"},
    {"microsoft_xbox_b"},
    {"microsoft_xbox_c_part1", "microsoft_xbox_c_part2"},
    {"microsoft_xbox_d_part1", "microsoft_xbox_d_part2"},
    {"microsoft_xbox_e"},
    {"microsoft_xbox_f"},
    {"microsoft_xbox_g"},
    {"microsoft_xbox_h"},
    {"microsoft_xbox_i"},
    {"microsoft_xbox_j"},
    {"microsoft_xbox_k"},
    {"microsoft_xbox_l"},
    {"microsoft_xbox_m_part1", "microsoft_xbox_m_part2"},
    {"microsoft_xbox_n_part1", "microsoft_xbox_n_part2"},
    {"microsoft_xbox_o_part1", "microsoft_xbox_o_part2"},
    {"microsoft_xbox_p"},
    {"microsoft_xbox_q"},
    {"microsoft_xbox_r"},
    {"microsoft_xbox_s_part1", "microsoft_xbox_s_part2"},
    {"microsoft_xbox_t_part1", "microsoft_xbox_t_part2"},
    {"microsoft_xbox_u"},
    {"microsoft_xbox_v"},
    {"microsoft_xbox_w"},
    {"microsoft_xbox_x"},
    {"microsoft_xbox_y"},
    {"microsoft_xbox_z"},
};
const char* const kLabels[Catalog::kLetterCount] = {"#", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
                                                    "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};

struct Letter
{
    Catalog::State state = Catalog::State::Empty;
    std::vector<Catalog::Game> games;
    std::string message;
};

Lock s_lock;
Letter s_letters[Catalog::kLetterCount];
volatile LONG s_pending[Catalog::kLetterCount];
HANDLE s_wake;
volatile LONG s_started;
volatile LONG s_version;

// --- JSON de /metadata/<id>/files ------------------------------------------------------

// Lee una cadena JSON que empieza en `p` (tras la comilla). Avanza `p` hasta despues del cierre.
std::string jsonString(const char*& p, const char* end)
{
    std::string out;
    while (p < end && *p != '"') {
        if (*p == '\\' && p + 1 < end) {
            ++p;
            switch (*p) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'u': {
                if (p + 4 >= end) break;
                char hex[5] = {p[1], p[2], p[3], p[4], 0};
                const unsigned c = strtoul(hex, nullptr, 16);
                p += 4;
                if (c < 0x80) {
                    out += static_cast<char>(c);
                } else if (c < 0x800) {
                    out += static_cast<char>(0xC0 | (c >> 6));
                    out += static_cast<char>(0x80 | (c & 0x3F));
                } else {
                    out += static_cast<char>(0xE0 | (c >> 12));
                    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (c & 0x3F));
                }
                break;
            }
            default: out += *p; break;   // \" \\ \/
            }
            ++p;
            continue;
        }
        out += *p++;
    }
    if (p < end) ++p;
    return out;
}

// Valor de "clave": "..." dentro de un objeto [p, end).
bool field(const char* p, const char* end, const char* key, std::string& out)
{
    const size_t keyLength = strlen(key);
    for (; p + keyLength + 2 < end; ++p) {
        if (*p != '"' || strncmp(p + 1, key, keyLength) != 0 || p[keyLength + 1] != '"') continue;
        p += keyLength + 2;
        while (p < end && (*p == ' ' || *p == ':')) ++p;
        if (p >= end || *p != '"') return false;
        ++p;
        out = jsonString(p, end);
        return true;
    }
    return false;
}

// "Halo 2 (USA, Europe) (En,Ja,Fr)" -> titulo "Halo 2", region "USA, Europe".
// Los grupos que no son region ni idiomas (Beta, fechas, Rev 1...) se quedan en el titulo.
void splitName(const std::string& base, std::string& title, std::string& region)
{
    const size_t first = base.find(" (");
    title = base.substr(0, first);
    region.clear();
    size_t at = first;
    bool firstGroup = true;
    while (at != std::string::npos) {
        const size_t open = base.find('(', at);
        const size_t close = open == std::string::npos ? open : base.find(')', open);
        if (close == std::string::npos) break;
        const std::string group = base.substr(open + 1, close - open - 1);
        // Idiomas: "En,Fr,De" (dos letras separadas por comas).
        bool languages = group.size() >= 5;
        for (size_t i = 0; languages && i < group.size(); ++i)
            languages = (i % 3 == 2) ? group[i] == ',' : isalpha(static_cast<unsigned char>(group[i])) != 0;
        if (firstGroup) region = group;
        else if (!languages) title += " (" + group + ")";
        firstGroup = false;
        at = close + 1;
    }
}

void parseFiles(const std::vector<unsigned char>& json, const char* collection, std::vector<Catalog::Game>& out)
{
    const char* p = reinterpret_cast<const char*>(json.data());
    const char* end = p + json.size();
    const char* result = strstr(p, "\"result\"");
    if (!result) return;
    p = result;
    for (;;) {
        const char* open = static_cast<const char*>(memchr(p, '{', end - p));
        if (!open) break;
        const char* close = static_cast<const char*>(memchr(open, '}', end - open));
        if (!close) break;
        std::string name, size;
        if (field(open, close, "name", name) && name.size() > 4 &&
            _stricmp(name.c_str() + name.size() - 4, ".zip") == 0 && name[0] != '_') {
            Catalog::Game g;
            g.file = name;
            g.collection = collection;
            field(open, close, "size", size);
            g.size = _atoi64(size.c_str());
            const std::string base = name.substr(0, name.size() - 4);
            splitName(base, g.title, g.region);
            g.art = ArtIndex::find(base.c_str());
            out.push_back(g);
        }
        p = close + 1;
    }
}

bool lessByTitle(const Catalog::Game& a, const Catalog::Game& b)
{
    const int c = _stricmp(a.title.c_str(), b.title.c_str());
    return c != 0 ? c < 0 : a.region < b.region;
}

// --- carga --------------------------------------------------------------------------

void cachePath(const char* collection, char* out, int size)
{
    _snprintf(out, size, "%s\\lists\\%s.json", Files::cacheRoot(), collection);
    out[size - 1] = 0;
}

void publish(int letter, Catalog::State state, std::vector<Catalog::Game>* games, const std::string& message)
{
    Guard guard(s_lock);
    Letter& l = s_letters[letter];
    l.state = state;
    if (games) l.games.swap(*games);
    l.message = message;
    InterlockedIncrement(&s_version);
}

void loadLetter(int letter)
{
    const char* const* ids = kCollections[letter];

    // 1) La cache, si esta: se ve al instante.
    std::vector<Catalog::Game> cached;
    bool haveCache = true;
    for (int i = 0; i < 3 && ids[i]; ++i) {
        char path[160];
        cachePath(ids[i], path, sizeof(path));
        std::vector<unsigned char> json;
        if (!Files::readAll(path, json)) {
            haveCache = false;
            break;
        }
        parseFiles(json, ids[i], cached);
    }
    if (haveCache) {
        std::sort(cached.begin(), cached.end(), lessByTitle);
        publish(letter, Catalog::State::Ready, &cached, "Actualizando...");
    } else {
        publish(letter, Catalog::State::Loading, nullptr, "Cargando la lista de archive.org...");
    }

    // 2) La red.
    std::vector<Catalog::Game> fresh;
    for (int i = 0; i < 3 && ids[i]; ++i) {
        char url[160], error[128];
        _snprintf(url, sizeof(url), "https://archive.org/metadata/%s/files", ids[i]);
        unsigned char* data = nullptr;
        int length = 0;
        Http::Response response;
        if (!Http::getToMemory(url, nullptr, 2 * 1024 * 1024, &data, &length, &response, error, sizeof(error))) {
            if (haveCache) publish(letter, Catalog::State::Ready, nullptr, std::string("Lista guardada (") + error + ")");
            else publish(letter, Catalog::State::Failed, nullptr, error);
            return;
        }
        std::vector<unsigned char> json(data, data + length);
        free(data);
        parseFiles(json, ids[i], fresh);
        char path[160];
        cachePath(ids[i], path, sizeof(path));
        Files::writeAll(path, json.data(), json.size());
    }
    std::sort(fresh.begin(), fresh.end(), lessByTitle);
    publish(letter, Catalog::State::Ready, &fresh, "");
}

DWORD WINAPI worker(void*)
{
    char lists[160];
    _snprintf(lists, sizeof(lists), "%s\\lists", Files::cacheRoot());
    Files::makeDirectories(lists);
    for (;;) {
        WaitForSingleObject(s_wake, INFINITE);
        for (int i = 0; i < Catalog::kLetterCount; ++i)
            if (InterlockedExchange(&s_pending[i], 0)) loadLetter(i);
    }
}
} // namespace

namespace Catalog
{
const char* letterLabel(int index)
{
    return index >= 0 && index < kLetterCount ? kLabels[index] : "?";
}

void request(int letter)
{
    if (letter < 0 || letter >= kLetterCount) return;
    if (InterlockedCompareExchange(&s_started, 1, 0) == 0) {
        s_wake = CreateEvent(NULL, FALSE, FALSE, NULL);
        HANDLE h = CreateThread(NULL, 256 * 1024, worker, NULL, 0, NULL);
        if (h) CloseHandle(h);
    }
    {
        Guard guard(s_lock);
        Letter& l = s_letters[letter];
        if (l.state == State::Loading) return;
        if (l.state == State::Empty) l.state = State::Loading;
    }
    InterlockedExchange(&s_pending[letter], 1);
    SetEvent(s_wake);
}

long version()
{
    return s_version;
}

State snapshot(int letter, std::vector<Game>& games, std::string& message)
{
    Guard guard(s_lock);
    const Letter& l = s_letters[letter];
    games = l.games;
    message = l.message;
    return l.state;
}

std::string urlEncode(const std::string& text)
{
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

std::string downloadUrl(const Game& game)
{
    return "https://archive.org/download/" + game.collection + "/" + urlEncode(game.file);
}
} // namespace Catalog
