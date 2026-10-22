#include "store/Session.h"

#include "ui/Lang.h"

#include "XboxXtl.h"
#include "net/Http.h"
#include "store/Catalog.h"
#include "system/Files.h"
#include "system/Log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

namespace
{
const char kConsolePath[] = "T:\\ia_session.json";
const char kPcPath[] = "D:\\ia_session.json";

bool s_loaded;
std::string s_user;
std::string s_cookie;
const char* s_source = "";

Lock s_lock;
volatile Session::Login s_login = Session::Login::Idle;
char s_loginMessage[160];
std::string s_loginEmail, s_loginPassword;

// Siguiente cadena JSON a partir de `p` (sin escapes raros: cookies y correos).
bool nextString(const char*& p, const char* end, std::string& out)
{
    while (p < end && *p != '"') {
        if (*p == '}') return false;
        ++p;
    }
    if (p >= end) return false;
    const char* start = ++p;
    while (p < end && *p != '"') p += (*p == '\\') ? 2 : 1;
    out.assign(start, p < end ? p : end);
    if (p < end) ++p;
    return true;
}

// "clave": "valor" dentro de `text`. Falso si no esta.
bool stringField(const std::string& text, const char* key, std::string& out)
{
    const std::string quoted = std::string("\"") + key + "\"";
    const size_t at = text.find(quoted);
    if (at == std::string::npos) return false;
    const char* p = text.c_str() + at + quoted.size();
    const char* end = text.c_str() + text.size();
    while (p < end && (*p == ' ' || *p == ':')) ++p;
    if (p >= end || *p != '"') return false;
    return nextString(p, end, out);
}

// Lee el objeto "cookies": {"nombre": "valor", ...} y lo pasa a "a=b; c=d".
std::string cookiesFrom(const std::string& text)
{
    const size_t at = text.find("\"cookies\"");
    if (at == std::string::npos) return "";
    const char* p = static_cast<const char*>(memchr(text.c_str() + at, '{', text.size() - at));
    if (!p) return "";
    const char* end = text.c_str() + text.size();
    ++p;
    std::string header, name, value;
    while (nextString(p, end, name) && nextString(p, end, value)) {
        if (!header.empty()) header += "; ";
        header += name + "=" + value;
    }
    return header;
}

void load()
{
    s_loaded = true;
    s_user.clear();
    s_cookie.clear();
    s_source = "";
    std::vector<unsigned char> bytes;
    // La de la consola primero: si inicias sesion aqui, manda sobre la copiada del PC.
    if (Files::readAll(kConsolePath, bytes)) s_source = "consola";
    else if (Files::readAll(kPcPath, bytes)) s_source = "PC";
    else return;
    const std::string text(bytes.begin(), bytes.end());
    stringField(text, "username", s_user);
    const std::string header = cookiesFrom(text);
    if (header.find("logged-in-sig=") == std::string::npos) return;   // sin firma no vale
    s_cookie = "Cookie: " + header + "\r\n";
}

void setLogin(Session::Login state, const char* message)
{
    Guard guard(s_lock);
    _snprintf(s_loginMessage, sizeof(s_loginMessage) - 1, "%s", message);
    s_login = state;
}

DWORD WINAPI loginThread(void*)
{
    std::string email, password;
    {
        Guard guard(s_lock);
        email.swap(s_loginEmail);
        password.swap(s_loginPassword);
    }
    const std::string body = "email=" + Catalog::urlEncode(email) + "&password=" + Catalog::urlEncode(password);
    password.assign(password.size(), '\0');
    unsigned char* data = nullptr;
    int length = 0;
    Http::Response response;
    char error[160];
    const bool ok = Http::postToMemory("https://archive.org/services/xauthn/?op=login",
                                       "Content-Type: application/x-www-form-urlencoded\r\n", body.data(),
                                       static_cast<int>(body.size()), 64 * 1024, &data, &length, &response, error,
                                       sizeof(error));
    if (!ok) {
        // Con datos que no valen, archive.org responde 401.
        Log::write("sesion: no se pudo iniciar (HTTP %d, %s)", response.status, error);
        setLogin(Session::Login::Failed, response.status == 401 ? tr("Correo o contraseña incorrectos")
                                                                 : tr("No se pudo conectar con archive.org"));
        return 0;
    }
    const std::string reply(reinterpret_cast<char*>(data), length);
    free(data);
    const std::string cookies = cookiesFrom(reply);
    if (reply.find("\"success\":true") == std::string::npos || cookies.find("logged-in-sig=") == std::string::npos) {
        Log::write("sesion: respuesta sin cookies");
        setLogin(Session::Login::Failed, tr("archive.org no devolvio la sesion"));
        return 0;
    }
    // Mismo formato que el ia_session.json de Classic Xbox Loader.
    std::string cookieObject;
    const char* p = reply.c_str() + reply.find("\"cookies\"");
    p = strchr(p, '{');
    const char* close = p ? strchr(p, '}') : nullptr;
    if (p && close) cookieObject.assign(p, close + 1);
    std::string screenName;
    stringField(reply, "screenname", screenName);
    const std::string file = "{\"logged_in\": true, \"username\": \"" + (screenName.empty() ? email : screenName) +
                             "\", \"cookies\": " + cookieObject + "}";
    if (!Files::writeDurable(kConsolePath, file.data(), file.size())) {
        setLogin(Session::Login::Failed, tr("No se pudo guardar la sesion en el disco"));
        return 0;
    }
    load();
    Log::write("sesion: iniciada como %s", s_user.c_str());
    char message[160];
    _snprintf(message, sizeof(message), tr("Sesion iniciada: %s"), s_user.c_str());
    setLogin(Session::Login::Ok, message);
    return 0;
}
} // namespace

namespace Session
{
bool loggedIn()
{
    if (!s_loaded) load();
    return !s_cookie.empty();
}

void reload()
{
    load();
}

const char* user()
{
    if (!s_loaded) load();
    return s_cookie.empty() ? "" : s_user.c_str();
}

const char* cookieHeader()
{
    if (!s_loaded) load();
    return s_cookie.c_str();
}

const char* source()
{
    if (!s_loaded) load();
    return s_cookie.empty() ? "" : s_source;
}

void startLogin(const char* email, const char* password)
{
    if (s_login == Login::Working) return;
    {
        Guard guard(s_lock);
        s_loginEmail = email;
        s_loginPassword = password;
    }
    setLogin(Login::Working, tr("Iniciando sesion en archive.org..."));
    HANDLE h = CreateThread(NULL, 256 * 1024, loginThread, NULL, 0, NULL);
    if (h) CloseHandle(h);
    else setLogin(Login::Failed, tr("No se pudo iniciar sesion"));
}

Login loginState()
{
    return s_login;
}

const char* loginMessage()
{
    return s_loginMessage;
}

void logout()
{
    DeleteFileA(kConsolePath);
    load();
    Log::write("sesion: cerrada en la consola");
    setLogin(Login::Idle, "");
}
} // namespace Session
