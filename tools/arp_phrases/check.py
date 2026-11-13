# Checks the factory phrases (phrases.py) before they go into the firmware:
# each in key and within two octaves in every scale, in Follow and in Lock
# from several keys, and well formed. Then regenerate the C++ table with
#   python3 tools/arp_phrases/gen_table.py
# and paste it over kPhrases in src/arpeggiator.cpp
# (tools/arp_tests/t_phrase.cpp then plays them through the real arp).
from phrases import *
ok=True
assert len(PHRASES)==24 and len({n for n,_ in PHRASES})==24
for name,ph in PHRASES:
    if len(name)>7 or not 1<=len(ph)<=16 or ph[0]==TIE: ok=False; print("BAD SHAPE",name)
    for st in ph:
        if st in (REST,TIE): continue
        d,v,l,oc,ch,rt=st
        if not(-7<=d<=14 and v in (0,1,2,3) and l in (0,1,2) and oc in (-1,0,1) and ch in (0,1) and rt in (1,2,3)): ok=False; print("BAD STEP",name,st)
    for scale in SCALES:
        for lock,root in ((False,60),(True,62),(True,65),(True,59),(True,61)):
            notes=[n for n in play(ph,root,60,scale,lock) if n not in (REST,TIE)]
            key=60 if lock else root
            if any((n-key)%12 not in SCALES[scale] for n in notes): ok=False; print("OUT OF KEY",name,scale,lock,root)
            if max(notes)-min(notes)>26: ok=False; print("RANGE",name,scale,lock,root)
print("24 phrases: all in key and in range" if ok else "PROBLEMS")
raise SystemExit(0 if ok else 1)
