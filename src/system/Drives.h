#pragma once

// Unidades del disco duro (E, F, G) y juegos instalados en ellas.
namespace Drives
{
struct Drive
{
    char letter;                    // 'E', 'F', 'G'
    bool present;
    unsigned long long freeBytes;
    unsigned long long totalBytes;
};

struct GameFolder
{
    char path[64];                  // "F:\Games"
    int count;                      // juegos (subcarpetas con default.xbe)
};

struct Game
{
    char name[48];
    char drive;
    bool app;                       // esta en una carpeta de aplicaciones (Apps...)
    char folder[64];                // carpeta que la contiene: "F:\Games"
};

const int kDriveCount = 3;
const int kMaxFolders = 16;
const int kMaxGames = 512;

// XAPI solo monta D:, T:, U: y Z:. Esto enlaza E:, F: y G: a sus particiones.
void mount();

// Lee el espacio de cada unidad y busca juegos. Tarda un poco (recorre FATX).
void scan();

const Drive* drives();              // kDriveCount entradas
const GameFolder* folders(int* count);
const Game* games(int* count);
}
