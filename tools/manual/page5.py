import os
from common import CASE_DIR, SCREENS_PY, LOGO, HERE
import sys
exec(open(os.path.join(HERE,'page2.py')).read().split("# ---- page 3")[0])
from PIL import ImageFilter
from draw import font
import math
sys.argv=['x']; exec(open(os.path.join(HERE,'mock2.py')).read().split("GAP=120")[0])   # strip()

def ssaw_preset():   # Ui::DrawSupersawScreen(), Preset page, browsing
    d=D(); d.title("SSaw:Preset",1); d.tt(0,16,"Factory Presets"); d.f6(0,24,"Super Pad")
    d.ctrl(FY1,False,FD1,"","","","Scroll"); d.ctrl(FY2,True,FD2,"Back","","","Load"); return d

def chain_figure():
    S=2; Wc,Hc=1060*S,620*S; c=Image.new('RGB',(Wc,Hc),(250,249,246)); d=ImageDraw.Draw(c)
    ink=(30,30,30); F1=font(26*S); F2=font(21*S,False)
    # keyboard
    kx,ky,kw,kh=10*S,100*S,300*S,130*S
    d.rounded_rectangle([kx,ky,kx+kw,ky+kh],14*S,fill=(52,52,58))
    d.rounded_rectangle([kx+14*S,ky+14*S,kx+kw-14*S,ky+40*S],6*S,fill=(80,80,88))
    for i in range(3): d.ellipse([kx+24*S+i*34*S,ky+20*S,kx+38*S+i*34*S,ky+34*S],fill=(150,150,160))
    wx0,wy0,wy1=kx+14*S,ky+52*S,ky+kh-12*S; nw=14; ww=(kw-28*S)/nw
    for i in range(nw): d.rectangle([wx0+i*ww,wy0,wx0+(i+1)*ww-2*S,wy1],fill=(246,246,242))
    for i in range(nw-1):
        if i%7 in (2,6): continue
        bx=wx0+(i+1)*ww-ww*0.3; d.rectangle([bx,wy0,bx+ww*0.6-2*S,wy0+(wy1-wy0)*0.6],fill=(30,30,34))
    # its own power
    px,py=kx+40*S,ky+kh
    d.line([px,py,px,py+50*S,px-10*S,py+80*S],fill=ink,width=4*S)
    d.rounded_rectangle([px-30*S,py+78*S,px+10*S,py+108*S],5*S,fill=(90,90,96))
    # one cable: a 5-pin DIN male plug in the keyboard's MIDI Out, a 3.5 mm
    # TRS male plug in the Groovebox's MIDI In
    ox,oy=kx+kw,ky+60*S; ay=oy
    dx=ox+8*S
    d.rounded_rectangle([dx,ay-30*S,dx+46*S,ay+30*S],6*S,fill=(70,70,76))        # DIN plug shell
    d.rectangle([dx+46*S,ay-20*S,dx+86*S,ay+20*S],fill=(40,40,44))                # its boot
    tx=ox+420*S
    d.line([dx+86*S,ay,tx,ay],fill=ink,width=7*S)
    d.rounded_rectangle([tx,ay-16*S,tx+60*S,ay+16*S],6*S,fill=(40,40,44))
    d.rectangle([tx+60*S,ay-7*S,tx+110*S,ay+7*S],fill=(200,196,180))
    for xx in (tx+76*S,tx+92*S): d.line([xx,ay-7*S,xx,ay+7*S],fill=(60,60,60),width=3*S)
    d.ellipse([tx+104*S,ay-7*S,tx+118*S,ay+7*S],fill=(200,196,180))
    # the Groovebox's right edge, with the MIDI In jack
    rx=tx+140*S; ex=rx+130*S
    d.rounded_rectangle([rx,ay-95*S,ex+60*S,ay+95*S],16*S,fill=(236,233,226),outline=(200,196,188),width=2*S)
    d.ellipse([rx+30*S-22*S,ay-22*S,rx+30*S+22*S,ay+22*S],fill=(28,28,30))
    d.ellipse([rx+30*S-9*S,ay-9*S,rx+30*S+9*S,ay+9*S],fill=(70,70,74))
    d.text((rx+70*S,ay),'MIDI',font=font(24*S),fill=(170,166,158),anchor='lm')
    def lab(x,y,t,sub=None,anchor='mm'):
        d.text((x,y),t,font=F1,fill=ink,anchor=anchor)
        if sub: d.text((x,y+32*S),sub,font=F2,fill=(110,110,110),anchor=anchor)
    lab(kx+kw/2,ky-62*S,'MIDI keyboard','with its MIDI Out')
    lab(px+30*S,py+92*S,'Its own power',None,'lm')
    lab((dx+tx)/2+40*S,ay+50*S,'MIDI cable, Type A','5-pin DIN male to 3.5 mm TRS male')
    lab(dx+30*S,ay-60*S,'DIN')
    lab(tx+60*S,ay-50*S,'TRS')
    lab(rx+90*S,ay-150*S,'Groovebox','right side, MIDI In')
    # second row: a controller with a 3.5 mm TRS MIDI Out, TRS to TRS
    def trs(x,y,flip=False):
        if not flip:
            d.rounded_rectangle([x,y-16*S,x+60*S,y+16*S],6*S,fill=(40,40,44)); d.rectangle([x+60*S,y-7*S,x+110*S,y+7*S],fill=(200,196,180))
            for xx in (x+76*S,x+92*S): d.line([xx,y-7*S,xx,y+7*S],fill=(60,60,60),width=3*S)
            d.ellipse([x+104*S,y-7*S,x+118*S,y+7*S],fill=(200,196,180))
        else:
            d.rounded_rectangle([x-60*S,y-16*S,x,y+16*S],6*S,fill=(40,40,44)); d.rectangle([x-110*S,y-7*S,x-60*S,y+7*S],fill=(200,196,180))
            for xx in (x-76*S,x-92*S): d.line([xx,y-7*S,xx,y+7*S],fill=(60,60,60),width=3*S)
            d.ellipse([x-118*S,y-7*S,x-104*S,y+7*S],fill=(200,196,180))
    y2=ay+340*S; cx0,cw2,ch2=80*S,170*S,120*S
    d.rounded_rectangle([cx0,y2-ch2//2,cx0+cw2,y2+ch2//2],12*S,fill=(52,52,58))
    for r in range(2):
        for q in range(4):
            px0=cx0+16*S+q*37*S; py0=y2-44*S+r*42*S
            d.rounded_rectangle([px0,py0,px0+30*S,py0+34*S],5*S,fill=(120,120,130) if (r+q)%3 else (226,64,64))
    sx=cx0+cw2+4*S
    d.rectangle([cx0+cw2-6*S,y2-12*S,cx0+cw2+4*S,y2+12*S],fill=(28,28,30))   # its TRS socket
    trs(sx+118*S,y2,flip=True)
    tx2=tx
    d.line([sx+118*S,y2,tx2,y2],fill=ink,width=7*S)
    trs(tx2,y2)
    gx=rx
    d.rounded_rectangle([gx,y2-70*S,ex+60*S,y2+70*S],16*S,fill=(236,233,226),outline=(200,196,188),width=2*S)
    d.ellipse([gx+30*S-22*S,y2-22*S,gx+30*S+22*S,y2+22*S],fill=(28,28,30)); d.ellipse([gx+30*S-9*S,y2-9*S,gx+30*S+9*S,y2+9*S],fill=(70,70,74))
    d.text((gx+70*S,y2),'MIDI',font=font(24*S),fill=(170,166,158),anchor='lm')
    lab(cx0+cw2/2,y2-ch2//2-50*S,'Controller','with a 3.5 mm MIDI Out')
    lab((sx+tx2)/2+60*S,y2+50*S,'TRS to TRS MIDI cable','Type A, or Type B to Type A')
    return c.resize((Wc//S,Hc//S),Image.LANCZOS)

p=Image.new('RGB',(PW,PH),(250,249,246)); d=ImageDraw.Draw(p)
header(p,d,4,'Connect a MIDI keyboard','Any keyboard or controller with a MIDI Out: 5-pin or 3.5 mm.')
y=290
y=step(p,d,y,'A','Plug it in',
 'One cable from the keyboard\'s MIDI Out to MIDI In: 5-pin DIN male to 3.5 mm TRS male, or TRS to TRS if the keyboard has a 3.5 mm MIDI Out. The keyboard needs its own power: the Groovebox doesn\'t power it.')
cf=chain_figure(); p.paste(cf,((PW-cf.width)//2,y-30)); y+=cf.height-20
tip=('MIDI In is Type A, the MIDI standard. If a controller\'s TRS output is Type B (some Arturia and Novation '
     'gear), use a Type B to Type A cable. MIDI comes in through MIDI In only, not over USB.')
n=len(wrap(d,tip,F(22),PW-2*M-90)); bh=24+n*int(22*1.4)
d.rounded_rectangle([M+44,y-20,PW-M,y-20+bh],12,fill=(240,238,232))
para(d,M+64,y-8,tip,F(22),PW-2*M-90,MUTE,1.4)
y+=bh+50
y=step(p,d,y,'B','Set the channel',
 'Set the keyboard to send on MIDI channel 1: every engine listens there to start with, except the drum sampler on 10. To give engines their own channels, use Global:MIDI.')
footer(d,5); p.save('page-qs-5.png')

p=Image.new('RGB',(PW,PH),(250,249,246)); d=ImageDraw.Draw(p)
header(p,d,5,'Choose an engine and a sound','Eight engines, each with its own factory sounds.')
y=300
y=step(p,d,y,'A','Switch an engine on',
 'Every engine starts switched off. Hold the encoder to open Global, turn to an engine (Global:SSaw is the Supersaw) and tap B1. The page shows ENABLED. Now play.',tile(g['global_supersaw']()))
y=step(p,d,y,'B','Pick a sound',
 'Click the encoder to open the engine, and turn to its Preset page. Tap B2, turn K2 through the sounds, and tap B2 to load one. Keep playing while you browse.',tile(ssaw_preset()))
y=step(p,d,y,'C','Make it yours',
 'Turn back through the engine\'s pages to change its sound: K1 and K2 set the two values on each page. Save it on the User page: tap B1, then hold B2.')
# the engines, each with its Global page icon from the firmware
def icon_img(name,scale=3):
    bits=g['icon_bits'](name); im=Image.new('RGB',(48*scale,20*scale),(250,249,246)); dr=ImageDraw.Draw(im)
    for yy in range(20):
        for xx in range(48):
            if bits[yy*6+xx//8]&(0x80>>(xx%8)): dr.rectangle([xx*scale,yy*scale,xx*scale+scale-1,yy*scale+scale-1],fill=INK)
    return im
d.text((M,y-6),'THE ENGINES',font=F(22,'Heavy'),fill=ACC); y+=40
rows=[('kGlobalIconDexedA','DXD-A · DXD-B','Dexed','DX7-style FM: electric pianos, bells, basses, brass. Two of them, to layer or split the keyboard.'),
      ('kGlobalIconEnsemble','Ensmbl','Ensemble','A string machine: lush strings and pads, with its own chorus.'),
      ('kGlobalIconOrgan','Organ','Organ','A drawbar tonewheel organ, with percussion and key click.'),
      ('kGlobalIconSupersaw','SSaw','Supersaw','Seven detuned saws: trance leads, plucks, big chords.'),
      ('kGlobalIconGrains','Grains','Grains','Granular textures and pads from audio you capture or import.'),
      ('kGlobalIconSmpvo','SMPVO','Sample Sound','One captured or imported sound, played across the keyboard.'),
      ('kGlobalIconSmpdr','SMPDR','Sample Drum','Drum kits and breaks on pads. Listens on channel 10.')]
for icon,gname,name,what in rows:
    d.line([M,y-14,PW-M,y-14],fill=(222,220,214),width=2)
    ic=icon_img(icon,2); p.paste(ic,(M,y))
    d.text((M+120,y-4),name,font=F(25,'Demi'),fill=INK)
    d.text((M+120,y+30),'Global:'+gname,font=F(20),fill=MUTE)
    yy=para(d,M+420,y-2,what,F(23),PW-2*M-420,INK,1.38)
    y=max(yy,y+60)+22
footer(d,6); p.save('page-qs-6.png')
