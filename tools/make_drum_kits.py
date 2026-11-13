# Synthesises the SMPDR drum kits and breaks shipped in sdcard-template/
# -- every sound made here from oscillators and noise, so they're ours to
# give away (CC0), no sample licences involved.
#
#   python3 tools/make_drum_kits.py [preview_dir]
#
# Writes sdcard-template/SAMPLES/KITS1/<kit>/NN Name.wav (16 pads, loaded
# in name order from C2) and SAMPLES/BREAKS1/<break>/NN <break>.wav (the
# break in 8 slices from C2), 48 kHz 16-bit mono, audio sector-aligned.
# Every kit uses the same pad order (General MIDI's drum map where it matters: kick C2, snare D2, closed hat F#2, open hat A#2...), so
# a pattern written for one kit plays on all of them. With preview_dir,
# also writes a WAV per kit to listen to on a computer: every pad in turn,
# then a short beat.
#
# Needs only numpy. Seeded, so it makes the same files every time.

import os
import struct
import sys

import numpy as np

SR = 48000
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'sdcard-template', 'SAMPLES')
rng = np.random.default_rng(1)

# --- building blocks ---------------------------------------------------------


def t_of(sec):
    return np.arange(int(sec * SR)) / SR


def env(sec, decay, attack=0.0005):
    t = t_of(sec)
    a = np.minimum(1.0, t / attack) if attack > 0 else 1.0
    return a * np.exp(-t / decay)


def sweep(sec, f0, f1, tau):
    # A sine whose pitch falls from f0 towards f1 with time constant tau.
    t = t_of(sec)
    f = f1 + (f0 - f1) * np.exp(-t / tau)
    return np.sin(2 * np.pi * np.cumsum(f) / SR)


def square(sec, f):
    t = t_of(sec)
    return np.sign(np.sin(2 * np.pi * f * t + 0.3))


def noise(sec):
    return rng.uniform(-1, 1, int(sec * SR))


def biquad(x, kind, f, q=0.707):
    # RBJ cookbook biquad, one sample at a time (no scipy here).
    w = 2 * np.pi * f / SR
    cw, sw = np.cos(w), np.sin(w)
    al = sw / (2 * q)
    if kind == 'lp':
        b = [(1 - cw) / 2, 1 - cw, (1 - cw) / 2]
    elif kind == 'hp':
        b = [(1 + cw) / 2, -(1 + cw), (1 + cw) / 2]
    else:  # band-pass, 0 dB peak
        b = [al, 0.0, -al]
    a0, a1, a2 = 1 + al, -2 * cw, 1 - al
    b0, b1, b2 = b[0] / a0, b[1] / a0, b[2] / a0
    a1, a2 = a1 / a0, a2 / a0
    y = np.zeros_like(x)
    x1 = x2 = y1 = y2 = 0.0
    for i, v in enumerate(x):
        o = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1, y2, y1 = x1, v, y1, o
        y[i] = o
    return y


def mix(*parts):
    n = max(len(p) for p in parts)
    out = np.zeros(n)
    for p in parts:
        out[:len(p)] += p
    return out


def drive(x, amount):
    return np.tanh(x * amount) / np.tanh(amount)


def metal(sec):
    # The 808's six detuned square waves -- the basis of its hats and cymbals.
    return sum(square(sec, f) for f in (205.3, 304.4, 369.6, 522.7, 540.0, 800.0)) / 6


def finish(x, level):
    # Trim trailing silence, fade the tail, set the peak.
    x = np.asarray(x, dtype=float)
    peak = np.max(np.abs(x)) or 1.0
    x = x / peak
    above = np.nonzero(np.abs(x) > 10 ** (-50 / 20))[0]
    x = x[:above[-1] + 1] if len(above) else x[:1]
    # A short fade, or a long one for ringing sounds so they don't stop dead.
    fade = min(len(x), max(int(0.004 * SR), len(x) // 5 if len(x) > SR // 2 else 0))
    x[-fade:] *= np.linspace(1, 0, fade)
    return x * level


def crush(x, bits, hold):
    # Lo-fi: fewer bits and a lower sample rate (each value held `hold` samples).
    x = np.repeat(x[::hold], hold)[:len(x)]
    q = 2 ** (bits - 1)
    return np.round(x * q) / q


# --- the sounds --------------------------------------------------------------
# Levels (the last argument to finish) set the kit's balance: kicks loudest,
# hats and cymbals well under.

def kick_808():
    body = sweep(1.2, 160, 49, 0.035) * env(1.2, 0.42)
    click = biquad(noise(0.01), 'hp', 2000) * env(0.01, 0.002) * 0.3
    return finish(drive(mix(body, click), 1.6), 0.97)


def kick_909():
    body = sweep(0.6, 260, 52, 0.022) * env(0.6, 0.2)
    click = biquad(noise(0.02), 'bp', 3500, 0.8) * env(0.02, 0.004) * 0.8
    return finish(drive(mix(body, click), 2.6), 0.97)


def snare(tone1, tone2, decay_t, decay_n, hp, level=0.85):
    sec = max(decay_t, decay_n) * 6
    tone = (sweep(sec, tone1 * 1.15, tone1, 0.01) + 0.6 * np.sin(2 * np.pi * tone2 * t_of(sec))) * env(sec, decay_t)
    rattle = biquad(noise(sec), 'hp', hp) * env(sec, decay_n)
    return finish(mix(tone * 0.6, rattle), level)


def clap(spread=0.011, tail=0.12, centre=1200, level=0.8):
    n = noise(0.6)
    e = np.zeros(len(n))
    for k in range(4):
        s = int(k * spread * SR)
        burst = env(0.6, 0.0035 if k < 3 else tail)[:len(n) - s]
        e[s:] = np.maximum(e[s:], burst)
    return finish(biquad(n * e, 'bp', centre, 0.9), level)


def rim(level=0.7):
    sec = 0.08
    click = np.sin(2 * np.pi * 1700 * t_of(sec)) * env(sec, 0.006) + 0.5 * np.sin(2 * np.pi * 480 * t_of(sec)) * env(sec, 0.012)
    return finish(mix(click, biquad(noise(sec), 'bp', 3000, 1.5) * env(sec, 0.004) * 0.4), level)


def tom(f, decay=0.28, level=0.82):
    sec = decay * 4
    return finish(mix(sweep(sec, f * 1.6, f, 0.05) * env(sec, decay),
                      biquad(noise(0.02), 'bp', f * 4, 1) * env(0.02, 0.003) * 0.2), level)


def hat_808(decay, level):
    sec = decay * 5
    return finish(biquad(biquad(metal(sec), 'bp', 9500, 1.1), 'hp', 7000) * env(sec, decay), level)


def hat_909(decay, level):
    sec = decay * 5
    m = biquad(metal(sec) * 0.6 + noise(sec) * 0.5, 'hp', 8000)
    return finish(m * env(sec, decay), level)


def cymbal(decay, hp, level, bright=0.6):
    sec = decay * 3.5
    m = biquad(metal(sec) * bright + noise(sec) * (1 - bright), 'hp', hp)
    return finish(m * env(sec, decay, 0.002), level)


def cowbell(level=0.6):
    sec = 0.6
    tone = biquad(square(sec, 540) + square(sec, 800), 'bp', 1600, 1.2)
    return finish(tone * (0.7 * env(sec, 0.02) + 0.3 * env(sec, 0.12)), level)


def conga(f=330, level=0.72):
    sec = 0.5
    return finish(mix(sweep(sec, f * 1.3, f, 0.008) * env(sec, 0.09),
                      biquad(noise(0.01), 'bp', 2500, 1) * env(0.01, 0.002) * 0.3), level)


def clave(level=0.65):
    sec = 0.15
    return finish(np.sin(2 * np.pi * 2500 * t_of(sec)) * env(sec, 0.018), level)


def shaker(level=0.5):
    sec = 0.25
    t = t_of(sec)
    e = np.minimum(1, t / 0.03) * np.exp(-np.maximum(0, t - 0.03) / 0.05)
    return finish(biquad(noise(sec), 'hp', 6000) * e, level)


def zap(f0=2500, f1=90, tau=0.025, decay=0.12, level=0.75):
    sec = decay * 5
    return finish(sweep(sec, f0, f1, tau) * env(sec, decay), level)


def blip(f, level=0.6):
    sec = 0.15
    return finish(square(sec, f) * env(sec, 0.025) * 0.7, level)


def fm_bell(f=880, decay=0.9, level=0.5):
    sec = decay * 4
    t = t_of(sec)
    mod = np.sin(2 * np.pi * f * 3.5 * t) * 2.5 * env(sec, decay * 0.4)
    return finish(np.sin(2 * np.pi * f * t + mod) * env(sec, decay), level)


def swell(level=0.5):
    # A crash that rises into its peak, for builds.
    sec = 1.6
    t = t_of(sec)
    e = (t / sec) ** 2.2
    return finish(biquad(metal(sec) * 0.5 + noise(sec) * 0.5, 'hp', 5000) * e, level)


def lofi(x, bits=7, hold=4, lp=5500):
    # Darken, reduce, then a touch of tape-ish saturation and dust.
    y = crush(biquad(x, 'lp', lp), bits, hold)
    y = drive(y, 1.4) + biquad(rng.uniform(-1, 1, len(y)), 'lp', 3000) * 0.004
    return finish(y, np.max(np.abs(x)))


# --- the kits ----------------------------------------------------------------
# One pad order for every kit (01 = C2):
PADS = ['Kick', 'Rim', 'Snare', 'Clap', 'Snare 2', 'Tom Low', 'Hat Closed', 'Tom Mid',
        'Hat Pedal', 'Tom High', 'Hat Open', 'Cowbell', 'Perc', 'Crash', 'Perc 2', 'Ride']


def kit_808():
    return [kick_808(), rim(), snare(238, 476, 0.05, 0.12, 1800), clap(), snare(300, 560, 0.03, 0.07, 2500, 0.8),
            tom(90), hat_808(0.045, 0.5), tom(130), hat_808(0.025, 0.42), tom(180),
            hat_808(0.32, 0.45), cowbell(), conga(330), cymbal(1.1, 4000, 0.45), conga(220),
            cymbal(1.0, 5000, 0.4, 0.8)]


def kit_909():
    return [kick_909(), rim(0.65), snare(185, 330, 0.04, 0.16, 1200, 0.9), clap(0.009, 0.16, 1400),
            snare(220, 400, 0.025, 0.09, 3000, 0.82), tom(100, 0.22), hat_909(0.04, 0.48), tom(150, 0.22),
            hat_909(0.022, 0.4), tom(210, 0.2), hat_909(0.28, 0.44), cowbell(0.55), clave(),
            cymbal(1.3, 3500, 0.45, 0.4), shaker(), cymbal(1.1, 6000, 0.38, 0.85)]


def kit_lofi():
    k808, k909 = kit_808(), kit_909()
    base = [k909[0], k808[1], k808[2], k909[3], k909[4], k808[5], k808[6], k808[7],
            k808[8], k808[9], k808[10], k808[11], k808[12], k909[13], k909[14], k808[15]]
    # A rounder, deeper kick under the crunch.
    base[0] = finish(drive(sweep(0.9, 140, 46, 0.04) * env(0.9, 0.3), 2.0), 0.97)
    return [lofi(x) for x in base]


def kit_electro():
    return [finish(drive(mix(sweep(0.5, 1200, 48, 0.012) * env(0.5, 0.17)), 2.2), 0.97),
            blip(1800), finish(mix(zap(3000, 400, 0.01, 0.06, 0.5), biquad(noise(0.3), 'bp', 2500, 2) * env(0.3, 0.07)), 0.85),
            clap(0.008, 0.09, 1800, 0.78), zap(4000, 200, 0.006, 0.05, 0.7),
            zap(600, 70, 0.06, 0.22, 0.8), hat_909(0.03, 0.46), zap(900, 110, 0.05, 0.2, 0.8),
            shaker(0.42), zap(1300, 160, 0.04, 0.18, 0.78), hat_909(0.22, 0.42), clave(0.6),
            blip(660, 0.55), swell(0.45), fm_bell(1320, 0.5, 0.5), fm_bell(880, 1.0, 0.45)]


KITS = [('808 Boom', kit_808), ('909 Punch', kit_909), ('Lo-Fi Dust', kit_lofi), ('Electro Perc', kit_electro)]

# --- breaks --------------------------------------------------------------------
# 16th-note grids: pad index (into the kit) and velocity, per step.


def render_beat(hits, bpm, bars, pat, swing=0.0):
    steps, length = pat
    step = 60 / bpm / 4
    out = np.zeros(int(bars * 16 * step * SR) + SR)
    for s in range(bars * 16):
        for pad, vel in steps.get(s % length, []):  # a shorter pattern repeats
            t = s * step + (step * swing if s % 2 else 0)
            i = int(t * SR)
            h = hits[pad] * vel
            out[i:i + len(h)] += h[:len(out) - i]
    loop = int(bars * 16 * step * SR)
    # Fold the overhang back onto the start, so the loop wraps seamlessly.
    out[:len(out) - loop] += out[loop:]
    out = out[:loop]
    return out / max(1.0, np.max(np.abs(out)) / 0.95)


def pattern(spec):
    # spec: {pad: "x..x" a character per 16th, x = full, o = soft}
    steps = {}
    for pad, row in spec.items():
        for s, c in enumerate(row):
            if c in 'xo':
                steps.setdefault(s, []).append((pad, 1.0 if c == 'x' else 0.55))
    return steps, max(len(row) for row in spec.values())


K, RIM, SN, CL, SN2, TL, HC, TM, HP, TH, HO = range(11)
SLICES = 8 # per break

BREAKS = [
    ('Boom Bap 90', kit_808, 90, 1, 0.12, {
        K:  'x.....x...x.....',
        SN: '....x.......x...',
        HC: 'x.x.x.x.x.x.xox.',
        HO: '.............x..',
        RIM:'.......o........'}),
    ('Punch 120', kit_909, 120, 2, 0.0, {
        K:  'x...x...x...x...x...x...x..xx...',
        CL: '....x.......x.......x.......x...',
        HC: '..x...x...x...x...x...x...x...x.',
        HO: '..............................x.',
        SN2:'...............o.............ooo'}),
]

# --- writing -----------------------------------------------------------------


def write_wav(path, x):
    # A plain 16-bit mono WAV, with a JUNK chunk padding the header so the
    # audio starts on a 512-byte boundary: the Pod then reads it in whole
    # SD sectors, the safest path for its SD driver.
    os.makedirs(os.path.dirname(path), exist_ok=True)
    pcm = (np.clip(x, -1, 1) * 32767).astype('<i2').tobytes()
    if len(pcm) & 1:
        pcm += b'\0'
    fmt = struct.pack('<4sIHHIIHH', b'fmt ', 16, 1, 1, SR, SR * 2, 2, 16)
    junk_len = 512 - 12 - len(fmt) - 8 - 8
    junk = struct.pack('<4sI', b'JUNK', junk_len) + bytes(junk_len)
    data = struct.pack('<4sI', b'data', len(pcm)) + pcm
    body = b'WAVE' + fmt + junk + data
    with open(path, 'wb') as f:
        f.write(struct.pack('<4sI', b'RIFF', len(body)) + body)


LICENCE = ("{name} -- synthesised for the PZD Daisy Pod Groovebox by "
           "tools/make_drum_kits.py. CC0 public domain: use it for anything.\n")


def main():
    preview = sys.argv[1] if len(sys.argv) > 1 else None
    built = {}
    for name, fn in KITS:
        hits = fn()
        built[fn] = hits
        d = os.path.join(ROOT, 'KITS1', name)
        for i, (pad, x) in enumerate(zip(PADS, hits)):
            write_wav(os.path.join(d, '%02d %s.wav' % (i + 1, pad)), x)
        with open(os.path.join(d, 'README.txt'), 'w') as f:
            f.write(LICENCE.format(name=name) + "16 pads from C2: " + ", ".join(PADS) + ".\n")
        secs = sum(len(x) for x in hits) / SR
        print('%-14s %2d pads  %.1f s' % (name, len(hits), secs))
        if preview:
            gap = int(0.35 * SR)
            seq = np.concatenate([np.concatenate([x, np.zeros(max(0, gap - len(x)))]) if len(x) < gap
                                  else x for x in hits])
            beat = render_beat(hits, 100, 2, pattern(BREAKS[0][5]), 0.1)
            write_wav(os.path.join(preview, name + '.wav'), np.concatenate([seq, np.zeros(SR // 2), beat, beat]))
    for name, fn, bpm, bars, swing, spec in BREAKS:
        hits = built.get(fn) or fn()
        x = render_beat(hits, bpm, bars, pattern(spec), swing)
        d = os.path.join(ROOT, 'BREAKS1', name)
        # 8 slices from C2, on the beat grid the pattern's built on: 8ths
        # for one bar, quarters for two -- each starts on a hit and is long
        # enough to play on its own (16 made them too short to be useful).
        n = len(x) // SLICES
        for k in range(SLICES):
            write_wav(os.path.join(d, '%02d %s.wav' % (k + 1, name)), x[k * n:(k + 1) * n])
        with open(os.path.join(d, 'README.txt'), 'w') as f:
            f.write(LICENCE.format(name=name) + "%d bar%s at %d BPM, in %d slices from C2 (each a %s) -- play"
                    " C2 to G2 in order for the whole break; set Global:Tempo to %d BPM for it to sit in time.\n"
                    % (bars, 's' if bars > 1 else '', bpm, SLICES, '8th' if bars == 1 else 'beat', bpm))
        print('%-14s break  %.2f s' % (name, len(x) / SR))
        if preview:
            write_wav(os.path.join(preview, 'Break ' + name + '.wav'), np.concatenate([x] * 4))


if __name__ == '__main__':
    main()
