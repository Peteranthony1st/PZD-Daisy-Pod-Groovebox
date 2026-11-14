# Draws README screen images of the Pod's 128x64 OLED, pixel for pixel,
# from the firmware's own fonts (src/font_tomthumb.cpp, libDaisy's
# Font_6x8) -- the same style as images/global-*.png: 3x pixels in
# (235,242,255) on black with a 6px border, 396x204.
#
#   python3 tools/readme_screens.py <out_dir>
#
# Each screen is a function that redraws one page the way ui.cpp does
# (same coordinates: DrawControlRow footers at rows 48/57 with dividers
# 46/55, titles in Font_6x8 with the beat dots at x=87, Tom Thumb text by
# baseline). To add a screen, copy the matching Draw*() code from ui.cpp
# into a new function and add it to SCREENS at the bottom. Check the real
# strings (e.g. MidiSequencer::EngineName() is GR/DXA/DXB/ENS/ORG/SMV/SMD)
# so the picture matches the device.
import re, sys
from PIL import Image
import os
SRC=os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','src')+'/'
t=re.sub(r'//[^\n]*','',open(SRC+'font_tomthumb.cpp').read())
bm=[int(x,16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', t.split('kTomThumbBitmaps[] = {')[1].split('};')[0])]
gl=[tuple(int(v) for v in m) for m in re.findall(r'\{(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+)\}', t.split('kTomThumbGlyphs[] = {')[1].split('};')[0])]
f=re.sub(r'//[^\n]*','',open(SRC+'libDaisy/src/util/oled_fonts.c').read())
f6=[int(x,16) for x in re.findall(r'0x([0-9A-Fa-f]{4})', f.split('Font6x8[] = {')[1].split('};')[0])]
W,H=128,64
FY1,FY2,FD1,FD2=48,57,46,55
class D:
    def __init__(s): s.p=[[0]*W for _ in range(H)]
    def px(s,x,y,on=True):
        if 0<=x<W and 0<=y<H: s.p[y][x]=1 if on else 0
    def rect(s,x0,y0,x1,y1,on=True,fill=False):
        for y in range(y0,y1+1):
            for x in range(x0,x1+1):
                if fill or y in(y0,y1) or x in(x0,x1): s.px(x,y,on)
    def line(s,x0,y0,x1,y1):
        if y0==y1: [s.px(x,y0) for x in range(min(x0,x1),max(x0,x1)+1)]
        else: [s.px(x0,y) for y in range(min(y0,y1),max(y0,y1)+1)]
    def ttw(s,txt): return sum(gl[ord(c)-32][3] for c in txt)
    def ttink(s,txt):
        if not txt: return 0
        g=gl[ord(txt[-1])-32]; return sum(gl[ord(c)-32][3] for c in txt[:-1])+g[4]+g[1]
    def tt(s,x,base,txt,on=True):
        cx=x
        for ch in txt:
            off,w,h,adv,xo,yo=gl[ord(ch)-32]; bp=off*8
            for r in range(h):
                for c in range(w):
                    if bm[bp>>3]&(0x80>>(bp&7)): s.px(cx+xo+c,base+yo+r,on)
                    bp+=1
            cx+=adv
        return cx-x
    def f6(s,x,y,txt,on=True,upper=True):
        for i,ch in enumerate(txt.upper() if upper else txt):
            o=ord(ch)-32
            for r in range(8):
                row=f6[o*8+r]
                for c in range(6):
                    if row&(0x8000>>c): s.px(x+i*6+c,y+r,on)
    def circle(s,cx,cy,r=2):
        for (dx,dy) in [(-2,-1),(-2,0),(-2,1),(2,-1),(2,0),(2,1),(-1,-2),(0,-2),(1,-2),(-1,2),(0,2),(1,2)]: s.px(cx+dx,cy+dy)
    def ctrl(s,row_y,sq,div,ll,lv,rv,rl):
        s.line(0,div,W-1,div); b=row_y+5
        s.tt(1,b,ll)
        if rl: s.tt(W-1-s.ttw(rl),b,rl)
        cx=W//2
        if lv: s.tt(cx-4-s.ttink(lv),b,lv)
        if rv: s.tt(cx+5,b,rv)
        cy=row_y+2
        if sq: s.rect(cx-2,cy-2,cx+2,cy+2)
        else: s.circle(cx,cy)
    def beat(s,x,y,size,beat,bars=4,bar=1):
        if bars>1: s.f6(x,y,"B%d"%bar)
        dx=x+21; gap=3 if size>=5 else 2
        for i in range(4):
            s.rect(dx,y,dx+size-1,y+size-1,True,i==beat); dx+=size+gap
    def title(s,txt,beat=1):
        s.f6(0,0,txt); s.beat(W-41,0,3,beat); s.line(0,9,W-1,9)
    def icon(s,x,y,k):
        if k=='-': s.line(x,y+3,x+3,y+3)
        elif k=='play':
            for c in range(4): s.line(x+c,y+c,x+c,y+6-c)
        elif k=='pause': s.rect(x,y,x+1,y+6,True,True); s.rect(x+3,y,x+4,y+6,True,True)
        else:
            for dy in range(-3,4):
                for dx in range(-3,4):
                    d=dx*dx+dy*dy
                    if (k=='rec' and d<=10) or (k=='cin' and 5<=d<=10): s.px(x+3+dx,y+3+dy)
def home_top(d,beat):
    d.beat(2,0,6,beat)
    line="BPM:120 BARS:4 [M]"; d.tt(W-1-d.ttw(line),5,line)
def home_boxes(d,items,sel,midi):
    gap=4; bw=(W-3*gap)//4; top=14; bh=24
    for i,it in enumerate(items):
        x0=i*(bw+gap); x1=x0+bw-1; y1=top+bh-1
        d.rect(x0,top,x1,y1)
        if i==sel: d.rect(x0+1,top+1,x1-1,y1-1)
        d.f6(x0+3,top+4,it['name']); d.icon(x0+16,top+4,it['state'])
        if midi:
            d.tt(x0+3,top+19,it['eng']); d.tt(x1-5,top+19,it['pat'])
        if it.get('pos') is not None:
            px=x0+2+int(it['pos']*(bw-5)); d.line(px,y1-2,px,y1-1)
def home_audio():
    d=D(); home_top(d,1)
    home_boxes(d,[dict(name='A1',state='play',pos=.35),dict(name='A2',state='play',pos=.35),dict(name='A3',state='pause',pos=.35),dict(name='A4',state='-',pos=None)],0,False)
    w="Audio Tracks"; d.tt((W-d.ttw(w))//2,44,w)
    d.ctrl(FY1,False,FD1,"Master Vol","80%","50%","Metro"); d.ctrl(FY2,True,FD2,"Pause/Hold=Rec","","","Bypass:Off")
    return d
def home_midi():
    d=D(); home_top(d,2)
    home_boxes(d,[dict(name='M1',state='play',eng='SMD',pat='A',pos=.6),dict(name='M2',state='play',eng='DXA',pat='B',pos=.3),dict(name='M3',state='rec',eng='ENS',pat='A',pos=.3),dict(name='M4',state='-',eng='ORG',pat='A',pos=None)],1,True)
    w="MIDI Tracks"; d.tt((W-d.ttw(w))//2,44,w)
    d.ctrl(FY1,False,FD1,"Master Vol","80%","50%","Metro"); d.ctrl(FY2,True,FD2,"Pause/Hold=Rec","","","Bypass:Off")
    return d
def scenes():
    d=D(); d.title("Scenes",0)
    used={0,1,2,3,4,8}; playing=2; cursor=4
    for i in range(16):
        x0=1+(i%8)*16; x1=x0+13; y0=12+(i//8)*16; y1=y0+11
        fill=i==playing
        if fill: d.rect(x0,y0,x1,y1,True,True)
        elif i in used: d.rect(x0,y0,x1,y1)
        else:
            for x in range(x0,x1+1,2): d.px(x,y0); d.px(x,y1)
        n=str(i+1); d.tt(x0+(14-d.ttw(n))//2+1,y0+8,n,not fill)
        if i==cursor: d.rect(x0,y1+2,x1,y1+3,True,True)
    d.ctrl(FY1,False,FD1,"Scene","5","80%","Master Vol"); d.ctrl(FY2,True,FD2,"Play/Hold=Save","","","Hold=Clear")
    return d
def scene_edit():
    d=D(); d.title("Scene 3:Edit",2)
    for c in (1,2): d.line(c*43-1,11,c*43-1,FD1-1)
    cells=[('L1','80%'),('L2','65%'),('L3','off'),('L4','off'),('M1','A+'),('M2','B'),('M3','A'),('M4','off'),('M5','off'),('M6','off'),('M7','off'),('M8','off')]
    sel=5
    for c,(n,v) in enumerate(cells):
        x0=(c//4)*43; y0=12+(c%4)*8; b=y0+6; s=c==sel
        if s: d.rect(x0,y0,x0+41,y0+7,True,True)
        d.tt(x0+2,b,n,not s); d.tt(x0+16,b,v,not s)
    d.ctrl(FY1,False,FD1,"Pick","M2","B","Pattern"); d.ctrl(FY2,True,FD2,"Play/Hold=Save","","","On/Off")
    return d
def song():
    d=D(); d.title("Song 5 1/2",1)
    steps=[(1,4),(2,4),(3,2),(2,4),(4,2),(3,2),(5,1),(6,4),(1,4),(7,2)]
    playing=4; cursor=6
    for c in (1,2): d.line(c*43-1,11,c*43-1,11+4*7-4)
    total=len(steps)+1
    for k in range(12):
        i=k
        if i>=total: break
        x0=(k//4)*43; y0=11+(k%4)*7; b=y0+6; s=i==cursor
        if s: d.rect(x0,y0,x0+41,y0+6,True,True)
        if i<len(steps):
            sc,rp=steps[i]
            if i==playing: d.tt(x0+1,b,">",not s)
            n=str(i+1)
            for j,ch in enumerate(n): d.tt(x0+13-4*(len(n)-j),b,ch,not s)
            d.tt(x0+16,b,"S%d"%sc,not s); d.tt(x0+30,b,"x%d"%rp,not s)
        else: d.tt(x0+5,b,"+ add",not s)
    h="CLICK ENC TO EDIT"; d.tt((W-d.ttw(h))//2,44,h)
    d.ctrl(FY1,False,FD1,"Step","7/10","80%","Master Vol"); d.ctrl(FY2,True,FD2,"Stop","","","Ins/Hold=Del")
    return d
def chain():
    d=D(); d.title("M1:Chain",3)
    ch=['A','A','A','B']; step=3
    for k in range(8):
        x0=4+k*15; x1=x0+12; y0=13; y1=27
        if k>=len(ch):
            for x in range(x0,x1+1,2): d.px(x,y0); d.px(x,y1)
            continue
        fill=k==step
        d.rect(x0,y0,x1,y1,True,fill)
        d.f6(x0+4,y0+4,ch[k],not fill)
    d.tt(0,39,"Chain playing")
    d.ctrl(FY1,False,FD1,"Pattern","B","",""); d.ctrl(FY2,True,FD2,"Pause/Hold=Rec","","","Add/Hold=Clear")
    return d
def patt():
    d=D(); d.title("M2:Patt",1)
    notes={0,1,2}; playing=0; nxt=1; cursor=2
    for p in range(8):
        x0=4+p*15; x1=x0+12; y0=13; y1=27
        fill=p==playing
        if fill: d.rect(x0,y0,x1,y1,True,True)
        elif p in notes: d.rect(x0,y0,x1,y1)
        else:
            for x in range(x0,x1+1,2): d.px(x,y0); d.px(x,y1)
        d.f6(x0+4,y0+4,chr(65+p),not fill)
        if p==cursor: d.rect(x0,y1+2,x1,y1+3,True,True)
    d.tt(0,39,"Playing A, B next")
    d.ctrl(FY1,False,FD1,"Pattern","C","+5","Transp"); d.ctrl(FY2,True,FD2,"Pause/Hold=Rec","","","Play/Hold=Copy")
    return d
# --- Arpeggiator (ui.cpp DrawArpGlobalPage/DrawArpScreen/DrawSeqArpPage) --
def dline(d,x0,y0,x1,y1):
    # Bresenham, as libDaisy's DrawLine.
    dx,dy=abs(x1-x0),-abs(y1-y0); sx=1 if x0<x1 else -1; sy=1 if y0<y1 else -1; err=dx+dy
    while True:
        d.px(x0,y0)
        if x0==x1 and y0==y1: break
        e2=2*err
        if e2>=dy: err+=dy; x0+=sx
        if e2<=dx: err+=dx; y0+=sy
def star_toggle(d,left,pair,second):
    st=d.ttw("*"); w=d.ttw(pair); x=1+st if left else W-1-w-st
    d.tt(x,FY2+5,pair); d.tt(x+w if second else x-st,FY2+5,"*")
def arp_global():
    d=D(); d.title("Global:Arp",0)
    rows=[("GR",0,""),("DXA",1,"Gen"),("DXB",1,"Phr"),("ENS",0,""),("ORG",1,"Gen"),("SSW",1,"Gen"),("SMV",0,""),("SMD",0,"")]
    for c in (1,2): d.line(c*43-1,11,c*43-1,36)
    sel=1
    for e,(n,on,m) in enumerate(rows):
        x0=(e//3)*43; y0=12+(e%3)*8; b=y0+6; s=e==sel
        if s: d.rect(x0,y0,x0+41,y0+7,True,True)
        d.tt(x0+2,b,n,not s); d.rect(x0+16,y0+1,x0+20,y0+5,not s,bool(on)); d.tt(x0+25,b,m if on else "off",not s)
    h="CLICK ENC TO EDIT"; d.tt((W-d.ttw(h))//2,43,h)
    d.ctrl(FY1,False,FD1,"Engine","DXD-A","",""); d.ctrl(FY2,True,FD2,"On/Off","","","")
    star_toggle(d,False,"Gen/Phr",False)
    return d
def arp_setup():
    d=D(); d.title("DXA:Setup",1)
    d.tt(2,17,"Generator"); t="Latch:On"; d.tt(W-1-d.ttw(t),17,t)
    for x in range(0,W,2): d.px(x,19)
    rows=[("Rate","1/16"),("Tempo","120 BPM"),("Range","2 oct")]; sel=1
    for i,(l,v) in enumerate(rows):
        y0=22+i*8; s=i==sel
        if s: d.rect(0,y0,W-1,y0+6,True,True)
        d.tt(2,y0+6,l,not s); d.tt(34,y0+6,v,not s)
    d.ctrl(FY1,False,FD1,"Edit","Tempo","120","BPM"); d.ctrl(FY2,True,FD2,"","","","Latch:On")
    star_toggle(d,True,"Gen/Phr",False)
    return d
MAJOR=[0,2,4,5,7,9,11]
def arp_pool(root,key,scale,chord,rng):
    # Arpeggiator::PoolNotes(): a chord's tones from the scale (Lock: in the key).
    inn=lambda n:(n-key)%12 in scale
    r=root
    if not inn(r):
        for dd in range(12):
            if inn(r+dd): r+=dd; break
            if inn(r-dd): r-=dd; break
    octv=[0]
    pick=lambda c:next((x for x in c if inn(r+x)),None)
    for c in ([[4,3,5,2],[7,6,8]] if chord=='triad' else [[4,3,5,2],[7,6,8],[10,11,9]]):
        x=pick(c)
        if x is not None: octv.append(x)
    return [r+12*o+i for o in range(rng) for i in octv]+[r+12*rng]
def gen_updown(n,k,jump=1):
    # Arpeggiator::GenIndex() for Up/Dn: up and back, turning at the ends.
    pos,dr=0,1
    for _ in range(k):
        if (dr>0 and pos==n-1) or (dr<0 and pos==0): dr=-dr
        pos=max(0,min(n-1,pos+dr*jump))
    return pos
def arp_pattern():
    d=D(); d.title("DXA:Pattern",2)
    pool=arp_pool(65,0,MAJOR,'triad',2); n=len(pool); steps=8; playing=3
    top,bot,x0=12,44,10; sw=(W-1-x0)//steps
    ry=lambda i: bot-((i*(bot-top)+(n-1)//2)//(n-1))
    for i in range(n):
        y=ry(i)
        if (pool[i]-pool[0])%12==0:
            for x in range(x0,W,2): d.px(x,y)
            d.tt(0,max(y+2,16),"R" if i==0 else "+%d"%((pool[i]-pool[0])//12))
        for k in range(steps): d.px(x0+k*sw+sw//2,y)
    prev=None
    for k in range(steps):
        x=x0+k*sw+sw//2; y=ry(gen_updown(n,k))
        if prev: dline(d,prev[0],prev[1],x,y)
        r=2 if k==playing else 1; d.rect(x-r,y-r,x+r,y+r,True,True); prev=(x,y)
    d.ctrl(FY1,False,FD1,"Steps","8","1","Jump"); d.ctrl(FY2,True,FD2,"Shape:Up/Dn","","","Rep:x1")
    return d
def arp_key():
    d=D(); d.title("DXA:Key",3)
    key=0; held=5  # holding F, locked to C major
    chord={(p-key)%12 for p in arp_pool(65,0,MAJOR,'triad',1)}
    names="C C# D D# E F F# G G# A A# B".split()
    for k in range(12):
        xa=4+k*10; inn=k in MAJOR
        d.rect(xa,13,xa+8,27,True,inn)
        if k in chord: d.rect(xa+3,22,xa+5,24,not inn,True)
        nm=names[(key+k)%12]; d.tt(xa+(9-d.ttw(nm))//2,34,nm)
        if k==held: d.rect(xa-1,11,xa+9,29); d.rect(xa+2,15,xa+6,25,not inn)
    d.tt(2,44,"C Major  Lock, from F")
    d.ctrl(FY1,False,FD1,"Key","C","Major","Scale"); d.ctrl(FY2,True,FD2,"","","","Chord:Triad")
    star_toggle(d,True,"Lock/Follow",False)
    return d
def midi_arp():
    d=D(); d.title("M1:Arp",0)
    on={0,1}; playing=0; cursor=1
    for p in range(8):
        x0=4+p*15; x1=x0+12; y0,y1=12,24; fill=p==playing
        d.rect(x0,y0,x1,y1,True,fill); d.f6(x0+4,y0+3,chr(65+p),not fill)
        if p in on: d.rect(x0+2,y1+2,x1-2,y1+3,True,True)
        if p==cursor: d.rect(x0-1,y0-1,x1+1,y1+5)
    d.tt(1,37,"B: Up/Dn 1/16 C Major")
    h="CLICK ENC TO EDIT"; d.tt((W-d.ttw(h))//2,44,h)
    d.ctrl(FY1,False,FD1,"Pattern","B","",""); d.ctrl(FY2,True,FD2,"Arp:On","","","Hold=Copy all")
    return d
def arp_phrase():
    # ui.cpp DrawArpPhrase(), with the factory phrases' own data.
    sys.path.insert(0,os.path.join(os.path.dirname(os.path.abspath(__file__)),'arp_phrases'))
    import phrases as P
    name='Acid'; ph=dict(P.PHRASES)[name]; root=60; playing=6
    d=D(); d.title("DXA:Phrase",2)
    notes=P.play(ph,root,60,'Major',False)
    ps=[n for n in notes if n not in (P.REST,P.TIE)]
    lo=min(min(ps),root); span=max(max(ps)-lo,7); top,bot,x0=13,41,9; L=len(ph); cw=(W-x0)//L
    y=lambda p: bot-((p-lo)*(bot-top)*2+span)//(2*span)
    for o in range(-2,4):
        p=root+12*o
        if lo<=p<=lo+span:
            yy=y(p)
            for x in range(x0,W,3): d.px(x,yy)
            d.tt(0,max(min(yy+2,43),16),"R" if o==0 else "%+d"%o)
    blocks=[]
    for k,st in enumerate(ph):
        if st in (P.REST,P.TIE): continue
        deg,vel,ln,oc,ch,rt=st; xa=x0+k*cw; yy=y(notes[k])
        ties=0
        while k+1+ties<L and ph[k+1+ties]==P.TIE: ties+=1
        w=max(2,int(cw*(ties+{0:0.45,1:0.75,2:1.0}[ln]))-(0 if ln==2 and ties==0 else 1))
        h0,h1={3:(-2,1),2:(-1,1),1:(-1,1),0:(0,0)}[vel]
        segs=[(xa,xa+w-1)] if rt==1 else [(xa+i*(w+1)//rt,max(xa+i*(w+1)//rt,xa+(i+1)*(w+1)//rt-2)) for i in range(rt)]
        blocks.append((segs,yy,h0,h1,vel,ch,k,xa))
    for segs,yy,h0,h1,vel,ch,k,xa in blocks:
        for a_,b_ in segs: d.rect(max(a_-1,x0),yy+h0-1,b_+1,yy+h1+1,False,True)
    for segs,yy,h0,h1,vel,ch,k,xa in blocks:
        for a_,b_ in segs:
            if ch:
                for x in range(a_,b_+1,2): d.px(x,yy+h0); d.px(x,yy+h1)
            elif vel==0: d.line(a_,yy,b_,yy)
            else: d.rect(a_,yy+h0,b_,yy+h1,True,vel>=2)
        if k==playing: d.line(xa,44,xa+cw-2,44)
    d.ctrl(FY1,False,FD1,"Phrase",name,"0","Octave"); d.ctrl(FY2,True,FD2,"","","","")
    return d
# --- Supersaw and the mixer pages ------------------------------------------
# The Global page icons, read from src/ui_graphics.cpp (48x20, MSB first).
_g=re.sub(r'//[^\n]*','',open(SRC+'ui_graphics.cpp').read())
def icon_bits(name):
    body=_g.split('const uint8_t %s[kGlobalIconBytes] = {'%name)[1].split('};')[0]
    return [int(x,16) for x in re.findall(r'0x([0-9A-Fa-f]{2})',body)]
def bitmap(d,x0,y0,w,h,bits):
    stride=(w+7)//8
    for y in range(h):
        for x in range(w):
            if bits[y*stride+x//8]&(0x80>>(x%8)): d.px(x0+x,y0+y)
def ctrl_noicon(d,row_y,div,ll,lv,rv,rl):
    # DrawControlRow(..., show_icon=false)
    d.line(0,div,W-1,div); b=row_y+5
    d.tt(1,b,ll)
    if rl: d.tt(W-1-d.ttw(rl),b,rl)
    cx=W//2
    if lv: d.tt(cx-4-d.ttink(lv),b,lv)
    if rv: d.tt(cx+5,b,rv)
def global_entry(title,enabled,icon):
    # Ui::DrawGlobalEntryPage()
    d=D(); d.title(title,0)
    d.tt(1,16,"ENABLED" if enabled else "DISABLED")
    bitmap(d,(W-48)//2,18,48,20,icon_bits(icon))
    h="PUSH ENC TO OPEN"; d.tt((W-d.ttw(h))//2,44,h)
    d.ctrl(FY1,False,FD1,"","","",""); d.ctrl(FY2,True,FD2,"Toggle On/Off","","","")
    return d
def global_supersaw(): return global_entry("Global:SSaw",True,'kGlobalIconSupersaw')
def hbar(d,x0,y0,w,h,v):
    d.rect(x0,y0,x0+w-1,y0+h-1); f=int(v*(w-2)+0.5)
    if f>0: d.rect(x0+1,y0+1,x0+f,y0+h-2,True,True)
def mixer_overview(title,names,levels):
    # Global:Tracks / Global:Synths -- Ui::DrawMixerOverviewGrid(16)
    d=D(); d.title(title,0)
    n=len(names); cols=7 if n>10 else 5; cw=126//cols; bw=cw-2
    for p,(nm,lv) in enumerate(zip(names,levels)):
        x=1+(p%cols)*cw; base=16+(p//cols)*14
        d.tt(x+(bw-d.ttink(nm))//2,base,nm); hbar(d,x,base+2,bw,5,lv)
    h="PUSH ENC TO OPEN"; d.tt((W-d.ttw(h))//2,44,h)
    d.ctrl(FY1,False,FD1,"Master Vol","80%","60%","Bypass Vol")
    ctrl_noicon(d,FY2,FD2,"CPU","31%","38%","Peak")
    return d
def global_tracks():
    return mixer_overview("Global:Tracks",["L1","L2","L3","L4","M1","M2","M3","M4","M5","M6","M7","M8","BYP","MAS"],
                          [.7,.55,0,.4,.8,.6,.65,0,.5,0,0,0,0,.85])
def global_synths():
    return mixer_overview("Global:Synths",["GR","DXA","DXB","ESB","ORG","SSW","VOI","DRM","BYP","MAS"],
                          [0,.6,.6,.45,0,.75,0,.8,0,.85])
def ssaw_supersaw():
    # SSaw:SSaw -- the bars follow the Detune knob, the readout is the true
    # spread (see ui_supersaw.cpp's SuperSaw page). Detune 60%, Mix 70%.
    det,mix,spread=0.6,0.7,27
    d=D(); d.title("SSaw:SSaw",0)
    off=[0,128,-128,408,-412,704,-720]; side=(1+32*mix)/33
    d.line(9,41,119,41)
    for t in (9,36,64,92,119): d.line(t,41,t,43)
    pos=[64+o/720*55*det for o in off]
    pr=pl=pos[0]
    for r,l in ((1,2),(3,4),(5,6)):
        if pos[r]-pr<5: pos[r]=pr+5
        pr=pos[r]
        if pl-pos[l]<5: pos[l]=pl-5
        pl=pos[l]
    for k in range(7):
        x=int(pos[k]+0.5); h=25/33 if k==0 else side; top=39-int(19*h)
        if k==0: d.rect(x-2,top,x+2,39,True,True)
        elif k>=5:
            for y in range(top,40,2): d.px(x-1,y); d.px(x+1,y)
            d.px(x,top)
        else: d.rect(x-1,top,x+1,39)
    d.tt(2,16,"Spread %dct"%spread)
    d.ctrl(FY1,False,FD1,"Detune","60%","70%","Mix"); d.ctrl(FY2,True,FD2,"","","","")
    return d
def save(d,name):
    S=3; im=Image.new('RGB',(W*S+12,H*S+12),(0,0,0)); px=im.load()
    for y in range(H):
        for x in range(W):
            if d.p[y][x]:
                for a in range(S):
                    for b in range(S): px[6+x*S+a,6+y*S+b]=(235,242,255)
    im.save(name)
SCREENS=[(home_audio,'home-audio'),(home_midi,'home-midi'),(patt,'midi-patt'),(chain,'midi-chain'),(scenes,'scenes'),(scene_edit,'scene-edit'),(song,'song'),(arp_global,'global-arp'),(arp_setup,'arp-setup'),(arp_pattern,'arp-pattern'),(arp_key,'arp-key'),(midi_arp,'midi-arp'),(arp_phrase,'arp-phrase'),(global_supersaw,'global-supersaw'),(ssaw_supersaw,'ssaw-supersaw'),(global_tracks,'global-tracks'),(global_synths,'global-synths')]
out=sys.argv[1]
os.makedirs(out,exist_ok=True)
for fn,n in SCREENS:
    save(fn(),out+'/'+n+'.png')
# contact sheet
ims=[Image.open(out+'/'+n+'.png') for _,n in SCREENS]
sheet=Image.new('RGB',(396*2+10,(204+10)*((len(ims)+1)//2)),(60,60,60))
for i,im in enumerate(ims): sheet.paste(im,((i%2)*(406),(i//2)*214))
sheet.save(out+'/sheet.png')
