import os
from common import CASE_DIR, SCREENS_PY, LOGO, HERE
from page import F, PW, PH, M, ACC, INK, MUTE, wrap, para, logo
from PIL import Image, ImageDraw
import numpy as np
src=open(SCREENS_PY).read().split('SCREENS=')[0]
g={'__file__':SCREENS_PY}; exec(src,g)
D=g['D']; W=g['W']; FY1,FY2,FD1,FD2=g['FY1'],g['FY2'],g['FD1'],g['FD2']

# screens (copied from ui.cpp the same way tools/readme_screens.py does)
def perf_browse():   # Ui::DrawFileScreen(), BrowsingLoad on PERF1
    d=D(); d.title("Global:Perf",1); d.tt(0,15,"Performances"); d.f6(0,24,"PERF1")
    d.ctrl(FY1,False,FD1,"","","","Scroll"); d.ctrl(FY2,True,FD2,"Back","","","Load"); return d
def home_rec(state):
    d=D(); g['home_top'](d,2)
    g['home_boxes'](d,[dict(name='A1',state=state,pos=.4 if state=='rec' else None),dict(name='A2',state='-',pos=None),
                       dict(name='A3',state='-',pos=None),dict(name='A4',state='-',pos=None)],0,False)
    w="Audio Tracks"; d.tt((W-d.ttw(w))//2,44,w)
    d.ctrl(FY1,False,FD1,"Master Vol","80%","50%","Metro"); d.ctrl(FY2,True,FD2,"Pause/Hold=Rec","","","Bypass:Off"); return d
def tile(d,scale=4):
    a=np.array(d.p,np.uint8); h,w=a.shape; B=10
    im=Image.new('RGB',(w*scale+2*B,h*scale+2*B),(30,31,36)); dr=ImageDraw.Draw(im)
    dr.rectangle([B-4,B-4,B+w*scale+3,B+h*scale+3],fill=(10,10,12))
    for y in range(h):
        for x in range(w):
            if a[y][x]: dr.rectangle([B+x*scale,B+y*scale,B+x*scale+scale-1.4,B+y*scale+scale-1.4],fill=(235,242,255))
    return im

def header(p,d,num,title,sub):
    d.rectangle([0,0,PW,110],fill=(30,30,34))
    lw=logo(p,M,38,24,(240,240,240)); d.text((M+lw+12,35),'GROOVEBOX',font=F(24,'Heavy'),fill=(240,240,240))
    t='Quick Start'; d.text((PW-M-d.textlength(t,font=F(26,'Medium')),32),t,font=F(26,'Medium'),fill=(170,170,175))
    d.ellipse([M,150,M+64,214],fill=ACC); tw=d.textlength(str(num),font=F(40,'Bold')); d.text((M+32-tw/2,157),str(num),font=F(40,'Bold'),fill=(255,255,255))
    d.text((M+88,149),title,font=F(46,'Demi'),fill=INK); d.text((M+88,211),sub,font=F(26),fill=MUTE)
def footer(d,n):
    p=d._image; lw=logo(p,M,PH-58,22,INK); d.text((M+lw+12,PH-60),'Groovebox · Quick Start',font=F(20),fill=MUTE); d.text((PW-M-20,PH-60),str(n),font=F(20,'Demi'),fill=MUTE)
def step(p,d,y,letter,title,text,img=None,tip=None):
    tx=M; tw=PW-2*M
    if img is not None:
        img=img.resize((int(img.width*0.88),int(img.height*0.88)),Image.LANCZOS)
        p.paste(img,(PW-M-img.width,y)); tw=PW-2*M-img.width-50
    d.text((tx,y-4),letter,font=F(30,'Heavy'),fill=ACC)
    d.text((tx+44,y-4),title,font=F(30,'Demi'),fill=INK)
    yy=para(d,tx+44,y+46,text,F(24),tw-44,INK,1.42)
    if tip:
        yy+=6; d.rounded_rectangle([tx+44,yy,tx+tw,yy+12+int(24*1.4)*len(wrap(d,tip,F(22),tw-80))+8],12,fill=(240,238,232))
        yy=para(d,tx+64,yy+12,tip,F(22),tw-80,MUTE,1.4)+14
    return max(yy,y+(img.height if img is not None else 0))+44

# ---- page 3: set up
p=Image.new('RGB',(PW,PH),(250,249,246)); d=ImageDraw.Draw(p)
header(p,d,2,'Get set up','A card, power, and something to listen on.')
side=Image.open('mB_full.png')
y=300
y=step(p,d,y,'A','Prepare the SD card',
 'On your computer, unzip sdcard-template.zip onto a FAT32 micro SD card, and copy main.bin onto it too. Both come with each release.',
 tip='The template holds the demo song, starter sounds and the folders the Groovebox saves into. A Pod that has never run this firmware needs a one-time bootloader install first: see the manual.')
y=step(p,d,y,'B','Put the card in, then plug in',
 'Slide the card into the left side, under the SD mark. Plug headphones into the headphone jack, or Audio Out into speakers. A MIDI keyboard goes into MIDI In. Power comes in over USB, by the lightning mark.',
 tip='Always switch off before taking the card out or putting it in.')
both=Image.open('sides-fig.png'); both=both.resize((int(both.width*0.36),int(both.height*0.36)),Image.LANCZOS)
p.paste(both,((PW-both.width)//2,y-20)); y+=both.height+10
y=step(p,d,y,'C','Switch on',
 'With main.bin on the card, the Groovebox installs it by itself the first time, then starts. The PZD GROOVEBOX logo shows for a moment, then the Home screen. The daisy lights to show the power is on.',
 tile(home_rec('-').__class__ and g['home_audio']()),
 tip='Home is where you always come back to: hold the encoder from anywhere.')
footer(d,3); p.save('page-qs-3.png')

# ---- page 4: play the demo, record a loop
p=Image.new('RGB',(PW,PH),(250,249,246)); d=ImageDraw.Draw(p)
header(p,d,3,'Hear the demo, record a loop','Ten minutes from switch-on to your own sound.')
y=300
y=step(p,d,y,'A','Load the demo',
 'Hold the encoder to open Global, then turn it to Global:Perf. Tap B2 (Load), turn K2 to PERF1 and tap B2 again.',tile(perf_browse()))
y=step(p,d,y,'B','Play the song',
 'Hold the encoder to go Home, then turn it past the tracks to the Song page. Tap B1 to play, and again to stop.',tile(g['song']()))
y=step(p,d,y,'C','Record your first loop',
 'On Home, turn to A1 and hold B1. The click counts in one bar, with LED 1 flashing blue. Play into Audio In: LED 2 is red while it records, and it stops by itself at the end of the loop.',tile(home_rec('rec')),
 tip='Tap B1 to pause or play the loop. Hold B1 again to record more on top.')
footer(d,4); p.save('page-qs-4.png')
