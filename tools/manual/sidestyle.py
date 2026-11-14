import numpy as np, pickle
from scipy import ndimage as nd
from PIL import Image
R=pickle.load(open('sides16.pkl','rb'))
def classify(name):
    zb,(u0,v0,sc)=R[name]; m=zb>-1e8
    top=np.percentile(zb[m],90)
    d=np.where(m,top-zb,99)
    wall=d<0.35; eng=(d>=0.35)&(d<1.2); hole=(d>=1.2)
    sil=nd.binary_fill_holes(m)
    # rim fillets near silhouette edge count as wall
    near=sil&~nd.binary_erosion(sil,iterations=40)
    eng&=~near; hole&=~near
    hole=nd.binary_opening(hole&sil,iterations=2); eng=nd.binary_opening(eng,iterations=1)
    return dict(zb=zb,sil=sil,hole=hole,eng=eng,v0=v0,u0=u0)
if __name__=='__main__':
    for n in ['left','right','bottom']:
        c=classify(n); a=np.full(c['zb'].shape+(3,),40,np.uint8)
        a[c['sil']]=(235,232,225); a[c['eng']]=(150,150,150); a[c['hole']]=(20,20,20)
        Image.fromarray(a).save('c-'+n+'.png'); print(n,a.shape)
