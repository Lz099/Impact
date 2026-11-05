"""The way into the ground pose, on its own clock: reads the [art] samples (every 0.1 s from the moment the
posture is triggered, or for the control the moment the body comes to rest) and prints per run:
  - joint turning speed (deg/s, smoothed over 0.1 s): peak and mean over the first 3 s (the trajectory),
    peak over 3-6 s (the pose being held), and which joint made the peak;
  - pelvis speed across the floor (cm/s, smoothed): peak over 0-3 s and 3-6 s.
Usage: python lire_trajet_aligne.py <name> [<name> ...]   (folders under Saved/_preuves_reprise/trajet)
"""
import glob
import os
import re
import sys

ROOT = r'D:/UEProjets/Impact/Saved/_preuves_reprise/trajet'
SAMPLE = re.compile(r'\[art\] rel=([\d.]+) art=(\d+) \((.*?)\) glisse=(\d+) posture=(\d)')
START = re.compile(r'\[art\] depart t=([\d.]+) \((\w+)\), ([\d.]+) s a terre')


def summary(path):
    samples, start = [], None
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = START.search(line)
            if m and start is None:
                start = (m[2], float(m[3]))
            m = SAMPLE.search(line)
            if m:
                samples.append((float(m[1]), int(m[2]), m[3], int(m[4])))
    return start, samples


def span(samples, lo, hi):
    return [s for s in samples if lo <= s[0] < hi]


all_rows = {}
for name in sys.argv[1:]:
    print(f'=== {name}')
    print(f'{"run":<12}{"depart":>16} | {"ART 0-3 s pic":>13} {"moy":>5} {"(joint)":<14} | {"ART 3-6 s pic":>13} | {"GLISSE 0-3":>10} {"3-6":>5}')
    for path in sorted(glob.glob(os.path.join(ROOT, name, '*.log'))):
        start, samples = summary(path)
        run = os.path.basename(path)[:-4]
        if not samples:
            print(f'{run:<12} pas de depart mesure')
            continue
        first, later = span(samples, 0.0, 3.0), span(samples, 3.0, 6.1)
        top = max(first, key=lambda s: s[1]) if first else None
        mean = sum(s[1] for s in first) / len(first) if first else 0
        print(f'{run:<12}{start[0]:>9} {start[1]:>4.1f}s | {top[1] if top else 0:>13} {mean:>5.0f} {("(" + top[2] + ")") if top else "":<14} | '
              f'{max((s[1] for s in later), default=0):>13} | {max((s[3] for s in first), default=0):>10} {max((s[3] for s in later), default=0):>5}')
        all_rows.setdefault(name, []).append((top[1] if top else 0, mean, max((s[1] for s in later), default=0),
                                              max((s[3] for s in first), default=0), max((s[3] for s in later), default=0)))
    rows = all_rows.get(name, [])
    if rows:
        print(f'{"moyenne":<12}{"":>16} | {sum(r[0] for r in rows)/len(rows):>13.0f} {sum(r[1] for r in rows)/len(rows):>5.0f} {"":<14} | '
              f'{sum(r[2] for r in rows)/len(rows):>13.0f} | {sum(r[3] for r in rows)/len(rows):>10.0f} {sum(r[4] for r in rows)/len(rows):>5.0f}')
