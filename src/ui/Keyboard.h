#pragma once

#include <string>

// Teclado en pantalla para el mando: cruceta para moverse, A escribe, X borra,
// Y mayusculas, Back enseña u oculta la contraseña, Start acepta, B cancela.
class Keyboard
{
public:
    enum Button : unsigned
    {
        kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kA = 16, kB = 32, kX = 64, kY = 128, kStart = 256, kBack = 512
    };
    enum class Result { Editing, Accepted, Cancelled };

    void open(const char* title, const char* initial, bool secret);
    Result handle(unsigned pressed);
    void draw() const;
    const std::string& text() const { return m_text; }

private:
    std::string m_title;
    std::string m_text;
    bool m_secret = false;
    bool m_show = false;
    bool m_shift = false;
    int m_row = 1;
    int m_column = 0;
};
