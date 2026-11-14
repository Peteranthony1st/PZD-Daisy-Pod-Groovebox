import os
from common import CASE_DIR, SCREENS_PY, LOGO, HERE
from PIL import Image, ImageDraw, ImageFont
AV='/System/Library/Fonts/Avenir Next.ttc'
def F(sz,face='Regular'):
    return ImageFont.truetype(AV,sz,index={'Bold':0,'Demi':2,'Medium':5,'Regular':7,'Heavy':8}[face])
PW,PH=1240,1754; M=90
ACC=(226,64,64); INK=(28,28,30); MUTE=(110,110,112)
_LOGO=Image.open(LOGO).convert('RGBA')
_LOGO=_LOGO.crop(_LOGO.getbbox())
def logo(p,x,y,h,colour):
    """Paste the PZD logo, h px tall, in one colour; returns its width."""
    w=int(_LOGO.width*h/_LOGO.height); a=_LOGO.resize((w,h),Image.LANCZOS).getchannel('A')
    p.paste(colour,(x,y),a); return w
def wrap(d,text,font,w):
    out=[];line=''
    for word in text.split():
        t=(line+' '+word).strip()
        if d.textlength(t,font=font)<=w: line=t
        else: out.append(line); line=word
    out.append(line); return out
def para(d,x,y,text,font,w,fill=INK,lh=1.45):
    for l in wrap(d,text,font,w):
        d.text((x,y),l,font=font,fill=fill); y+=int(font.size*lh)
    return y
def diagram(path,w):
    im=Image.open(path); # crop the empty margins
    bg=im.getpixel((5,5)); import numpy as np
    a=np.array(im); m=(np.abs(a.astype(int)-bg).sum(2)>12); ys,xs=m.nonzero()
    im=im.crop((xs.min()-20,ys.min()-20,xs.max()+20,ys.max()+20))
    return im.resize((w,int(im.height*w/im.width)),Image.LANCZOS)

# ---- Quick start, page 2 (colour)
p=Image.new('RGB',(PW,PH),(250,249,246)); d=ImageDraw.Draw(p)
d.rectangle([0,0,PW,170],fill=(30,30,34))
lw=logo(p,M,46,30,(240,240,240)); d.text((M+lw+14,44),'GROOVEBOX',font=F(30,'Heavy'),fill=(240,240,240))
d.text((M,88),'Quick Start',font=F(44,'Medium'),fill=(240,240,240))
d.ellipse([M,215,M+64,279],fill=ACC); d.text((M+21,222),'1',font=F(40,'Bold'),fill=(255,255,255))
d.text((M+88,214),'Meet the Groovebox',font=F(46,'Demi'),fill=INK)
d.text((M+88,276),'Everything you touch, plug in and look at.',font=F(26),fill=MUTE)
dg=diagram('mB_full.png',PW-2*M+40); p.paste(dg,(M-20,340)); y=340+dg.height+30
d.line([M,y,PW-M,y],fill=(220,218,212),width=2); y+=36
cw=(PW-2*M-2*50)//3
cols=[('The encoder',[('Turn','move between pages'),('Click','go in, or back out'),('Hold','Home, or Global settings'),('Triple-click','all notes off')]),
      ('Knobs & buttons',[('K1 · K2','set the two values on the screen\'s upper row'),('B1 · B2','do what the lower row says'),('Volume wheel','headphone level')]),
      ('The lights',[('LED 1','green playing, blue paused, flashing blue counting in'),('LED 2','red while recording'),('Daisy','power is on')])]
for i,(h,rows) in enumerate(cols):
    x=M+i*(cw+50); yy=y
    d.text((x,yy),h.upper(),font=F(22,'Heavy'),fill=ACC); yy+=44
    for k,v in rows:
        d.text((x,yy),k,font=F(26,'Demi'),fill=INK); yy+=34
        yy=para(d,x,yy,v,F(24),cw,MUTE,1.35)+14
lw=logo(p,M,PH-58,22,INK); d.text((M+lw+12,PH-60),'Groovebox · Quick Start',font=F(20),fill=MUTE)
d.text((PW-M-20,PH-60),'2',font=F(20,'Demi'),fill=MUTE)
p.save('page-quickstart.png')

# ---- Manual page (line art)
p=Image.new('RGB',(PW,PH),(255,255,255)); d=ImageDraw.Draw(p)
d.text((M,70),'2  THE HARDWARE',font=F(22,'Heavy'),fill=ACC)
d.line([M,108,PW-M,108],fill=INK,width=3); logo(p,PW-M-int(_LOGO.width*30/_LOGO.height),66,30,INK)
d.text((M,140),'2.1  Controls and connections',font=F(44,'Demi'),fill=INK)
y=para(d,M,214,'The Groovebox is a Daisy Pod in the PZD case, with a 128 × 64 screen added. Every page is worked the same way: the encoder moves around, the two knobs set the two values on the upper row of the screen, and the two buttons do what the lower row says.',F(25),PW-2*M,INK)
dg=diagram('mD_full.png',PW-2*M+40); p.paste(dg,(M-20,y+20)); y+=20+dg.height+30
rows=[('Control','Action','What it does'),
      ('Encoder','Turn','Moves between tracks, or a screen\'s pages'),
      ('','Click','Goes one level in, or back out'),
      ('','Hold','Home; from Home, the Global settings'),
      ('','Triple-click','All notes off, on every engine'),
      ('K1, K2','Turn','The left and right values on the upper row'),
      ('B1, B2','Tap / hold','What the lower row shows: B1 left, B2 right'),
      ('LED 1','','Green playing · blue paused · blue flash on count-in'),
      ('LED 2','','Red while recording'),
      ('Volume wheel','Roll','Headphone level')]
c=[M,M+250,M+450]
for i,r in enumerate(rows):
    f=F(23,'Demi') if i==0 else F(23)
    if i==0: d.rectangle([M,y-8,PW-M,y+36],fill=(240,240,238))
    for x,t in zip(c,r): d.text((x+10,y),t,font=F(23,'Demi') if (i==0 or x==c[0]) else f,fill=INK)
    y+=48; d.line([M,y-8,PW-M,y-8],fill=(215,215,215),width=1)
lw=logo(p,M,PH-58,22,INK); d.text((M+lw+12,PH-60),'Groovebox · User Manual',font=F(20),fill=MUTE)
d.text((PW-M-30,PH-60),'12',font=F(20,'Demi'),fill=MUTE)
p.save('page-manual.png')
