# Synthesises the SMPVO sounds shipped in sdcard-template/ -- multi-samples
# and single Sounds, every one made here from sines, harmonics and noise, so
# they're ours to give away (CC0), no sample licences involved.
#
#   python3 tools/make_sounds.py [preview_dir]
#
# Writes sdcard-template/SAMPLES/MULTISMP1/<sound>/<sound> <note>.wav: one
# zone every few notes (each plays from its nearest recording), named with
# the note as the Pod reads it (C4 = MIDI 60); and SAMPLES/SOUNDS1/<sound>
# <note>.wav, single samples played across the keyboard from that root --
# stabs, chords, vocal hits and textures, where the stretch is part of it. 48 kHz 16-bit mono, audio
# sector-aligned like make_drum_kits.py's. The sampler plays a sample once
# (no looping), so a held note lasts as long as its zone: pads are recorded
# long, plucked and struck sounds die away naturally. With preview_dir,
# also writes a WAV per sound: a short phrase across its range.
#
# Needs only numpy. Seeded, so it makes the same files every time.

import os
import struct
import sys

import numpy as np

SR = 48000
SAMPLES = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'sdcard-template', 'SAMPLES')
ROOT = os.path.join(SAMPLES, 'MULTISMP1')
rng = np.random.default_rng(2)
NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B']


def note_name(m):
    return '%s%d' % (NAMES[m % 12], m // 12 - 1)


def hz(m):
    return 440.0 * 2 ** ((m - 69) / 12)


def t_of(sec):
    return np.arange(int(sec * SR)) / SR


def adsr(sec, a, d, s, r_start=None, r=0.3):
    # Attack, decay to sustain level s, then (from r_start) a release to 0
    # over r -- baked into the zone, since the sample plays once.
    t = t_of(sec)
    e = np.where(t < a, t / max(a, 1e-6), s + (1 - s) * np.exp(-(t - a) / max(d, 1e-6)))
    if r_start is not None:
        e = e * np.clip(1 - (t - r_start) / r, 0, 1)
    return e


def saw(sec, f, detune_cents=0.0, top=12000):
    # Band-limited: harmonics up to `top` (and Nyquist), each 1/k.
    f = f * 2 ** (detune_cents / 1200)
    t = t_of(sec)
    ph = 2 * np.pi * f * t + rng.uniform(0, 2 * np.pi)
    out = np.zeros(len(t))
    for k in range(1, int(min(top, SR / 2 - 200) / f) + 1):
        out += np.sin(k * ph) / k
    return out * 0.55


def one_pole_lp(x, fc):
    a = np.exp(-2 * np.pi * fc / SR)
    y = np.empty_like(x)
    z = 0.0
    for i, v in enumerate(x):
        z = (1 - a) * v + a * z
        y[i] = z
    return y


def finish(x, level=0.8):
    x = np.asarray(x, dtype=float)
    x = x / (np.max(np.abs(x)) or 1.0)
    above = np.nonzero(np.abs(x) > 10 ** (-54 / 20))[0]
    x = x[:above[-1] + 1] if len(above) else x[:1]
    fade = min(len(x), max(int(0.005 * SR), len(x) // 6 if len(x) > SR else 0))
    x[-fade:] *= np.linspace(1, 0, fade) ** 2
    return x * level


# --- the sounds: (name, zone notes, zone length s, render(midi, sec)) --------


def fm_epiano(m, sec):
    # Two-operator FM at 1:1 for the body, a quick 14:1 "tine" on top; the
    # brightness falls away faster than the note, lower notes ring longer.
    f = hz(m)
    t = t_of(sec)
    ring = 1.6 * (1.0 + (60 - m) / 60)
    idx = 1.6 * np.exp(-t / 0.35) + 0.25
    body = np.sin(2 * np.pi * f * t + idx * np.sin(2 * np.pi * f * t))
    tine = np.sin(2 * np.pi * f * t + 1.2 * np.exp(-t / 0.03) * np.sin(2 * np.pi * 14 * f * t))
    amp = np.minimum(1, t / 0.002) * np.exp(-t / ring)
    return finish((body + 0.35 * tine * np.exp(-t / 0.25)) * amp, 0.8)


def warm_pad(m, sec):
    # Three detuned saws and a sub sine through a gentle low-pass, slow
    # attack, held, then released at the end of the zone.
    f = hz(m)
    x = saw(sec, f, -9) + saw(sec, f, 0) + saw(sec, f, +8) + 0.6 * np.sin(2 * np.pi * f / 2 * t_of(sec))
    x = one_pole_lp(one_pole_lp(x, min(3200, f * 6)), min(4500, f * 9))
    return finish(x * adsr(sec, 0.45, 0.8, 0.85, sec - 0.9, 0.9), 0.75)


def soft_pluck(m, sec):
    # Karplus-Strong: a noise burst circulating through a delay line, each
    # pass averaged with its neighbour (the averaging is the string's
    # damping). The averager adds half a sample of delay, and the rest of
    # the fraction is interpolated, so the pitch is exact.
    f = hz(m)
    n = int(sec * SR)
    period = SR / f - 0.5
    d = int(period)
    frac = period - d
    burst = one_pole_lp(rng.uniform(-1, 1, d + 2), min(9000, f * 12))
    line = list(burst - np.mean(burst))
    # Damping per pass, chosen so every note rings about the same time (a
    # little longer in the bass): the averager already loses cos(pi f/SR)
    # of the fundamental each pass, so only the rest is added here.
    t60 = 3.0 if m < 48 else 2.4
    loss = min(0.9999, 0.001 ** (1 / (f * t60)) / np.cos(np.pi * f / SR))
    out = np.zeros(n)
    for i in range(n):
        out[i] = line[0]
        a = 0.5 * (line[0] + line[1])
        b = 0.5 * (line[1] + line[2])
        line.pop(0)
        line.append(loss * ((1 - frac) * a + frac * b))
    # DC blocker, then a soft pick attack.
    y = np.empty(n)
    px = py = 0.0
    for i, v in enumerate(out):
        py = v - px + 0.995 * py
        px = v
        y[i] = py
    return finish(y * np.minimum(1, t_of(sec) / 0.001), 0.8)


def sub_bass(m, sec):
    # A sine with a touch of second harmonic and soft drive for weight on
    # small speakers, a short pluck at the start, sustained then released.
    f = hz(m)
    t = t_of(sec)
    x = np.sin(2 * np.pi * f * t) + 0.25 * np.sin(4 * np.pi * f * t)
    x = np.tanh(1.6 * x) + 0.3 * np.sin(2 * np.pi * f * 3 * t) * np.exp(-t / 0.04)
    return finish(x * adsr(sec, 0.004, 0.5, 0.75, sec - 0.35, 0.35), 0.85)


def glass_bell(m, sec):
    # FM at an inharmonic 3.5:1 for the clang, plus a pure partial an
    # octave and a fifth up that outlasts it.
    f = hz(m)
    t = t_of(sec)
    idx = 2.2 * np.exp(-t / 0.5)
    x = np.sin(2 * np.pi * f * t + idx * np.sin(2 * np.pi * 3.5 * f * t)) * np.exp(-t / 1.1)
    x += 0.3 * np.sin(2 * np.pi * 3 * f * t) * np.exp(-t / 1.8)
    return finish(x * np.minimum(1, t / 0.001), 0.7)


def saw_lead(m, sec):
    # Two saws a few cents apart and a square an octave down, with a filter
    # that opens on the attack and settles.
    f = hz(m)
    t = t_of(sec)
    x = saw(sec, f, -6) + saw(sec, f, 6) + 0.4 * np.sign(np.sin(np.pi * f * t))
    cut = 1200 + 3800 * np.exp(-t / 0.18)
    y = np.empty_like(x)
    z1 = z2 = 0.0
    for i, v in enumerate(x):
        a = np.exp(-2 * np.pi * min(cut[i], f * 12) / SR)
        z1 = (1 - a) * v + a * z1
        z2 = (1 - a) * z1 + a * z2
        y[i] = z2
    return finish(y * adsr(sec, 0.006, 0.4, 0.8, sec - 0.3, 0.3), 0.75)


def zones(lo, hi, step):
    return list(range(lo, hi + 1, step))


SOUNDS = [
    ('FM E.Piano', zones(36, 84, 6), 2.6, fm_epiano),
    ('Warm Pad', zones(36, 84, 12), 4.5, warm_pad),
    ('Soft Pluck', zones(36, 84, 6), 2.0, soft_pluck),
    ('Sub Bass', zones(24, 60, 6), 2.0, sub_bass),
    ('Glass Bell', zones(48, 96, 6), 2.4, glass_bell),
    ('Saw Lead', zones(36, 84, 6), 2.2, saw_lead),
]

# --- single Sounds (SOUNDS1): (name, root note, render()) ---------------------


def biquad_bp(x, f, q):
    # RBJ band-pass (0 dB peak), one sample at a time.
    w = 2 * np.pi * f / SR
    al = np.sin(w) / (2 * q)
    a0 = 1 + al
    b0, b2 = al / a0, -al / a0
    a1, a2 = -2 * np.cos(w) / a0, (1 - al) / a0
    y = np.zeros_like(x)
    x1 = x2 = y1 = y2 = 0.0
    for i, v in enumerate(x):
        o = b0 * v + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1, y2, y1 = x1, v, y1, o
        y[i] = o
    return y


def env_filter(x, start, end, tau, f_cap=16000):
    # A two-pole low-pass whose cutoff falls from start to end Hz.
    t = t_of(len(x) / SR)
    cut = np.minimum(f_cap, end + (start - end) * np.exp(-t / tau))
    y = np.empty_like(x)
    z1 = z2 = 0.0
    for i, v in enumerate(x):
        a = np.exp(-2 * np.pi * cut[i] / SR)
        z1 = (1 - a) * v + a * z1
        z2 = (1 - a) * z1 + a * z2
        y[i] = z2
    return y


def kalimba():
    # A metal tine: the fundamental and its odd, inharmonic overtones, which
    # die first, and a little thumb noise.
    f, sec = hz(60), 2.5
    t = t_of(sec)
    x = np.sin(2 * np.pi * f * t) * np.exp(-t / 0.9)
    x += 0.35 * np.sin(2 * np.pi * f * 5.4 * t) * np.exp(-t / 0.12)
    x += 0.15 * np.sin(2 * np.pi * f * 10.9 * t) * np.exp(-t / 0.05)
    return finish(x * np.minimum(1, t / 0.001), 0.8)


def marimba():
    # A wooden bar: the fundamental and the tuned 4x overtone, short and round.
    f, sec = hz(60), 1.4
    t = t_of(sec)
    x = np.sin(2 * np.pi * f * t) * np.exp(-t / 0.35) + 0.4 * np.sin(2 * np.pi * 4 * f * t) * np.exp(-t / 0.06)
    x += 0.08 * biquad_bp(rng.uniform(-1, 1, len(t)), 1800, 1.5) * np.exp(-t / 0.008)
    return finish(x * np.minimum(1, t / 0.0015), 0.8)


def vox(formants, sec=3.0):
    # A sung vowel: a glottal buzz (a saw, with a little vibrato) through
    # three formant band-passes.
    f = hz(57)
    t = t_of(sec)
    vib = 1 + 0.006 * np.sin(2 * np.pi * 5.2 * t) * np.minimum(1, t / 0.6)
    ph = 2 * np.pi * np.cumsum(f * vib) / SR
    buzz = sum(np.sin(k * ph) / k for k in range(1, int(9000 / f)))
    x = sum(g * biquad_bp(buzz, ff, q) for ff, q, g in formants)
    return finish(x * adsr(sec, 0.12, 0.5, 0.9, sec - 0.5, 0.5), 0.75)


def chord_stab():
    # The house-music stab: a minor 7th of saws, filter snapping shut.
    sec = 1.0
    x = sum(saw(sec, hz(m), dc) for m, dc in ((60, -5), (63, 4), (67, -3), (70, 6)))
    return finish(env_filter(x, 9000, 400, 0.09) * adsr(sec, 0.002, 0.25, 0.0), 0.8)


def brass_hit():
    # A brassy swell: saws whose filter opens a moment after the note.
    sec = 1.6
    t = t_of(sec)
    x = saw(sec, hz(60), -4) + saw(sec, hz(60), 4) + 0.5 * saw(sec, hz(48))
    cut = 500 + 4500 * (1 - np.exp(-t / 0.06)) * np.exp(-t / 0.7)
    y = np.empty_like(x)
    z1 = z2 = 0.0
    for i, v in enumerate(x):
        a = np.exp(-2 * np.pi * cut[i] / SR)
        z1 = (1 - a) * v + a * z1
        z2 = (1 - a) * z1 + a * z2
        y[i] = z2
    return finish(y * adsr(sec, 0.03, 0.4, 0.6, sec - 0.35, 0.35), 0.8)


def acid_bass():
    # Saw bass with a resonant-sounding filter blip -- the squelch comes from
    # a sharp cutoff drop, plus a band-pass "peak" riding it.
    f, sec = hz(36), 0.9
    t = t_of(sec)
    x = saw(sec, f, 0, 8000)
    base = env_filter(x, 3500, 250, 0.08)
    peak = biquad_bp(x, 900, 6) * np.exp(-t / 0.08)
    return finish((base + 0.8 * peak) * adsr(sec, 0.002, 0.3, 0.5, sec - 0.15, 0.15), 0.85)


def rave_hoover():
    # The rave "hoover": wide detuned saws an octave apart, bending down into
    # the note, with a slow vibrato.
    sec = 2.0
    t = t_of(sec)
    bend = 2 ** ((-5 * np.exp(-t / 0.08)) / 12)
    out = np.zeros(len(t))
    for mult, dc in ((0.5, -20), (0.5, 20), (1, -12), (1, 12), (1, 0)):
        f = hz(48) * mult * 2 ** (dc / 1200) * bend * (1 + 0.01 * np.sin(2 * np.pi * 4 * t))
        ph = 2 * np.pi * np.cumsum(f) / SR
        out += sum(np.sin(k * ph) / k for k in range(1, 40))
    return finish(env_filter(out, 6000, 2500, 0.4) * adsr(sec, 0.01, 0.6, 0.8, sec - 0.4, 0.4), 0.8)


def chip_square():
    # 8-bit: a 25% pulse with an instant attack and a stepped decay.
    f, sec = hz(72), 0.8
    t = t_of(sec)
    x = np.where((f * t) % 1 < 0.25, 1.0, -0.33)
    steps = np.floor(np.exp(-t / 0.25) * 8) / 8
    return finish(x * steps, 0.55)


def air_texture():
    # A breathy bed: filtered noise with a soft sine chord inside it,
    # slowly swelling and fading.
    sec = 6.0
    t = t_of(sec)
    noise = one_pole_lp(rng.uniform(-1, 1, len(t)), 2500)
    tones = sum(np.sin(2 * np.pi * hz(m) * t) for m in (60, 67, 74)) / 3
    shape = np.sin(np.pi * t / sec) ** 1.5
    return finish((0.6 * noise + 0.5 * tones) * shape, 0.6)


def breath_flute():
    # Mostly a sine with a little octave, breath noise on the attack and
    # under the tone, and vibrato that arrives late.
    f, sec = hz(72), 2.5
    t = t_of(sec)
    vib = 1 + 0.005 * np.sin(2 * np.pi * 5 * t) * np.clip((t - 0.4) / 0.5, 0, 1)
    ph = 2 * np.pi * np.cumsum(f * vib) / SR
    tone = np.sin(ph) + 0.15 * np.sin(2 * ph)
    breath = biquad_bp(rng.uniform(-1, 1, len(t)), f * 2, 2) * (0.5 * np.exp(-t / 0.08) + 0.12)
    return finish((tone + breath) * adsr(sec, 0.06, 0.3, 0.85, sec - 0.3, 0.3), 0.75)


SINGLES = [
    ('Kalimba', 60, kalimba),
    ('Marimba', 60, marimba),
    ('Vox Ahh', 57, lambda: vox([(800, 6, 1.0), (1150, 8, 0.6), (2900, 10, 0.25)])),
    ('Vox Ooh', 57, lambda: vox([(400, 6, 1.0), (800, 8, 0.4), (2600, 10, 0.12)])),
    ('Chord Stab', 60, chord_stab),
    ('Brass Hit', 48, brass_hit),  # named by its octave-down layer, the pitch you hear
    ('Acid Bass', 36, acid_bass),
    ('Rave Hoover', 36, rave_hoover),  # likewise
    ('Chip Square', 72, chip_square),
    ('Air Texture', 60, air_texture),
    ('Breath Flute', 72, breath_flute),
]

# --- writing -----------------------------------------------------------------


def write_wav(path, x):
    # 16-bit mono, the audio on a 512-byte boundary (see make_drum_kits.py).
    os.makedirs(os.path.dirname(path), exist_ok=True)
    pcm = (np.clip(x, -1, 1) * 32767).astype('<i2').tobytes()
    fmt = struct.pack('<4sIHHIIHH', b'fmt ', 16, 1, 1, SR, SR * 2, 2, 16)
    junk_len = 512 - 12 - len(fmt) - 8 - 8
    junk = struct.pack('<4sI', b'JUNK', junk_len) + bytes(junk_len)
    data = struct.pack('<4sI', b'data', len(pcm)) + pcm
    body = b'WAVE' + fmt + junk + data
    with open(path, 'wb') as f:
        f.write(struct.pack('<4sI', b'RIFF', len(body)) + body)


def main():
    preview = sys.argv[1] if len(sys.argv) > 1 else None
    for name, notes, sec, fn in SOUNDS:
        d = os.path.join(ROOT, name)
        rendered = {}
        for m in notes:
            x = fn(m, sec)
            rendered[m] = x
            write_wav(os.path.join(d, '%s %s.wav' % (name, note_name(m))), x)
        total = sum(len(x) for x in rendered.values()) / SR
        with open(os.path.join(d, 'README.txt'), 'w') as f:
            f.write("%s -- synthesised for the PZD Daisy Pod Groovebox by tools/make_sounds.py. "
                    "CC0 public domain: use it for anything.\n%d zones, %s to %s, %.0f s of audio.\n"
                    % (name, len(notes), note_name(notes[0]), note_name(notes[-1]), total))
        print('%-11s %2d zones %s-%s  %.1f s' % (name, len(notes), note_name(notes[0]),
                                                 note_name(notes[-1]), total))
        if preview:
            # Each zone in turn, rising -- a quick tour of the range.
            gap = int(0.9 * SR)
            parts = []
            for m in notes:
                x = rendered[m][:gap]
                parts.append(np.concatenate([x, np.zeros(gap - len(x))]))
            write_wav(os.path.join(preview, name + '.wav'), np.concatenate(parts))
    singles = os.path.join(SAMPLES, 'SOUNDS1')
    for name, root, fn in SINGLES:
        x = fn()
        write_wav(os.path.join(singles, '%s %s.wav' % (name, note_name(root))), x)
        print('%-12s single  %s  %.1f s' % (name, note_name(root), len(x) / SR))
        if preview:
            write_wav(os.path.join(preview, 'Sound ' + name + '.wav'), x)


if __name__ == '__main__':
    main()
