"""Does the body stay where it fell? Reads the [place] lines (pelvis position relative to where it fell, and heading
of the pelvis-to-head line, every 2 s), the [art] depart line, the [sol] and the [posture] lines, and prints per run
the net travel across the floor (cm) in each phase:
  avant    from 2 s on the ground to the start of the posture (or of rest, for the control): the body still
           settling, muscles of the writhing take pulling on it
  trajet   the 4 s from the start of the posture: the way into the pose
  tenue    from 4 s after the start to the end: the pose held, and the same per second, and the turn (deg)
and, on a fixed clock that does not depend on when the posture started:
  2-10 s   path travelled by the pelvis from 2 to 10 s on the ground (sum of the 2 s steps, cm)
  membres  mean limb speed in the pelvis frame over 2-10 s on the ground (cm/s): the thrashing
and the mean head-to-knees over the hold (cm: the fold, lower is more curled).
Usage: python lire_fluage.py <name> [<name> ...]   (folders under Saved/_preuves_reprise/trajet)
"""
import glob
import math
import os
import re
import sys

ROOT = r'D:/UEProjets/Impact/Saved/_preuves_reprise/trajet'
START = re.compile(r'\[art\] depart t=([\d.]+) \((\w+)\)')
PLACE = re.compile(r'\[place\] t=([\d.]+) x=(-?[\d.]+) y=(-?[\d.]+) cap=(-?[\d.]+)')
SHAPE = re.compile(r'\[posture\] t=([\d.]+) tete-genoux (\d+) cm')
LIMBS = re.compile(r'\[sol\] t=([\d.]+) .*?membres ([\d.]+) cm/s')


def read(path):
    start, places, shapes, limbs = None, [], [], []
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = START.search(line)
            if m and start is None:
                start = float(m[1])
            m = PLACE.search(line)
            if m:
                places.append((float(m[1]), float(m[2]), float(m[3]), float(m[4])))
            m = SHAPE.search(line)
            if m:
                shapes.append((float(m[1]), int(m[2])))
            m = LIMBS.search(line)
            if m:
                limbs.append((float(m[1]), float(m[2])))
    return start, places, shapes, limbs


def nearest(places, t):
    return min(places, key=lambda p: abs(p[0] - t))


def dist(a, b):
    return math.hypot(b[1] - a[1], b[2] - a[2])


for name in sys.argv[1:]:
    print(f'=== {name}')
    print(f'{"run":<12}{"depart":>7} | {"avant":>6} {"trajet":>7} {"tenue":>6} {"cm/s":>5} {"tour":>5} | {"2-10 s":>6} {"membres":>7} | {"tete-gen":>8}')
    totals = []
    for path in sorted(glob.glob(os.path.join(ROOT, name, '*.log'))):
        start, places, shapes, limbs = read(path)
        run = os.path.splitext(os.path.basename(path))[0]
        if start is None or len(places) < 3:
            print(f'{run:<12} pas de depart ou pas de [place]')
            continue
        ground = places[0][0]
        a = nearest(places, ground + 2.0)
        b = nearest(places, start)
        c = nearest(places, start + 4.0)
        held = [p for p in places if p[0] >= c[0]]
        last = held[-1]
        span = max(0.1, last[0] - c[0])
        turn = sum(abs((q[3] - p[3] + 180.0) % 360.0 - 180.0) for p, q in zip(held, held[1:]))
        fold = [s for t, s in shapes if t >= c[0]]
        fold_mean = sum(fold) / len(fold) if fold else float('nan')
        window = [p for p in places if ground + 1.5 <= p[0] <= ground + 10.5]
        path_len = sum(dist(p, q) for p, q in zip(window, window[1:]))
        # A [sol] line covers the two seconds before it, so 2-10 s is the lines at 4, 6, 8 and 10 s.
        thrash = [v for t, v in limbs if ground + 3.5 <= t <= ground + 10.5]
        thrash_mean = sum(thrash) / len(thrash) if thrash else float('nan')
        row = (dist(a, b) if b[0] > a[0] else 0.0, dist(b, c), dist(c, last), dist(c, last) / span, turn,
               path_len, thrash_mean, fold_mean)
        totals.append(row)
        print(f'{run:<12}{start - ground:>6.1f}s | {row[0]:>6.0f} {row[1]:>7.0f} {row[2]:>6.0f} {row[3]:>5.1f} {row[4]:>5.0f} | '
              f'{row[5]:>6.0f} {row[6]:>7.0f} | {row[7]:>8.0f}')
    if totals:
        n = len(totals)
        mean = [sum(r[k] for r in totals) / n for k in range(8)]
        print(f'{"moyenne":<12}{"":>7} | {mean[0]:>6.0f} {mean[1]:>7.0f} {mean[2]:>6.0f} {mean[3]:>5.1f} {mean[4]:>5.0f} | '
              f'{mean[5]:>6.0f} {mean[6]:>7.0f} | {mean[7]:>8.0f}')
