# Prints the C++ phrase table (kPhrases in src/arpeggiator.cpp) from
# phrases.py -- run check.py first.
from phrases import *
def step(st):
    if st==REST: return "REST"
    if st==TIE: return "TIE"
    d,v,l,oc,ch,rt=st
    return "{%d, %d, %d, %d, %d, %d}"%(d,v,l,oc,ch,rt)
out=["// The factory phrases -- generated from the verified phrase list (see",
     "// DESIGN.md, Arpeggiator: Phrase mode). A step: {scale step, velocity",
     "// 0 ghost..3 accent, length 0 short/1 normal/2 long, octave, chance,",
     "// ratchet}, or REST/TIE.",
     "const Arpeggiator::Phrase kPhrases[Arpeggiator::kNumPhrases] = {"]
for name,ph in PHRASES:
    out.append('    {"%s", %d, {%s}},'%(name,len(ph),", ".join(step(s) for s in ph)))
out.append("};")
print("\n".join(out))
