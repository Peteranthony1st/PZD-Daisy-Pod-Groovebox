import numpy as np, struct, sys
from PIL import Image
def load(p):
    b=open(p,'rb').read()
    n=struct.unpack('<I',b[80:84])[0]
    a=np.frombuffer(b[84:84+n*50],dtype=np.dtype([('n','<f4',3),('v','<f4',(3,3)),('a','<u2')]))
    return a['v'].astype(np.float64)
def render(tris,view,scale=8,out='o.png'):
    # view: axes (u,v,depth) indices & signs; depth larger = nearer
    ax=view if isinstance(view,tuple) else {'top':((0,1),(1,-1),(2,1)),'bottom':((0,-1),(1,-1),(2,-1)),'front':((0,1),(2,-1),(1,-1)),'back':((0,-1),(2,-1),(1,1)),'left':((1,-1),(2,-1),(0,-1)),'right':((1,1),(2,-1),(0,1))}[view]
    U=tris[:,:,ax[0][0]]*ax[0][1]; V=tris[:,:,ax[1][0]]*ax[1][1]; Z=tris[:,:,ax[2][0]]*ax[2][1]
    u0,v0=U.min(),V.min(); U=(U-u0)*scale; V=(V-v0)*scale
    w=int(U.max())+2; h=int(V.max())+2
    zb=np.full((h,w),-1e9); sh=np.zeros((h,w))
    # normals in view space
    P=np.stack([U/scale,V/scale,Z],-1)
    nrm=np.cross(P[:,1]-P[:,0],P[:,2]-P[:,0]); nrm/=np.linalg.norm(nrm,axis=1,keepdims=True)+1e-12
    L=np.array([-0.4,-0.5,0.77]); L/=np.linalg.norm(L)
    for t in range(len(tris)):
        x=U[t];y=V[t];z=Z[t]
        xmin=max(int(x.min()),0);xmax=min(int(x.max())+1,w-1);ymin=max(int(y.min()),0);ymax=min(int(y.max())+1,h-1)
        if xmax<xmin or ymax<ymin: continue
        xs,ys=np.meshgrid(np.arange(xmin,xmax+1)+.5,np.arange(ymin,ymax+1)+.5)
        d=(y[1]-y[2])*(x[0]-x[2])+(x[2]-x[1])*(y[0]-y[2])
        if abs(d)<1e-12: continue
        l0=((y[1]-y[2])*(xs-x[2])+(x[2]-x[1])*(ys-y[2]))/d
        l1=((y[2]-y[0])*(xs-x[2])+(x[0]-x[2])*(ys-y[2]))/d
        l2=1-l0-l1
        m=(l0>=-1e-6)&(l1>=-1e-6)&(l2>=-1e-6)
        if not m.any(): continue
        zz=l0*z[0]+l1*z[1]+l2*z[2]
        sub=zb[ymin:ymax+1,xmin:xmax+1]; ssub=sh[ymin:ymax+1,xmin:xmax+1]
        k=m&(zz>sub)
        n=nrm[t]; s=abs(n@L)
        sub[k]=zz[k]; ssub[k]=s
    img=np.where(zb>-1e8,40+200*sh,15).astype(np.uint8)
    Image.fromarray(img).save(out)
    return zb,(u0,v0,scale)
if __name__=='__main__':
    t=load(sys.argv[1]); print(len(t),'tris','min',t.reshape(-1,3).min(0),'max',t.reshape(-1,3).max(0))
    for v in sys.argv[3:]:
        render(t,v,float(sys.argv[2]),out=sys.argv[1].split('/')[-1][:-4]+'-'+v+'.png')
