#pragma once

// Idioma de la interfaz: ingles (por defecto), español o portugues.
//
// Los textos se escriben en español en el codigo y pasan por tr(), que devuelve
// la version del idioma elegido (tabla en LangTable.inc, generada desde
// scripts/translations.py). Si un texto no esta en la tabla, sale tal cual.
namespace Lang
{
enum Language { kEnglish, kSpanish, kPortuguese, kCount };

Language current();
void set(Language language);
const char* name(Language language);     // "English", "Español", "Português"
const char* code(Language language);     // "en", "es", "pt" (para los ajustes)
Language fromCode(const char* code);     // desconocido: ingles
}

// El texto en el idioma actual. `spanish` debe ser el texto exacto del codigo.
const char* tr(const char* spanish);
