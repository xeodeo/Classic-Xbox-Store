#pragma once

#include <string>
#include <vector>

// Archivos y carpetas con XAPI. Rutas tipo "T:\cache\art\x.jpg".
namespace Files
{
bool exists(const char* path);
bool readAll(const char* path, std::vector<unsigned char>& out);
// Escribe a un temporal y lo renombra: un corte a mitad no deja un archivo roto.
bool writeAll(const char* path, const void* data, size_t length);
// Escribe y obliga a que llegue al disco (FlushFileBuffers), sin renombrar: para
// lo que tiene que sobrevivir a un apagado o a salir con la combinacion de botones.
bool writeDurable(const char* path, const void* data, size_t length);
// Crea la carpeta y las que falten por encima.
void makeDirectories(const char* path);
// Raiz para la cache: T:\ (E:\TDATA\<id del titulo>), que XAPI monta solo.
const char* cacheRoot();
}

// Cerrojo para datos que comparten el hilo de la pantalla y los de trabajo.
class Lock
{
public:
    Lock();
    ~Lock();
    void enter();
    void leave();

private:
    void* m_cs;
};

class Guard
{
public:
    explicit Guard(Lock& lock) : m_lock(lock) { m_lock.enter(); }
    ~Guard() { m_lock.leave(); }

private:
    Lock& m_lock;
};
