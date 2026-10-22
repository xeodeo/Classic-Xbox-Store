"""Mete archivos dentro del XBE: genera un .cpp con un arreglo por archivo.

    python embed_files.py salida.cpp nombre1=archivo1 nombre2=archivo2 ...

Cada nombre queda como `extern const unsigned char nombre[]; extern const unsigned int nombre_size;`.
Detras de los datos va siempre un 0 que no cuenta en `_size` (mbedTLS lee los
certificados PEM como texto terminado en 0).
"""
import sys


def main():
    out_path = sys.argv[1]
    lines = ['// Generado por scripts/embed_files.py: no editar.\n']
    for arg in sys.argv[2:]:
        name, path = arg.split('=', 1)
        data = open(path, 'rb').read()
        lines.append(f'extern const unsigned char {name}[] = {{\n')
        for i in range(0, len(data), 24):
            lines.append(','.join(str(b) for b in data[i:i + 24]) + ',\n')
        lines.append('0};\n')
        lines.append(f'extern const unsigned int {name}_size = {len(data)};\n')
    with open(out_path, 'w', encoding='ascii') as f:
        f.writelines(lines)


if __name__ == '__main__':
    main()
