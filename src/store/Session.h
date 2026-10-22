#pragma once

// Sesion de archive.org: T:\ia_session.json (la de iniciar sesion en la consola)
// o D:\ia_session.json (copiada del PC, de Classic Xbox Loader; mismo formato).
namespace Session
{
// Lee el archivo (una vez; reload() vuelve a mirar).
bool loggedIn();
void reload();

const char* user();               // correo o nombre, "" si no hay sesion
// "Cookie: logged-in-user=...; logged-in-sig=...\r\n", o "" si no hay sesion.
const char* cookieHeader();

// Iniciar sesion desde la consola: POST al servicio oficial de archive.org
// (services/xauthn, el que usa su programa "ia"). La contraseña no se guarda;
// solo las cookies que devuelve, en T:\ia_session.json.
enum class Login { Idle, Working, Ok, Failed };
void startLogin(const char* email, const char* password);
Login loginState();
const char* loginMessage();
// Cierra la sesion de la consola (borra T:\ia_session.json).
void logout();
// De donde salio la sesion: "consola", "PC" o "".
const char* source();
}
