import os
from common import CASE_DIR, SCREENS_PY, LOGO, HERE
exec(open(os.path.join(HERE,'page2.py')).read().split("# ---- page 3")[0])

def perf_save():   # Ui::DrawFileScreen(), ChoosingSave, after loading PERF1
    d=D(); d.title("Global:Perf",1); d.tt(0,15,"Performances")
    d.f6(0,24,"> Overwrite"); d.f6(0,34,"  Save New")
    d.ctrl(FY1,False,FD1,"Select","","",""); d.ctrl(FY2,True,FD2,"Back","","","Hold=Save"); return d
def home_midi_rec():
    d=D(); g['home_top'](d,1)
    g['home_boxes'](d,[dict(name='M1',state='rec',eng='DXA',pat='A',pos=.3),dict(name='M2',state='-',eng='ENS',pat='A',pos=None),
                       dict(name='M3',state='-',eng='SSW',pat='A',pos=None),dict(name='M4',state='-',eng='SMD',pat='A',pos=None)],0,True)
    w="MIDI Tracks"; d.tt((W-d.ttw(w))//2,44,w)
    d.ctrl(FY1,False,FD1,"Master Vol","80%","50%","Metro"); d.ctrl(FY2,True,FD2,"Pause/Hold=Rec","","","Bypass:Off"); return d

# ---- page 7: make a song
p=Image.new('RGB',(PW,PH),(250,249,246)); d=ImageDraw.Draw(p)
header(p,d,6,'Make a song','Tracks, then scenes, then the song.')
y=290
y=step(p,d,y,'A','Record a MIDI track',
 'On Home, turn past A4 to M1. Click in and pick its engine with K1, then click back out. Hold B1, play a pass after the count-in, and it loops.',tile(home_midi_rec()),
 tip='MIDI tracks work like audio tracks: tap B1 to pause, hold to add more.')
y=step(p,d,y,'B','Save scenes',
 'Get the tracks playing the way the first part should sound, then go to the Scenes page and hold B1: that\'s scene 1. Change things and save scene 2, and so on. Tap B1 to launch one.',tile(g['scenes']()))
y=step(p,d,y,'C','Build the song',
 'On the Song page, hold B1 and launch scenes on the Scenes page: each becomes a step, for as long as it played. Or click in and give each step a scene (K1) and its repeats (K2). Tap B1 to hear it.',tile(g['song']()))
y=step(p,d,y,'D','Export it',
 'On Global:Export, turn K1 to Song and tap B1. The song is written to the card as a WAV file, ready to play on a computer.')
footer(d,7); p.save('page-qs-7.png')

# ---- page 8: save your work
p=Image.new('RGB',(PW,PH),(250,249,246)); d=ImageDraw.Draw(p)
header(p,d,7,'Save your work','Two kinds of save: the whole song, or just the sounds.')
y=290
y=step(p,d,y,'A','Save a Performance',
 'A Performance is everything: the audio tracks, the MIDI tracks with their patterns, the scenes, the song and every engine\'s sound. On Global:Perf tap B1, turn K1 to Overwrite or Save New, and hold B2.',tile(perf_save()),
 tip='Saving only reaches the card when you do it: save before you switch off.')
y=step(p,d,y,'B','Save a Snapshot',
 'A Snapshot keeps every engine\'s sound at once, and which engines are on, so you can bring the same set of sounds into a new song. On Global:Snap it saves the same way: tap B1, pick with K1, hold B2.')
y=step(p,d,y,'C','Load them again',
 'On Global:Perf or Global:Snap, tap B2 to open the list, turn K2, and tap B2 to load.')
d.text((M,y-6),'WHERE NEXT',font=F(22,'Heavy'),fill=ACC); y+=44
y=para(d,M,y,'The User Manual covers every page in detail: the sequencer\'s patterns, chains and piano roll, the arpeggiator, every engine, the mixer, effects and the card.',F(24),PW-2*M,INK,1.42)
y+=40
items=[('A note won\'t stop','Triple-click the encoder: all notes off, on every engine.'),
       ('No sound','Is the engine ENABLED on its Global page? Is the keyboard on channel 1? Check Master Vol (K1 on Home) and the headphone wheel.'),
       ('Screen is dark','The screen saver: touch any control to wake it. That first touch does nothing else.'),
       ('"No SD card"','Switch off, push the card fully in, and switch on again.'),
       ('The keyboard does nothing','Check the cable is Type A, and that it goes into MIDI In.'),
       ('New firmware didn\'t install','A Pod new to this firmware needs the bootloader once: see the manual.')]
rh=[]
for k,v in items: rh.append(max(1,len(wrap(d,v,F(22),PW-2*M-420)))*int(22*1.4)+18)
bh=70+sum(rh)
d.rounded_rectangle([M,y,PW-M,y+bh],16,fill=(240,238,232))
d.text((M+30,y+22),'SOMETHING NOT RIGHT?',font=F(22,'Heavy'),fill=ACC); yy=y+70
for (k,v),h in zip(items,rh):
    d.text((M+30,yy),k,font=F(22,'Demi'),fill=INK); para(d,M+390,yy,v,F(22),PW-2*M-420,INK,1.4); yy+=h
footer(d,8); p.save('page-qs-8.png')
