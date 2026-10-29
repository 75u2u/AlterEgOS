#!/usr/bin/env python3
# Test pictures for the agent's eye (「絵を見て」): a line, a circle, a square,
# a face, a filled disk, a circle with two lines, a triangle and a heart,
# drawn with round brush strokes into 640x400 PPM files like paint saves.
# Usage: shape-pictures.py OUTDIR
import sys, math, os
W,H=640,400
def new(): return bytearray(b'\xff'*(W*H*3))
def dot(px,cx,cy,r,c):
    for y in range(cy-r,cy+r+1):
        for x in range(cx-r,cx+r+1):
            if 0<=x<W and 0<=y<H and (x-cx)**2+(y-cy)**2<=r*r:
                i=(y*W+x)*3; px[i:i+3]=bytes(c)
def line(px,x0,y0,x1,y1,r,c):
    n=max(abs(x1-x0),abs(y1-y0),1)
    for i in range(n+1):
        dot(px,round(x0+(x1-x0)*i/n),round(y0+(y1-y0)*i/n),r,c)
def circle(px,cx,cy,R,r,c,a0=0,a1=360):
    pts=[(round(cx+R*math.cos(math.radians(a))),round(cy+R*math.sin(math.radians(a)))) for a in range(a0,a1+1,5)]
    for p,q in zip(pts,pts[1:]): line(px,*p,*q,r,c)
def save(px,name): open(os.path.join(sys.argv[1],name),'wb').write(b'P6\n%d %d\n255\n'%(W,H)+px)
BLACK=(0,0,0); RED=(0xE5,0x39,0x35); BLUE=(0x1E,0x88,0xE5)
px=new(); line(px,100,300,500,100,3,BLACK); save(px,'line.ppm')
px=new(); circle(px,320,200,120,3,RED); save(px,'circle.ppm')
px=new(); line(px,200,100,440,100,3,BLUE); line(px,440,100,440,300,3,BLUE); line(px,440,300,200,300,3,BLUE); line(px,200,300,200,100,3,BLUE); save(px,'square.ppm')
px=new(); circle(px,320,200,140,3,BLACK); dot(px,270,160,12,BLACK); dot(px,370,160,12,BLACK); circle(px,320,210,70,3,RED,20,160); save(px,'face.ppm')
px=new(); dot(px,320,200,80,BLUE); save(px,'disk.ppm')
px=new(); circle(px,200,200,80,3,RED); line(px,350,120,550,120,3,BLUE); line(px,350,280,550,280,3,BLUE); save(px,'mixed.ppm')
px=new(); line(px,320,80,560,330,3,BLACK); line(px,560,330,80,330,3,BLACK); line(px,80,330,320,80,3,BLACK); save(px,'triangle.ppm')
# heart via the same parametric curve, hand-ish
pts=[]
for i in range(40):
    t=2*math.pi*i/40
    x=16*math.sin(t)**3; y=13*math.cos(t)-5*math.cos(2*t)-2*math.cos(3*t)-math.cos(4*t)
    pts.append((round(320+x*9),round(210-y*9)))
pts.append(pts[0])
px=new()
for p,q in zip(pts,pts[1:]): line(px,*p,*q,3,RED)
save(px,'heart.ppm')
