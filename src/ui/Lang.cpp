#include "ui/Lang.h"

#include <stdlib.h>
#include <string.h>

namespace
{
struct Entry
{
    const char* spanish;
    const char* english;
    const char* portuguese;
};

// Ordenada por bytes del texto en español (la genera scripts/make_lang.py).
const Entry kTable[] = {
#include "ui/LangTable.inc"
};
const int kCount = sizeof(kTable) / sizeof(kTable[0]);

volatile Lang::Language s_language = Lang::kEnglish;

int compare(const void* key, const void* item)
{
    return strcmp(static_cast<const char*>(key), static_cast<const Entry*>(item)->spanish);
}
} // namespace

namespace Lang
{
Language current()
{
    return s_language;
}

void set(Language language)
{
    if (language >= 0 && language < kCount) s_language = language;
}

const char* name(Language language)
{
    switch (language) {
    case kSpanish: return "Español";
    case kPortuguese: return "Português";
    default: return "English";
    }
}

const char* code(Language language)
{
    switch (language) {
    case kSpanish: return "es";
    case kPortuguese: return "pt";
    default: return "en";
    }
}

Language fromCode(const char* text)
{
    if (text && strcmp(text, "es") == 0) return kSpanish;
    if (text && strcmp(text, "pt") == 0) return kPortuguese;
    return kEnglish;
}
} // namespace Lang

const char* tr(const char* spanish)
{
    const Lang::Language language = s_language;
    if (!spanish || language == Lang::kSpanish) return spanish;
    const Entry* e = static_cast<const Entry*>(bsearch(spanish, kTable, kCount, sizeof(Entry), compare));
    if (!e) return spanish;
    return language == Lang::kPortuguese ? e->portuguese : e->english;
}
