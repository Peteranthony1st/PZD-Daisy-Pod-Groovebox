import os
from common import CASE_DIR, SCREENS_PY, LOGO, HERE
# The two side edges with only their ports labelled (for the set-up page).
import sys; sys.argv=['x']
from draw import *
exec(open(os.path.join(HERE,'mock2.py')).read().split("GAP=120")[0])   # strip()
def sides_figure():
    L,Ls=strip('left','colour'); R,Rs=strip('right','colour')
    y0,y1=830,1680; F=font(50); Fs=font(38,False); ink=(30,30,30); bg=(250,249,246)
    lab=560; gap=260
    W=lab+L.width+gap+R.width+lab; H=y1-y0+80
    c=Image.new('RGB',(W,H),bg); d=ImageDraw.Draw(c)
    xl,xr=lab,lab+L.width+gap
    for im,ms,x in [(L,Ls,xl),(R,Rs,xr)]:
        c.paste(im.crop((0,y0,im.width,y1)),(x,40),Image.fromarray((ms[y0:y1]*255).astype(np.uint8)))
    def dot(x,y): d.ellipse([x-11,y-11,x+11,y+11],fill=ink,outline=(255,255,255),width=3)
    for txt,sub,tx,ty in [('USB','Power',xl+315,1120),('SD card','',xl+75,1380)]:
        y=ty-y0+40; d.line([(lab-60,y),(tx,y)],fill=ink,width=4); dot(tx,y)
        tw=d.textlength(txt,font=F); d.text((lab-80-tw,y-30),txt,font=F,fill=ink)
        if sub: tw=d.textlength(sub,font=Fs); d.text((lab-80-tw,y+24),sub,font=Fs,fill=(110,110,110))
    for txt,ty in [('MIDI In',945),('Audio In',1147),('Audio Out',1349),('Headphones',1548)]:
        y=ty-y0+40; tx=xr+250; d.line([(tx,y),(xr+R.width+60,y)],fill=ink,width=4); dot(tx,y)
        d.text((xr+R.width+80,y-30),txt,font=F,fill=ink)
    for txt,x,w in [('LEFT SIDE',xl,L.width),('RIGHT SIDE',xr,R.width)]:
        tw=d.textlength(txt,font=Fs); d.text((x+w/2-tw/2,0),txt,font=Fs,fill=(120,120,120))
    return c
sides_figure().save('sides-fig.png')
