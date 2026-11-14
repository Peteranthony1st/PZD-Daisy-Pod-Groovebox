import os
from common import CASE_DIR, SCREENS_PY, LOGO, HERE
import numpy as np, pickle
from stl import load
# Top view of the encoder knob, in the case render's pixel grid (16 px/mm,
# x from -2.2 mm, y down from 53.2 mm): depth and Lambert shade per pixel.
t=load(CASE_DIR+'/PZD-GROOVE-KNOB.stl')
SC=16; U=(t[:,:,0]+2.2)*SC; V=(53.2-t[:,:,1])*SC; Z=t[:,:,2]
x0,y0=int(U.min())-2,int(V.min())-2; w=int(U.max())-x0+3; h=int(V.max())-y0+3
zb=np.full((h,w),-1e9); sh=np.zeros((h,w))
P=np.stack([U/SC,V/SC,Z],-1); n=np.cross(P[:,1]-P[:,0],P[:,2]-P[:,0]); n/=np.linalg.norm(n,axis=1,keepdims=True)+1e-12
n[n[:,2]<0]*=-1
L=np.array([-0.35,-0.45,0.82]); L/=np.linalg.norm(L)
for k in range(len(t)):
    x=U[k]-x0;y=V[k]-y0;z=Z[k]
    a,b=int(x.min()),int(x.max())+1; c,e=int(y.min()),int(y.max())+1
    xs,ys=np.meshgrid(np.arange(a,b+1)+.5,np.arange(c,e+1)+.5)
    dd=(y[1]-y[2])*(x[0]-x[2])+(x[2]-x[1])*(y[0]-y[2])
    if abs(dd)<1e-12: continue
    l0=((y[1]-y[2])*(xs-x[2])+(x[2]-x[1])*(ys-y[2]))/dd; l1=((y[2]-y[0])*(xs-x[2])+(x[0]-x[2])*(ys-y[2]))/dd; l2=1-l0-l1
    m=(l0>=-1e-6)&(l1>=-1e-6)&(l2>=-1e-6); zz=l0*z[0]+l1*z[1]+l2*z[2]
    sub=zb[c:e+1,a:b+1]; ss=sh[c:e+1,a:b+1]; kk=m&(zz>sub)
    sub[kk]=zz[kk]; ss[kk]=max(n[k]@L,0)
pickle.dump((zb,sh,x0,y0),open('knob16.pkl','wb'))
print(x0,y0,w,h,np.unique(np.round(zb[zb>-1e8],1))[-8:])
