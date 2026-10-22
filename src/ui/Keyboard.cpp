#include "ui/Keyboard.h"

#include "ui/Gfx.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

namespace
{
// Cuatro filas de 10 teclas; la quinta son las especiales.
const char* const kRows[4] = {"1234567890", "qwertyuiop", "asdfghjkl@", "zxcvbnm._-"};
const char* const kSymbolsShifted[4] = {"!#$%&*()+=", "QWERTYUIOP", "ASDFGHJKL@", "ZXCVBNM,?/"};
const int kColumns = 10;
const int kSpecialCount = 4;
const char* const kSpecial[kSpecialCount] = {"Espacio", ".com", "Borrar", "Listo"};

const unsigned long kText = 0xFFF2F7F2;
const unsigned long kTextDim = 0xFF8FA88F;
const unsigned long kGreen = 0xFF7CD67C;
const unsigned long kKey = 0x30FFFFFF;
const unsigned long kKeyFocus = 0xFF3FB53F;

const float kKeyW = 44, kKeyH = 34, kGap = 6;
} // namespace

void Keyboard::open(const char* title, const char* initial, bool secret)
{
    m_title = title;
    m_text = initial ? initial : "";
    m_secret = secret;
    m_show = false;
    m_shift = false;
    m_row = 1;
    m_column = 0;
}

Keyboard::Result Keyboard::handle(unsigned pressed)
{
    const int rows = 5;
    const int columnsInRow = m_row == 4 ? kSpecialCount : kColumns;
    if (pressed & kUp) m_row = (m_row + rows - 1) % rows;
    if (pressed & kDown) m_row = (m_row + 1) % rows;
    if (pressed & (kUp | kDown)) {
        // Al entrar o salir de la fila especial, conservar mas o menos la posicion.
        const int newColumns = m_row == 4 ? kSpecialCount : kColumns;
        if (columnsInRow != newColumns)
            m_column = columnsInRow == kColumns ? m_column * kSpecialCount / kColumns
                                                : m_column * kColumns / kSpecialCount + 1;
        if (m_column >= newColumns) m_column = newColumns - 1;
    }
    const int columns = m_row == 4 ? kSpecialCount : kColumns;
    if (pressed & kLeft) m_column = (m_column + columns - 1) % columns;
    if (pressed & kRight) m_column = (m_column + 1) % columns;
    if (pressed & kY) m_shift = !m_shift;
    if (pressed & kBack) m_show = !m_show;
    if ((pressed & kX) && !m_text.empty()) m_text.pop_back();
    if (pressed & kB) return Result::Cancelled;
    if (pressed & kStart) return Result::Accepted;
    if (pressed & kA) {
        if (m_row < 4) {
            if (m_text.size() < 120) m_text += (m_shift ? kSymbolsShifted : kRows)[m_row][m_column];
        } else if (m_column == 0) {
            if (m_text.size() < 120) m_text += ' ';
        } else if (m_column == 1) {
            m_text += ".com";
        } else if (m_column == 2) {
            if (!m_text.empty()) m_text.pop_back();
        } else {
            return Result::Accepted;
        }
    }
    return Result::Editing;
}

void Keyboard::draw() const
{
    const float width = kColumns * kKeyW + (kColumns - 1) * kGap;
    const float x0 = (Gfx::kWidth - width) / 2, y0 = 120;

    Gfx::rect(0, 0, Gfx::kWidth, Gfx::kHeight, 0xF0040A05, 0xF0040A05);
    Gfx::text(Gfx::kLabel, m_title.c_str(), x0, y0 - 64, kGreen);

    // Lo escrito (la contraseña con puntos, salvo que se pida verla).
    std::string shown = m_text;
    if (m_secret && !m_show) shown.assign(m_text.size(), '*');
    shown += "_";
    Gfx::rect(x0, y0 - 38, width, 28, 0xFF0B160D, 0xFF0B160D);
    Gfx::rect(x0, y0 - 11, width, 1, kGreen, kGreen);
    float textX = x0 + 8;
    const float textW = Gfx::textWidth(Gfx::kBody, shown.c_str());
    if (textW > width - 16) textX -= textW - (width - 16);   // se ve el final al escribir mucho
    Gfx::text(Gfx::kBody, shown.c_str(), textX, y0 - 35, kText);

    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < kColumns; ++c) {
            const float x = x0 + c * (kKeyW + kGap), y = y0 + r * (kKeyH + kGap);
            const bool focus = r == m_row && c == m_column;
            Gfx::roundRect(x, y, kKeyW, kKeyH, 6, focus ? kKeyFocus : kKey, focus ? 0xFF1E7E1E : kKey);
            char label[2] = {(m_shift ? kSymbolsShifted : kRows)[r][c], 0};
            Gfx::textCenter(Gfx::kBody, label, x + kKeyW / 2, y + (kKeyH - Gfx::lineHeight(Gfx::kBody)) / 2, kText);
        }
    const float specialW = (width - (kSpecialCount - 1) * kGap) / kSpecialCount;
    for (int c = 0; c < kSpecialCount; ++c) {
        const float x = x0 + c * (specialW + kGap), y = y0 + 4 * (kKeyH + kGap);
        const bool focus = m_row == 4 && c == m_column;
        Gfx::roundRect(x, y, specialW, kKeyH, 6, focus ? kKeyFocus : kKey, focus ? 0xFF1E7E1E : kKey);
        Gfx::textCenter(Gfx::kBody, kSpecial[c], x + specialW / 2, y + (kKeyH - Gfx::lineHeight(Gfx::kBody)) / 2, kText);
    }

    const char* help = m_secret ? "A escribir   X borrar   Y mayusculas   Back ver   Start aceptar   B cancelar"
                                : "A escribir   X borrar   Y mayusculas   Start aceptar   B cancelar";
    Gfx::textCenter(Gfx::kSmall, help, Gfx::kWidth / 2.0f, y0 + 5 * (kKeyH + kGap) + 10, kTextDim);
}
