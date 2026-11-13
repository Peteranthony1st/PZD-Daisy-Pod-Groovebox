# Phrase mode, redesigned: phrases in scale steps with per-step dynamics.
# A step: (degree, vel, length, octave, chance, ratchet) or REST / TIE.
#   degree  scale steps from the root (0 root, 2 third, 4 fifth, 7 octave in a 7-note scale)
#   vel     0 ghost, 1 soft, 2 normal, 3 accent
#   length  0 short, 1 normal, 2 long (legato into the next)
#   octave  -1, 0, +1 on top of the degree
#   chance  0 always, 1 = plays half the time
#   ratchet 1..3 hits in the step
REST='rest'; TIE='tie'
def S(deg,vel=2,ln=1,oc=0,ch=0,rt=1): return (deg,vel,ln,oc,ch,rt)
A=lambda d,**k:S(d,vel=3,**k)            # accent
G=lambda d,**k:S(d,vel=0,ln=0,**k)       # ghost: soft and short
L=lambda d,**k:S(d,oc=-1,**k)            # a bass note, an octave down
LA=lambda d,**k:S(d,vel=3,oc=-1,**k)
LG=lambda d,**k:S(d,vel=0,ln=0,oc=-1,**k)
PHRASES=[
 # --- bass ---
 ("Acid",   [A(0),G(0),S(7,ln=0),S(0),S(2,ln=2),TIE,A(7),G(0),S(0,ln=0),S(6),A(7,rt=2),G(0),S(4,ln=2),TIE,S(7),S(2,ch=1)]),
 ("Funk",   [LA(0),REST,G(0),L(7,ln=0),REST,LG(0),LA(4),REST,L(0),G(0),REST,L(6,ln=0),LA(7),REST,LG(4),L(5,ch=1)]),
 ("Octaves",[LA(0),L(7,ln=0),L(0,vel=1),L(7,ln=0),LA(0),L(7,ln=0),L(0,vel=1),L(7,ln=0,rt=2)]),
 ("Walk",   [LA(0),L(2),L(4),L(5),LA(7),L(5),L(4),L(1)]),
 ("Disco",  [LA(0,ln=0),L(7,ln=0),LA(0,ln=0),L(7,ln=0),LA(3,ln=0),L(10,ln=0),LA(4,ln=0),L(11,ln=0,rt=2)]),
 ("Rolling",[LA(0),LG(0),L(0),LG(0),LA(4),LG(4),L(4),LG(4),LA(3),LG(3),L(3),LG(3),LA(4),LG(4),L(6),L(7,ch=1)]),
 # --- arps ---
 ("Rise",   [A(0),S(2),S(4),S(7),S(2,vel=1),S(4),S(7),S(9),A(4),S(7),S(9),S(11),S(7,vel=1),S(9),S(11),A(14,ln=2)]),
 ("Fall",   [A(14),S(11),S(9),S(7),S(11,vel=1),S(9),S(7),S(4),A(9),S(7),S(4),S(2),S(7,vel=1),S(4),S(2),A(0,ln=2)]),
 ("Alberti",[A(0),S(4,vel=1),S(2),S(4,vel=1),A(0),S(4,vel=1),S(2),S(5,vel=1)]),
 ("Broken", [A(0),S(4),S(2),S(7),A(4),S(9),S(7),S(4,vel=1)]),
 ("Cascade",[A(14),S(11),S(9),S(7),A(11),S(9),S(7),S(4),A(9),S(7),S(4),S(2),A(7),S(4),S(2),S(0,ln=2)]),
 ("Spread", [A(0),S(4),S(9),S(14,vel=1),A(11),S(7),S(2),S(4,ch=1)]),
 ("Pedal",  [A(0),S(2),S(0,vel=1),S(4),S(0,vel=1),S(5),S(0,vel=1),S(7)]),
 ("Arc",    [A(0),S(2),S(4),S(7),S(9),S(11),A(14),S(11),S(9),S(7),S(4),S(2)]),
 # --- riffs ---
 ("Pulse",  [A(0),S(0,vel=1,ln=0),S(4,ln=0),S(0,vel=1,ln=0),A(7),S(0,vel=1,ln=0),S(4,ln=0,rt=2),S(0,vel=1,ln=0,ch=1)]),
 ("Bounce", [A(0,ln=2),TIE,S(7,ln=0),G(4),S(0,ln=2),TIE,A(7,rt=3),S(4,ln=0,ch=1)]),
 ("Stab",   [A(0,ln=0),REST,REST,S(4,ln=0),REST,REST,A(7,ln=0),REST,S(0,ln=0,vel=1),REST,S(4,ln=0),REST,A(9,ln=0,rt=2),REST,S(7,ln=0,ch=1),REST]),
 ("Gate",   [A(0),S(0,vel=1,ln=0),REST,S(0,ln=0),S(0,vel=1,ln=0),REST,S(4,ln=0),S(0,vel=1,ln=0),A(0),S(0,vel=1,ln=0),REST,S(0,ln=0),S(7,ln=0),REST,S(4,ln=0),S(0,vel=1,ln=0,ch=1)]),
 ("Dub",    [A(0,ln=2),TIE,TIE,S(2,ln=0,vel=1),S(4,ln=2),TIE,S(2,rt=3,vel=1),REST]),
 ("Offbeat",[REST,A(0,ln=0),REST,S(2,ln=0),REST,A(4,ln=0),REST,S(2,ln=0,ch=1)]),
 # --- melodies ---
 ("Hook",   [A(4),S(2),S(0),S(2,ln=2),TIE,S(4),S(7),S(4,ln=2),TIE,S(2),S(0),S(1),A(2,ln=2),TIE,TIE,REST]),
 ("Call",   [A(0),S(2),S(4),REST,S(4),S(5),A(7,ln=2),TIE,S(7),S(5),S(4),REST,S(2),S(1),A(0,ln=2),TIE]),
 ("Sparkle",[A(7,ln=0),S(9,ln=0,ch=1),S(11,ln=0),S(14,ln=0,ch=1),A(9,ln=0),S(11,ln=0,ch=1),S(7,ln=0),S(4,ln=0,ch=1),A(7,ln=0),S(11,ln=0,ch=1),S(9,ln=0),S(14,ln=0,ch=1),A(11,ln=0),S(9,ln=0,ch=1),S(7,ln=0),S(4,ln=0)]),
 ("Fill",   [S(0,rt=2),S(2,rt=2),S(4,rt=2),S(7,rt=2),S(9,rt=3),S(11,rt=3),A(14,rt=3),REST]),
]
SCALES={'Major':[0,2,4,5,7,9,11],'Minor':[0,2,3,5,7,8,10],'Dorian':[0,2,3,5,7,9,10],'Pent Minor':[0,3,5,7,10],'Blues':[0,3,5,6,7,10]}
NAMES="C C# D D# E F F# G G# A A# B".split()
MAJOR_REF=[0,2,4,5,7,9,11]
def degree_semis(scale,deg):
    # A phrase step is a 7-note-scale step (0 root, 2 third, 4 fifth, 7 the
    # octave). In a 7-note scale it's that scale's note; in a 5- or 6-note
    # one, the scale's note nearest that step's place in a major scale, within
    # the octave (the lower when it's half way) -- so an octave stays an
    # octave and a fifth a fifth.
    sc=SCALES[scale]; o,r=divmod(deg,7)
    if len(sc)==7: return sc[r]+12*o
    ref=MAJOR_REF[r]
    best=min(sc,key=lambda x:(abs(x-ref),x))
    return best+12*o
def step_index(scale,r):
    # Which scale note a 7-note step r (0..6) is: itself in a 7-note scale,
    # else the one nearest its place in a major scale (the lower when tied).
    sc=SCALES[scale]
    if len(sc)==7: return r
    ref=MAJOR_REF[r]; return min(range(len(sc)),key=lambda i:(abs(sc[i]-ref),i))
def snap(n,key,sc):
    for d in range(12):  # the nearest scale note, the one above when tied (as the firmware's Snap())
        if (n+d-key)%12 in sc: return n+d
        if (n-d-key)%12 in sc: return n-d
def degree_note(root,key,scale,deg,lock):
    sc=SCALES[scale]; n=len(sc); o,r=divmod(deg,7); st=step_index(scale,r)
    if not lock: return root+sc[st]+12*o
    start=snap(root,key,sc); idx=sc.index((start-key)%12); i=idx+st
    return start+sc[i%n]-sc[idx]+12*(i//n+o)
def play(phrase,root,key,scale,lock):
    out=[]
    for st in phrase:
        if st in (REST,TIE): out.append(st); continue
        d,v,l,oc,ch,rt=st; out.append(degree_note(root,key,scale,d,lock)+12*oc)
    return out
def nm(n): return NAMES[n%12]+str(n//12-1)
