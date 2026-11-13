# Writes the demo song into a Performance saved on the Pod.
#
#   python3 tools/make_demo_song.py <base PERFnnn.DAT> <out PERFnnn.DAT>
#
# The base is a Performance saved with the demo's sounds set up (engines
# on, kit/sound/patches loaded, 4 bars) and the MIDI tracks empty -- the
# synth settings block is the firmware's own layout, so it's taken as
# saved rather than written here. This script replaces everything after
# the layer audio except that block: the sequencer (SEQ2: every track's
# patterns), the scenes (SCN1), the song (SNG1) and the live chains (CHN1),
# in the layouts performance_store.cpp reads. It also sets 96 BPM.
#
# The music: A minor, 96 BPM, a 4-bar loop.
#   M1 SMPDR 808 Boom drums: A beat, B fill, C breakdown hats -- 1-bar
#      patterns (1/4 of the loop), chained A A A B, swung 1/16.
#   M2 DXD-A bass: A verse, B chorus.        M3 Ensemble chords: A, B.
#   M4 SMPVO FM E.Piano arpeggios: A, B, and C = A transposed up an octave.
#   M5 DXD-B lead: A chorus hook, B verse line.   M6 Grains swells: A.
# Scenes: Intro, Groove, Verse, Breakdown, Chorus, Outro; the song runs
# about 3 minutes.

import struct
import sys

PPQ = 96
BAR = 4 * PPQ
LOOP = 4 * BAR
GR, DXA, DXB, ENS, ORG, SMV, SMD = range(7)
# Quantise indices (sequencer.h): 0 Off .. 4 = 1/16. Lengths: 0 = 1/4 .. 2 = 1x.
Q16, LEN_QUARTER, LEN_1X = 4, 0, 2

# --- notes -------------------------------------------------------------------


class Pattern:
    def __init__(self, length=LEN_1X, quant=0, swing=0, transpose=0):
        self.length, self.quant, self.swing, self.transpose = length, quant, swing, transpose
        self.notes = []  # (tick, note, velocity, length)

    def add(self, tick, note, vel, length):
        self.notes.append((tick, note, vel, length))

    def events(self):
        # Note on and note off as separate events, in time order, offs
        # first at the same tick (so a repeated note re-strikes cleanly).
        ev = []
        span = [BAR, 2 * BAR, LOOP][self.length]
        for tick, note, vel, ln in self.notes:
            ev.append((tick, 1, note, vel))
            ev.append(((tick + ln) % span, 0, note, 0))
        ev.sort(key=lambda e: (e[0], e[1]))
        return [(t, n, v) for t, _, n, v in ev]


def s16(step):
    return step * PPQ // 4  # a 16th-note step in ticks


# Drums (808 Boom pads from C2).
KICK, RIM, SNARE, CLAP, SNARE2, TOM_L, HAT_C, TOM_M, HAT_P, TOM_H, HAT_O, COWBELL, CONGA, CRASH, CONGA_LO, RIDE = range(36, 52)


def drums_beat():
    p = Pattern(LEN_QUARTER, Q16, 18)
    for s in (0, 7, 10):
        p.add(s16(s), KICK, 112 if s == 0 else 96, s16(1))
    for s in (4, 12):
        p.add(s16(s), SNARE, 104, s16(1))
        p.add(s16(s), CLAP, 70, s16(1))
    for s in range(0, 16, 2):
        p.add(s16(s), HAT_C, 78 if s % 4 == 0 else 58, s16(1))
    p.add(s16(14), HAT_O, 60, s16(2))
    p.add(s16(11), RIM, 50, s16(1))
    return p


def drums_fill():
    p = Pattern(LEN_QUARTER, Q16, 18)
    p.add(0, KICK, 112, s16(1))
    p.add(s16(4), SNARE, 104, s16(1))
    for s in (0, 2, 4, 6):
        p.add(s16(s), HAT_C, 64, s16(1))
    for s, tom in ((8, TOM_H), (9, TOM_H), (10, TOM_M), (11, TOM_M), (12, TOM_L), (13, TOM_L)):
        p.add(s16(s), tom, 92, s16(1))
    for s in (14, 15):
        p.add(s16(s), SNARE, 80 + 10 * (s - 14), s16(1))
    return p


def drums_breakdown():
    p = Pattern(LEN_QUARTER, Q16, 18)
    for s in range(0, 16, 2):
        p.add(s16(s), HAT_C, 60, s16(1))
    p.add(s16(6), RIM, 64, s16(1))
    p.add(s16(14), RIM, 50, s16(1))
    p.add(s16(10), CONGA, 70, s16(1))
    p.add(s16(13), CONGA_LO, 66, s16(1))
    return p


# Harmony: verse Am F C G, chorus F G Em Am (one chord a bar).
VERSE = [('Am', 33), ('F', 29), ('C', 36), ('G', 31)]
CHORUS = [('F', 29), ('G', 31), ('Em', 28), ('Am', 33)]
PAD = {'Am': (57, 60, 64), 'F': (57, 60, 65), 'C': (55, 60, 64), 'G': (55, 59, 62), 'Em': (55, 59, 64)}
ARP = {'Am': (57, 60, 64, 69), 'F': (53, 57, 60, 65), 'C': (55, 60, 64, 67), 'G': (55, 59, 62, 67),
       'Em': (52, 55, 59, 64)}


def bass(prog, driving):
    p = Pattern()
    for bar, (_, root) in enumerate(prog):
        steps = (0, 2, 4, 6, 8, 10, 12, 14) if driving else (0, 3, 6, 8, 11, 14)
        for s in steps:
            note = root + 12 if s == 14 else root
            p.add(bar * BAR + s16(s), note, 100 if s == 0 else 84, s16(2) - 6)
    return p


def pad(prog):
    p = Pattern()
    for bar, (chord, _) in enumerate(prog):
        for n in PAD[chord]:
            p.add(bar * BAR, n, 76, BAR - 8)
    return p


def keys(prog, transpose=0):
    p = Pattern(transpose=transpose)
    order = (0, 1, 2, 3, 2, 1, 2, 3)
    for bar, (chord, _) in enumerate(prog):
        for k, i in enumerate(order):
            p.add(bar * BAR + k * PPQ // 2, ARP[chord][i], 88 if k % 4 == 0 else 70, PPQ // 2 - 4)
    return p


A4, B4, C5, D5, E5, F5, G5 = 69, 71, 72, 74, 76, 77, 79


def lead_hook():
    # Over F G Em Am.
    p = Pattern()
    for tick, note, ln in ((0, C5, 3), (s16(4), A4, 2), (s16(8), C5, 2), (s16(10), D5, 6),
                           (BAR, D5, 3), (BAR + s16(4), B4, 2), (BAR + s16(8), D5, 2), (BAR + s16(10), G5, 6),
                           (2 * BAR, E5, 4), (2 * BAR + s16(6), D5, 2), (2 * BAR + s16(8), B4, 8),
                           (3 * BAR, A4, 14)):
        p.add(tick, note, 96, s16(ln) - 6)
    return p


def lead_verse():
    # Over Am F C G: long notes, room for the keys.
    p = Pattern()
    for tick, note, ln in ((s16(2), E5, 12), (BAR + s16(2), C5, 12), (2 * BAR + s16(2), E5, 6),
                           (2 * BAR + s16(10), G5, 6), (3 * BAR + s16(2), D5, 12)):
        p.add(tick, note, 84, s16(ln) - 6)
    return p


def swells():
    p = Pattern()
    p.add(0, 57, 80, 2 * BAR - 16)
    p.add(2 * BAR, 64, 80, 2 * BAR - 16)
    return p


TRACKS = [
    (SMD, [drums_beat(), drums_fill(), drums_breakdown()]),
    (DXA, [bass(VERSE, False), bass(CHORUS, True)]),
    (ENS, [pad(VERSE), pad(CHORUS)]),
    (SMV, [keys(VERSE), keys(CHORUS), keys(VERSE, transpose=12)]),
    (DXB, [lead_hook(), lead_verse()]),
    (GR, [swells()]),
    (DXA, []),
    (DXA, []),
]
DRUM_CHAIN = [0, 0, 0, 1]  # A A A B

# Scenes: per track the pattern (None = off; 'chain' = the drum chain).
A, B, C = 0, 1, 2
SCENES = [
    ('Intro', [None, None, A, C, None, A, None, None]),
    ('Groove', ['chain', A, A, None, None, None, None, None]),
    ('Verse', ['chain', A, A, A, B, None, None, None]),
    ('Breakdown', [C, None, None, C, B, A, None, None]),
    ('Chorus', ['chain', B, B, B, A, None, None, None]),
    ('Outro', [None, None, A, C, None, A, None, None]),
]
SONG = [(0, 2), (1, 2), (2, 4), (4, 4), (3, 2), (4, 4), (5, 2)]  # (scene index, loops)
START = 2  # loading it plays the Verse scene's tracks

# --- writing -----------------------------------------------------------------


def track_save(engine, length, quant, swing, paused, r0, r1, count):
    return struct.pack('<5B3Bi', engine, length, quant, swing, paused, r0 & 0xFF, r1, 0, count)


def seq_block():
    out = struct.pack('<4sIII', b'SEQ2', 2, 8, PPQ)
    start = SCENES[START][1]
    for t, (engine, pats) in enumerate(TRACKS):
        on = t < len(start) and start[t] is not None
        cur = 0 if start[t] in (None, 'chain') else start[t]
        if not pats:
            on, cur = False, 0
        head = pats[cur] if pats else Pattern()
        out += track_save(engine, head.length, head.quant, head.swing, 0 if on else 1, cur, 8, 0)
        for p in range(8):
            pat = pats[p] if p < len(pats) else Pattern()
            ev = pat.events()
            out += track_save(engine, pat.length, pat.quant, pat.swing, 0, pat.transpose, 0, len(ev))
            for tick, note, vel in ev:
                out += struct.pack('<IBBBB', tick, note, vel, 0, 0)
    return out


def scene_block():
    out = struct.pack('<4sIII', b'SCN1', 1, 16, 104)
    for i in range(16):
        if i >= len(SCENES):
            out += struct.pack('<B8b4B3x4f8B64b', 0, *([-1] * 8), *([0] * 4), *([0.0] * 4),
                               *([0] * 8), *([0] * 64))
            continue
        pats = SCENES[i][1]
        pattern = [-1 if p is None else (DRUM_CHAIN[0] if p == 'chain' else p) for p in pats]
        chain_len = [len(DRUM_CHAIN) if p == 'chain' else 0 for p in pats]
        chains = []
        for p in pats:
            chains += (DRUM_CHAIN + [0] * 8)[:8] if p == 'chain' else [0] * 8
        # The audio tracks stay off, at full volume, until someone records.
        out += struct.pack('<B8b4B3x4f8B64b', 1, *pattern, *([0] * 4), *([1.0] * 4), *chain_len, *chains)
    return out


def song_block():
    steps = b''.join(struct.pack('<BB', s, r) for s, r in SONG) + bytes(2 * (64 - len(SONG)))
    return struct.pack('<4sIII', b'SNG1', 1, 1, 132) + struct.pack('<B3x', len(SONG)) + steps


def chain_block():
    out = struct.pack('<4sIII', b'CHN1', 1, 8, 9)
    start = SCENES[START][1]
    for t in range(8):
        if start[t] == 'chain':
            out += struct.pack('<B8b', len(DRUM_CHAIN), *(DRUM_CHAIN + [0] * 8)[:8])
        else:
            out += struct.pack('<B8b', 0, *([0] * 8))
    return out


def main():
    base = open(sys.argv[1], 'rb').read()
    assert base[:4] == b'OURO', 'not a Performance'
    seq = base.find(b'SEQ2')
    snap = base.find(b'SNAP', seq)
    scn = base.find(b'SCN1', snap)
    assert 0 < seq < snap < scn, 'expected SEQ2, SNAP, SCN1 in that order'
    head = bytearray(base[:seq])
    struct.pack_into('<f', head, 12, 96.0)  # FileHeader.bpm
    out = bytes(head) + seq_block() + base[snap:scn] + scene_block() + song_block() + chain_block()
    open(sys.argv[2], 'wb').write(out)
    print('%s: %d bytes, %d scenes, %d song steps (%d loops)' % (sys.argv[2], len(out), len(SCENES),
                                                                  len(SONG), sum(r for _, r in SONG)))


if __name__ == '__main__':
    main()
