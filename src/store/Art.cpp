#include "store/Art.h"

#include "XboxXtl.h"
#include "net/Http.h"
#include "system/Files.h"
#include "ui/ImageDecode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

namespace
{
Lock s_lock;
const ArtIndex::Entry* s_wanted;
std::vector<Art::Pixels> s_done;
HANDLE s_wake;
volatile LONG s_started;

const ArtIndex::Entry* wanted()
{
    Guard guard(s_lock);
    return s_wanted;
}

// "AC/AC01301E.jpg" -> "T:\cache\art\thumbnail\AC_AC01301E.jpg"
void cacheFile(const char* folder, const char* path, char* out, int size)
{
    _snprintf(out, size, "%s\\art\\%s\\%s", Files::cacheRoot(), folder, path);
    out[size - 1] = 0;
    for (char* p = out + strlen(Files::cacheRoot()) + 5 + strlen(folder) + 1; *p; ++p)
        if (*p == '/') *p = '_';
}

// Del disco o de la red. Devuelve false si no hay o fallo.
bool fetch(const char* folder, const char* path, std::vector<unsigned char>& bytes)
{
    char file[200];
    cacheFile(folder, path, file, sizeof(file));
    if (Files::readAll(file, bytes) && !bytes.empty()) return true;

    char url[300], error[128];
    ArtIndex::url(folder, path, url, sizeof(url));
    unsigned char* data = nullptr;
    int length = 0;
    Http::Response response;
    if (!Http::getToMemory(url, nullptr, 4 * 1024 * 1024, &data, &length, &response, error, sizeof(error)))
        return false;
    bytes.assign(data, data + length);
    free(data);
    Files::writeAll(file, bytes.data(), bytes.size());
    return true;
}

void load(const ArtIndex::Entry* entry, Art::Kind kind)
{
    const char* folder = kind == Art::kCover ? "thumbnail" : "background";
    const char* path = kind == Art::kCover ? entry->thumbnail : entry->background;
    if (!path[0]) return;
    std::vector<unsigned char> bytes;
    if (!fetch(folder, path, bytes) || wanted() != entry) return;
    Art::Pixels px;
    px.rgba = ImageDecode::decode(bytes.data(), static_cast<int>(bytes.size()), &px.width, &px.height);
    if (!px.rgba) return;
    px.kind = kind;
    px.entry = entry;
    Guard guard(s_lock);
    s_done.push_back(px);
}

DWORD WINAPI worker(void*)
{
    char dir[160];
    _snprintf(dir, sizeof(dir), "%s\\art\\thumbnail", Files::cacheRoot());
    Files::makeDirectories(dir);
    _snprintf(dir, sizeof(dir), "%s\\art\\background", Files::cacheRoot());
    Files::makeDirectories(dir);
    const ArtIndex::Entry* last = nullptr;
    for (;;) {
        WaitForSingleObject(s_wake, INFINITE);
        const ArtIndex::Entry* entry = wanted();
        if (!entry || entry == last) continue;
        last = entry;
        load(entry, Art::kCover);   // la portada primero: es lo que mas se nota
        if (wanted() == entry) load(entry, Art::kBackground);
        if (wanted() != entry) SetEvent(s_wake);   // cambio mientras tanto: otra vuelta
    }
}
} // namespace

namespace Art
{
void want(const ArtIndex::Entry* entry)
{
    if (InterlockedCompareExchange(&s_started, 1, 0) == 0) {
        s_wake = CreateEvent(NULL, FALSE, FALSE, NULL);
        HANDLE h = CreateThread(NULL, 256 * 1024, worker, NULL, 0, NULL);
        if (h) CloseHandle(h);
    }
    {
        Guard guard(s_lock);
        if (s_wanted == entry) return;
        s_wanted = entry;
    }
    SetEvent(s_wake);
}

bool take(Pixels& out)
{
    Guard guard(s_lock);
    if (s_done.empty()) return false;
    out = s_done.front();
    s_done.erase(s_done.begin());
    return true;
}
} // namespace Art
