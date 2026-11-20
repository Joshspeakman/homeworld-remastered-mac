#!/usr/bin/env python3
"""hw_art.py <art folder> [--no-downloads]

The launch window's game cards (install.sh), written to the data folder's art/ (never to this project): a screenshot of
each game from the Homeworld Remastered Collection's Steam store page (app 244160).
  shot_hw1.jpg  Homeworld Remastered: the Mothership in a red nebula
  shot_hw2.jpg  Homeworld 2 Remastered: a battle in blue (its corner caption is cropped off by the launcher)
Without them the cards use the backgrounds of Aspyr's launcher in the game's own Resources folder. Logos and the
window's background are read from there at run time.
"""
import json, os, sys, urllib.request

SHOTS = {'shot_hw1.jpg': ('ss_c5e0efbdbf2a645ef59abfd4295da3090753978e.1920x1080.jpg', 10),
         'shot_hw2.jpg': ('ss_adeedef67dc718c2bccdd36eae0a8c41d64885aa.1920x1080.jpg', 14)}
BASE = 'https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/244160/'

def download(url, path):
    try:
        with urllib.request.urlopen(url, timeout=20) as r: data = r.read()
    except Exception:
        return False
    if not data.startswith(b'\xff\xd8\xff') or len(data) < 20000: return False
    with open(path + '.part', 'wb') as f: f.write(data)
    os.replace(path + '.part', path); return True

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if '--no-downloads' in sys.argv or not args: return 1
    art = args[0]; os.makedirs(art, exist_ok=True); got = []; listing = None
    for out, (name, idx) in SHOTS.items():
        path = os.path.join(art, out)
        if os.path.exists(path) or download(BASE + name, path): got.append(out); continue
        try:                                     # Steam renamed it: the same place in the store page's list
            if listing is None:
                with urllib.request.urlopen('https://store.steampowered.com/api/appdetails?appids=244160&filters=screenshots', timeout=20) as r:
                    listing = json.load(r)['244160']['data']['screenshots']
            if download(listing[min(idx, len(listing) - 1)]['path_full'].split('?')[0], path): got.append(out)
        except Exception:
            pass
    print(', '.join(got) or 'none')
    return 0 if got else 1

if __name__ == '__main__':
    sys.exit(main())
