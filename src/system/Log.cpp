// Registro a archivo y captador de fallos (adaptado de XboxCrashLog de OptiCraft,
// que manda lo mismo por la red).
#include "system/Log.h"

#include "XboxXtl.h"

#include <exception>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace
{
const char kPath[] = "T:\\cxs_log.txt";

volatile LONG s_lock;
const int kRecent = 16;
char s_recent[kRecent][128];
int s_recentNext;
int s_recentCount;
DWORD s_mainThreadId;
volatile LONG s_reported;
char s_crashText[160];

void lock()
{
    while (InterlockedCompareExchange(&s_lock, 1, 0) != 0) Sleep(1);
}

void unlock()
{
    InterlockedExchange(&s_lock, 0);
}

void append(const char* text, int length)
{
    HANDLE f = CreateFileA(kPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    SetFilePointer(f, 0, NULL, FILE_END);
    DWORD written = 0;
    WriteFile(f, text, length, &written, NULL);
    CloseHandle(f);   // cerrado en cada linea: si la consola se cuelga, lo escrito queda
}

// Direcciones dentro del XBE cargado (base 0x10000); en el .map salen 0x3F0000 mas arriba.
bool looksLikeCode(DWORD value)
{
    return value >= 0x00011000 && value < 0x00800000;
}

void logStackWords(DWORD esp)
{
    char line[256];
    int length = 0, found = 0;
    const DWORD* stack = reinterpret_cast<const DWORD*>(esp);
    for (int i = 0; i < 512 && found < 24; ++i) {
        DWORD value = 0;
        __try {
            value = stack[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
        if (!looksLikeCode(value)) continue;
        ++found;
        length += _snprintf(line + length, sizeof(line) - length, " %08lx", value + 0x3F0000);
        if (length > 200) {
            Log::write("pila (direcciones del .map):%s", line);
            length = 0;
        }
    }
    if (length > 0) Log::write("pila (direcciones del .map):%s", line);
}

[[noreturn]] void park()
{
    // El hilo se queda quieto: si era de trabajo, la pantalla sigue y enseña el fallo.
    for (;;) Sleep(1000);
}

LONG WINAPI unhandledFilter(EXCEPTION_POINTERS* info)
{
    const DWORD thread = GetCurrentThreadId();
    if (InterlockedExchange(&s_reported, 1) != 0) park();
    const EXCEPTION_RECORD* record = info ? info->ExceptionRecord : nullptr;
    const CONTEXT* context = info ? info->ContextRecord : nullptr;
    const DWORD mapAddress = record ? reinterpret_cast<DWORD>(record->ExceptionAddress) + 0x3F0000 : 0;
    _snprintf(s_crashText, sizeof(s_crashText), "Fallo %08lx en el hilo %s (mapa %08lx)",
              record ? record->ExceptionCode : 0, thread == s_mainThreadId ? "principal" : "de trabajo", mapAddress);
    Log::write("FALLO: excepcion %08lx en %p, hilo %s %lu (direccion en el .map %08lx)",
               record ? record->ExceptionCode : 0, record ? record->ExceptionAddress : nullptr,
               thread == s_mainThreadId ? "principal" : "de trabajo", thread, mapAddress);
    if (record && record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
        Log::write("acceso invalido: %s %08lx", record->ExceptionInformation[0] ? "escritura en" : "lectura de",
                   static_cast<DWORD>(record->ExceptionInformation[1]));
    if (context) {
        Log::write("eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx eip=%08lx esp=%08lx ebp=%08lx",
                   context->Eax, context->Ebx, context->Ecx, context->Edx, context->Esi, context->Edi, context->Eip,
                   context->Esp, context->Ebp);
        logStackWords(context->Esp);
    }
    park();
}

[[noreturn]] void terminateHandler()
{
    const char* what = "desconocida";
    try {
        const std::exception_ptr current = std::current_exception();
        if (current) std::rethrow_exception(current);
        what = "std::terminate sin excepcion";
    } catch (const std::exception& e) {
        _snprintf(s_crashText, sizeof(s_crashText), "Excepcion: %s", e.what());
        Log::write("FALLO: excepcion C++ sin capturar: %s", e.what());
        park();
    } catch (...) {
        what = "excepcion que no es std";
    }
    _snprintf(s_crashText, sizeof(s_crashText), "Excepcion: %s", what);
    Log::write("FALLO: excepcion C++ sin capturar: %s", what);
    park();
}
} // namespace

namespace Log
{
void write(const char* format, ...)
{
    char line[512];
    const DWORD now = GetTickCount();
    int length = _snprintf(line, sizeof(line), "[%6lu.%03lu] ", now / 1000, now % 1000);
    va_list args;
    va_start(args, format);
    const int n = _vsnprintf(line + length, sizeof(line) - length - 3, format, args);
    va_end(args);
    length = n < 0 ? static_cast<int>(sizeof(line)) - 3 : length + n;
    line[length++] = '\r';
    line[length++] = '\n';
    lock();
    append(line, length);
    _snprintf(s_recent[s_recentNext], sizeof(s_recent[0]), "%.*s", length - 2, line);
    s_recent[s_recentNext][sizeof(s_recent[0]) - 1] = 0;
    s_recentNext = (s_recentNext + 1) % kRecent;
    if (s_recentCount < kRecent) ++s_recentCount;
    unlock();
}

void installCrashHandler()
{
    s_mainThreadId = GetCurrentThreadId();
    SetUnhandledExceptionFilter(unhandledFilter);
    std::set_terminate(terminateHandler);
    Log::write("--- Classic Xbox Store arranca ---");
}

int recent(char lines[][128], int maxLines)
{
    lock();
    const int n = s_recentCount < maxLines ? s_recentCount : maxLines;
    for (int i = 0; i < n; ++i) {
        const int index = (s_recentNext - n + i + kRecent) % kRecent;
        memcpy(lines[i], s_recent[index], sizeof(s_recent[0]));
    }
    unlock();
    return n;
}

const char* crashText()
{
    return s_crashText;
}
} // namespace Log
