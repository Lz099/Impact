"""Reads the pain bench logs of Saved/_preuves_reprise/trajet/<name>/ and prints, per run, the peaks that matter
for the way into the ground pose.

Each [sol] line covers the previous two seconds on the ground: mean and PEAK limb speed in the pelvis frame,
mean and PEAK pelvis speed across the floor (sliding), net drift since the fall.

Windows are split by time on the ground:
  chute      0 - 3 s   the fall itself, the same with or without the posture
  trajet     3 - 9 s   where the posture starts and the body goes into it
  pose       9 s +     the pose held
Usage: python lire_banc_trajet.py <name> [<name> ...]
"""
import glob
import os
import re
import sys

ROOT = r'D:/UEProjets/Impact/Saved/_preuves_reprise/trajet'
SOL = re.compile(r'\[sol\] t=([\d.]+) .*?membres ([\d.]+) cm/s .*?\(PIC (\d+)\) \| GLISSE ([\d.]+) cm/s \(PIC (\d+)\) \| (.*?), (\d+) s a terre.*?DERIVE (\d+) cm')
POSTURE = re.compile(r'\[posture\] t=([\d.]+) tete-genoux (\d+) cm')
SMOOTH = re.compile(r'\[lisse\] t=([\d.]+) (\d+) s a terre \| ARTICULATION PIC (\d+) deg/s \((.*?)\) \| GLISSE PIC (\d+) cm/s')
START = re.compile(r'\[sol\] \S+ au sol:')


def read(path):
    rows, shapes, start_index, smooth = [], [], None, []
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if START.search(line) and start_index is None:
                start_index = len(rows)
            m = SOL.search(line)
            if m:
                rows.append(dict(t=float(m[1]), limb=float(m[2]), limb_peak=int(m[3]), slide=float(m[4]),
                                 slide_peak=int(m[5]), state=m[6], ground=int(m[7]), drift=int(m[8])))
            m = SMOOTH.search(line)
            if m:
                smooth.append(dict(ground=int(m[2]), joint=int(m[3]), name=m[4], slide=int(m[5])))
            m = POSTURE.search(line)
            if m:
                shapes.append(int(m[2]))
    return rows, shapes, start_index, smooth


def peak(rows, lo, hi, key):
    values = [r[key] for r in rows if lo <= r['ground'] < hi]
    return max(values) if values else None


def mean(rows, lo, hi, key):
    values = [r[key] for r in rows if lo <= r['ground'] < hi]
    return sum(values) / len(values) if values else None


def fmt(v, digits=0):
    return '-' if v is None else (f'{v:.{digits}f}')


for name in sys.argv[1:]:
    print(f'=== {name}')
    print(f'{"run":<14}{"debut":>6} | {"PIC membres":>24} | {"PIC glisse":>24} | {"glisse moy pose":>15} | {"derive":>6} | {"tete-gen":>8}')
    print(f'{"":<14}{"pose":>6} | {"chute  trajet  pose":>24} | {"chute  trajet  pose":>24} |')
    for path in sorted(glob.glob(os.path.join(ROOT, name, '*.log'))):
        rows, shapes, start, smooth = read(path)
        if not rows:
            print(f'{os.path.basename(path):<14} pas de mesure au sol')
            continue
        started = rows[start]['ground'] if start is not None and start < len(rows) else None
        print(f'{os.path.basename(path)[:-4]:<14}{fmt(started):>6} | '
              f'{fmt(peak(rows, 0, 3, "limb_peak")):>6} {fmt(peak(rows, 3, 9, "limb_peak")):>7} {fmt(peak(rows, 9, 99, "limb_peak")):>6}    | '
              f'{fmt(peak(rows, 0, 3, "slide_peak")):>6} {fmt(peak(rows, 3, 9, "slide_peak")):>7} {fmt(peak(rows, 9, 99, "slide_peak")):>6}    | '
              f'{fmt(mean(rows, 9, 99, "slide"), 1):>15} | {rows[-1]["drift"]:>6} | {shapes[-1] if shapes else "-":>8}')
        if smooth:
            worst = max((r for r in smooth if 3 <= r['ground'] < 9), key=lambda r: r['joint'], default=None)
            print(f'{"":<14}  lisse: ARTICULATION chute/trajet/pose {fmt(peak(smooth, 0, 3, "joint")):>5} {fmt(peak(smooth, 3, 9, "joint")):>5} {fmt(peak(smooth, 9, 99, "joint")):>5} deg/s'
                  f' ({worst["name"] if worst else "-"}) | GLISSE trajet/pose {fmt(peak(smooth, 3, 9, "slide")):>4} {fmt(peak(smooth, 9, 99, "slide")):>4} cm/s')
