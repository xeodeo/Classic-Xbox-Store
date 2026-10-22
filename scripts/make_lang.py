"""Genera src/ui/LangTable.inc desde scripts/translations.py.

    python scripts/make_lang.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from translations import TEXTS  # noqa: E402

OUT = os.path.join(os.path.dirname(__file__), '..', 'src', 'ui', 'LangTable.inc')


def c_string(text):
    return '"' + text.replace('\\', '\\\\').replace('"', '\\"') + '"'


def main():
    lines = ['// Generado por scripts/make_lang.py desde scripts/translations.py: no editar.\n',
             '// {español (como esta en el codigo), ingles, portugues}\n']
    for spanish in sorted(TEXTS, key=lambda s: s.encode('utf-8')):
        english, portuguese = TEXTS[spanish]
        lines.append(f'{{{c_string(spanish)}, {c_string(english)}, {c_string(portuguese)}}},\n')
    with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.writelines(lines)
    print(f'{len(TEXTS)} textos -> {os.path.normpath(OUT)}')


if __name__ == '__main__':
    main()
