"""Crea assets/data/artindex.tsv: de cada juego de redump, su arte en el
repositorio de MobCat (https://github.com/MobCat/MobCats-original-xbox-game-list).

    python scripts/make_art_index.py

Una linea por juego, separada por tabuladores:
    nombre redump (= nombre del ZIP sin .zip)  XMID  TitleID  portada  fondo  icono
Las tres rutas son relativas a las carpetas thumbnail/, background/ e icon/ del
repositorio, y van vacias si ese arte no existe (la Xbox no pide lo que no hay).
Se ejecuta a mano cuando MobCat actualice su base; no forma parte de la compilacion.
"""
import json
import os
import sqlite3
import tempfile
import urllib.request

REPO = 'MobCat/MobCats-original-xbox-game-list'
RAW = f'https://raw.githubusercontent.com/{REPO}/main/'
OUT = os.path.join(os.path.dirname(__file__), '..', 'assets', 'data', 'artindex.tsv')


def get(url, attempts=4):
    req = urllib.request.Request(url, headers={'User-Agent': 'ClassicXboxStore'})
    for attempt in range(attempts):
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                return r.read()
        except Exception as e:   # cortes a mitad (IncompleteRead) incluidos
            if attempt == attempts - 1:
                raise
            print(f'reintento {attempt + 1}: {e}')


def tree(folder_sha):
    t = json.loads(get(f'https://api.github.com/repos/{REPO}/git/trees/{folder_sha}?recursive=1'))
    if t.get('truncated'):
        raise SystemExit('GitHub corto la lista de archivos: hay que paginar')
    return {e['path'] for e in t['tree'] if e['type'] == 'blob'}


def main():
    redump = json.loads(get(RAW + 'redump-xbox-info.json'))
    with tempfile.TemporaryDirectory() as tmp:
        db_path = os.path.join(tmp, 'titles.db')
        open(db_path, 'wb').write(get(RAW + 'MobCatsOGXboxTitleIDs.db'))
        db = sqlite3.connect(db_path)
        xmid_to_tid = {}
        for xmid, tid in db.execute('select XMID, Title_ID from TitleIDs'):
            if xmid and tid:
                xmid_to_tid.setdefault(xmid, tid.upper())
        db.close()

    root = json.loads(get(f'https://api.github.com/repos/{REPO}/git/trees/main'))
    sha = {e['path']: e['sha'] for e in root['tree']}
    thumbs, backgrounds, icons = tree(sha['thumbnail']), tree(sha['background']), tree(sha['icon'])

    def thumb_for(xmid):
        base = f'{xmid[:2]}/{xmid}.jpg'
        if base in thumbs:
            return base
        # Sin portada base: la primera variante de region (MS10003W-PAL-GBR.jpg...).
        variants = sorted(p for p in thumbs if p.startswith(f'{xmid[:2]}/{xmid}-'))
        return variants[0] if variants else ''

    lines = []
    for xmid, info in redump.items():
        name = info.get('Filename', '').strip()
        if not name or '\t' in name:
            continue
        tid = xmid_to_tid.get(xmid, '')
        bg = f'{tid[:4]}/{tid}.jpg' if tid and f'{tid[:4]}/{tid}.jpg' in backgrounds else ''
        icon = f'{tid[:4]}/{tid}.png' if tid and f'{tid[:4]}/{tid}.png' in icons else ''
        lines.append('\t'.join([name, xmid, tid, thumb_for(xmid), bg, icon]))
    # Ordenado por bytes: la Xbox busca con busqueda binaria (strcmp).
    lines.sort(key=lambda s: s.split('\t', 1)[0].encode('utf-8'))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    with_thumb = sum(1 for l in lines if l.split('\t')[3])
    with_bg = sum(1 for l in lines if l.split('\t')[4])
    print(f'{len(lines)} juegos, {with_thumb} con portada, {with_bg} con fondo -> {os.path.normpath(OUT)}')


if __name__ == '__main__':
    main()
