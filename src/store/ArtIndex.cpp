#include "store/ArtIndex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const unsigned char kArtIndex[];
extern const unsigned int kArtIndex_size;

namespace
{
ArtIndex::Entry* s_entries;
int s_count;

// Parte la tabla una vez: copia el texto y cambia tabuladores y saltos por 0.
void load()
{
    if (s_entries) return;
    char* text = static_cast<char*>(malloc(kArtIndex_size + 1));
    memcpy(text, kArtIndex, kArtIndex_size);
    text[kArtIndex_size] = 0;
    int lines = 0;
    for (unsigned i = 0; i < kArtIndex_size; ++i)
        if (text[i] == '\n') ++lines;
    s_entries = static_cast<ArtIndex::Entry*>(calloc(lines + 1, sizeof(ArtIndex::Entry)));
    char* p = text;
    while (*p) {
        char* fields[6] = {};
        for (int f = 0; f < 6; ++f) {
            fields[f] = p;
            while (*p && *p != '\t' && *p != '\n') ++p;
            const bool endOfLine = *p != '\t';
            if (*p) *p++ = 0;
            if (endOfLine) {
                for (int rest = f + 1; rest < 6; ++rest) fields[rest] = p - 1;   // "" (el 0 recien puesto)
                break;
            }
        }
        ArtIndex::Entry& e = s_entries[s_count++];
        e.name = fields[0];
        e.xmid = fields[1];
        e.titleId = fields[2];
        e.thumbnail = fields[3];
        e.background = fields[4];
        e.icon = fields[5];
    }
}

int compare(const void* key, const void* item)
{
    return strcmp(static_cast<const char*>(key), static_cast<const ArtIndex::Entry*>(item)->name);
}
} // namespace

namespace ArtIndex
{
const Entry* find(const char* redumpName)
{
    load();
    return static_cast<const Entry*>(bsearch(redumpName, s_entries, s_count, sizeof(Entry), compare));
}

void url(const char* folder, const char* path, char* out, int size)
{
    out[0] = 0;
    if (!path || !path[0]) return;
    _snprintf(out, size, "https://raw.githubusercontent.com/MobCat/MobCats-original-xbox-game-list/main/%s/%s",
              folder, path);
    out[size - 1] = 0;
}
} // namespace ArtIndex
