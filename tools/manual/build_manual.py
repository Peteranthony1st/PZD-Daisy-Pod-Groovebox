# Builds the User Manual: docs/user-manual.pdf.
#
# The chapters (manual/chapters/*.html, in name order) are plain HTML. Each
# screen is a script for the host renderer (host/render.cpp) written where
# the picture goes:
#
#   <pre class="shot" id="layer-status">wait 2000
#   click</pre>
#
# The script starts from power-on with the card template as the SD card;
# the screen at its end becomes the picture. An annotated screen adds
# numbered markers at screen pixels:
#
#   <figure class="annot"><pre class="shot" id="home">wait 2000</pre>
#     <ol><li data-x="2" data-y="0">The bar ...</li></ol></figure>
#
# A script line can be a shorthand from MACROS: "@demo" powers on and loads
# the card's demo song (PERF1) through Global:Perf, ending on Home;
# "@engine 25" powers on, switches on the engine on the 25th Global page
# after Tracks and opens it; "@airy" (inside Grains) loads Airy Piano and
# comes back to the Grain page; "@808" (inside SMPDR) loads 808 Boom.
#
# Then a contents page from the chapter (h1) and section (h2) headings,
# and Chrome prints it all to PDF.
import hashlib, html, os, re, shutil, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor
from PIL import Image
from common import HERE, REPO

CHROME = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'
OUT    = os.path.join(HERE, 'build', 'manual')
SHOTS  = os.path.join(OUT, 'shots')
RENDER = os.path.join(HERE, 'host', 'render')
SCALE  = 6                          # PNG pixels per screen pixel
INK, PANEL = (24, 26, 30), (240, 241, 237)
MACROS = {
    '@demo': 'wait 2000\nhold enc 1200\nturn 4\ntap b2\nknob 2 1.0\ntap b2\nwait 3000\nhold enc 1200\nwait 500',
    '@engine': 'wait 2000\nhold enc 1200\nturn {0}\ntap b1\nclick',
    '@airy': 'turn 13\ntap b2\ntap b2\ntap b2\nwait 3000\nturn -13',
    '@808': 'turn 7\ntap b2\ntap b2\ntap b2\ntap b2\nwait 3000\nturn -7',
}


def expand(line):
    word, _, arg = line.strip().partition(' ')
    return MACROS[word].format(arg) if word in MACROS else line


def render_shots(shots):
    """shots: {id: script}. Renders the ones whose script, or the
    renderer itself (so the firmware), changed."""
    os.makedirs(SHOTS, exist_ok=True)
    todo = []
    fw = hashlib.sha1(open(RENDER, 'rb').read()).hexdigest()
    for sid, script in shots.items():
        h = hashlib.sha1((fw + script).encode()).hexdigest()[:10]
        png = os.path.join(SHOTS, '%s-%s.png' % (sid, h))
        shots[sid] = png
        if not os.path.exists(png):
            todo.append((sid, script, png))
    if not todo:
        return
    card = tempfile.mkdtemp(prefix='pzd-card-')
    shutil.copytree(os.path.join(REPO, 'sdcard-template'), card, dirs_exist_ok=True)

    def one(job):
        sid, script, png = job
        with tempfile.TemporaryDirectory() as tmp:
            sp = os.path.join(tmp, 's.txt')
            open(sp, 'w').write(script + '\nshot screen\n')
            subprocess.run([RENDER, sp, tmp], check=True, env=dict(os.environ, PZD_HOST_SD=card))
            pbm = Image.open(os.path.join(tmp, 'screen.pbm')).convert('1')
            img = Image.new('RGB', (128 * SCALE, 64 * SCALE), PANEL)
            ink = pbm.convert('L').point(lambda v: 255 if v == 0 else 0).resize(img.size, Image.NEAREST)
            img.paste(INK, (0, 0), ink)
            img.save(png)
        print('  shot', sid)

    with ThreadPoolExecutor(8) as ex:
        list(ex.map(one, todo))
    shutil.rmtree(card)


def build():
    subprocess.run(['make', '-s', '-C', os.path.join(HERE, 'host')], check=True)
    chapters = sorted(f for f in os.listdir(os.path.join(HERE, 'manual', 'chapters')) if f.endswith('.html'))
    body = ''.join(open(os.path.join(HERE, 'manual', 'chapters', f)).read() for f in chapters)

    # Screens
    shots = {}
    for m in re.finditer(r'<pre class="shot" id="([^"]+)">(.*?)</pre>', body, re.S):
        script = html.unescape(m.group(2)).strip()
        shots[m.group(1)] = '\n'.join(expand(l) for l in script.split('\n'))
    render_shots(shots)
    body = re.sub(r'<pre class="shot" id="([^"]+)">.*?</pre>',
                  lambda m: '<img class="screen" src="%s" alt="">' % os.path.relpath(shots[m.group(1)], OUT),
                  body, flags=re.S)

    # Annotation markers: <li data-x data-y> -> numbered markers on the screen
    def annot(m):
        fig = m.group(0)
        items = re.findall(r'<li data-x="(-?\d+)" data-y="(-?\d+)">', fig)
        # Screen pixels to the box: inside its border (1.2px) and padding (2.2mm).
        inset = '(2.2mm + 1.2px)'
        marks = ''.join('<span class="mark" style="left:calc(%s + (100%% - 2 * %s) * %.4f);'
                        'top:calc(%s + (100%% - 2 * %s) * %.4f)">%d</span>'
                        % (inset, inset, (int(x) + .5) / 128, inset, inset, (int(y) + .5) / 64, i + 1)
                        for i, (x, y) in enumerate(items))
        return re.sub(r'(<img class="screen"[^>]*>)', r'<div class="annot-screen">\1%s</div>' % marks, fig, count=1)
    body = re.sub(r'<figure class="annot">.*?</figure>', annot, body, flags=re.S)

    # Numbering and contents
    toc, ch, sec = [], 0, 0
    def head(m):
        nonlocal ch, sec
        level, text = m.group(1), m.group(2)
        if level == '1':
            ch += 1; sec = 0; num = str(ch)
            hid = 'ch%d' % ch
            toc.append((1, num, text, hid))
            return ('<h1 id="%s"><span class="chap">Chapter %s</span>%s</h1>' % (hid, num, text))
        sec += 1; num = '%d.%d' % (ch, sec); hid = 's%d-%d' % (ch, sec)
        toc.append((2, num, text, hid))
        return '<h2 id="%s"><span class="num">%s</span>%s</h2>' % (hid, num, text)
    body = re.sub(r'<h([12])>(.*?)</h\1>', head, body)
    toc_html = '<section class="toc"><h3>Contents</h3><ol>' + ''.join(
        '<li class="l%d"><a href="#%s"><span class="num">%s</span>%s</a></li>' % (lv, hid, num, text)
        for lv, num, text, hid in toc) + '</ol></section>'

    cover = os.path.join(HERE, 'build', 'manual-cover.png')
    page = open(os.path.join(HERE, 'manual', 'manual.html')).read()
    page = page.replace('{{COVER}}', os.path.relpath(cover, OUT)).replace('{{LOGO}}', os.path.relpath(
        os.path.join(HERE, 'assets', 'pzd-logo-white.png'), OUT)).replace('{{TOC}}', toc_html).replace('{{BODY}}', body)
    os.makedirs(OUT, exist_ok=True)
    shutil.copy(os.path.join(HERE, 'manual', 'manual.css'), OUT)
    src = os.path.join(OUT, 'manual.html')
    open(src, 'w').write(page)
    pdf = os.path.join(REPO, 'docs', 'user-manual.pdf')
    subprocess.run([CHROME, '--headless=new', '--disable-gpu', '--no-pdf-header-footer',
                    '--generate-pdf-document-outline', '--print-to-pdf=' + pdf, 'file://' + src],
                   check=True, stderr=subprocess.DEVNULL)
    print('  docs/user-manual.pdf')


if __name__ == '__main__':
    build()
