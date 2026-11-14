from page import F, PW, PH, M, ACC, logo
from PIL import Image, ImageDraw, ImageFilter
import numpy as np
p=Image.new('RGB',(PW,PH),(30,30,34)); d=ImageDraw.Draw(p)
lw=logo(p,M,120,90,(242,242,242))
d.text((M,240),'GROOVEBOX',font=F(64,'Heavy'),fill=(242,242,242))
d.text((M,330),'Quick Start',font=F(52,'Medium'),fill=(190,190,196))
d.rectangle([M,420,M+90,428],fill=ACC)
# the product render, straight from draw.py (case, knob, display, wheel)
from draw import draw_parts, body, flower_glow, wheel, sil
front=flower_glow(draw_parts(body('colour'),'colour'))
cv=Image.new('RGB',front.size,(30,30,34)); m=Image.fromarray((sil*255).astype(np.uint8))
wl=Image.new('L',front.size,0); wheel(ImageDraw.Draw(cv),0,0,False); wheel(ImageDraw.Draw(wl.convert('RGB')),0,0,False)
wm=Image.new('L',front.size,0); ImageDraw.Draw(wm).ellipse([1020-105,1602-105,1020+105,1602+105],fill=255)
full=Image.composite(front,cv,m); fm=Image.fromarray(np.maximum(np.array(m),np.array(wm)))
w=720; h=int(full.height*w/full.width); full=full.resize((w,h),Image.LANCZOS); fm=fm.resize((w,h),Image.LANCZOS)
x=(PW-w)//2; y=PH-h-200
sh=Image.new('L',p.size,0); sh.paste(fm.point(lambda v:170 if v>40 else 0),(x+24,y+40)); p.paste((6,6,8),(0,0),sh.filter(ImageFilter.GaussianBlur(34)))
p.paste(full,(x,y),fm)
d.text((M,PH-110),'Eight engines · four-track looper · eight-track MIDI sequencer',font=F(24),fill=(160,160,166))
p.save('page-qs-1.png')
