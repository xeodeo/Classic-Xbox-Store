#pragma once

// Registro en T:\cxs_log.txt (= E:\TDATA\FFFF4358\cxs_log.txt, se saca por FTP)
// y captador de fallos: un acceso invalido o una excepcion que escapa de
// cualquier hilo queda anotado con registros y pila para buscar en el .map.
namespace Log
{
void write(const char* format, ...);

// Llamar al principio de main(), en el hilo de la pantalla.
void installCrashHandler();

// Ultimas lineas escritas (las mas nuevas al final), para enseñarlas en pantalla.
int recent(char lines[][128], int maxLines);

// Texto del fallo de un hilo de trabajo ("" si no hubo): la pantalla lo enseña.
const char* crashText();
}
