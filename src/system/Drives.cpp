#include "system/Drives.h"

#include "XboxXtl.h"

#include <stdio.h>
#include <string.h>

namespace
{
// ANSI_STRING del kernel.
struct KernelString
{
    USHORT Length;
    USHORT MaximumLength;
    PCHAR Buffer;
};
} // namespace

extern "C" __declspec(dllimport) LONG __stdcall IoCreateSymbolicLink(KernelString* link, KernelString* device);
extern "C" __declspec(dllimport) LONG __stdcall IoDeleteSymbolicLink(KernelString* link);
// El XBE que se esta ejecutando, como ruta de dispositivo:
// "\Device\Harddisk0\Partition1\Apps\ClassicXboxStore\default.xbe".
extern "C" __declspec(dllimport) KernelString XeImageFileName;

namespace
{
// Particiones del disco de la Xbox: 1 = E, 6 = F, 7 = G (F y G solo existen
// con discos grandes y un BIOS/kernel que las active).
const struct { char letter; const char* device; } kPartitions[Drives::kDriveCount] = {
    {'E', "\\Device\\Harddisk0\\Partition1"},
    {'F', "\\Device\\Harddisk0\\Partition6"},
    {'G', "\\Device\\Harddisk0\\Partition7"},
};

Drives::Drive s_drives[Drives::kDriveCount];
Drives::GameFolder s_folders[Drives::kMaxFolders];
int s_folderCount;
Drives::Game s_games[Drives::kMaxGames];
int s_gameCount;

void mountOne(char letter, const char* device)
{
    char linkName[] = "\\??\\X:";
    linkName[4] = letter;
    char deviceName[64];
    _snprintf(deviceName, sizeof(deviceName), "%s", device);
    deviceName[sizeof(deviceName) - 1] = 0;
    KernelString link = {static_cast<USHORT>(strlen(linkName)), static_cast<USHORT>(sizeof(linkName)), linkName};
    KernelString target = {static_cast<USHORT>(strlen(deviceName)), static_cast<USHORT>(sizeof(deviceName)), deviceName};
    IoDeleteSymbolicLink(&link);
    IoCreateSymbolicLink(&link, &target);
}

bool fileExists(const char* path)
{
    return GetFileAttributesA(path) != (DWORD)-1;
}

// Juegos dentro de `folder` (cada subcarpeta con default.xbe). Devuelve cuantos.
// "F:\Apps", "E:\Applications", "F:\Aplicaciones"... -> aplicaciones, no juegos.
bool isAppFolder(const char* folder)
{
    const char* name = strrchr(folder, '\\');
    name = name ? name + 1 : folder;
    if (_strnicmp(name, "apl", 3) == 0) return true;   // Aplicaciones
    for (; *name; ++name)
        if (_strnicmp(name, "app", 3) == 0) return true;   // Apps, Applications, MyApps
    return false;
}

int scanGameFolder(const char* folder, char drive)
{
    const bool app = isAppFolder(folder);
    char pattern[96];
    _snprintf(pattern, sizeof(pattern), "%s\\*", folder);
    pattern[sizeof(pattern) - 1] = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    int found = 0;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.')
            continue;
        char xbe[160];
        _snprintf(xbe, sizeof(xbe), "%s\\%s\\default.xbe", folder, fd.cFileName);
        xbe[sizeof(xbe) - 1] = 0;
        if (!fileExists(xbe))
            continue;
        ++found;
        if (s_gameCount < Drives::kMaxGames)
        {
            Drives::Game& g = s_games[s_gameCount++];
            _snprintf(g.name, sizeof(g.name), "%s", fd.cFileName);
            g.name[sizeof(g.name) - 1] = 0;
            g.drive = drive;
            g.app = app;
            _snprintf(g.folder, sizeof(g.folder), "%s", folder);
            g.folder[sizeof(g.folder) - 1] = 0;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

// Cada carpeta de primer nivel de la unidad que tenga juegos dentro
// (E:\Games, F:\Juegos, ...) se apunta como carpeta de juegos.
void scanDrive(char letter)
{
    char pattern[8];
    _snprintf(pattern, sizeof(pattern), "%c:\\*", letter);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.')
            continue;
        char folder[64];
        _snprintf(folder, sizeof(folder), "%c:\\%s", letter, fd.cFileName);
        folder[sizeof(folder) - 1] = 0;
        const int count = scanGameFolder(folder, letter);
        if (count > 0 && s_folderCount < Drives::kMaxFolders)
        {
            Drives::GameFolder& f = s_folders[s_folderCount++];
            memcpy(f.path, folder, sizeof(f.path));
            f.count = count;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

void sortGames()
{
    for (int i = 1; i < s_gameCount; ++i)
        for (int j = i; j > 0 && (s_games[j - 1].app != s_games[j].app ? s_games[j - 1].app
                                                                          : _stricmp(s_games[j - 1].name, s_games[j].name) > 0);
             --j)
        {
            const Drives::Game tmp = s_games[j];
            s_games[j] = s_games[j - 1];
            s_games[j - 1] = tmp;
        }
}
} // namespace

namespace Drives
{
void mount()
{
    for (int i = 0; i < kDriveCount; ++i)
        mountOne(kPartitions[i].letter, kPartitions[i].device);

    // D: a la carpeta del XBE. Algunos paneles (UnleashX, XBMC...) dejan D: en
    // el lector de DVD, y entonces no se encuentra lo que va junto al
    // default.xbe (ia_session.json). Igual que XboxGameDrive en OptiCraft.
    const KernelString& image = XeImageFileName;
    if (image.Buffer == nullptr || image.Length == 0) return;
    int slash = -1;
    for (int i = 0; i < image.Length; ++i)
        if (image.Buffer[i] == '\\') slash = i;
    char folder[260];
    if (slash <= 0 || slash >= static_cast<int>(sizeof(folder))) return;
    memcpy(folder, image.Buffer, slash);
    folder[slash] = 0;
    char linkName[] = "\\??\\D:";
    KernelString link = {static_cast<USHORT>(sizeof(linkName) - 1), static_cast<USHORT>(sizeof(linkName)), linkName};
    KernelString device = {static_cast<USHORT>(slash), static_cast<USHORT>(sizeof(folder)), folder};
    IoDeleteSymbolicLink(&link);
    IoCreateSymbolicLink(&link, &device);
}

void scan()
{
    s_folderCount = 0;
    s_gameCount = 0;
    for (int i = 0; i < kDriveCount; ++i)
    {
        Drive& d = s_drives[i];
        d.letter = kPartitions[i].letter;
        char root[4] = {d.letter, ':', '\\', 0};
        ULARGE_INTEGER freeToCaller, total, freeTotal;
        d.present = GetDiskFreeSpaceExA(root, &freeToCaller, &total, &freeTotal) != 0;
        d.freeBytes = d.present ? freeToCaller.QuadPart : 0;
        d.totalBytes = d.present ? total.QuadPart : 0;
        if (d.present)
            scanDrive(d.letter);
    }
    sortGames();
}

const Drive* drives() { return s_drives; }

const GameFolder* folders(int* count)
{
    *count = s_folderCount;
    return s_folders;
}

const Game* games(int* count)
{
    *count = s_gameCount;
    return s_games;
}
} // namespace Drives
