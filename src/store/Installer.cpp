#include "store/Installer.h"

#include "XboxXtl.h"
#include "net/Http.h"
#include "store/Session.h"
#include "store/Settings.h"
#include "system/Files.h"
#include "system/Log.h"

#include "zlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

namespace
{
const long long kPartSize = 0xF0000000LL;        // 3,75 GB: FATX no admite archivos de 4 GB
const long long kRedumpGameOffset = 0x18300000LL; // particion del juego en una imagen redump
const long long kHeaderOffset = 0x10000;          // cabecera XDVDFS (sector 32)
const int kSector = 2048;
const char kMagic[] = "MICROSOFT*XBOX*MEDIA";
const int kMagicLength = 20;
const int kMaxParts = 4;

Lock s_lock;
Installer::Progress s_progress;
volatile LONG s_busy;
volatile LONG s_cancel;
Catalog::Game s_game;
char s_drive;

void setPhase(Installer::Phase phase, const char* text)
{
    Guard guard(s_lock);
    s_progress.phase = phase;
    _snprintf(s_progress.text, sizeof(s_progress.text), "%s", text);
    s_progress.text[sizeof(s_progress.text) - 1] = 0;
}

void setCounters(long long done, long long total, float speed)
{
    Guard guard(s_lock);
    s_progress.done = done;
    s_progress.total = total;
    s_progress.speed = speed;
}

void fail(const char* text)
{
    Log::write("instalar: FALLO: %s", text);
    setPhase(Installer::Phase::Failed, text);
}

long long freeBytes(char drive)
{
    char root[4] = {drive, ':', '\\', 0};
    ULARGE_INTEGER freeToCaller, total, freeTotal;
    if (!GetDiskFreeSpaceExA(root, &freeToCaller, &total, &freeTotal)) return 0;
    return static_cast<long long>(freeToCaller.QuadPart);
}

void formatGb(long long bytes, char* out, int size)
{
    _snprintf(out, size, "%.1f GB", double(bytes) / (1024.0 * 1024.0 * 1024.0));
    out[size - 1] = 0;
}

// --- trozos de la particion del juego en el disco --------------------------------------

class Parts
{
public:
    explicit Parts(const char* folder)
    {
        _snprintf(m_folder, sizeof(m_folder), "%s", folder);
        m_folder[sizeof(m_folder) - 1] = 0;
        for (int i = 0; i < kMaxParts; ++i) m_write[i] = m_read[i] = INVALID_HANDLE_VALUE;
    }

    ~Parts()
    {
        close();
        free(m_buffer);
    }

    bool create()
    {
        Files::makeDirectories(m_folder);
        removeFiles();
        return Files::exists(m_folder);
    }

    // Para seguir una descarga: usa los trozos que ya hay, recortados a `written`
    // bytes (lo que habia en disco en el ultimo punto de control).
    bool openExisting(long long written)
    {
        for (int part = 0; part < kMaxParts; ++part) {
            const long long start = part * kPartSize;
            char path[128];
            partPath(part, path, sizeof(path));
            if (start >= written) {
                DeleteFileA(path);   // lo de despues del punto de control sobra
                continue;
            }
            const long long size = written - start < kPartSize ? written - start : kPartSize;
            HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            if (h == INVALID_HANDLE_VALUE) return false;
            LONG high = 0;
            if (SetFilePointer(h, static_cast<LONG>(size & 0xFFFFFFFF), &high, FILE_BEGIN) == INVALID_SET_FILE_POINTER &&
                GetLastError() != NO_ERROR) {
                CloseHandle(h);
                return false;
            }
            if (!SetEndOfFile(h)) {
                CloseHandle(h);
                return false;
            }
            if (size == kPartSize) CloseHandle(h);   // trozo lleno: no se vuelve a escribir
            else m_write[part] = h;                  // aqui sigue escribiendo
        }
        m_flushed = written;
        m_accepted = written;
        return true;
    }

    // Bytes ya escritos en disco y legibles con read().
    long long flushed() const { return m_flushed; }
    // Bytes aceptados (en disco, en cola o en el bufer).
    long long written() const { return m_accepted; }

    // Copia los datos a un bufer; los bufer llenos los escribe otro hilo, asi
    // descargar y descomprimir no se paran mientras el disco trabaja.
    bool write(const unsigned char* data, int length)
    {
        while (length > 0) {
            if (m_failed) return false;
            if (!m_buffer) m_buffer = static_cast<unsigned char*>(malloc(kBuffer));
            if (!m_buffer) return false;
            const int n = length < kBuffer - m_fill ? length : kBuffer - m_fill;
            memcpy(m_buffer + m_fill, data, n);
            m_fill += n;
            m_accepted += n;
            data += n;
            length -= n;
            if (m_fill == kBuffer && !queueBuffer()) return false;
        }
        return true;
    }

    // Obliga a que lo escrito llegue de verdad al disco (no solo a la cache de la
    // Xbox): un punto de control tiene que sobrevivir a un apagado.
    bool sync()
    {
        if (!flush()) return false;
        for (int i = 0; i < kMaxParts; ++i)
            if (m_write[i] != INVALID_HANDLE_VALUE && !FlushFileBuffers(m_write[i])) return false;
        return true;
    }

    // Espera a que todo lo aceptado este en disco.
    bool flush()
    {
        if (m_fill > 0 && !queueBuffer()) return false;
        for (;;) {
            {
                Guard guard(m_lock);
                if (m_queue.empty() && !m_writing) break;
            }
            if (m_failed) return false;
            Sleep(2);
        }
        return !m_failed;
    }

    // Lee de lo ya escrito (offset dentro de la particion del juego).
    bool read(long long offset, void* out, int length)
    {
        unsigned char* dst = static_cast<unsigned char*>(out);
        while (length > 0) {
            if (offset + length > m_flushed) return false;
            const int part = static_cast<int>(offset / kPartSize);
            if (m_read[part] == INVALID_HANDLE_VALUE) {
                char path[128];
                partPath(part, path, sizeof(path));
                m_read[part] = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                                           0, NULL);
                if (m_read[part] == INVALID_HANDLE_VALUE) return false;
            }
            const long long inPart = offset % kPartSize;
            LONG high = 0;
            if (SetFilePointer(m_read[part], static_cast<LONG>(inPart & 0xFFFFFFFF), &high, FILE_BEGIN) ==
                    INVALID_SET_FILE_POINTER &&
                GetLastError() != NO_ERROR)
                return false;
            const long long room = kPartSize - inPart;
            const DWORD n = static_cast<DWORD>(length < room ? length : room);
            DWORD got = 0;
            if (!ReadFile(m_read[part], dst, n, &got, NULL) || got != n) return false;
            offset += n;
            dst += n;
            length -= n;
        }
        return true;
    }

    void close()
    {
        stopWriter();
        for (int i = 0; i < kMaxParts; ++i) {
            if (m_write[i] != INVALID_HANDLE_VALUE) CloseHandle(m_write[i]);
            if (m_read[i] != INVALID_HANDLE_VALUE) CloseHandle(m_read[i]);
            m_write[i] = m_read[i] = INVALID_HANDLE_VALUE;
        }
    }

    void removeAll()
    {
        close();
        removeFiles();
        RemoveDirectoryA(m_folder);
    }

private:
    static const int kBuffer = 512 * 1024;
    static const int kMaxQueued = 12;   // 6 MB esperando al disco como mucho

    struct Pending
    {
        unsigned char* data;
        int length;
    };

    void partPath(int part, char* out, int size) const
    {
        _snprintf(out, size, "%s\\part%d.bin", m_folder, part);
        out[size - 1] = 0;
    }

    void removeFiles()
    {
        for (int i = 0; i < kMaxParts; ++i) {
            char path[128];
            partPath(i, path, sizeof(path));
            DeleteFileA(path);
        }
    }

    // Entrega el bufer lleno al hilo escritor (lo arranca la primera vez).
    bool queueBuffer()
    {
        if (!m_writer) {
            m_stop = 0;
            m_wake = CreateEvent(NULL, FALSE, FALSE, NULL);
            m_writer = CreateThread(NULL, 64 * 1024, writerThread, this, 0, NULL);
            if (!m_writer) return writeNow(m_buffer, m_fill);   // sin hilo: escribir aqui
        }
        // Si el disco va muy por detras, esperar (no llenar la memoria).
        for (;;) {
            {
                Guard guard(m_lock);
                if (static_cast<int>(m_queue.size()) < kMaxQueued) {
                    m_queue.push_back(Pending{m_buffer, m_fill});
                    break;
                }
            }
            if (m_failed) return false;
            Sleep(2);
        }
        m_buffer = nullptr;
        m_fill = 0;
        SetEvent(m_wake);
        return !m_failed;
    }

    static DWORD WINAPI writerThread(void* param)
    {
        Parts& p = *static_cast<Parts*>(param);
        for (;;) {
            Pending next = {nullptr, 0};
            {
                Guard guard(p.m_lock);
                if (!p.m_queue.empty()) {
                    next = p.m_queue.front();
                    p.m_queue.erase(p.m_queue.begin());
                    p.m_writing = true;
                }
            }
            if (!next.data) {
                if (p.m_stop) return 0;
                WaitForSingleObject(p.m_wake, 50);
                continue;
            }
            if (!p.m_failed && !p.writeNow(next.data, next.length)) p.m_failed = true;
            free(next.data);
            Guard guard(p.m_lock);
            p.m_writing = false;
        }
    }

    bool writeNow(const unsigned char* data, int length)
    {
        while (length > 0) {
            const int part = static_cast<int>(m_flushed / kPartSize);
            if (part >= kMaxParts) return false;
            if (m_write[part] == INVALID_HANDLE_VALUE) {
                char path[128];
                partPath(part, path, sizeof(path));
                m_write[part] = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
                if (m_write[part] == INVALID_HANDLE_VALUE) return false;
            }
            const long long room = kPartSize - m_flushed % kPartSize;
            const DWORD n = static_cast<DWORD>(length < room ? length : room);
            DWORD done = 0;
            if (!WriteFile(m_write[part], data, n, &done, NULL) || done != n) return false;
            m_flushed += n;
            data += n;
            length -= n;
            if (m_flushed % kPartSize == 0) {   // trozo lleno
                CloseHandle(m_write[part]);
                m_write[part] = INVALID_HANDLE_VALUE;
            }
        }
        return true;
    }

    void stopWriter()
    {
        if (!m_writer) return;
        m_stop = 1;
        SetEvent(m_wake);
        WaitForSingleObject(m_writer, INFINITE);
        CloseHandle(m_writer);
        CloseHandle(m_wake);
        m_writer = NULL;
        m_wake = NULL;
        for (const Pending& p : m_queue) free(p.data);   // lo que no llego a escribirse (cancelar)
        m_queue.clear();
    }

    char m_folder[96];
    HANDLE m_write[kMaxParts];
    HANDLE m_read[kMaxParts];
    unsigned char* m_buffer = nullptr;
    int m_fill = 0;
    long long m_accepted = 0;
    volatile long long m_flushed = 0;   // solo lo cambia el escritor
    Lock m_lock;
    std::vector<Pending> m_queue;
    bool m_writing = false;
    HANDLE m_writer = NULL;
    HANDLE m_wake = NULL;
    volatile LONG m_stop = 0;
    volatile bool m_failed = false;
};

// --- XDVDFS: el sistema de archivos del disco de Xbox ---------------------------------

struct FileEntry
{
    std::string path;   // relativa: "media\\sound.xwb"
    unsigned sector;
    unsigned size;
    bool directory;
};

enum class Parse { Ok, NeedMore, Bad };

unsigned u32(const unsigned char* p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<unsigned>(p[3]) << 24);
}

unsigned u16(const unsigned char* p)
{
    return p[0] | (p[1] << 8);
}

struct TreeReader
{
    Parts& parts;
    std::vector<FileEntry>& files;
    long long end = 0;      // donde acaba el ultimo dato util
    int visited = 0;

    Parse directory(unsigned sector, unsigned size, const std::string& prefix, int depth)
    {
        if (size == 0) return Parse::Ok;
        if (depth > 32 || size > 4 * 1024 * 1024) return Parse::Bad;
        const long long offset = static_cast<long long>(sector) * kSector;
        if (offset + size > parts.flushed()) return Parse::NeedMore;
        std::vector<unsigned char> table(size);
        if (!parts.read(offset, table.data(), static_cast<int>(size))) return Parse::NeedMore;
        if (offset + size > end) end = offset + size;
        return node(table, 0, prefix, depth);
    }

    // Arbol binario: izquierda, el nodo, derecha (desplazamientos en grupos de 4 bytes).
    Parse node(const std::vector<unsigned char>& table, unsigned at, const std::string& prefix, int depth)
    {
        if (++visited > 200000 || at + 14 > table.size()) return Parse::Bad;
        const unsigned char* e = table.data() + at;
        const unsigned left = u16(e), right = u16(e + 2);
        if (left == 0xFFFF) return Parse::Ok;   // relleno: directorio vacio
        const unsigned sector = u32(e + 4), size = u32(e + 8);
        const unsigned char attributes = e[12], nameLength = e[13];
        if (at + 14 + nameLength > table.size() || nameLength == 0) return Parse::Bad;
        if (left) {
            const Parse r = node(table, left * 4, prefix, depth);
            if (r != Parse::Ok) return r;
        }
        FileEntry f;
        f.path = prefix + std::string(reinterpret_cast<const char*>(e + 14), nameLength);
        f.sector = sector;
        f.size = size;
        f.directory = (attributes & 0x10) != 0;
        files.push_back(f);
        if (f.directory) {
            const Parse r = directory(sector, size, f.path + "\\", depth + 1);
            if (r != Parse::Ok) return r;
        } else {
            const long long fileEnd = static_cast<long long>(sector) * kSector + size;
            if (fileEnd > end) end = fileEnd;
        }
        if (right) return node(table, right * 4, prefix, depth);
        return Parse::Ok;
    }
};

Parse readTree(Parts& parts, std::vector<FileEntry>& files, long long* end)
{
    files.clear();
    unsigned char header[kSector];
    if (parts.flushed() < kHeaderOffset + kSector || !parts.read(kHeaderOffset, header, kSector)) return Parse::NeedMore;
    if (memcmp(header, kMagic, kMagicLength) != 0 || memcmp(header + kSector - kMagicLength, kMagic, kMagicLength) != 0)
        return Parse::Bad;
    TreeReader reader{parts, files};
    const Parse r = reader.directory(u32(header + 20), u32(header + 24), "", 0);
    *end = reader.end;
    return r;
}

// --- descarga ---------------------------------------------------------------------------
//
// Se baja el ZIP (no la ISO de dentro: archive.org la saca muy despacio y no
// deja reanudarla) y se descomprime aqui con zlib. El ZIP si admite Range, asi
// que si la conexion se corta se pide desde el byte exacto donde iba y zlib
// sigue como si nada: su estado esta en memoria.

struct Download
{
    Parts* parts;
    // ZIP
    long long dataStart = 0;         // donde empiezan los datos comprimidos de la ISO
    long long consumed = 0;          // bytes comprimidos ya usados
    long long isoSize = -1;          // tamaño de la ISO (si el ZIP lo dice)
    bool stored = false;             // ZIP sin comprimir
    z_stream zs;
    unsigned char* out = nullptr;
    bool streamEnd = false;
    std::string dataUrl;             // URL final del ZIP (tras las redirecciones)
    const volatile LONG* fetchedKB = nullptr;   // KB bajados por las conexiones (velocidad)
    // ISO
    long long position = 0;          // en la ISO entera
    long long gameOffset = -1;       // donde empieza la particion del juego (-1: aun no se sabe)
    std::vector<unsigned char> head; // el principio, hasta saber gameOffset
    long long needed = -1;           // bytes de particion que hacen falta (tras leer el arbol)
    long long nextParse = 0;
    bool enough = false;
    char error[160] = "";
    std::vector<FileEntry> files;
    // Avisos
    DWORD speedTick = 0;
    long long speedBytes = 0;
    float speed = 0;
    DWORD reportTick = 0;
    // Donde se va el tiempo (ms), para el renglon de medidas.
    DWORD startTick = 0;
    DWORD inflateMs = 0;
    DWORD writeMs = 0;
    DWORD waitMs = 0;          // el que descomprime, esperando trozos de la red
    DWORD logTick = 0;
    // Seguir despues
    unsigned char ring[32768];         // ultimos 32 KB de ISO (ventana de zlib)
    long long lastCheckpoint = 0;
    bool networkFailure = false;       // la red fallo: lo descargado se puede seguir
};

const int kInflateChunk = 256 * 1024;

// --- seguir una descarga a medias -----------------------------------------------------
//
// Cada 128 MB de ISO se guarda un punto de control en T:\cxs_resume.bin, en un
// limite entre bloques deflate (como el ejemplo zran de zlib): donde va el ZIP,
// los bits sueltos del ultimo byte, donde va la ISO, lo guardado en disco y los
// ultimos 32 KB de la ISO (el diccionario de zlib). Con eso se puede seguir
// descomprimiendo desde ahi aunque la consola se haya apagado.

const char kResumePath[] = "T:\\cxs_resume.bin";
const unsigned kResumeMagic = 0x32535843;   // "CXS2"
const long long kCheckpointEvery = 128LL * 1024 * 1024;
const int kWindow = 32768;

struct ResumeState
{
    unsigned magic;
    char file[256];
    char title[96];
    char region[64];
    char collection[64];
    long long zipSize;
    char drive;
    char folder[96];
    long long dataStart;
    long long consumed;          // bytes comprimidos usados (desde dataStart)
    int bits;                    // bits sin usar del ultimo byte leido (inflatePrime)
    long long position;          // bytes de ISO producidos
    long long gameOffset;
    long long written;           // bytes de particion en los trozos
    int stage;                   // 0 descargando, 1 ya descargado: solo falta copiar los archivos
    unsigned char window[kWindow];
};

bool loadResume(ResumeState& out)
{
    std::vector<unsigned char> bytes;
    if (!Files::readAll(kResumePath, bytes) || bytes.size() != sizeof(ResumeState)) return false;
    memcpy(&out, bytes.data(), sizeof(ResumeState));
    return out.magic == kResumeMagic;
}

void deleteResume()
{
    DeleteFileA(kResumePath);
}

char s_folder[96];   // carpeta final de la instalacion en curso

bool feed(Download& d, const unsigned char* data, int length)
{
    // La parte que cae dentro de la particion del juego va a los trozos.
    const long long end = d.position + length;
    if (end > d.gameOffset) {
        const long long skip = d.gameOffset > d.position ? d.gameOffset - d.position : 0;
        const DWORD t = GetTickCount();
        const bool written = d.parts->write(data + skip, static_cast<int>(length - skip));
        d.writeMs += GetTickCount() - t;
        if (!written) {
            _snprintf(d.error, sizeof(d.error), "No se pudo escribir en el disco (¿lleno?)");
            return false;
        }
    }
    d.position = end;
    return true;
}

// Bytes de la ISO ya descomprimidos. Falso para parar (error o ya basta).
bool iso(Download& d, const unsigned char* data, int length)
{
    // Hasta tener la cabecera no se sabe si es redump (con particion de video) o ya un XISO.
    if (d.gameOffset < 0) {
        d.head.insert(d.head.end(), data, data + length);
        if (d.head.size() < kHeaderOffset + kMagicLength) return true;
        d.gameOffset = memcmp(d.head.data() + kHeaderOffset, kMagic, kMagicLength) == 0 ? 0 : kRedumpGameOffset;
        std::vector<unsigned char> head;
        head.swap(d.head);
        if (!feed(d, head.data(), static_cast<int>(head.size()))) return false;
    } else if (!feed(d, data, length)) {
        return false;
    }

    const long long written = d.parts->written();
    // Cada 64 MB se intenta leer el arbol de archivos: en cuanto se lee, se sabe donde parar.
    if (d.needed < 0 && written >= d.nextParse && written > kHeaderOffset + kSector) {
        d.nextParse = written + 64LL * 1024 * 1024;
        if (!d.parts->flush()) {
            _snprintf(d.error, sizeof(d.error), "No se pudo escribir en el disco (¿lleno?)");
            return false;
        }
        long long end = 0;
        const Parse r = readTree(*d.parts, d.files, &end);
        if (r == Parse::Bad) {
            _snprintf(d.error, sizeof(d.error), "La ISO no tiene el formato de un disco de Xbox");
            return false;
        }
        if (r == Parse::Ok) {
            d.needed = end;
            Log::write("instalar: arbol leido: %d entradas, hace falta hasta %I64d de la particion",
                       static_cast<int>(d.files.size()), d.needed);
            // Ahora ya se sabe cuanto falta: lo que queda por bajar + el juego instalado.
            long long gameBytes = 0;
            for (const FileEntry& f : d.files)
                if (!f.directory) gameBytes += f.size;
            const long long need = (d.needed - written) + gameBytes + 64LL * 1024 * 1024;
            if (freeBytes(s_drive) < need) {
                char gb[24];
                formatGb(need, gb, sizeof(gb));
                _snprintf(d.error, sizeof(d.error), "No hay espacio en %c: (faltan unos %s)", s_drive, gb);
                return false;
            }
        }
    }
    if (d.needed > 0 && written >= d.needed) {
        d.enough = true;   // el resto es relleno
        return false;
    }
    return true;
}

// Progreso en bytes del ZIP de verdad descargados: los primeros 387 MB de la
// ISO (la parte de video) no se guardan, y contar solo lo guardado parecia parado.
void report(Download& d)
{
    const DWORD now = GetTickCount();
    // Velocidad: lo que bajan las conexiones, no lo ya descomprimido.
    const long long fetched = d.fetchedKB ? static_cast<long long>(*d.fetchedKB) * 1024 : d.consumed;
    if (!d.speedTick) {
        d.speedTick = now;
        d.speedBytes = fetched;
    }
    if (now - d.speedTick >= 1000) {
        const float instant = (fetched - d.speedBytes) * 1000.0f / (now - d.speedTick);
        d.speed = d.speed > 0 ? d.speed * 0.7f + instant * 0.3f : instant;
        d.speedTick = now;
        d.speedBytes = fetched;
    }
    if (now - d.logTick >= 10000) {
        // Medidas al registro cada 10 s: en la consola se mira por FTP.
        d.logTick = now;
        const DWORD elapsed = now - d.startTick > 0 ? now - d.startTick : 1;
        Log::write("medidas: red %.2f MB/s, bajado %I64d MB, ZIP usado %I64d MB, ISO %I64d MB, guardado %I64d MB "
                   "(en disco %I64d MB); zlib %lu%%, disco %lu%%, esperando red %lu%%",
                   d.speed / 1048576.0, fetched >> 20, d.consumed >> 20, d.position >> 20, d.parts->written() >> 20,
                   d.parts->flushed() >> 20, d.inflateMs * 100 / elapsed, d.writeMs * 100 / elapsed,
                   d.waitMs * 100 / elapsed);
    }
    if (now - d.reportTick < 250) return;
    d.reportTick = now;
    // Total: el ZIP entero, o, cuando ya se sabe donde parar, una estimacion
    // con la proporcion comprimido/ISO que llevamos.
    long long total = s_game.size > 0 ? s_game.size - d.dataStart : -1;
    if (d.needed > 0 && d.position > 0) {
        const double ratio = double(d.consumed) / double(d.position);
        const long long estimate = static_cast<long long>(ratio * double(d.gameOffset + d.needed));
        if (estimate > d.consumed && (total < 0 || estimate < total)) total = estimate;
    }
    setCounters(d.consumed, total, d.speed);

    {
        // Medidas: cuanto del tiempo se va en zlib y en el disco (el resto es red y cifrado).
        const DWORD elapsed = now - d.startTick > 0 ? now - d.startTick : 1;
        Guard guard(s_lock);
        _snprintf(s_progress.detail, sizeof(s_progress.detail), "zlib %lu%%  ·  disco %lu%%  ·  esperando red %lu%%",
                  d.inflateMs * 100 / elapsed, d.writeMs * 100 / elapsed, d.waitMs * 100 / elapsed);
    }
    char text[160], iso[24];
    formatGb(d.position, iso, sizeof(iso));
    if (d.gameOffset < 0 || d.position < d.gameOffset)
        _snprintf(text, sizeof(text), "Pasando la parte de video del disco, que no se guarda (ISO: %s)", iso);
    else if (d.needed < 0)
        _snprintf(text, sizeof(text), "Descargando el juego; aun buscando su lista de archivos (ISO: %s)", iso);
    else
        _snprintf(text, sizeof(text), "Descargando el juego (ISO: %s)", iso);
    text[sizeof(text) - 1] = 0;
    Guard guard(s_lock);
    if (s_progress.phase == Installer::Phase::Downloading)
        _snprintf(s_progress.text, sizeof(s_progress.text), "%s", text);
}

// Recibe bytes del ZIP comprimido, en orden: los descomprime y los pasa a iso().
// Falso para parar (error, ya basta o fin del ZIP).
// Guarda lo producido en la ventana circular de 32 KB (antes de que iso() avance position).
void remember(Download& d, const unsigned char* data, int length)
{
    if (length >= kWindow) {
        const long long p = d.position + length - kWindow;
        for (int i = 0; i < kWindow; ++i) d.ring[(p + i) % kWindow] = data[length - kWindow + i];
        return;
    }
    const int at = static_cast<int>(d.position % kWindow);
    const int first = kWindow - at < length ? kWindow - at : length;
    memcpy(d.ring + at, data, first);
    memcpy(d.ring, data + first, length - first);
}

// Punto de control: todo lo producido ya en disco y el estado de zlib en T:\.
void checkpoint(Download& d)
{
    d.lastCheckpoint = d.position;
    if (d.gameOffset < 0 || d.position < d.gameOffset + kWindow) return;
    if (!d.parts->sync()) return;
    ResumeState* st = new ResumeState;
    memset(st, 0, sizeof(*st));
    st->magic = kResumeMagic;
    _snprintf(st->file, sizeof(st->file) - 1, "%s", s_game.file.c_str());
    _snprintf(st->title, sizeof(st->title) - 1, "%s", s_game.title.c_str());
    _snprintf(st->region, sizeof(st->region) - 1, "%s", s_game.region.c_str());
    _snprintf(st->collection, sizeof(st->collection) - 1, "%s", s_game.collection.c_str());
    st->zipSize = s_game.size;
    st->drive = s_drive;
    _snprintf(st->folder, sizeof(st->folder) - 1, "%s", s_folder);
    st->dataStart = d.dataStart;
    st->consumed = d.consumed;
    st->bits = d.zs.data_type & 7;
    st->position = d.position;
    st->gameOffset = d.gameOffset;
    st->written = d.parts->flushed();
    for (int i = 0; i < kWindow; ++i) st->window[i] = d.ring[(d.position - kWindow + i) % kWindow];
    if (Files::writeDurable(kResumePath, st, sizeof(*st)))
        Log::write("instalar: punto de control: ZIP %I64d, ISO %I64d MB, guardado %I64d MB", d.consumed,
                   d.position >> 20, st->written >> 20);
    delete st;
}

bool consume(Download& d, const unsigned char* data, int length)
{
    if (s_cancel) return false;
    if (d.stored) {
        d.consumed += length;
        const bool go = iso(d, data, length);
        report(d);
        return go;
    }
    d.zs.next_in = const_cast<unsigned char*>(data);
    d.zs.avail_in = static_cast<uInt>(length);
    while (d.zs.avail_in > 0 && !d.streamEnd) {
        d.zs.next_out = d.out;
        d.zs.avail_out = kInflateChunk;
        const uInt before = d.zs.avail_in;
        const DWORD t = GetTickCount();
        // Z_BLOCK: para en cada limite entre bloques, donde se pueden poner puntos de control.
        const int ret = inflate(&d.zs, Z_BLOCK);
        d.inflateMs += GetTickCount() - t;
        d.consumed += before - d.zs.avail_in;
        const int produced = kInflateChunk - static_cast<int>(d.zs.avail_out);
        if (produced > 0) {
            remember(d, d.out, produced);
            if (!iso(d, d.out, produced)) return false;
        }
        if ((d.zs.data_type & 128) && !(d.zs.data_type & 64) && d.position - d.lastCheckpoint >= kCheckpointEvery)
            checkpoint(d);
        if (ret == Z_STREAM_END) {
            d.streamEnd = true;
        } else if (ret != Z_OK && ret != Z_BUF_ERROR) {
            _snprintf(d.error, sizeof(d.error), "El ZIP llego dañado (zlib %d)", ret);
            return false;
        } else if (before == d.zs.avail_in && produced == 0) {
            break;   // zlib no avanza con esto: espera mas datos
        }
    }
    report(d);
    return !d.streamEnd;
}

// Lee la cabecera local del ZIP: donde empiezan los datos, como estan y cuanto ocupa la ISO.
// Guarda tambien la URL final (tras las redirecciones), que es la que piden los trozos.
bool readZipHeader(const std::string& url, Download& d, char* error, int errorSize)
{
    std::string headers = Session::cookieHeader();
    headers += "Range: bytes=0-65535\r\n";
    unsigned char* data = nullptr;
    int length = 0;
    Http::Response response;
    if (!Http::getToMemory(url.c_str(), headers.c_str(), 128 * 1024, &data, &length, &response, error, errorSize)) {
        if (response.status == 403 || response.status == 401)
            _snprintf(error, errorSize, "archive.org no deja descargarlo: la sesión caducó o no vale");
        return false;
    }
    d.dataUrl = response.finalUrl;
    bool ok = false;
    Log::write("instalar: cabecera del ZIP: %d bytes, HTTP %d, %s", length, response.status, response.finalUrl);
    if (length >= 30 && u32(data) == 0x04034B50) {
        const unsigned flags = u16(data + 6), method = u16(data + 8);
        const unsigned nameLength = u16(data + 26), extraLength = u16(data + 28);
        d.dataStart = 30 + nameLength + extraLength;
        d.stored = method == 0;
        ok = method == 0 || method == 8;
        if (!ok) _snprintf(error, errorSize, "El ZIP usa un metodo que no conozco (%u)", method);
        // Tamaño de la ISO: en la cabecera o, si no cabe en 32 bits, en el extra ZIP64.
        const unsigned size32 = u32(data + 22);
        if (!(flags & 8) && size32 != 0xFFFFFFFF) d.isoSize = size32;
        const unsigned char* extra = data + 30 + nameLength;
        for (unsigned at = 0; at + 4 <= extraLength && 30 + nameLength + at + 4 <= static_cast<unsigned>(length);) {
            const unsigned id = u16(extra + at), size = u16(extra + at + 2);
            if (id == 1 && size >= 8 && size32 == 0xFFFFFFFF)
                d.isoSize = static_cast<long long>(u32(extra + at + 4)) | (static_cast<long long>(u32(extra + at + 8)) << 32);
            at += 4 + size;
        }
    } else {
        _snprintf(error, errorSize, "Lo descargado no es un ZIP");
    }
    free(data);
    Log::write("instalar: datos desde %I64d, %s, ISO de %I64d bytes", d.dataStart, d.stored ? "sin comprimir" : "deflate",
               d.isoSize);
    return ok;
}

// --- descarga en paralelo -------------------------------------------------------------
//
// La pila TCP de la Xbox no amplia la ventana: como mucho 64 KB en vuelo por
// conexion. Con ~0,2 s de ida y vuelta a archive.org eso son ~0,3 MB/s (medido
// en la consola). Varias conexiones piden trozos seguidos del ZIP a la vez y el
// hilo del instalador los descomprime en orden.

const int kSegment = 1024 * 1024;   // trozo de cada peticion
// Medido en la consola: cada conexion da ~0,25 MB/s (ventana de 64 KB) y el que
// descomprime pasaba el 85-90 % del tiempo esperando a la red con 8. Con 16 se
// reparte el doble; el disco (3 %) y zlib (<20 %) van sobrados.
const int kWorkers = 16;            // conexiones a la vez
const int kMaxAhead = 32;           // trozos como mucho por delante (lo limita la memoria libre)

struct Fetcher
{
    std::vector<std::string> proxies;   // de los ajustes, el mejor primero (vacio = todas directas)
    std::string url;          // URL final del ZIP
    std::string headers;      // la cookie
    long long base = 0;       // donde empiezan los datos comprimidos
    long long end = 0;        // donde acaba el ZIP
    Lock lock;
    long long next = 0;       // siguiente trozo por repartir
    long long consumed = 0;   // primer trozo aun sin descomprimir
    int ahead = 16;           // trozos por delante permitidos (segun la memoria libre)
    unsigned char* data[kMaxAhead] = {};
    int length[kMaxAhead] = {};
    volatile LONG ready[kMaxAhead] = {};
    volatile LONG stop = 0;
    volatile LONG running = 0;
    volatile LONG fetchedKB = 0;
    char error[200] = "";
};

struct WorkerStart
{
    Fetcher* fetcher;
    int index;
};

DWORD WINAPI fetchWorker(void* param)
{
    WorkerStart* startInfo = static_cast<WorkerStart*>(param);
    Fetcher& f = *startInfo->fetcher;
    const int index = startInfo->index;
    delete startInfo;
    Http::RangeClient client;
    // Con proxies: las pares van por ellos (repartidas entre la lista), las impares
    // directas. Se suman las rutas, y si los proxies caen solo se pierde la mitad.
    const int proxyCount = static_cast<int>(f.proxies.size());
    int proxy = -1;   // indice en la lista, o -1 = directa
    int proxiesTried = 0;
    if (proxyCount > 0 && index % 2 == 0) {
        proxy = (index / 2) % proxyCount;
        client.setProxy(f.proxies[proxy].c_str());
    }
    int proxyFailures = 0;
    while (!f.stop && !s_cancel) {
        long long segment = -1;
        {
            Guard guard(f.lock);
            if (f.base + f.next * kSegment < f.end && f.next - f.consumed < f.ahead) segment = f.next++;
            else if (f.base + f.next * kSegment >= f.end) break;   // ya esta todo repartido
        }
        if (segment < 0) {
            Sleep(20);   // vamos muy por delante: esperar a que se descomprima
            continue;
        }
        const long long start = f.base + segment * kSegment;
        const int length = static_cast<int>(f.end - start < kSegment ? f.end - start : kSegment);
        unsigned char* buffer = static_cast<unsigned char*>(malloc(length));
        bool ok = buffer != nullptr;
        char error[160] = "sin memoria";
        for (int attempt = 1; ok && attempt <= 10 && !f.stop && !s_cancel; ++attempt) {
            if (client.fetch(f.url.c_str(), f.headers.c_str(), start, length, buffer, error, sizeof(error))) break;
            Log::write("instalar: trozo %I64d, intento %d: %s", segment, attempt, error);
            if (proxy >= 0 && ++proxyFailures >= 2) {
                // Este proxy no sirve: probar el siguiente de la lista; si ya se
                // probaron todos, seguir directa.
                proxyFailures = 0;
                if (++proxiesTried < proxyCount) {
                    proxy = (proxy + 1) % proxyCount;
                    client.setProxy(f.proxies[proxy].c_str());
                    Log::write("instalar: conexion %d pasa al proxy %s", index, f.proxies[proxy].c_str());
                } else {
                    proxy = -1;
                    client.setProxy("");
                    Log::write("instalar: conexion %d: ningun proxy sirve, sigue directa", index);
                }
            }
            if (attempt == 10) ok = false;
            for (int i = 0; i < attempt * 10 && !f.stop && !s_cancel; ++i) Sleep(100);
        }
        if (!ok || f.stop || s_cancel) {
            free(buffer);
            if (!ok && !f.stop && !s_cancel) {
                Guard guard(f.lock);
                _snprintf(f.error, sizeof(f.error), "Se perdio la conexion con archive.org (%s)", error);
                f.stop = 1;
            }
            break;
        }
        const int slot = static_cast<int>(segment % kMaxAhead);
        f.data[slot] = buffer;
        f.length[slot] = length;
        InterlockedExchangeAdd(&f.fetchedKB, length / 1024);
        InterlockedExchange(&f.ready[slot], 1);
    }
    InterlockedDecrement(&f.running);
    return 0;
}

// Baja y descomprime el juego. Falso si fallo o se cancelo.
bool download(const std::string& url, Download& d, const ResumeState* resume)
{
    char error[160] = "";
    setPhase(Installer::Phase::Downloading, "Conectando con archive.org...");
    if (!readZipHeader(url, d, error, sizeof(error))) {
        d.networkFailure = true;
        fail(error);
        return false;
    }
    memset(&d.zs, 0, sizeof(d.zs));
    if (!d.stored && inflateInit2(&d.zs, -MAX_WBITS) != Z_OK) {
        fail("No arranca zlib");
        return false;
    }
    if (resume) {
        if (d.stored || resume->dataStart != d.dataStart) {
            fail("El ZIP de archive.org cambio: hay que empezar de nuevo");
            return false;
        }
        d.consumed = resume->consumed;
        d.position = resume->position;
        d.gameOffset = resume->gameOffset;
        d.lastCheckpoint = d.position;
        for (int i = 0; i < kWindow; ++i) d.ring[(d.position - kWindow + i) % kWindow] = resume->window[i];
        Log::write("instalar: sigo desde ZIP %I64d (bits %d), ISO %I64d MB, guardado %I64d MB", d.consumed, resume->bits,
                   d.position >> 20, resume->written >> 20);
    }
    d.out = static_cast<unsigned char*>(malloc(kInflateChunk));
    d.startTick = GetTickCount();

    Fetcher* f = new Fetcher;
    f->url = d.dataUrl;
    f->headers = Session::cookieHeader();
    // Al seguir con bits sueltos se pide tambien el byte anterior (para inflatePrime).
    const int primeBits = resume ? resume->bits : 0;
    f->base = d.dataStart + d.consumed - (primeBits ? 1 : 0);
    f->end = s_game.size > 0 ? s_game.size : d.dataStart + 16LL * 1024 * 1024 * 1024;
    d.fetchedKB = &f->fetchedKB;
    // Trozos por delante: lo que quepa dejando 12 MB libres (64 MB en total en la Xbox).
    MEMORYSTATUS memory;
    GlobalMemoryStatus(&memory);
    const int freeMB = static_cast<int>(memory.dwAvailPhys >> 20);
    f->ahead = freeMB - 12 - 8;   // 8 MB: bufer del disco, zlib y conexiones
    if (f->ahead > kMaxAhead) f->ahead = kMaxAhead;
    if (f->ahead < kWorkers) f->ahead = kWorkers;
    Log::write("instalar: %d conexiones, trozos de %d KB, %d por delante (memoria libre %d MB), hasta el byte %I64d",
               kWorkers, kSegment / 1024, f->ahead, freeMB, f->end);
    // Los proxies se eligen en la vista Red (Y busca, Back activa); aqui solo se usan.
    if (Settings::proxyOn())
        for (int i = 0; i < Settings::proxyCount(); ++i) f->proxies.push_back(Settings::proxyAt(i));
    {
        Guard guard(s_lock);
        if (f->proxies.empty()) _snprintf(s_progress.route, sizeof(s_progress.route), "directo");
        else _snprintf(s_progress.route, sizeof(s_progress.route), "%d proxies + directo (el mejor %s)",
                       static_cast<int>(f->proxies.size()), f->proxies[0].c_str());
    }
    for (int i = 0; i < kWorkers; ++i) {
        InterlockedIncrement(&f->running);
        WorkerStart* startInfo = new WorkerStart{f, i};
        HANDLE h = CreateThread(NULL, 128 * 1024, fetchWorker, startInfo, 0, NULL);
        if (h) {
            CloseHandle(h);
        } else {
            delete startInfo;
            InterlockedDecrement(&f->running);
        }
    }
    setPhase(Installer::Phase::Downloading, "Descargando...");

    bool ok = false;
    for (long long segment = 0;; ++segment) {
        if (f->base + segment * kSegment >= f->end) {
            ok = true;   // ZIP entero (si no hubo Z_STREAM_END, el arbol dira si basta)
            break;
        }
        const int slot = static_cast<int>(segment % kMaxAhead);
        const DWORD waitStart = GetTickCount();
        while (!f->ready[slot] && !f->stop && !s_cancel) {
            Sleep(5);
            report(d);
        }
        d.waitMs += GetTickCount() - waitStart;
        if (s_cancel || !f->ready[slot]) break;   // cancelado o los trabajadores fallaron
        unsigned char* buffer = f->data[slot];
        const int length = f->length[slot];
        f->data[slot] = nullptr;
        InterlockedExchange(&f->ready[slot], 0);
        int skip = 0;
        if (segment == 0 && resume) {
            // Recuperar el estado de zlib: bits sueltos y diccionario (ultimos 32 KB).
            if (primeBits) {
                inflatePrime(&d.zs, primeBits, buffer[0] >> (8 - primeBits));
                skip = 1;
            }
            inflateSetDictionary(&d.zs, resume->window, kWindow);
        }
        const bool go = consume(d, buffer + skip, length - skip);
        free(buffer);
        {
            Guard guard(f->lock);
            f->consumed = segment + 1;
        }
        if (!go) {
            ok = d.enough || d.streamEnd;
            break;
        }
    }

    // Parar a los trabajadores (como mucho lo que tarde su peticion en curso).
    InterlockedExchange(&f->stop, 1);
    if (!ok && !s_cancel && !d.error[0] && f->error[0]) {
        _snprintf(d.error, sizeof(d.error), "%s", f->error);
        d.networkFailure = true;
    }
    if (!ok && !s_cancel && !d.error[0]) d.networkFailure = true;   // se interrumpio sin motivo concreto
    setPhase(Installer::Phase::Downloading, ok ? "Terminando la descarga..." : "Cerrando conexiones...");
    while (f->running) Sleep(20);
    for (int i = 0; i < kMaxAhead; ++i) free(f->data[i]);
    d.fetchedKB = nullptr;
    delete f;
    Log::write("instalar: descarga terminada: ZIP %I64d, ISO %I64d, guardado %I64d%s%s", d.consumed, d.position,
               d.parts->written(), d.enough ? ", basta" : "", d.streamEnd ? ", fin del ZIP" : "");

    if (!d.stored) inflateEnd(&d.zs);
    free(d.out);
    d.out = nullptr;
    if (!ok && !s_cancel) fail(d.error[0] ? d.error : "La descarga se interrumpio");
    return ok;
}

// --- instalacion ---------------------------------------------------------------------

// Nombre valido en FATX: 42 caracteres como mucho y sin simbolos raros.
std::string folderName(const std::string& title)
{
    std::string out;
    for (unsigned char c : title) {
        if (c >= 0x80) continue;   // FATX no admite tildes
        if (isalnum(c) || c == ' ' || strchr("!#$%&'()-.@[]^_`{}~", c)) out += static_cast<char>(c);
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    if (out.size() > 40) out.resize(40);
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    return out.empty() ? "Juego" : out;
}

// Tamaño de un archivo, o -1 si no existe.
long long fileSize(const char* path)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    const DWORD size = GetFileSize(h, NULL);
    CloseHandle(h);
    return size;
}

// Borra lo copiado de un juego (archivos y carpetas del arbol, de dentro afuera) y
// su carpeta. Solo se usa con carpetas que creo la tienda en esta instalacion.
void removeInstalled(const char* folder, const std::vector<FileEntry>& files)
{
    for (const FileEntry& f : files)
        if (!f.directory) DeleteFileA((std::string(folder) + "\\" + f.path).c_str());
    for (size_t i = files.size(); i-- > 0;)
        if (files[i].directory) RemoveDirectoryA((std::string(folder) + "\\" + files[i].path).c_str());
    RemoveDirectoryA(folder);
}

bool isRootXbe(const FileEntry& f)
{
    return !f.directory && _stricmp(f.path.c_str(), "default.xbe") == 0;
}

bool extract(Parts& parts, std::vector<FileEntry> files, const char* folder)
{
    // El default.xbe el ultimo: los paneles solo listan una carpeta como juego si
    // lo tiene, asi que un juego a medias nunca aparece como si estuviera bien.
    std::stable_partition(files.begin(), files.end(), [](const FileEntry& f) { return !isRootXbe(f); });
    long long total = 0;
    for (const FileEntry& f : files)
        if (!f.directory) total += f.size;
    long long done = 0;
    setPhase(Installer::Phase::Installing, "Instalando archivos...");
    setCounters(0, total, 0);

    std::vector<unsigned char> buffer(1024 * 1024);
    int skipped = 0;
    for (const FileEntry& f : files) {
        if (s_cancel) return false;
        std::string path = std::string(folder) + "\\" + f.path;
        if (f.directory) {
            CreateDirectoryA(path.c_str(), NULL);
            continue;
        }
        // Al seguir una instalacion cortada: lo que ya esta completo no se copia otra vez
        // (el default.xbe siempre, por si quedo a medias).
        if (!isRootXbe(f) && fileSize(path.c_str()) == static_cast<long long>(f.size)) {
            done += f.size;
            ++skipped;
            setCounters(done, total, 0);
            continue;
        }
        HANDLE out = CreateFileA(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (out == INVALID_HANDLE_VALUE) {
            char text[160];
            _snprintf(text, sizeof(text), "No se pudo crear %s", f.path.c_str());
            fail(text);
            return false;
        }
        long long offset = static_cast<long long>(f.sector) * kSector;
        unsigned left = f.size;
        bool ok = true;
        while (left > 0 && ok) {
            if (s_cancel) {
                ok = false;
                break;
            }
            const int n = left < buffer.size() ? static_cast<int>(left) : static_cast<int>(buffer.size());
            DWORD written = 0;
            ok = parts.read(offset, buffer.data(), n) && WriteFile(out, buffer.data(), n, &written, NULL) &&
                 written == static_cast<DWORD>(n);
            offset += n;
            left -= n;
            done += n;
            setCounters(done, total, 0);
        }
        if (ok) ok = FlushFileBuffers(out) != 0;
        CloseHandle(out);
        if (!ok) {
            if (!s_cancel) {
                char text[160];
                _snprintf(text, sizeof(text), "Fallo al copiar %s (¿disco lleno?)", f.path.c_str());
                fail(text);
            }
            return false;
        }
    }
    if (skipped) Log::write("instalar: %d archivos ya estaban copiados", skipped);

    // Comprobacion final: cada archivo con el tamaño que dice el disco original.
    setPhase(Installer::Phase::Installing, "Comprobando los archivos...");
    int bad = 0;
    for (const FileEntry& f : files) {
        if (f.directory) continue;
        const std::string path = std::string(folder) + "\\" + f.path;
        if (fileSize(path.c_str()) != static_cast<long long>(f.size)) {
            if (bad < 5) Log::write("instalar: %s no cuadra (%I64d en vez de %u)", f.path.c_str(), fileSize(path.c_str()),
                                    f.size);
            ++bad;
        }
    }
    if (bad) {
        char text[160];
        _snprintf(text, sizeof(text), "%d archivos no quedaron bien. Vuelve a intentarlo (se sigue donde iba).", bad);
        fail(text);
        return false;
    }
    Log::write("instalar: %d archivos comprobados", static_cast<int>(files.size()));
    return true;
}

ResumeState* s_resume;   // si no es nulo, la instalacion en curso sigue una a medias

void run()
{
    const bool autotest = STORE_AUTOTEST && s_game.collection == "autotest";
    if (!autotest && !Session::loggedIn()) {
        fail("Falta la sesión de archive.org: pulsa Start para iniciarla");
        return;
    }

    char games[16], temp[96];
    _snprintf(games, sizeof(games), "%c:\\Games", s_drive);
    Files::makeDirectories(games);
    _snprintf(temp, sizeof(temp), "%s\\_cxs_tmp", games);
    if (s_resume) {
        _snprintf(s_folder, sizeof(s_folder), "%s", s_resume->folder);
    } else {
        // Carpeta final libre: "F:\Games\Halo 2", o "Halo 2 (2)" si ya existe.
        const std::string name = folderName(s_game.title);
        _snprintf(s_folder, sizeof(s_folder), "%s\\%s", games, name.c_str());
        for (int n = 2; Files::exists(s_folder) && n < 100; ++n)
            _snprintf(s_folder, sizeof(s_folder), "%s\\%s (%d)", games, name.substr(0, 36).c_str(), n);
    }
    char folder[96];
    _snprintf(folder, sizeof(folder), "%s", s_folder);
    {
        Guard guard(s_lock);
        _snprintf(s_progress.folder, sizeof(s_progress.folder), "%s", folder);
    }

    Parts parts(temp);
    if (s_resume ? !parts.openExisting(s_resume->written) : !parts.create()) {
        deleteResume();
        parts.removeAll();
        fail(s_resume ? "Lo descargado antes ya no esta en el disco: hay que empezar de nuevo"
                      : "No se pudo crear la carpeta temporal en el disco");
        return;
    }

    Download d;
    d.parts = &parts;
    const std::string url = autotest ? "http://10.0.2.2:8000/" + Catalog::urlEncode(s_game.file)
                                     : Catalog::downloadUrl(s_game);
    const bool alreadyDownloaded = s_resume && s_resume->stage == 1;
    if (alreadyDownloaded) Log::write("instalar: ya estaba descargado; sigo copiando los archivos");
    const bool ok = alreadyDownloaded || download(url, d, s_resume);
    if (s_cancel) {
        Log::write("instalar: cancelado durante la descarga; borrando los trozos");
        parts.removeAll();
        deleteResume();
        setPhase(Installer::Phase::Cancelled, "Cancelado");
        Log::write("instalar: cancelado, limpio");
        return;
    }
    if (!ok) {   // download() ya dejo el motivo
        // Si fallo la red y hay punto de control, lo descargado se guarda para seguir.
        if (d.networkFailure && Files::exists(kResumePath)) {
            parts.close();
            Installer::Progress text = Installer::progress();
            char message[200];
            _snprintf(message, sizeof(message), "%s. Lo descargado se guarda: al abrir la tienda podras seguir.", text.text);
            setPhase(Installer::Phase::Failed, message);
            Log::write("instalar: se guarda lo descargado para seguir otro dia");
        } else {
            parts.removeAll();
            deleteResume();
        }
        return;
    }
    if (alreadyDownloaded) d.needed = -1;   // el arbol se lee abajo, de los trozos
    if (!parts.flush()) {
        parts.removeAll();
        fail("No se pudo escribir en el disco (¿lleno?)");
        return;
    }
    // Si el arbol aun no se habia podido leer (estaba al final), ahora si.
    if (d.needed < 0) {
        long long end = 0;
        if (readTree(parts, d.files, &end) != Parse::Ok) {
            parts.removeAll();
            fail("La ISO descargada no tiene el formato de un disco de Xbox");
            return;
        }
    }

    long long gameBytes = 0;
    for (const FileEntry& f : d.files)
        if (!f.directory) gameBytes += f.size;
    if (freeBytes(s_drive) < gameBytes + 64LL * 1024 * 1024) {
        char gb[24], text[160];
        formatGb(gameBytes, gb, sizeof(gb));
        _snprintf(text, sizeof(text), "No hay espacio en %c: para instalar (%s)", s_drive, gb);
        parts.removeAll();
        fail(text);
        return;
    }

    // Si se corta copiando, al abrir se ofrece seguir: los trozos se quedan hasta el final.
    {
        ResumeState* st = new ResumeState;
        memset(st, 0, sizeof(*st));
        st->magic = kResumeMagic;
        _snprintf(st->file, sizeof(st->file) - 1, "%s", s_game.file.c_str());
        _snprintf(st->title, sizeof(st->title) - 1, "%s", s_game.title.c_str());
        _snprintf(st->region, sizeof(st->region) - 1, "%s", s_game.region.c_str());
        _snprintf(st->collection, sizeof(st->collection) - 1, "%s", s_game.collection.c_str());
        st->zipSize = s_game.size;
        st->drive = s_drive;
        _snprintf(st->folder, sizeof(st->folder) - 1, "%s", folder);
        st->written = parts.flushed();
        st->stage = 1;
        parts.sync();
        Files::writeDurable(kResumePath, st, sizeof(*st));
        delete st;
    }

    CreateDirectoryA(folder, NULL);
    const bool installed = extract(parts, d.files, folder);
    if (!installed) {
        if (s_cancel) {
            // Cancelar a mitad de copiar: no queda nada a medias.
            Log::write("instalar: cancelado copiando; borro la carpeta a medias %s", folder);
            removeInstalled(folder, d.files);
            parts.removeAll();
            deleteResume();
            setPhase(Installer::Phase::Cancelled, "Cancelado: se borró todo lo descargado y copiado");
        } else {
            parts.close();   // fallo copiando: los trozos se quedan para seguir
        }
        return;
    }
    parts.removeAll();
    deleteResume();
    char text[160];
    _snprintf(text, sizeof(text), "Instalado en %s", folder);
    Log::write("instalar: %s", text);
    setPhase(Installer::Phase::Done, text);
}

DWORD WINAPI worker(void*)
{
    run();
    delete s_resume;
    s_resume = nullptr;
    InterlockedExchange(&s_busy, 0);
    return 0;
}

bool launch(const Catalog::Game& game, char drive)
{
    s_game = game;
    s_drive = drive;
    s_cancel = 0;
    {
        Guard guard(s_lock);
        s_progress = Installer::Progress();
        s_progress.phase = Installer::Phase::Starting;
        s_progress.art = game.art;
        _snprintf(s_progress.title, sizeof(s_progress.title), "%s", game.title.c_str());
        s_progress.title[sizeof(s_progress.title) - 1] = 0;
        _snprintf(s_progress.text, sizeof(s_progress.text), "Conectando con archive.org...");
        s_progress.total = -1;
    }
    HANDLE h = CreateThread(NULL, 256 * 1024, worker, NULL, 0, NULL);
    if (!h) {
        delete s_resume;
        s_resume = nullptr;
        InterlockedExchange(&s_busy, 0);
        return false;
    }
    CloseHandle(h);
    return true;
}

// Borra los trozos de una descarga a medias y su punto de control.
void discardResumeFiles()
{
    ResumeState* st = new ResumeState;
    if (loadResume(*st)) {
        char temp[32];
        _snprintf(temp, sizeof(temp), "%c:\\Games\\_cxs_tmp", st->drive);
        Parts(temp).removeAll();
    }
    delete st;
    deleteResume();
}
} // namespace

namespace Installer
{
bool start(const Catalog::Game& game, char drive)
{
    if (InterlockedCompareExchange(&s_busy, 1, 0) != 0) return false;
    Log::write("instalar: %s en %c:", game.file.c_str(), drive);
    discardResumeFiles();   // una descarga nueva sustituye a la que quedo a medias
    return launch(game, drive);
}

bool pending(Pending& out)
{
    out = Pending();
    ResumeState* st = new ResumeState;
    const bool ok = loadResume(*st);
    if (ok) {
        _snprintf(out.title, sizeof(out.title) - 1, "%s", st->title);
        _snprintf(out.folder, sizeof(out.folder) - 1, "%s", st->folder);
        out.written = st->written;
        out.isoPosition = st->position;
        out.installing = st->stage == 1;
        std::string base = st->file;
        if (base.size() > 4) base.resize(base.size() - 4);
        out.art = ArtIndex::find(base.c_str());
    }
    delete st;
    return ok;
}

bool resume()
{
    if (InterlockedCompareExchange(&s_busy, 1, 0) != 0) return false;
    ResumeState* st = new ResumeState;
    if (!loadResume(*st)) {
        delete st;
        InterlockedExchange(&s_busy, 0);
        return false;
    }
    Catalog::Game game;
    game.file = st->file;
    game.title = st->title;
    game.region = st->region;
    game.collection = st->collection;
    game.size = st->zipSize;
    std::string base = game.file.size() > 4 ? game.file.substr(0, game.file.size() - 4) : game.file;
    game.art = ArtIndex::find(base.c_str());
    s_resume = st;
    Log::write("instalar: sigo con %s en %c:", game.file.c_str(), st->drive);
    return launch(game, st->drive);
}

void discardPending()
{
    if (s_busy) return;
    Log::write("instalar: descarto la descarga a medias");
    discardResumeFiles();
}

void cancel()
{
    Log::write("instalar: el usuario cancela");
    InterlockedExchange(&s_cancel, 1);
}

Progress progress()
{
    Guard guard(s_lock);
    return s_progress;
}

void cleanupLeftovers()
{
    if (s_busy) return;
    // Los trozos de una descarga que se puede seguir no se tocan.
    ResumeState* st = new ResumeState;
    const char keep = loadResume(*st) ? st->drive : 0;
    delete st;
    for (char drive = 'E'; drive <= 'G'; ++drive) {
        if (drive == keep) continue;
        char folder[32];
        _snprintf(folder, sizeof(folder), "%c:\\Games\\_cxs_tmp", drive);
        if (!Files::exists(folder)) continue;
        Log::write("instalar: borrando trozos que quedaron en %s", folder);
        Parts(folder).removeAll();
    }
}

void dismiss()
{
    if (s_busy) return;
    Guard guard(s_lock);
    s_progress = Progress();
}
} // namespace Installer
