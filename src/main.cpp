// Classic Xbox Store - prueba 3.
//
// La pantalla de inicio de la tienda, con lo que ya funciona en la consola:
//   - espacio real de E:, F: y G: (montadas a mano: XAPI solo monta D:);
//   - donde estan los juegos y cuales son;
//   - que la red llega a archive.org: IP, DNS (el de la Xbox o, si no responde,
//     1.1.1.1 / 8.8.8.8) y conexion TCP al puerto 443. El cifrado viene despues.
// El estilo sigue a PKGi: una lista compacta entre dos lineas finas, el estado
// arriba a la derecha y el espacio libre abajo.

#include "XboxXtl.h"

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "net/Http.h"
#include "store/Art.h"
#include "store/Catalog.h"
#include "store/Installer.h"
#include "store/Route.h"
#include "store/Settings.h"
#include "store/Session.h"
#include "system/Drives.h"
#include "system/Files.h"
#include "system/Log.h"
#include "system/Network.h"
#include "ui/Gfx.h"
#include "ui/ImageDecode.h"
#include "ui/Keyboard.h"
#include "ui/Lang.h"

namespace {

// --- colores ----------------------------------------------------------------------

const unsigned long kGreen = 0xFF7CD67C;
const unsigned long kRed = 0xFFFF6B5E;
const unsigned long kText = 0xFFF2F7F2;
const unsigned long kTextList = 0xFFDDE6DD;
const unsigned long kTextDim = 0xFF8FA88F;
const unsigned long kTextFaint = 0xFF5F7A5F;
const unsigned long kRule = 0x80FFFFFF;
const unsigned long kSelected = 0x40FFFFFF;

const float kMargin = 32;

// --- mando ------------------------------------------------------------------------

enum : unsigned {
    kUp = 1, kDown = 2, kA = 4, kB = 8, kX = 16, kLeft = 32, kRight = 64, kY = 128, kPageUp = 256, kPageDown = 512,
    kStart = 1024, kBack = 2048,
};

HANDLE g_pads[4];

unsigned readButtons()
{
    DWORD insertions = 0, removals = 0;
    if (XGetDeviceChanges(XDEVICE_TYPE_GAMEPAD, &insertions, &removals)) {
        for (int p = 0; p < 4; ++p) {
            if ((removals & (1u << p)) && g_pads[p]) {
                XInputClose(g_pads[p]);
                g_pads[p] = NULL;
            }
            if ((insertions & (1u << p)) && !g_pads[p])
                g_pads[p] = XInputOpen(XDEVICE_TYPE_GAMEPAD, p, XDEVICE_NO_SLOT, NULL);
        }
    }
    unsigned bits = 0;
    for (int p = 0; p < 4; ++p) {
        XINPUT_STATE s;
        if (!g_pads[p] || XInputGetState(g_pads[p], &s) != ERROR_SUCCESS) continue;
        const XINPUT_GAMEPAD& g = s.Gamepad;
        if ((g.wButtons & XINPUT_GAMEPAD_DPAD_UP) || g.sThumbLY > 16000) bits |= kUp;
        if ((g.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) || g.sThumbLY < -16000) bits |= kDown;
        if (g.bAnalogButtons[XINPUT_GAMEPAD_A] > 30) bits |= kA;
        if (g.bAnalogButtons[XINPUT_GAMEPAD_B] > 30) bits |= kB;
        if (g.bAnalogButtons[XINPUT_GAMEPAD_X] > 30) bits |= kX;
        if (g.bAnalogButtons[XINPUT_GAMEPAD_Y] > 30) bits |= kY;
        if ((g.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) || g.sThumbLX < -16000) bits |= kLeft;
        if ((g.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) || g.sThumbLX > 16000) bits |= kRight;
        if (g.bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] > 60) bits |= kPageUp;
        if (g.bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] > 60) bits |= kPageDown;
        if (g.wButtons & XINPUT_GAMEPAD_START) bits |= kStart;
        if (g.wButtons & XINPUT_GAMEPAD_BACK) bits |= kBack;
    }
    return bits;
}

void initInput()
{
    XInitDevices(0, NULL);
    const DWORD present = XGetDevices(XDEVICE_TYPE_GAMEPAD);
    for (int p = 0; p < 4; ++p)
        if (present & (1u << p)) g_pads[p] = XInputOpen(XDEVICE_TYPE_GAMEPAD, p, XDEVICE_NO_SLOT, NULL);
}

// --- pruebas (en otro hilo, para que la pantalla no se congele) --------------------

enum class NetState { Starting, Offline, Online };
enum class ArchiveState { Waiting, Checking, Ok, Failed };

// El hilo escribe los textos antes de cambiar el estado; la pantalla solo lee.
volatile LONG g_drivesReady;
volatile LONG g_testRunning;
volatile NetState g_net = NetState::Starting;
volatile ArchiveState g_archive = ArchiveState::Waiting;
char g_ip[24];
char g_archiveText[128];

// Diagnostico de red (boton X): varias conexiones para ver cual falla y por que.
enum class ProbeState { Pending, Ok, Failed, Info };
struct Probe {
    char label[48];
    char result[80];
    volatile ProbeState state;
};
const int kMaxProbes = 10;
Probe g_probes[kMaxProbes];
volatile LONG g_probeCount;

const char* errorName(int error)
{
    switch (error) {
    case WSAETIMEDOUT: return tr("nadie contesta (tiempo agotado)");
    case WSAECONNREFUSED: return tr("rechazada");
    case WSAEHOSTUNREACH: return tr("sin ruta al servidor");
    case WSAENETUNREACH: return tr("red inalcanzable");
    case WSAECONNRESET: return tr("cortada por el otro lado");
    case WSAEWOULDBLOCK: return tr("sigue esperando");
    default: return tr("error");
    }
}

Probe& addProbe(const char* label)
{
    Probe& p = g_probes[g_probeCount < kMaxProbes ? g_probeCount : kMaxProbes - 1];
    _snprintf(p.label, sizeof(p.label), "%s", label);
    p.label[sizeof(p.label) - 1] = 0;
    p.result[0] = 0;
    p.state = ProbeState::Pending;
    if (g_probeCount < kMaxProbes) InterlockedIncrement(&g_probeCount);
    return p;
}

void info(const char* label, const char* value)
{
    Probe& p = addProbe(label);
    _snprintf(p.result, sizeof(p.result), "%s", value);
    p.result[sizeof(p.result) - 1] = 0;
    p.state = ProbeState::Info;
}

void probeTcp(const char* label, unsigned long address, int port, bool blocking)
{
    Probe& p = addProbe(label);
    const DWORD start = GetTickCount();
    const intptr_t s = Network::tcpConnect(address, port, 10000, blocking);
    const DWORD ms = GetTickCount() - start;
    if (s == Network::kInvalidSocket) {
        const int error = Network::lastError();
        _snprintf(p.result, sizeof(p.result), "%s  (%d, %lu ms)", errorName(error), error, ms);
        p.state = ProbeState::Failed;
    } else {
        Network::tcpClose(s);
        _snprintf(p.result, sizeof(p.result), tr("conecta  (%lu ms)"), ms);
        p.state = ProbeState::Ok;
    }
    p.result[sizeof(p.result) - 1] = 0;
}

void probeHttps(const char* label, const char* url)
{
    Probe& p = addProbe(label);
    const DWORD start = GetTickCount();
    unsigned char* data = nullptr;
    int length = 0;
    Http::Response response;
    char error[128];
    if (!Http::getToMemory(url, nullptr, 4 * 1024 * 1024, &data, &length, &response, error, sizeof(error))) {
        _snprintf(p.result, sizeof(p.result), "%s", error);
        p.result[sizeof(p.result) - 1] = 0;
        p.state = ProbeState::Failed;
        return;
    }
    const DWORD ms = GetTickCount() - start;
    _snprintf(p.result, sizeof(p.result), tr("%d  ·  %d KB en %lu ms"), response.status, length / 1024, ms);
    p.result[sizeof(p.result) - 1] = 0;
    p.state = ProbeState::Ok;
    free(data);
}

void runProbes(unsigned long archive)
{
    g_probeCount = 0;
    const unsigned long flags = Network::addressFlags();
    char line[80];
    _snprintf(line, sizeof(line), tr("%s  ·  %s  ·  puerta de enlace %s  ·  DNS %s"), g_ip,
              (flags & XNET_GET_XNADDR_DHCP) ? "DHCP" : (flags & XNET_GET_XNADDR_STATIC) ? tr("IP fija") : "?",
              (flags & XNET_GET_XNADDR_GATEWAY) ? tr("sí") : tr("no"), (flags & XNET_GET_XNADDR_DNS) ? tr("sí") : tr("no"));
    info(tr("Consola"), line);
    const unsigned long cloudflare = inet_addr("1.1.1.1");
    unsigned long router = 0;
    if (g_ip[0]) {
        router = inet_addr(g_ip);
        reinterpret_cast<unsigned char*>(&router)[3] = 1;   // a.b.c.1, el router habitual
    }
    if (archive) {
        probeTcp("archive.org : 443", archive, 443, false);
        probeTcp(tr("archive.org : 443 con bloqueo"), archive, 443, true);
        probeTcp("archive.org : 80", archive, 80, false);
    }
    probeTcp("1.1.1.1 : 443", cloudflare, 443, false);
    probeTcp("1.1.1.1 : 80", cloudflare, 80, false);
    if (router) {
        char label[48], routerText[24];
        Network::formatIPv4(router, routerText, sizeof(routerText));
        _snprintf(label, sizeof(label), "Router %s : 80", routerText);
        probeTcp(label, router, 80, false);
    }
    // Cifrado de verdad: datos de archive.org y una imagen de GitHub (portadas).
    probeHttps("HTTPS archive.org", "https://archive.org/metadata/microsoft_xbox_a/metadata");
    probeHttps("HTTPS GitHub (portadas)",
               "https://raw.githubusercontent.com/MobCat/MobCats-original-xbox-game-list/main/background/4D53/4D530064.jpg");
}

DWORD WINAPI testThread(void*)
{
    if (!g_drivesReady) {
        Drives::scan();
        Installer::cleanupLeftovers();
        InterlockedExchange(&g_drivesReady, 1);
    }

    g_archive = ArchiveState::Waiting;
    if (!Network::initialize()) {
        _snprintf(g_archiveText, sizeof(g_archiveText), tr("La red no arranca (XNetStartup)"));
        g_net = NetState::Offline;
        g_archive = ArchiveState::Failed;
        InterlockedExchange(&g_testRunning, 0);
        return 0;
    }
    Network::localAddress(g_ip, sizeof(g_ip));
    const bool cable = Network::cableConnected();
    g_net = cable && g_ip[0] ? NetState::Online : NetState::Offline;
    if (!cable) {
        _snprintf(g_archiveText, sizeof(g_archiveText), tr("Sin cable de red"));
        g_archive = ArchiveState::Failed;
        InterlockedExchange(&g_testRunning, 0);
        return 0;
    }

    g_archive = ArchiveState::Checking;
    unsigned long address = 0;
    const char* via = "";
    if (!Network::resolveIPv4("archive.org", &address, &via)) {
        _snprintf(g_archiveText, sizeof(g_archiveText), tr("Sin DNS  ·  puerta de enlace %s  ·  DNS en el panel %s"),
                  Network::hasGateway() ? tr("sí") : tr("no"), Network::hasDnsServer() ? tr("sí") : tr("no"));
        g_archive = ArchiveState::Failed;
        runProbes(0);
        InterlockedExchange(&g_testRunning, 0);
        return 0;
    }
    char remote[24];
    Network::formatIPv4(address, remote, sizeof(remote));
    const DWORD start = GetTickCount();
    const intptr_t s = Network::tcpConnect(address, 443, 10000);
    const DWORD ms = GetTickCount() - start;
    if (s == Network::kInvalidSocket) {
        const int error = Network::lastError();
        _snprintf(g_archiveText, sizeof(g_archiveText), tr("archive.org %s: 443 %s (%d)  ·  X para diagnóstico"), remote,
                  errorName(error), error);
        g_archive = ArchiveState::Failed;
    } else {
        Network::tcpClose(s);
        _snprintf(g_archiveText, sizeof(g_archiveText), "archive.org %lu ms  ·  DNS %s", ms, via);
        g_archive = ArchiveState::Ok;
    }
    runProbes(address);
    InterlockedExchange(&g_testRunning, 0);
    return 0;
}

DWORD WINAPI rescanThread(void*)
{
    Drives::scan();
    InterlockedExchange(&g_drivesReady, 1);
    return 0;
}

// Vuelve a buscar juegos (tras instalar uno). La lista no se lee mientras tanto.
void rescanDrives()
{
    if (g_testRunning || !g_drivesReady) return;
    InterlockedExchange(&g_drivesReady, 0);
    HANDLE h = CreateThread(NULL, 64 * 1024, rescanThread, NULL, 0, NULL);
    if (h) CloseHandle(h);
    else InterlockedExchange(&g_drivesReady, 1);
}

#if STORE_AUTOTEST
bool countSink(const unsigned char*, int length, void* user)
{
    *static_cast<long long*>(user) += length;
    return true;
}

void speedTest(const char* label, bool blocking)
{
    Network::setBlockingStreams(blocking);
    Probe& p = addProbe(label);
    long long bytes = 0;
    Http::Response response;
    char error[128];
    const DWORD start = GetTickCount();
    const bool ok = Http::get("http://10.0.2.2:8000/Halo%20-%20Combat%20Evolved%20%28Europe%29.zip",
                              "Range: bytes=0-31457279\r\n", countSink, &bytes, &response, error, sizeof(error));
    const DWORD ms = GetTickCount() - start;
    _snprintf(p.result, sizeof(p.result), "%s%.2f MB/s  (%d MB en %lu ms)", ok ? "" : tr("FALLO "),
              bytes / 1048576.0 / (ms / 1000.0 + 0.001), static_cast<int>(bytes / 1048576), ms);
    p.state = ok ? ProbeState::Ok : ProbeState::Failed;
    Network::setBlockingStreams(false);
}

DWORD WINAPI speedTestThread(void*)
{
    while (g_testRunning) Sleep(100);
    g_probeCount = 0;
    speedTest("HTTP local, sin bloqueo", false);
    speedTest("HTTP local, bloqueante", true);
    speedTest("HTTP local, sin bloqueo (otra vez)", false);
    return 0;
}
#endif

void startTest()
{
    if (InterlockedCompareExchange(&g_testRunning, 1, 0) != 0) return;
    HANDLE h = CreateThread(NULL, 256 * 1024, testThread, NULL, 0, NULL);
    if (h) CloseHandle(h);
    else InterlockedExchange(&g_testRunning, 0);
}

// --- pantalla (estilo PKGi: listas compactas entre dos lineas finas) -----------------
//
// Tres vistas: Tienda (catalogo de archive.org por letras), Instalados y Red.
// En Tienda e Instalados, el juego elegido pone su fondo detras (fundido) y su
// portada a la derecha; el arte llega de store/Art en otro hilo.

enum class View { Store, Installed, Network, Account };

const float kRuleTop = 50;
const float kLettersY = 56;
const float kListTopStore = 84;
const float kListTopInstalled = 58;
const float kRowHeight = 22;
const float kRuleBottom = 404;
const float kCoverW = 150, kCoverH = 210;
const float kListRight = Gfx::kWidth - kMargin - kCoverW - 16;   // la portada va a la derecha

struct Cursor {
    int selected = 0;
    int scroll = 0;
};

// Una capa del fondo: la imagen y su opacidad animada.
struct Layer {
    Gfx::Image image;
    const ArtIndex::Entry* entry = nullptr;
    float alpha = 0;
};

struct Ui {
    View view = View::Store;
    View before = View::Store;   // a donde vuelve X desde Red
    int letter = 1;              // A
    Cursor store, installed;
    // Cuenta (Start): menu, o teclado para el correo (1) o la contraseña (2).
    View beforeAccount = View::Store;
    int accountStep = 0;
    std::string loginEmail;
    Keyboard keyboard;
    char launchError[96] = "";    // si un juego no arranco
    bool proxyPanel = false;      // Red: panel de proxies en vez del diagnostico
    char toast[96] = "";          // aviso corto en pantalla (unos segundos)
    DWORD toastUntil = 0;
    int installedRowScroll = 0;   // Instalados: desplazamiento en filas (con los titulos)

    // Copia del catalogo de la letra actual.
    std::vector<Catalog::Game> games;
    Catalog::State state = Catalog::State::Empty;
    std::string message;
    long version = -1;
    int shownLetter = -1;

    // Arte del juego elegido.
    const ArtIndex::Entry* selectedArt = nullptr;
    DWORD selectedSince = 0;
    bool artRequested = false;
    Gfx::Image cover;
    const ArtIndex::Entry* coverFor = nullptr;
    Layer background, fading;   // la nueva y la que se va

    // Instalar.
    bool confirm = false;          // dialogo "Instalar X en F:\Games"
    Catalog::Game confirmGame;
    char confirmDrive = 'E';
    bool confirmCancel = false;    // "¿Cancelar la descarga?"

    // Descarga a medias de otra vez: se pregunta al arrancar.
    bool resumeChecked = false;
    bool resumePrompt = false;
    Installer::Pending resumeInfo;
};

// Corta el texto con "..." para que quepa en `maxWidth`.
void fit(Gfx::Font font, const char* src, float maxWidth, char* out, int size)
{
    _snprintf(out, size, "%s", src);
    out[size - 1] = 0;
    if (Gfx::textWidth(font, out) <= maxWidth) return;
    int len = static_cast<int>(strlen(out));
    while (len > 0) {
        --len;
        while (len > 0 && (static_cast<unsigned char>(out[len]) & 0xC0) == 0x80) --len;   // no partir una tilde
        memcpy(out + len, "...", 4);
        if (Gfx::textWidth(font, out) <= maxWidth) return;
    }
}

// Parrafo con saltos de linea por palabras; como mucho `maxLines`. Devuelve la y de debajo.
float wrap(Gfx::Font font, const char* text, float x, float y, float width, int maxLines, unsigned long color)
{
    const float lineH = Gfx::lineHeight(font);
    std::string rest = text;
    for (int line = 0; line < maxLines && !rest.empty(); ++line) {
        size_t take = rest.size();
        while (take > 0 && Gfx::textWidth(font, rest.substr(0, take).c_str()) > width) {
            const size_t space = rest.rfind(' ', take - 1);
            take = space == std::string::npos || space == 0 ? take - 1 : space;
        }
        if (take == 0) take = 1;
        std::string piece = rest.substr(0, take);
        rest = rest.substr(take);
        while (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
        if (line == maxLines - 1 && !rest.empty()) {
            char fitted[160];
            fit(font, (piece + " " + rest).c_str(), width, fitted, sizeof(fitted));
            piece = fitted;
        }
        Gfx::text(font, piece.c_str(), x, y, color);
        y += lineH;
    }
    return y;
}

void rule(float y)
{
    Gfx::rect(kMargin, y, Gfx::kWidth - kMargin * 2, 1, kRule, kRule);
}

void formatSize(long long bytes, char* out, int size)
{
    if (bytes >= 1024LL * 1024 * 1024)
        _snprintf(out, size, "%.1f GB", double(bytes) / (1024.0 * 1024.0 * 1024.0));
    else
        _snprintf(out, size, "%.0f MB", double(bytes) / (1024.0 * 1024.0));
    out[size - 1] = 0;
}

// "USA, Europe" -> "USA/EUR"
std::string shortRegion(const std::string& region)
{
    static const char* const kNames[][2] = {
        {"USA", "USA"}, {"Europe", "EUR"}, {"Japan", "JPN"}, {"Australia", "AUS"}, {"Germany", "GER"},
        {"France", "FRA"}, {"Spain", "ESP"}, {"Italy", "ITA"}, {"Korea", "KOR"}, {"Asia", "ASIA"},
        {"UK", "UK"}, {"Netherlands", "NED"}, {"Sweden", "SWE"}, {"Russia", "RUS"}, {"Brazil", "BRA"},
        {"China", "CHN"}, {"Taiwan", "TWN"}, {"World", "WLD"}, {"Canada", "CAN"}, {"Scandinavia", "SCA"},
    };
    std::string out;
    size_t at = 0;
    while (at < region.size()) {
        size_t comma = region.find(',', at);
        if (comma == std::string::npos) comma = region.size();
        std::string word = region.substr(at, comma - at);
        while (!word.empty() && word[0] == ' ') word.erase(0, 1);
        const char* code = nullptr;
        for (const auto& n : kNames)
            if (word == n[0]) code = n[1];
        if (!out.empty()) out += "/";
        out += code ? code : word.substr(0, 3);
        at = comma + 1;
    }
    return out;
}

// --- arte del juego elegido -------------------------------------------------------------

// Lo llama la vista cada fotograma con el juego elegido; pide el arte cuando
// la eleccion lleva quieta un momento (al bajar rapido no se pide cada fila).
void chooseArt(Ui& ui, const ArtIndex::Entry* entry, DWORD now)
{
    if (entry != ui.selectedArt) {
        ui.selectedArt = entry;
        ui.selectedSince = now;
        ui.artRequested = false;
    }
    if (!ui.artRequested && now - ui.selectedSince > 180) {
        Art::want(entry);
        ui.artRequested = true;
    }
}

void adoptArt(Ui& ui)
{
    Art::Pixels px;
    while (Art::take(px)) {
        if (px.entry == ui.selectedArt) {
            if (px.kind == Art::kCover) {
                Gfx::freeImage(&ui.cover);
                Gfx::createImage(px.rgba, px.width, px.height, &ui.cover, 512);
                ui.coverFor = px.entry;
            } else {
                // La de antes se desvanece; la nueva aparece encima.
                Gfx::freeImage(&ui.fading.image);
                ui.fading = ui.background;
                ui.background = Layer();
                Gfx::createImage(px.rgba, px.width, px.height, &ui.background.image, 640);
                ui.background.entry = px.entry;
            }
        }
        ImageDecode::release(px.rgba);
    }
}

void animateBackground(Ui& ui, float dt)
{
    const float speed = dt * 4;   // ~0,25 s
    const bool current = ui.background.entry && ui.background.entry == ui.selectedArt;
    ui.background.alpha += current ? speed : -speed;
    ui.background.alpha = ui.background.alpha < 0 ? 0 : ui.background.alpha > 1 ? 1 : ui.background.alpha;
    ui.fading.alpha -= speed;
    if (ui.fading.alpha <= 0 && ui.fading.image.texture) {
        Gfx::freeImage(&ui.fading.image);
        ui.fading = Layer();
    }
}

void drawLayer(const Layer& layer)
{
    if (!layer.image.texture || layer.alpha <= 0) return;
    // Recortado a 4:3 y oscurecido para que se lea la lista.
    const float aspect = float(layer.image.width) / layer.image.height;
    const float keep = aspect > 4.0f / 3.0f ? (4.0f / 3.0f) / aspect : 1, u0 = (1 - keep) / 2;
    Gfx::image(layer.image, 0, 0, Gfx::kWidth, Gfx::kHeight, Gfx::alpha(0xFF5A5A5A, layer.alpha), u0, 0, u0 + keep, 1);
}

void drawBackground(const Ui& ui)
{
    Gfx::rect(0, 0, Gfx::kWidth, Gfx::kHeight, 0xFF0F2A14, 0xFF050E07);
    Gfx::glow(Gfx::kWidth, Gfx::kHeight, 420, 300, 0x301F9F2F);
    drawLayer(ui.fading);
    drawLayer(ui.background);
    if (ui.background.alpha > 0 || ui.fading.alpha > 0)
        Gfx::rect(0, 0, Gfx::kWidth, Gfx::kHeight, 0x40000000, 0xD0000000);
}

// Portada y datos del juego a la derecha.
void drawDetails(const Ui& ui, float top, const char* title, const char* region, const char* sizeText,
                 const ArtIndex::Entry* art)
{
    const float x = Gfx::kWidth - kMargin - kCoverW;
    Gfx::rect(x - 1, top - 1, kCoverW + 2, kCoverH + 2, 0x60FFFFFF, 0x60FFFFFF);
    if (ui.cover.texture && ui.coverFor == art && art) {
        Gfx::image(ui.cover, x, top, kCoverW, kCoverH);
    } else {
        // Sin portada (todavia o nunca): una tapa con el nombre.
        Gfx::rect(x, top, kCoverW, kCoverH, 0xFF1C3A22, 0xFF0B160D);
        wrap(Gfx::kLabel, title, x + 10, top + 14, kCoverW - 20, 5, 0xC0FFFFFF);
        if (art && art->thumbnail[0])
            Gfx::text(Gfx::kSmall, tr("Cargando portada..."), x + 10, top + kCoverH - 26, kTextDim);
    }
    float y = top + kCoverH + 8;
    y = wrap(Gfx::kLabel, title, x, y, kCoverW, 2, kText);
    if (region && region[0]) y = wrap(Gfx::kSmall, region, x, y, kCoverW, 2, kTextDim);
    if (sizeText && sizeText[0]) {
        Gfx::text(Gfx::kSmall, sizeText, x, y, kTextDim);
        y += Gfx::lineHeight(Gfx::kSmall);
    }
    if (art && art->titleId[0]) {
        char id[32];
        _snprintf(id, sizeof(id), "ID %s", art->titleId);
        Gfx::text(Gfx::kSmall, id, x, y, kTextFaint);
    }
}

void drawScrollbar(float top, int visible, int count, int scroll)
{
    if (count <= visible) return;
    const float trackH = visible * kRowHeight;
    const float thumbH = trackH * visible / count < 12 ? 12 : trackH * visible / count;
    const float thumbY = top + (trackH - thumbH) * scroll / (count - visible);
    Gfx::rect(kListRight + 4, thumbY, 2, thumbH, kTextDim, kTextDim);
}

void moveCursor(Cursor& c, unsigned pressed, int count, int visible)
{
    if (count <= 0) {
        c.selected = c.scroll = 0;
        return;
    }
    if (pressed & kDown) ++c.selected;
    if (pressed & kUp) --c.selected;
    if (pressed & kPageDown) c.selected += visible;
    if (pressed & kPageUp) c.selected -= visible;
    if (c.selected >= count) c.selected = count - 1;
    if (c.selected < 0) c.selected = 0;
    if (c.selected < c.scroll) c.scroll = c.selected;
    if (c.selected >= c.scroll + visible) c.scroll = c.selected - visible + 1;
    if (c.scroll > count - visible) c.scroll = count - visible > 0 ? count - visible : 0;
}

// --- cabecera y pie -----------------------------------------------------------------------

void drawHeader(const Ui& ui)
{
    const float w = Gfx::text(Gfx::kTitle, "Classic Xbox Store", kMargin, 14, kText);
    const char* section = Installer::progress().phase != Installer::Phase::Idle ? tr("Instalando")
                           : ui.view == View::Store                         ? tr("Tienda")
                           : ui.view == View::Installed                     ? tr("Instalados")
                           : ui.view == View::Account                       ? tr("Cuenta")
                                                                            : tr("Red");
    Gfx::text(Gfx::kBody, section, kMargin + w + 14, 22, kGreen);

    // Red y archive.org, a la derecha, como la temperatura en PKGi.
    char line[160];
    unsigned long color = kTextDim;
    if (g_net == NetState::Starting || g_archive == ArchiveState::Waiting) {
        _snprintf(line, sizeof(line), tr("Conectando..."));
    } else if (g_archive == ArchiveState::Checking) {
        _snprintf(line, sizeof(line), tr("%s  ·  buscando archive.org..."), g_ip);
    } else if (g_archive == ArchiveState::Ok) {
        _snprintf(line, sizeof(line), "%s  ·  %s", g_ip, g_archiveText);
        color = kGreen;
    } else {
        _snprintf(line, sizeof(line), "%s", g_archiveText);
        color = kRed;
    }
    char fitted[160];
    fit(Gfx::kSmall, line, 250, fitted, sizeof(fitted));
    Gfx::textRight(Gfx::kSmall, fitted, Gfx::kWidth - kMargin, 22, color);
    rule(kRuleTop);
}

// Boton del mando: circulo pequeño de color con la letra, y lo que hace.
float hint(float x, float y, const char* button, unsigned long color, const char* action)
{
    // Un circulo para A, B, X, Y; una pastilla mas ancha para "Back" o "Start".
    const float cy = y + Gfx::lineHeight(Gfx::kBody) / 2;
    const float textW = Gfx::textWidth(Gfx::kSmall, button);
    const float w = textW + 10 > 16 ? textW + 10 : 16;
    Gfx::roundRect(x, cy - 8, w, 16, 8, color, Gfx::mix(color, 0xFF000000, 0.35f));
    Gfx::textCenter(Gfx::kSmall, button, x + w / 2, cy - Gfx::lineHeight(Gfx::kSmall) / 2, 0xFFFFFFFF);
    return x + w + 6 + Gfx::text(Gfx::kBody, action, x + w + 6, y, kText) + 16;
}

void drawFooter(const Ui& ui, const char* left)
{
    rule(kRuleBottom);
    const float y = 414;
    char fitted[96];
    fit(Gfx::kBody, left, 150, fitted, sizeof(fitted));
    Gfx::text(Gfx::kBody, fitted, kMargin, y, kText);

    float x = ui.view == View::Store ? 150 : 196;
    if (ui.view == View::Network && ui.proxyPanel) {
        x = 110;
        if (Route::state() != Route::State::Searching) x = hint(x, y, "A", 0xFF3FB53F, tr("buscar"));
        if (Settings::proxyCount() > 0)
            x = hint(x, y, "Y", 0xFFE0B419, Settings::proxyOn() ? tr("desactivar") : tr("activar"));
        hint(x, y, "B", 0xFFD03A3A, tr("volver"));
        return;
    } else if (ui.view == View::Network) {
        x = 110;
        x = hint(x, y, "A", 0xFF3FB53F, tr("probar"));
        x = hint(x, y, "Y", 0xFFE0B419, tr("proxies"));
        hint(x, y, "X", 0xFF2F6FD0, tr("volver"));
        return;
    } else if (ui.view == View::Account) {
        x = 110;
        if (Session::loginState() != Session::Login::Working) x = hint(x, y, "A", 0xFF3FB53F, tr("iniciar sesión"));
        if (strcmp(Session::source(), "consola") == 0) x = hint(x, y, "X", 0xFF2F6FD0, tr("cerrar sesión"));
        x = hint(x, y, "Y", 0xFFE0B419, tr("idioma"));
        hint(x, y, "B", 0xFFD03A3A, tr("volver"));
        return;
    } else {
        if (ui.view == View::Store) x = hint(x, y, "A", 0xFF3FB53F, tr("instalar"));
        if (ui.view == View::Installed) x = hint(x, y, "A", 0xFF3FB53F, tr("abrir"));
        x = hint(x, y, "Y", 0xFFE0B419, ui.view == View::Store ? tr("instalados") : tr("tienda"));
        x = hint(x, y, "X", 0xFF2F6FD0, tr("red"));
    }
    hint(x, y, "B", 0xFFD03A3A, tr("salir"));

    // Espacio libre de cada unidad que existe.
    char space[96] = "";
    if (g_drivesReady) {
        const Drives::Drive* d = Drives::drives();
        for (int i = 0; i < Drives::kDriveCount; ++i) {
            if (!d[i].present) continue;
            char gb[24], part[32];
            formatSize(static_cast<long long>(d[i].freeBytes), gb, sizeof(gb));
            _snprintf(part, sizeof(part), "%s%c: %s", space[0] ? "  " : "", d[i].letter, gb);
            part[sizeof(part) - 1] = 0;
            strcat(space, part);
        }
    }
    Gfx::textRight(Gfx::kSmall, space, Gfx::kWidth - kMargin, y + 2, kTextDim);
}

// --- vista Tienda ----------------------------------------------------------------------

void syncCatalog(Ui& ui)
{
    const long version = Catalog::version();
    if (ui.shownLetter == ui.letter && version == ui.version) return;
    if (ui.shownLetter != ui.letter) {
        ui.store = Cursor();
        std::vector<Catalog::Game> unused;
        std::string message;
        if (Catalog::snapshot(ui.letter, unused, message) == Catalog::State::Empty) Catalog::request(ui.letter);
    }
    ui.state = Catalog::snapshot(ui.letter, ui.games, ui.message);
    ui.version = version;
    ui.shownLetter = ui.letter;
}

void drawLetters(const Ui& ui)
{
    const float step = (Gfx::kWidth - kMargin * 2) / Catalog::kLetterCount;
    for (int i = 0; i < Catalog::kLetterCount; ++i) {
        const float cx = kMargin + step * (i + 0.5f);
        const bool current = i == ui.letter;
        Gfx::textCenter(current ? Gfx::kLabel : Gfx::kSmall, Catalog::letterLabel(i), cx, kLettersY + (current ? 0 : 2),
                        current ? kGreen : kTextDim);
        if (current) Gfx::rect(cx - 6, kLettersY + 21, 12, 2, kGreen, kGreen);
    }
}

const int kStoreRows = 14;

void drawStore(Ui& ui, DWORD now)
{
    drawLetters(ui);
    const float textY = (kRowHeight - Gfx::lineHeight(Gfx::kBody)) / 2;
    const int count = static_cast<int>(ui.games.size());
    if (count == 0) {
        chooseArt(ui, nullptr, now);
        const char* text = ui.state == Catalog::State::Failed ? ui.message.c_str() : tr("Cargando la lista de archive.org...");
        Gfx::text(Gfx::kBody, text, kMargin + 8, kListTopStore + textY,
                  ui.state == Catalog::State::Failed ? kRed : kTextDim);
        return;
    }
    for (int i = 0; i < kStoreRows && ui.store.scroll + i < count; ++i) {
        const int index = ui.store.scroll + i;
        const Catalog::Game& g = ui.games[index];
        const float y = kListTopStore + i * kRowHeight;
        const bool selected = index == ui.store.selected;
        if (selected) Gfx::rect(kMargin, y, kListRight - kMargin, kRowHeight, kSelected, kSelected);
        char size[24];
        formatSize(g.size, size, sizeof(size));
        const float sizeW = Gfx::textWidth(Gfx::kSmall, size);
        const std::string region = shortRegion(g.region);
        const float regionX = kListRight - 8 - sizeW - 12 - Gfx::textWidth(Gfx::kSmall, region.c_str());
        char name[96];
        fit(Gfx::kBody, g.title.c_str(), regionX - kMargin - 20, name, sizeof(name));
        Gfx::text(Gfx::kBody, name, kMargin + 8, y + textY, selected ? 0xFFFFFFFF : kTextList);
        const float smallY = y + (kRowHeight - Gfx::lineHeight(Gfx::kSmall)) / 2;
        Gfx::text(Gfx::kSmall, region.c_str(), regionX, smallY, selected ? kText : kTextDim);
        Gfx::textRight(Gfx::kSmall, size, kListRight - 8, smallY, selected ? kText : kTextDim);
    }
    drawScrollbar(kListTopStore, kStoreRows, count, ui.store.scroll);

    const Catalog::Game& g = ui.games[ui.store.selected];
    chooseArt(ui, g.art, now);
    char size[24];
    formatSize(g.size, size, sizeof(size));
    drawDetails(ui, kListTopStore, g.title.c_str(), g.region.c_str(), size, g.art);
}

// --- vista Instalados ----------------------------------------------------------------

const int kInstalledRows = 15;

void drawInstalled(Ui& ui, DWORD now)
{
    const float textY = (kRowHeight - Gfx::lineHeight(Gfx::kBody)) / 2;
    if (!g_drivesReady) {
        chooseArt(ui, nullptr, now);
        Gfx::text(Gfx::kBody, tr("Buscando juegos..."), kMargin + 8, kListTopInstalled + textY, kTextDim);
        return;
    }
    int count = 0;
    const Drives::Game* games = Drives::games(&count);
    if (count == 0) {
        chooseArt(ui, nullptr, now);
        Gfx::text(Gfx::kBody, tr("No hay juegos en las carpetas de E:, F: ni G:"), kMargin + 8, kListTopInstalled + textY,
                  kTextDim);
        return;
    }
    // Filas de la lista: un titulo antes de los juegos y otro antes de las
    // aplicaciones (vienen ordenados: primero juegos, luego apps).
    const int kGamesHeader = -1, kAppsHeader = -2;
    std::vector<int> rows;
    int gameCount = 0, appCount = 0, selectedRow = 0;
    for (int i = 0; i < count; ++i) (games[i].app ? appCount : gameCount)++;
    for (int i = 0; i < count; ++i) {
        if (i == 0 && !games[i].app) rows.push_back(kGamesHeader);
        if (games[i].app && (i == 0 || !games[i - 1].app)) rows.push_back(kAppsHeader);
        if (i == ui.installed.selected) selectedRow = static_cast<int>(rows.size());
        rows.push_back(i);
    }
    const int rowCount = static_cast<int>(rows.size());
    // Que se vea la fila elegida (y, si cabe, su titulo encima).
    int& scroll = ui.installedRowScroll;
    if (selectedRow - 1 < scroll) scroll = selectedRow > 0 && rows[selectedRow - 1] < 0 ? selectedRow - 1 : selectedRow;
    if (selectedRow >= scroll + kInstalledRows) scroll = selectedRow - kInstalledRows + 1;
    if (scroll > rowCount - kInstalledRows) scroll = rowCount - kInstalledRows > 0 ? rowCount - kInstalledRows : 0;
    if (scroll < 0) scroll = 0;

    for (int i = 0; i < kInstalledRows && scroll + i < rowCount; ++i) {
        const int row = rows[scroll + i];
        const float y = kListTopInstalled + i * kRowHeight;
        if (row < 0) {
            // Titulo de la seccion, con una linea fina hasta el borde.
            char label[48];
            _snprintf(label, sizeof(label), "%s  (%d)", row == kGamesHeader ? tr("Juegos") : tr("Aplicaciones"),
                      row == kGamesHeader ? gameCount : appCount);
            const float w = Gfx::text(Gfx::kLabel, label, kMargin + 8, y + (kRowHeight - Gfx::lineHeight(Gfx::kLabel)) / 2,
                                      kGreen);
            Gfx::rect(kMargin + 18 + w, y + kRowHeight / 2, kListRight - kMargin - 26 - w, 1, kRule, kRule);
            continue;
        }
        const Drives::Game& g = games[row];
        const bool selected = row == ui.installed.selected;
        if (selected) Gfx::rect(kMargin, y, kListRight - kMargin, kRowHeight, kSelected, kSelected);
        char drive[4] = {g.drive, ':', 0};
        Gfx::text(Gfx::kBody, drive, kMargin + 8, y + textY, selected ? kText : kTextDim);
        char name[64];
        fit(Gfx::kBody, g.name, kListRight - kMargin - 56, name, sizeof(name));
        Gfx::text(Gfx::kBody, name, kMargin + 40, y + textY, selected ? 0xFFFFFFFF : kTextList);
    }
    drawScrollbar(kListTopInstalled, kInstalledRows, rowCount, scroll);

    // La carpeta suele llamarse como el ZIP: asi se encuentra su arte.
    const Drives::Game& g = games[ui.installed.selected];
    const ArtIndex::Entry* art = ArtIndex::find(g.name);
    chooseArt(ui, art, now);
    std::string title, region;
    const char* paren = strstr(g.name, " (");
    title.assign(g.name, paren ? paren - g.name : strlen(g.name));
    char where[16];
    _snprintf(where, sizeof(where), tr("En %c:"), g.drive);
    drawDetails(ui, kListTopInstalled, title.c_str(), paren ? paren + 1 : "", where, art);
}

// --- vista Red -----------------------------------------------------------------------

// Ultimas lineas importantes del registro (T:\cxs_log.txt) desde `y` hasta el pie.
// Las de cada peticion web (http: GET, cifrado...) se quedan solo en el archivo.
void drawRecentLog(float y)
{
    char lines[16][128];
    const int n = Log::recent(lines, 16);
    const char* keep[16];
    int kept = 0;
    for (int i = 0; i < n; ++i)
        if (!strstr(lines[i], "] http:")) keep[kept++] = lines[i];
    const int room = static_cast<int>((kRuleBottom - 6 - y - 18) / 17);
    if (kept == 0 || room <= 0) return;
    Gfx::text(Gfx::kSmall, tr("Registro (E:\\TDATA\\FFFF4358\\cxs_log.txt):"), kMargin + 8, y, kTextDim);
    y += 18;
    for (int i = kept > room ? kept - room : 0; i < kept; ++i, y += 17) {
        char fitted[140];
        fit(Gfx::kSmall, keep[i], Gfx::kWidth - kMargin * 2 - 16, fitted, sizeof(fitted));
        Gfx::text(Gfx::kSmall, fitted, kMargin + 8, y, kTextFaint);
    }
}

// Proxy guardado, en la linea del titulo de la vista Red.
void drawProxyStatus(float y)
{
    char line[96];
    if (Settings::proxyCount() == 0) _snprintf(line, sizeof(line), tr("Proxies: ninguno (directo)"));
    else if (Settings::proxyCount() == 1)
        _snprintf(line, sizeof(line), "Proxy: %s (%s)", Settings::proxyAt(0), Settings::proxyOn() ? tr("activo") : tr("apagado"));
    else
        _snprintf(line, sizeof(line), tr("Proxies: %d, el mejor %s (%s)"), Settings::proxyCount(), Settings::proxyAt(0),
                  Settings::proxyOn() ? tr("activos") : tr("apagados"));
    Gfx::textRight(Gfx::kSmall, line, Gfx::kWidth - kMargin - 8, y, Settings::proxyOn() ? kGreen : kTextDim);
}

// Resultado de buscar proxies (Y). Devuelve la y de debajo.
float drawRoute()
{
    const float textY = (kRowHeight - Gfx::lineHeight(Gfx::kBody)) / 2;
    const float top = kListTopInstalled;
    Gfx::text(Gfx::kLabel, "Proxies", kMargin + 8, top + textY, kText);
    drawProxyStatus(top + textY + 2);
    float y = top + kRowHeight + 4;
    if (Route::state() == Route::State::Idle) {
        // Sin buscar todavia en esta sesion: la lista guardada.
        if (Settings::proxyCount() == 0) {
            Gfx::text(Gfx::kBody, tr("No hay proxies guardados. Pulsa A para buscar."), kMargin + 8, y + textY, kTextDim);
            return y + kRowHeight + 8;
        }
        for (int i = 0; i < Settings::proxyCount(); ++i, y += kRowHeight) {
            Gfx::text(Gfx::kBody, Settings::proxyAt(i), kMargin + 8, y + textY, kTextList);
            Gfx::textRight(Gfx::kBody, i == 0 ? tr("guardado (el mejor)") : tr("guardado"), Gfx::kWidth - kMargin - 8,
                           y + textY, Settings::proxyOn() ? kGreen : kTextDim);
        }
        Gfx::text(Gfx::kSmall,
                  Settings::proxyOn() ? tr("Activos: la mitad de las conexiones va por ellos y, si uno falla, sigue directa.")
                                      : tr("Desactivados: todo va directo."),
                  kMargin + 8, y + 4, kTextDim);
        return y + 26;
    }
    Route::Result results[Route::kMaxResults];
    char message[160];
    const int count = Route::results(results, message, sizeof(message));
    for (int i = 0; i < count; ++i, y += kRowHeight) {
        Gfx::text(Gfx::kBody, results[i].label, kMargin + 8, y + textY, kTextList);
        char speed[48];
        if (!results[i].done) _snprintf(speed, sizeof(speed), tr("probando..."));
        else if (results[i].speed > 0) _snprintf(speed, sizeof(speed), tr("%.2f MB/s por conexion"), results[i].speed);
        else _snprintf(speed, sizeof(speed), tr("no funciona"));
        Gfx::textRight(Gfx::kBody, speed, Gfx::kWidth - kMargin - 8, y + textY,
                       !results[i].done ? kTextDim : results[i].speed > 0 ? kGreen : kRed);
    }
    char fitted[160];
    fit(Gfx::kSmall, message, Gfx::kWidth - kMargin * 2 - 16, fitted, sizeof(fitted));
    Gfx::text(Gfx::kSmall, fitted, kMargin + 8, y + 4, Route::state() == Route::State::Done ? kText : kTextDim);
    return y + 26;
}

float drawProbes()
{
    const float textY = (kRowHeight - Gfx::lineHeight(Gfx::kBody)) / 2;
    const float top = kListTopInstalled;
    Gfx::text(Gfx::kLabel, tr("Diagnóstico de red"), kMargin + 8, top + textY, kText);
    drawProxyStatus(top + textY + 2);
    const int count = g_probeCount;
    if (count == 0) {
        Gfx::text(Gfx::kBody, g_testRunning ? tr("Probando...") : tr("Pulsa A para probar"), kMargin + 8,
                  top + kRowHeight + textY, kTextDim);
        return top + kRowHeight * 2 + 8;
    }
    for (int i = 0; i < count; ++i) {
        const Probe& p = g_probes[i];
        const float y = top + (i + 1) * kRowHeight + 4;
        Gfx::text(Gfx::kBody, p.label, kMargin + 8, y + textY, kTextList);
        unsigned long color = kTextDim;
        const char* result = p.result;
        if (p.state == ProbeState::Pending) result = tr("probando...");
        else if (p.state == ProbeState::Ok) color = kGreen;
        else if (p.state == ProbeState::Failed) color = kRed;
        else color = kText;
        char fitted[96];
        fit(Gfx::kBody, result, 360, fitted, sizeof(fitted));
        Gfx::textRight(Gfx::kBody, fitted, Gfx::kWidth - kMargin - 8, y + textY, color);
    }
    return top + (count + 1) * kRowHeight + 12;
}

// --- vista Cuenta (Start) ---------------------------------------------------------------

void drawAccount(const Ui& ui)
{
    const float x = kMargin + 8;
    float y = kListTopInstalled + 6;
    Gfx::text(Gfx::kTitle, tr("Cuenta de archive.org"), x, y, kText);
    y += 40;
    if (Session::loggedIn()) {
        char line[160];
        _snprintf(line, sizeof(line), tr("Sesión iniciada: %s"), Session::user()[0] ? Session::user() : tr("(sin nombre)"));
        Gfx::text(Gfx::kBody, line, x, y, kGreen);
        y += 24;
        Gfx::text(Gfx::kSmall,
                  strcmp(Session::source(), "consola") == 0 ? tr("Iniciada en esta consola (T:\\ia_session.json).")
                                                            : tr("Copiada del PC (ia_session.json junto al default.xbe)."),
                  x, y, kTextDim);
    } else {
        Gfx::text(Gfx::kBody, tr("Sin sesión: archive.org no deja bajar los juegos sin ella."), x, y, kRed);
    }
    y += 40;
    wrap(Gfx::kSmall,
         tr("Pulsa A para iniciar sesión con tu correo y contraseña de archive.org. La contraseña no se guarda: solo la sesión que devuelve archive.org."),
         x, y, Gfx::kWidth - kMargin * 2 - 16, 3, kTextDim);
    y += 60;
    char language[96];
    _snprintf(language, sizeof(language), tr("Idioma: %s  ·  Y para cambiar"), Lang::name(Lang::current()));
    Gfx::text(Gfx::kBody, language, x, y, kText);
    y += 34;
    const Session::Login state = Session::loginState();
    if (state != Session::Login::Idle)
        Gfx::text(Gfx::kBody, Session::loginMessage(), x, y,
                  state == Session::Login::Ok ? kGreen : state == Session::Login::Failed ? kRed : kText);
    if (ui.accountStep > 0) ui.keyboard.draw();
}

// --- instalar: confirmacion y pantalla de descarga -------------------------------------

// Unidad con mas espacio libre (las que existen), para proponerla.
char bestDrive()
{
    char best = 'E';
    unsigned long long most = 0;
    if (!g_drivesReady) return best;
    const Drives::Drive* d = Drives::drives();
    for (int i = 0; i < Drives::kDriveCount; ++i)
        if (d[i].present && d[i].freeBytes > most) {
            most = d[i].freeBytes;
            best = d[i].letter;
        }
    return best;
}

// Siguiente unidad presente, hacia delante o atras.
char cycleDrive(char current, int step)
{
    if (!g_drivesReady) return current;
    const Drives::Drive* d = Drives::drives();
    int index = 0;
    for (int i = 0; i < Drives::kDriveCount; ++i)
        if (d[i].letter == current) index = i;
    for (int n = 0; n < Drives::kDriveCount; ++n) {
        index = (index + step + Drives::kDriveCount) % Drives::kDriveCount;
        if (d[index].present) return d[index].letter;
    }
    return current;
}

unsigned long long driveFree(char letter)
{
    if (!g_drivesReady) return 0;
    const Drives::Drive* d = Drives::drives();
    for (int i = 0; i < Drives::kDriveCount; ++i)
        if (d[i].letter == letter && d[i].present) return d[i].freeBytes;
    return 0;
}

void panel(float x, float y, float w, float h)
{
    Gfx::rect(0, 0, Gfx::kWidth, Gfx::kHeight, 0xA0000000, 0xA0000000);
    Gfx::rect(x - 1, y - 1, w + 2, h + 2, kRule, kRule);
    Gfx::rect(x, y, w, h, 0xF0102414, 0xF0060E08);
}

void drawConfirm(const Ui& ui)
{
    const float x = 100, y = 120, w = Gfx::kWidth - 200, h = 230;
    panel(x, y, w, h);
    const Catalog::Game& g = ui.confirmGame;
    float ty = y + 16;
    Gfx::text(Gfx::kSmall, tr("Instalar"), x + 20, ty, kGreen);
    ty += 20;
    ty = wrap(Gfx::kTitle, g.title.c_str(), x + 20, ty, w - 40, 2, kText);
    ty += 6;

    char line[160], size[24], freeText[24];
    formatSize(g.size, size, sizeof(size));
    _snprintf(line, sizeof(line), tr("%s  ·  ZIP de %s"), g.region.c_str(), size);
    Gfx::text(Gfx::kSmall, line, x + 20, ty, kTextDim);
    ty += 28;

    formatSize(static_cast<long long>(driveFree(ui.confirmDrive)), freeText, sizeof(freeText));
    _snprintf(line, sizeof(line), tr("<  %c:\\Games  >     libre %s"), ui.confirmDrive, freeText);
    Gfx::text(Gfx::kBody, line, x + 20, ty, kText);
    ty += 24;
    if (Session::loggedIn()) {
        _snprintf(line, sizeof(line), tr("Sesión de archive.org: %s"), Session::user()[0] ? Session::user() : tr("sí"));
        Gfx::text(Gfx::kSmall, line, x + 20, ty, kTextDim);
    } else {
        wrap(Gfx::kSmall, tr("Sin sesión de archive.org: pulsa Start para iniciarla aquí mismo"), x + 20, ty,
             w - 40, 2, kRed);
    }

    const float hy = y + h - 34;
    float hx = hint(x + 20, hy, "A", 0xFF3FB53F, tr("instalar"));
    hint(hx, hy, "B", 0xFFD03A3A, tr("cancelar"));
}

void drawResumePrompt(const Ui& ui)
{
    const float x = 100, y = 120, w = Gfx::kWidth - 200, h = 220;
    panel(x, y, w, h);
    float ty = y + 16;
    Gfx::text(Gfx::kSmall, ui.resumeInfo.installing ? tr("Tienes una instalacion a medias") : tr("Tienes una descarga a medias"),
              x + 20, ty, kGreen);
    ty += 20;
    ty = wrap(Gfx::kTitle, ui.resumeInfo.title, x + 20, ty, w - 40, 2, kText) + 6;
    char saved[24], line[160];
    formatSize(ui.resumeInfo.written, saved, sizeof(saved));
    _snprintf(line, sizeof(line), tr("Llevas %s guardados  ·  %s"), saved, ui.resumeInfo.folder);
    wrap(Gfx::kSmall, line, x + 20, ty, w - 40, 2, kTextDim);
    ty += 40;
    Gfx::text(Gfx::kSmall,
              ui.resumeInfo.installing ? tr("Ya esta descargado: solo falta copiar los archivos que faltan.")
                                       : tr("Se sigue desde el ultimo punto guardado (como mucho se repiten 128 MB)."),
              x + 20, ty, kTextFaint);
    const float hy = y + h - 34;
    float hx = hint(x + 20, hy, "A", 0xFF3FB53F, tr("seguir"));
    hint(hx, hy, "B", 0xFFD03A3A, tr("descartar"));
}

void drawInstall(Ui& ui, const Installer::Progress& p, DWORD now)
{
    chooseArt(ui, p.art, now);
    const float coverX = 56, coverY = 92, coverW = 180, coverH = 252;
    Gfx::rect(coverX - 1, coverY - 1, coverW + 2, coverH + 2, 0x60FFFFFF, 0x60FFFFFF);
    if (ui.cover.texture && ui.coverFor == p.art && p.art) {
        Gfx::image(ui.cover, coverX, coverY, coverW, coverH);
    } else {
        Gfx::rect(coverX, coverY, coverW, coverH, 0xFF1C3A22, 0xFF0B160D);
        wrap(Gfx::kLabel, p.title, coverX + 12, coverY + 14, coverW - 24, 6, 0xC0FFFFFF);
    }

    const float x = coverX + coverW + 28, w = Gfx::kWidth - kMargin - x;
    float y = coverY;
    y = wrap(Gfx::kTitle, p.title, x, y, w, 2, kText) + 10;

    const bool working = p.phase == Installer::Phase::Starting || p.phase == Installer::Phase::Downloading ||
                         p.phase == Installer::Phase::Installing;
    unsigned long color = kText;
    if (p.phase == Installer::Phase::Done) color = kGreen;
    if (p.phase == Installer::Phase::Failed) color = kRed;
    if (p.phase == Installer::Phase::Cancelled) color = kTextDim;
    const char* step = p.phase == Installer::Phase::Downloading  ? tr("Paso 1 de 2: descargando")
                       : p.phase == Installer::Phase::Installing ? tr("Paso 2 de 2: instalando archivos")
                                                                 : "";
    if (step[0]) {
        Gfx::text(Gfx::kSmall, step, x, y, kGreen);
        y += 22;
    }
    y = wrap(Gfx::kBody, p.text, x, y, w, 4, color) + 10;

    if (working) {
        // Barra de progreso.
        const float fraction = p.total > 0 ? float(double(p.done) / double(p.total)) : 0;
        Gfx::roundRect(x, y, w, 10, 5, 0x30FFFFFF, 0x20FFFFFF);
        if (p.total > 0) {
            Gfx::roundRect(x, y, w * (fraction < 1 ? fraction : 1) < 10 ? 10 : w * (fraction < 1 ? fraction : 1), 10, 5,
                           0xFF8BEB6B, 0xFF1E7E1E);
        } else {
            // Sin total todavia: un trozo que va y viene.
            const float t = (now % 1600) / 1600.0f, pos = t < 0.5f ? t * 2 : 2 - t * 2;
            Gfx::roundRect(x + (w - 60) * pos, y, 60, 10, 5, 0xFF8BEB6B, 0xFF1E7E1E);
        }
        y += 22;

        char done[24], total[24], line[160];
        formatSize(p.done, done, sizeof(done));
        if (p.total > 0) {
            formatSize(p.total, total, sizeof(total));
            _snprintf(line, sizeof(line), tr("%s de %s  ·  %d%%"), done, total, int(fraction * 100));
        } else {
            _snprintf(line, sizeof(line), "%s", done);
        }
        Gfx::text(Gfx::kBody, line, x, y, kText);
        y += 24;
        if (p.phase == Installer::Phase::Downloading && p.speed > 0) {
            char speed[48] = "";
            _snprintf(speed, sizeof(speed), "%.1f MB/s", p.speed / (1024 * 1024));
            if (p.total > 0) {
                const int seconds = int((p.total - p.done) / p.speed);
                char eta[48];
                if (seconds >= 3600) _snprintf(eta, sizeof(eta), tr("  ·  quedan %d h %d min"), seconds / 3600, seconds / 60 % 60);
                else _snprintf(eta, sizeof(eta), tr("  ·  quedan %d min"), seconds / 60 + 1);
                strcat(speed, eta);
            }
            Gfx::text(Gfx::kBody, speed, x, y, kTextDim);
            y += 24;
            if (p.detail[0]) {
                Gfx::text(Gfx::kSmall, p.detail, x, y, kTextFaint);
                y += 20;
            }
            if (p.route[0]) {
                char route[96];
                _snprintf(route, sizeof(route), tr("Ruta: %s"), p.route);
                Gfx::text(Gfx::kSmall, route, x, y, kTextFaint);
                y += 20;
            }
        }
    }
    if (p.folder[0]) Gfx::text(Gfx::kSmall, p.folder, x, coverY + coverH - 18, kTextFaint);

    // Ultimas lineas del registro: si algo se atasca, se ve aqui mismo.
    char lines[3][128];
    const int n = Log::recent(lines, 3);
    for (int i = 0; i < n; ++i) {
        char fitted[140];
        fit(Gfx::kSmall, lines[i], Gfx::kWidth - kMargin * 2, fitted, sizeof(fitted));
        Gfx::text(Gfx::kSmall, fitted, kMargin, coverY + coverH + 8 + i * 17, kTextFaint);
    }

    if (ui.confirmCancel) {
        const float px = 150, py = 180, pw = Gfx::kWidth - 300, ph = 110;
        panel(px, py, pw, ph);
        Gfx::textCenter(Gfx::kLabel, tr("¿Cancelar la descarga?"), Gfx::kWidth / 2.0f, py + 20, kText);
        Gfx::textCenter(Gfx::kSmall, tr("Se borra lo descargado hasta ahora."), Gfx::kWidth / 2.0f, py + 46, kTextDim);
        const float hy = py + ph - 32;
        float hx = hint(px + 60, hy, "A", 0xFF3FB53F, tr("sí, cancelar"));
        hint(hx, hy, "B", 0xFFD03A3A, tr("no"));
    }
}

// --- todo junto ------------------------------------------------------------------------

void draw(Ui& ui, DWORD now)
{
    Gfx::beginFrame(0xFF000000);
    drawBackground(ui);
    drawHeader(ui);
    const Installer::Progress progress = Installer::progress();
    if (progress.phase != Installer::Phase::Idle) {
        drawInstall(ui, progress, now);
        rule(kRuleBottom);
        if (Log::crashText()[0]) Gfx::textRight(Gfx::kSmall, Log::crashText(), Gfx::kWidth - kMargin, 416, kRed);
        const bool working = progress.phase == Installer::Phase::Starting ||
                             progress.phase == Installer::Phase::Downloading ||
                             progress.phase == Installer::Phase::Installing;
        if (working) hint(kMargin, 414, "B", 0xFFD03A3A, tr("cancelar"));
        else hint(kMargin, 414, "A", 0xFF3FB53F, tr("volver a la tienda"));
        Gfx::endFrame();
        return;
    }
    char left[96];
    if (ui.view == View::Store) {
        drawStore(ui, now);
        if (ui.games.empty()) _snprintf(left, sizeof(left), tr("Letra %s"), Catalog::letterLabel(ui.letter));
        else if (!ui.message.empty()) _snprintf(left, sizeof(left), "%s", ui.message.c_str());
        else _snprintf(left, sizeof(left), tr("%d juegos"), static_cast<int>(ui.games.size()));
    } else if (ui.view == View::Installed) {
        drawInstalled(ui, now);
        int count = 0;
        if (g_drivesReady) Drives::games(&count);
        int apps = 0;
        const Drives::Game* games = g_drivesReady ? Drives::games(&count) : nullptr;
        for (int i = 0; i < count; ++i) apps += games[i].app ? 1 : 0;
        _snprintf(left, sizeof(left), tr("%d juegos  ·  %d apps"), count - apps, apps);
        if (ui.launchError[0]) _snprintf(left, sizeof(left), "%s", ui.launchError);
    } else if (ui.view == View::Account) {
        chooseArt(ui, nullptr, now);
        _snprintf(left, sizeof(left), tr("Cuenta"));
    } else {
        chooseArt(ui, nullptr, now);
        const float below = ui.proxyPanel ? drawRoute() : drawProbes();
        drawRecentLog(below);
        _snprintf(left, sizeof(left), ui.proxyPanel ? "Proxies" : tr("Red"));
    }
    left[sizeof(left) - 1] = 0;
    if (Log::crashText()[0]) _snprintf(left, sizeof(left), "%s", Log::crashText());
    drawFooter(ui, left);
    if (ui.view == View::Account) drawAccount(ui);   // encima del pie: el teclado tapa la pantalla
    if (ui.toast[0] && static_cast<long>(ui.toastUntil - now) > 0) {
        // Aviso centrado encima del pie.
        const float w = Gfx::textWidth(Gfx::kBody, ui.toast) + 32;
        const float x = (Gfx::kWidth - w) / 2, y = kRuleBottom - 44;
        Gfx::roundRect(x, y, w, 32, 16, 0xF0204A26, 0xF0102814);
        Gfx::textCenter(Gfx::kBody, ui.toast, Gfx::kWidth / 2.0f, y + (32 - Gfx::lineHeight(Gfx::kBody)) / 2, kText);
    }
    if (ui.confirm) drawConfirm(ui);
    if (ui.resumePrompt) drawResumePrompt(ui);
    Gfx::endFrame();
}

} // namespace

void __cdecl main()
{
    Log::installCrashHandler();
    Gfx::init();
    initInput();
    Drives::mount();
    Settings::load();
    startTest();

    Ui ui;
    unsigned previous = readButtons();
    DWORD repeatAt = 0;
    DWORD lastTick = GetTickCount();
    for (;;) {
        const DWORD now = GetTickCount();
        const float dt = (now - lastTick) / 1000.0f;
        lastTick = now;

        // Flechas y gatillos se repiten si se mantienen.
        const unsigned held = readButtons();
        unsigned pressed = held & ~previous;
        const unsigned repeating = held & (kUp | kDown | kLeft | kRight | kPageUp | kPageDown);
        if (repeating && (pressed & repeating)) {
            repeatAt = now + 400;
        } else if (repeating && now >= repeatAt) {
            pressed |= repeating;
            repeatAt = now + ((repeating & (kLeft | kRight | kPageUp | kPageDown)) ? 180 : 70);
        }
        previous = held;

        const Installer::Progress progress = Installer::progress();
        // Proxies activos y la lista tiene horas: renovarla en segundo plano (nunca
        // durante una descarga, para no quitarle red). Se mira cada minuto.
        static DWORD proxyCheckAt = 0;
        if (g_drivesReady && now - proxyCheckAt > 60000 && progress.phase == Installer::Phase::Idle &&
            Route::state() != Route::State::Searching) {
            proxyCheckAt = now;
            if (Settings::proxySearchDue()) {
                Log::write("ruta: la lista de proxies tiene horas; la renuevo");
                Route::startSearch();
            }
        }
        // Al arrancar (con los discos ya leidos): ¿quedo una descarga a medias?
        if (!ui.resumeChecked && g_drivesReady && progress.phase == Installer::Phase::Idle) {
            ui.resumeChecked = true;
            ui.resumePrompt = Installer::pending(ui.resumeInfo);
            if (ui.resumePrompt) chooseArt(ui, ui.resumeInfo.art, now);
        }
        if (progress.phase != Installer::Phase::Idle) {
            // Pantalla de instalacion: solo cancelar o volver.
            const bool working = progress.phase == Installer::Phase::Starting ||
                                 progress.phase == Installer::Phase::Downloading ||
                                 progress.phase == Installer::Phase::Installing;
            if (working) {
                if (ui.confirmCancel) {
                    if (pressed & kA) Installer::cancel();
                    if (pressed & (kA | kB)) ui.confirmCancel = false;
                } else if (pressed & kB) {
                    ui.confirmCancel = true;
                }
            } else {
                ui.confirmCancel = false;
                if (pressed & (kA | kB)) {
                    if (progress.phase == Installer::Phase::Done) rescanDrives();
                    Installer::dismiss();
                }
            }
            pressed = 0;
        } else if (ui.resumePrompt) {
            if (pressed & kA) {
                Session::reload();
                Installer::resume();
                ui.resumePrompt = false;
            } else if (pressed & kB) {
                Installer::discardPending();
                ui.resumePrompt = false;
            }
            pressed = 0;
        } else if (ui.view == View::Account) {
            if (ui.accountStep > 0) {
                // Teclado: correo (1) y luego contraseña (2).
                unsigned keys = 0;
                if (pressed & kUp) keys |= Keyboard::kUp;
                if (pressed & kDown) keys |= Keyboard::kDown;
                if (pressed & kLeft) keys |= Keyboard::kLeft;
                if (pressed & kRight) keys |= Keyboard::kRight;
                if (pressed & kA) keys |= Keyboard::kA;
                if (pressed & kB) keys |= Keyboard::kB;
                if (pressed & kX) keys |= Keyboard::kX;
                if (pressed & kY) keys |= Keyboard::kY;
                if (pressed & kStart) keys |= Keyboard::kStart;
                if (pressed & kBack) keys |= Keyboard::kBack;
                const Keyboard::Result r = ui.keyboard.handle(keys);
                if (r == Keyboard::Result::Cancelled) {
                    ui.accountStep = 0;
                } else if (r == Keyboard::Result::Accepted && ui.accountStep == 1) {
                    ui.loginEmail = ui.keyboard.text();
                    ui.keyboard.open(tr("Contraseña de archive.org"), "", true);
                    ui.accountStep = 2;
                } else if (r == Keyboard::Result::Accepted) {
                    Session::startLogin(ui.loginEmail.c_str(), ui.keyboard.text().c_str());
                    ui.keyboard.open("", "", true);   // no dejar la contraseña en memoria
                    ui.accountStep = 0;
                }
            } else {
                if ((pressed & kA) && Session::loginState() != Session::Login::Working) {
                    ui.keyboard.open(tr("Correo de archive.org"), ui.loginEmail.c_str(), false);
                    ui.accountStep = 1;
                }
                if ((pressed & kX) && strcmp(Session::source(), "consola") == 0) Session::logout();
                if (pressed & kY) Settings::setLanguage((Lang::current() + 1) % Lang::kCount);   // se guarda
                if (pressed & (kB | kStart)) ui.view = ui.beforeAccount;
            }
            pressed = 0;
        } else if (ui.confirm) {
            if (pressed & kStart) {   // sin sesion: iniciarla desde aqui
                ui.confirm = false;
                ui.beforeAccount = ui.view;
                ui.view = View::Account;
                pressed = 0;
            }
            if (pressed & kLeft) ui.confirmDrive = cycleDrive(ui.confirmDrive, -1);
            if (pressed & kRight) ui.confirmDrive = cycleDrive(ui.confirmDrive, 1);
            if (pressed & kA) {
                Session::reload();   // por si se acaba de copiar ia_session.json
                Installer::start(ui.confirmGame, ui.confirmDrive);
                ui.confirm = false;
            }
            if (pressed & kB) ui.confirm = false;
            pressed = 0;
        }

        if (ui.view == View::Store) {
            syncCatalog(ui);
            if ((pressed & kA) && !ui.games.empty()) {
                ui.confirm = true;
                ui.confirmGame = ui.games[ui.store.selected];
                ui.confirmDrive = bestDrive();
                Session::reload();
            }
            if (pressed & kLeft) ui.letter = (ui.letter + Catalog::kLetterCount - 1) % Catalog::kLetterCount;
            if (pressed & kRight) ui.letter = (ui.letter + 1) % Catalog::kLetterCount;
            if (pressed & (kLeft | kRight)) syncCatalog(ui);
            moveCursor(ui.store, pressed, static_cast<int>(ui.games.size()), kStoreRows);
        } else if (ui.view == View::Installed) {
            int count = 0;
            const Drives::Game* games = g_drivesReady ? Drives::games(&count) : nullptr;
            moveCursor(ui.installed, pressed, count, kInstalledRows);
            if ((pressed & kA) && games && ui.installed.selected < count) {
                // Abrir el juego: su default.xbe, como el panel.
                const Drives::Game& g = games[ui.installed.selected];
                char xbe[160];
                _snprintf(xbe, sizeof(xbe), "%s\\%s\\default.xbe", g.folder, g.name);
                xbe[sizeof(xbe) - 1] = 0;
                Log::write("abrir: %s", xbe);
                Gfx::beginFrame(0xFF000000);
                drawBackground(ui);
                Gfx::textCenter(Gfx::kTitle, g.name, Gfx::kWidth / 2.0f, 210, kText);
                Gfx::textCenter(Gfx::kBody, tr("Abriendo..."), Gfx::kWidth / 2.0f, 250, kTextDim);
                Gfx::endFrame();
                Gfx::persist();
                XLaunchNewImage(xbe, NULL);
                // Solo se llega aqui si no arranco.
                _snprintf(ui.launchError, sizeof(ui.launchError), tr("No se pudo abrir %s"), g.name);
                Log::write("abrir: fallo (%lu)", GetLastError());
            }
        } else if (ui.view == View::Network && ui.proxyPanel) {
            // Proxies: todo lo del proxy en un sitio (buscar, activar, desactivar).
            if ((pressed & kA) && Route::state() != Route::State::Searching) Route::startSearch();
            if (pressed & kY) {
                if (Settings::proxyCount() == 0) {
                    _snprintf(ui.toast, sizeof(ui.toast), tr("No hay proxies guardados: pulsa A para buscar"));
                } else {
                    Settings::setProxyOn(!Settings::proxyOn());
                    _snprintf(ui.toast, sizeof(ui.toast), Settings::proxyOn() ? tr("Proxies activados (%d)")
                                                                             : tr("Proxies desactivados: todo directo"),
                              Settings::proxyCount());
                }
                ui.toastUntil = now + 3000;
            }
            if (pressed & kB) ui.proxyPanel = false;
            if (pressed & kX) ui.proxyPanel = false;   // X tambien sale de Red (abajo)
            pressed &= ~(kA | kY | kB);
        } else if (ui.view == View::Network) {
            if (pressed & kA) startTest();
            if (pressed & kY) ui.proxyPanel = true;
            pressed &= ~kY;
        }

        if (pressed & kY) {
            if (ui.view == View::Store) ui.view = View::Installed;
            else if (ui.view == View::Installed) ui.view = View::Store;
        }
        if ((pressed & kStart) && ui.view != View::Account) {
            ui.beforeAccount = ui.view;
            ui.view = View::Account;
        }
        if (pressed & kX) {
            if (ui.view == View::Network) {
                ui.view = ui.before;
            } else {
                ui.before = ui.view;
                ui.view = View::Network;
            }
        }
        if (pressed & kB) XLaunchNewImage(NULL, NULL);   // vuelve al panel (pressed = 0 en dialogos)

#if STORE_AUTOTEST
        // Prueba en xemu sin mando: instala un ZIP servido desde el PC (servidor
        // con Range en 10.0.2.2:8000), sin depender de archive.org ni de la sesion.
        static bool autotestStarted = false;
        if (!autotestStarted && ui.resumeChecked && ui.resumePrompt) {
            // Quedo una a medias (la vuelta anterior se corto): seguirla.
            autotestStarted = true;
            ui.resumePrompt = false;
            Installer::resume();
        } else if (!autotestStarted && ui.resumeChecked && ui.view == View::Store && g_drivesReady) {
            autotestStarted = true;
            Catalog::Game game;
            game.file = "Halo - Combat Evolved (Europe).zip";
            game.title = "Halo - Combat Evolved";
            game.region = "Europe";
            game.collection = "autotest";
            game.size = 6890733475LL;
            game.art = ArtIndex::find("Halo - Combat Evolved (Europe)");
            Installer::start(game, 'E');
        }
#endif
        adoptArt(ui);
        animateBackground(ui, dt);
        draw(ui, now);
    }
}
