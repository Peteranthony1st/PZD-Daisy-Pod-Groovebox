from draw import *
from sidestyle import classify
def strip(name,style):
    c=classify(name); h,w=c['zb'].shape
    if style=='colour':
        a=np.full((h,w,3),255,np.uint8)
        sh=hillshade(np.where(c['sil'],c['zb'],0)*16,c['sil']); sh=np.clip(0.55+0.45*sh,0.75,1.12)
        col=np.array([238,235,228])[None,None,:]*sh[...,None]
        a=np.where(c['sil'][...,None],np.clip(col,0,255),255).astype(np.uint8)
        a[c['eng']]=(185,182,175); a[c['hole']]=(22,22,24)
    else:
        a=np.full((h,w,3),255,np.uint8)
        e=lambda m,i: nd.binary_dilation(m,iterations=i)&~nd.binary_erosion(m,iterations=i)
        a[e(c['sil'],3)|e(c['hole'],3)]=(20,20,20); a[e(c['eng'],2)]=(90,90,90)
    return Image.fromarray(a),c['sil']
GAP=120
def compose(style):
    if style=='colour': f=flower_glow(draw_parts(body('colour'),'colour'))
    else: f=draw_parts(body('line'),'line',leds=False)
    L,Ls=strip('left',style); R,Rs=strip('right',style)
    W=L.width+GAP+f.width+GAP+R.width; H=f.height
    padL,padR,padT,padB=900,900,260,420
    bg=(250,249,246) if style=='colour' else (255,255,255)
    c=Image.new('RGB',(W+padL+padR,H+padT+padB),bg)
    xs=[padL,padL+L.width+GAP,padL+L.width+GAP+f.width+GAP]
    wheel(ImageDraw.Draw(c),xs[1],padT,style=='line')
    for im,ms,x in [(L,Ls,xs[0]),(f,sil,xs[1]),(R,Rs,xs[2])]:
        if style=='colour':
            s=Image.fromarray((ms*120).astype(np.uint8)).filter(ImageFilter.GaussianBlur(26)); c.paste((160,158,150),(x+16,padT+28),s)
        c.paste(im,(x,padT),Image.fromarray((ms*255).astype(np.uint8)))
    d=ImageDraw.Draw(c); F=font(54); Fs=font(40,False); ink=(30,30,30)
    for txt,x,w in [('LEFT SIDE',xs[0],L.width),('TOP',xs[1],f.width),('RIGHT SIDE',xs[2],R.width)]:
        tw=d.textlength(txt,font=Fs); d.text((x+w/2-tw/2,padT-90),txt,font=Fs,fill=(120,120,120))
    fx,fy=xs[1],padT; rx=xs[2]; lx=xs[0]
    left=[('Screen','',fx+300,fy+300),('Power light','Daisy power',fx+FLOWER[0]-100,fy+FLOWER[1]),
          ('USB','Power',lx+315,fy+1120),('SD card','',lx+75,fy+1380)]
    right=[('Encoder','Turn · Click · Hold',fx+ENC[0]+60,fy+ENC[1]-60),
           ('MIDI In','',rx+250,fy+945),('Audio In','',rx+250,fy+1147),('Audio Out','',rx+250,fy+1349),('Headphones','',rx+250,fy+1548)]
    bottom=[('B1 + LED 1','Green play · Blue pause',fx+BTN[0][0],fy+BTN[0][1]),('B2 + LED 2','Red: recording',fx+BTN[1][0],fy+BTN[1][1]),
            ('K1','Knob 1',fx+POT[0][0],fy+POT[0][1]),('K2','Knob 2',fx+POT[1][0],fy+POT[1][1]),('Volume','Headphone wheel',fx+1020,fy+1697)]
    def dot(tx,ty): d.ellipse([tx-11,ty-11,tx+11,ty+11],fill=ink,outline=(255,255,255),width=3)
    def callout(items,side):
        ys=np.linspace(fy+250,fy+1650,len(items)) if side=='L' else [fy+300]+list(np.linspace(fy+945,fy+1548,4))
        for (lab,sub,tx,ty),y in zip(items,ys):
            if side=='L':
                x0=padL-120; d.line([(x0,y),(x0+60,y),(tx,ty)],fill=ink,width=4); dot(tx,ty)
                tw=d.textlength(lab,font=F); d.text((x0-20-tw,y-40),lab,font=F,fill=ink)
                if sub: tw=d.textlength(sub,font=Fs); d.text((x0-20-tw,y+18),sub,font=Fs,fill=(110,110,110))
            else:
                x0=xs[2]+R.width+120; d.line([(x0,y),(x0-60,y),(tx,ty)],fill=ink,width=4); dot(tx,ty)
                d.text((x0+20,y-40),lab,font=F,fill=ink)
                if sub: d.text((x0+20,y+18),sub,font=Fs,fill=(110,110,110))
    callout(left,'L'); callout(right,'R')
    # under the device, in x order
    yb=padT+H+130; items=sorted(bottom,key=lambda t:t[2]); n=len(items)
    lxs=np.linspace(fx-260,fx+f.width+300,n)
    for (lab,sub,tx,ty),x in zip(items,lxs):
        d.line([(tx,ty),(tx,padT+H-60),(x,yb-70),(x,yb-30)],fill=ink,width=4); dot(tx,ty)
        tw=d.textlength(lab,font=F); d.text((x-tw/2,yb-20),lab,font=F,fill=ink)
        if sub: tw=d.textlength(sub,font=Fs); d.text((x-tw/2,yb+42),sub,font=Fs,fill=(110,110,110))
    return c
for st,n in [('colour','mB'),('line','mD')]:
    c=compose(st); c.save(n+'_full.png'); c.resize((c.width//3,c.height//3),Image.LANCZOS).save(n+'.png')
