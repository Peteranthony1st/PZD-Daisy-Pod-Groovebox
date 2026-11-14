import os
from common import CASE_DIR, SCREENS_PY, LOGO, HERE
import numpy as np, pickle
from stl import load,render
D=CASE_DIR+'/'
t=load(D+'PZD-GROOVE-TOP.stl')
zb,(u0,v0,sc)=render(t,'top',16,out='tmp.png')
pickle.dump((zb,u0,v0,sc),open('top16.pkl','wb'))
m=zb>-1e8

# The case's outline, its holes (screen window, grille, controls) and its
# engravings (the daisy, the LED rings, MIDI and the jack icons).
from scipy import ndimage as nd
sil=nd.binary_fill_holes(m)
np.save('sil.npy',sil); np.save('holes.npy',sil&~m)
np.save('eng.npy',m&(zb<17.3)&(zb>15.9)); np.save('surf.npy',m&(zb>=17.3))
