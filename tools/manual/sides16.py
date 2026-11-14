import os
from common import CASE_DIR, SCREENS_PY, LOGO, HERE
import numpy as np, pickle
from stl import load,render
D=CASE_DIR+'/'
t=np.concatenate([load(D+'PZD-GROOVE-TOP.stl'),load(D+'PZD-GROOVE-BASE.stl')])
print('y range',t[:,:,1].min(),t[:,:,1].max(),'z',t[:,:,2].min(),t[:,:,2].max())
R={}
for name,ax in [('right',((2,-1),(1,-1),(0,1))),('left',((2,1),(1,-1),(0,-1))),('bottom',((0,1),(2,-1),(1,-1)))]:
    zb,o=render(t,ax,16,out='tmp.png'); R[name]=(zb,o)
pickle.dump(R,open('sides16.pkl','wb'))
