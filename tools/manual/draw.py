import os
from common import CASE_DIR, SCREENS_PY, LOGO, HERE
import numpy as np, pickle, math
from scipy import ndimage as nd
from PIL import Image, ImageDraw, ImageFilter, ImageFont
zb,u0,v0,sc=pickle.load(open('top16.pkl','rb'))
sil=np.load('sil.npy'); holes=np.load('holes.npy'); eng=np.load('eng.npy')
Hh,Ww=zb.shape
# the firmware screen
src=open(SCREENS_PY).read().split('SCREENS=')[0]
g={'__file__':SCREENS_PY}; exec(src,g)
def oled(fn, on=(235,242,255)):
    d=g[fn](); a=np.array(d.p,dtype=np.uint8)
    return a
FONT='/System/Library/Fonts/Supplemental/Arial Bold.ttf'
FONT_R='/System/Library/Fonts/Supplemental/Arial.ttf'
def font(sz,bold=True): return ImageFont.truetype(FONT if bold else FONT_R,sz)

# component geometry (px at 16 px/mm)
ENC=(1083,1191,69); POT=[(587,1406,69),(831,1406,69)]
LED=[(163,1355,33),(379,1355,33)]; BTN=[(162,1460,40),(377,1460,40)]
WIN=(258,118,1258,762); WHEEL=(1020,1708); FLOWER=(271,1122)

def hillshade(z,mask):
    zz=np.where(mask,z,z[mask].min() if mask.any() else 0)
    gy,gx=np.gradient(zz)
    s=np.clip(1-0.9*(gx*0.6+gy*0.8)/ (1+np.hypot(gx,gy)),0.4,1.3)
    return s

def body(style):
    """style: 'colour' | 'line'"""
    img=Image.new('RGB',(Ww,Hh),(255,255,255))
    m=zb>-1e8
    if style=='colour':
        base=np.array([238,235,228],float)
        sh=hillshade(np.where(m,zb,0)*16,m)  # mm -> px units
        sh=np.clip(0.55+0.45*sh,0.75,1.12)
        col=base[None,None,:]*sh[...,None]
        col=np.where(eng[...,None],col*0.86,col)
        a=np.where(m[...,None],np.clip(col,0,255),255).astype(np.uint8)
        img=Image.fromarray(a)
        a=np.array(img); a[holes]=(22,22,24); img=Image.fromarray(a)
    else:
        a=np.full((Hh,Ww,3),255,np.uint8)
        edge=(nd.binary_dilation(sil,iterations=3)&~nd.binary_erosion(sil,iterations=3))
        hedge=nd.binary_dilation(holes,iterations=3)&~holes
        eedge=nd.binary_dilation(eng,iterations=2)&~nd.binary_erosion(eng,iterations=1)
        a[holes]=(255,255,255); a[eedge]=(90,90,90); a[hedge|edge]=(20,20,20)
        img=Image.fromarray(a)
    return img

def draw_parts(img,style,screen='home_audio',leds=True):
    d=ImageDraw.Draw(img)
    line=style=='line'
    ink=(20,20,20)
    # the OLED module behind the window (a 2.42" panel: glass 60.5 x 37 mm,
    # viewing area 57 x 29.5, active 55 x 27.5 near the top, the wider
    # border below it over the driver; the notch is for its flex tab)
    MM=16; x0,y0,x1,y1=WIN
    gx0=(x0+x1)/2-30.25*MM; gy0=y0+1.0*MM; gx1=gx0+60.5*MM; gy1=gy0+37*MM
    ax0=(x0+x1)/2-27.5*MM; ay0=gy0+2.6*MM; ax1=ax0+55*MM; ay1=ay0+27.5*MM
    if not line:
        a=np.array(img); win=holes.copy(); win[y1+1:,:]=False; win[:, :x0-2]=False; win[:, x1+2:]=False
        a[win]=(8,8,9); img=Image.fromarray(a); d=ImageDraw.Draw(img)
        d.rectangle([gx0,gy0,gx1,gy1],fill=(30,31,36))
        d.line([gx0,gy0,gx1,gy0],fill=(58,60,68),width=3)            # glass edge catching the light
        d.rectangle([ax0-1*MM,ay0-1*MM,ax1+1*MM,ay1+1*MM],fill=(10,10,12))  # viewing area
    else:
        d.rectangle([gx0,gy0,gx1,gy1],outline=(90,90,90),width=3)
        d.rectangle([ax0-1*MM,ay0-1*MM,ax1+1*MM,ay1+1*MM],outline=(150,150,150),width=2)
    a=oled(screen); px=(ax1-ax0)/128; ox,oy=ax0,ay0
    on=(235,242,255) if not line else ink
    for y in range(64):
        for x in range(128):
            if a[y][x]: d.rectangle([ox+x*px,oy+y*px,ox+(x+1)*px-1.2,oy+(y+1)*px-1.2],fill=on)
    # grille slots (holes already drawn)
    # pots: dark knurled knobs with a white pointer
    for (cx,cy,r) in POT:
        R=r-4
        if line:
            d.ellipse([cx-R,cy-R,cx+R,cy+R],outline=ink,width=6); d.ellipse([cx-r+12,cy-r+12,cx+r-12,cy+r-12],outline=ink,width=3)
        else:
            d.ellipse([cx-r-4,cy-r-4,cx+r+4,cy+r+4],fill=(55,48,40))
            d.ellipse([cx-R,cy-R,cx+R,cy+R],fill=(28,28,30))
            d.ellipse([cx-r+12,cy-r+12,cx+r-12,cy+r-12],fill=(40,40,44))
        ang=math.radians(-60 if cx<700 else 150)
        d.line([cx+math.cos(ang)*(r-40),cy+math.sin(ang)*(r-40),cx+math.cos(ang)*(r-8),cy+math.sin(ang)*(r-8)],fill=(240,240,240) if not line else ink,width=9)
    # encoder: the knob's own STL from above (knob16.pkl, see knob.py)
    kz,ks,kx,ky=pickle.load(open('knob16.pkl','rb')); km=kz>-1e8
    if not line:
        sh=Image.new('L',img.size,0); sh.paste(Image.fromarray((km*120).astype(np.uint8)),(kx+10,ky+16))
        img.paste((120,115,105),(0,0),sh.filter(ImageFilter.GaussianBlur(12)))
        col=np.array([236,233,224])[None,None,:]*(0.62+0.42*ks[...,None])
        a=np.array(img); reg=a[ky:ky+kz.shape[0],kx:kx+kz.shape[1]]
        reg[km]=np.clip(col[km],0,255).astype(np.uint8)
        daisy=km&(kz>27.3)&(kz<27.7)    # the daisy, sunk 0.4 mm into the top
        dedge=nd.binary_dilation(daisy,iterations=2)&~daisy&km&(kz>27.7)
        reg[daisy]=(np.clip(col[daisy]*0.9,0,255)).astype(np.uint8); reg[dedge]=(178,172,160)
        img=Image.fromarray(a)
    else:
        # outlines between the flat top, the sunk daisy and the rim
        top=km&(kz>27.7); daisy=km&(kz>27.3)&(kz<27.7)
        e=lambda r,i: nd.binary_dilation(r,iterations=i)&~r
        edge=(e(top,3)&km)|(e(daisy,2)&top)|(km&~nd.binary_erosion(km,iterations=5))
        # the scallops: where the rim's shading jumps between facets
        rim=km&~nd.binary_dilation(top,iterations=4)&nd.binary_erosion(km,iterations=6)
        gy,gx=np.gradient(nd.gaussian_filter(ks,1.0))
        edge|=rim&(np.hypot(gx,gy)>0.09)
        a=np.array(img); reg=a[ky:ky+kz.shape[0],kx:kx+kz.shape[1]]
        reg[km]=(255,255,255); reg[edge]=(20,20,20); img=Image.fromarray(a)
    d=ImageDraw.Draw(img)
    # LEDs and buttons
    cols=[(70,220,90),(225,225,225)] if leds else [(200,200,200),(200,200,200)]
    for i,(lx,ly,lr) in enumerate(LED):
        if line: d.ellipse([lx-lr,ly-lr,lx+lr,ly+lr],outline=ink,width=5)
        else:
            if leds and i==0:
                gl=Image.new('L',img.size,0); ImageDraw.Draw(gl).ellipse([lx-lr*2,ly-lr*2,lx+lr*2,ly+lr*2],fill=160)
                img.paste(cols[0],(0,0),gl.filter(ImageFilter.GaussianBlur(18))); d=ImageDraw.Draw(img)
            d.ellipse([lx-lr,ly-lr,lx+lr,ly+lr],fill=cols[i],outline=(150,150,150),width=3)
    for (bx,by,br) in BTN:
        if line: d.rounded_rectangle([bx-br-4,by-br-4,bx+br+4,by+br+4],8,outline=ink,width=5)
        else:
            d.rounded_rectangle([bx-br-2,by-br+4,bx+br+2,by+br+8],10,fill=(185,182,175))
            d.rounded_rectangle([bx-br-2,by-br-2,bx+br+2,by+br+2],10,fill=(244,242,236),outline=(200,196,188),width=3)
    return img

def flower_glow(img,colour=(255,60,60)):
    m=eng.copy(); x,y=FLOWER
    box=np.zeros_like(m); box[y-140:y+140,x-140:x+140]=True; m&=box
    a=np.array(img).astype(float)
    a[m]=a[m]*0.3+np.array(colour)*0.7
    gl=Image.fromarray((nd.binary_dilation(m,iterations=10)*150).astype(np.uint8)).filter(ImageFilter.GaussianBlur(20))
    out=Image.fromarray(a.astype(np.uint8)); out.paste(colour,(0,0),gl)
    a2=np.array(out); a2[m]=(a[m]).astype(np.uint8); return Image.fromarray(a2)

def framed(img,bg=(246,246,243),pad=160,shadow=True):
    W,H=img.size; c=Image.new('RGB',(W+pad*2,H+pad*2),bg)
    if shadow:
        s=Image.fromarray((sil*130).astype(np.uint8)).filter(ImageFilter.GaussianBlur(28))
        c.paste((150,148,140),(pad+18,pad+30),s)
    mask=Image.fromarray((sil*255).astype(np.uint8))
    c.paste(img,(pad,pad),mask)
    return c

def wheel(d,ox,oy,line):
    # the Pod's headphone volume thumbwheel: a flat disc seen from above,
    # drawn before the case so only the part in the bottom notch shows
    cx,cy,r=ox+1020,oy+1602,105
    ink=(20,20,20)
    if line:
        d.ellipse([cx-r,cy-r,cx+r,cy+r],fill=(255,255,255),outline=ink,width=5)
        d.ellipse([cx-r+22,cy-r+22,cx+r-22,cy+r-22],outline=ink,width=3)
    else:
        d.ellipse([cx-r,cy-r,cx+r,cy+r],fill=(58,58,62))
        for k in range(48):
            t=k*math.pi/24
            d.line([cx+math.cos(t)*(r-3),cy+math.sin(t)*(r-3),cx+math.cos(t)*(r-16),cy+math.sin(t)*(r-16)],fill=(100,100,106),width=5)
        d.ellipse([cx-r+22,cy-r+22,cx+r-22,cy+r-22],fill=(70,70,75))
