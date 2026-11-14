# The README's gallery: the Groovebox rendered from the case STLs, each with
# a different firmware screen on it (images/pod-*.png, 1000 x 1000).
import os
from common import REPO
from draw import *

SHOTS = [('home_audio', 'pod-home'), ('home_midi', 'pod-midi-tracks'), ('global_tracks', 'pod-global-tracks'),
         ('ssaw_supersaw', 'pod-supersaw'), ('song', 'pod-song')]
BG = (238, 238, 240)

def shot(screen):
    front = flower_glow(draw_parts(body('colour'), 'colour', screen=screen))
    cv = Image.new('RGB', front.size, BG); wheel(ImageDraw.Draw(cv), 0, 0, False)
    wm = Image.new('L', front.size, 0); ImageDraw.Draw(wm).ellipse([915, 1497, 1125, 1707], fill=255)
    m = Image.fromarray(np.maximum(sil * 255, np.array(wm)).astype(np.uint8))
    dev = Image.composite(front, cv, Image.fromarray((sil * 255).astype(np.uint8)))
    S = 1000 * 2; h = int(S * 0.84); w = int(dev.width * h / dev.height)
    dev = dev.resize((w, h), Image.LANCZOS); m = m.resize((w, h), Image.LANCZOS)
    out = Image.new('RGB', (S, S), BG); x = (S - w) // 2; y = (S - h) // 2
    sh = Image.new('L', out.size, 0); sh.paste(m.point(lambda v: 120 if v > 40 else 0), (x + 30, y + 46))
    out.paste((150, 150, 156), (0, 0), sh.filter(ImageFilter.GaussianBlur(40)))
    out.paste(dev, (x, y), m)
    return out.resize((1000, 1000), Image.LANCZOS)

for screen, name in SHOTS:
    shot(screen).save(os.path.join(REPO, 'images', name + '.png'), optimize=True)
