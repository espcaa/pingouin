#!/usr/bin/env python3
"""Traces the penguin (app/Resources/icon/penguin.png, black on transparent)
into a vector outline for the boot splash: ui/pingouin-splash-art.h, points
normalized to the image (x / width, y / height, y down), one loop after the
other. The splash draws them as a smooth closed curve (Catmull-Rom), sharp at
any size. Marching squares on the alpha at 50 %, then Douglas-Peucker.
    runtime/tools/trace-penguin.py > header   (paste into the patch's new file)
"""
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
from pngread import read_png

here = os.path.dirname(os.path.abspath(__file__))
w, h, bpp, rows = read_png(os.path.join(here, '../../app/Resources/icon/penguin.png'))
assert bpp == 4
A = [[rows[y][x * 4 + 3] / 255.0 for x in range(w)] for y in range(h)]

def a(x, y):
    return A[y][x] if 0 <= x < w and 0 <= y < h else 0.0

# marching squares over cells (x, y)-(x+1, y+1), corners in pixel centres
segs = {}
def edge_pt(x0, y0, x1, y1):
    v0, v1 = a(x0, y0), a(x1, y1)
    t = (0.5 - v0) / (v1 - v0)
    return (x0 + (x1 - x0) * t + 0.5, y0 + (y1 - y0) * t + 0.5)

for y in range(-1, h):
    for x in range(-1, w):
        c = ((a(x, y) >= .5) << 3) | ((a(x + 1, y) >= .5) << 2) | ((a(x + 1, y + 1) >= .5) << 1) | (a(x, y + 1) >= .5)
        if c in (0, 15):
            continue
        T = lambda: edge_pt(x, y, x + 1, y)          # top
        R = lambda: edge_pt(x + 1, y, x + 1, y + 1)  # right
        B = lambda: edge_pt(x, y + 1, x + 1, y + 1)  # bottom
        L = lambda: edge_pt(x, y, x, y + 1)          # left
        table = {1: [(L, B)], 2: [(B, R)], 3: [(L, R)], 4: [(R, T)], 5: [(L, T), (R, B)],
                 6: [(B, T)], 7: [(L, T)], 8: [(T, L)], 9: [(T, B)], 10: [(T, R), (B, L)],
                 11: [(T, R)], 12: [(R, L)], 13: [(R, B)], 14: [(B, L)]}
        for p, q in table[c]:
            P, Q = p(), q()
            segs.setdefault((round(P[0], 4), round(P[1], 4)), []).append((round(Q[0], 4), round(Q[1], 4)))

loops = []
while segs:
    start = next(iter(segs))
    loop, cur = [start], start
    while True:
        nxt = segs[cur].pop()
        if not segs[cur]:
            del segs[cur]
        if nxt == start:
            break
        loop.append(nxt)
        cur = nxt
        if cur not in segs:
            break
    loops.append(loop)

def area(l):
    return sum(l[i][0] * l[(i + 1) % len(l)][1] - l[(i + 1) % len(l)][0] * l[i][1] for i in range(len(l))) / 2

def dp(pts, eps):
    # Douglas-Peucker on an open polyline
    if len(pts) < 3:
        return pts
    (x0, y0), (x1, y1) = pts[0], pts[-1]
    dx, dy = x1 - x0, y1 - y0
    n = (dx * dx + dy * dy) ** .5 or 1e-9
    i, dmax = 0, 0
    for k in range(1, len(pts) - 1):
        d = abs(dy * pts[k][0] - dx * pts[k][1] + x1 * y0 - y1 * x0) / n
        if d > dmax:
            i, dmax = k, d
    if dmax <= eps:
        return [pts[0], pts[-1]]
    return dp(pts[:i + 1], eps)[:-1] + dp(pts[i:], eps)

out = []
for l in loops:
    if abs(area(l)) < 200:
        continue
    half = len(l) // 2
    s = dp(l[:half + 1], 0.35)[:-1] + dp(l[half:] + [l[0]], 0.35)[:-1]
    out.append(s)

print('/*')
print(' * Pingouin boot splash: the penguin of the app\'s icon as a vector outline,')
print(' * traced from app/Resources/icon/penguin.png (%d x %d) by' % (w, h))
print(' * runtime/tools/trace-penguin.py. Loops of (x, y) normalized to the image,')
print(' * y down; drawn as smooth closed curves, filled even-odd.')
print(' */')
print('#ifndef PINGOUIN_SPLASH_ART_H')
print('#define PINGOUIN_SPLASH_ART_H')
print()
print('#define PINGOUIN_PENGUIN_ASPECT %.6ff   /* width / height */' % (w / h))
print('static const unsigned short pingouin_penguin_loop_len[] = { %s };' % ', '.join(str(len(s)) for s in out))
print('static const float pingouin_penguin_pts[][2] = {')
for s in out:
    for i in range(0, len(s), 4):
        print('    ' + ' '.join('{%.5ff, %.5ff},' % (p[0] / w, p[1] / h) for p in s[i:i + 4]))
print('};')
print()
print('#endif')
sys.stderr.write('%d loops, %s points\n' % (len(out), [len(s) for s in out]))
