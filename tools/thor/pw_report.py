"""pw_report.py <raw> <log> <tag>: one line from a thor_power.sh run."""
import re
import sys

raw = open(sys.argv[1]).read().replace('\r', '').split('\n')
tag = sys.argv[3]
snaps = {}
for l in raw:
    f = l.split()
    if f and f[0] in ('P0', 'P3', 'P7', 'G'):
        vals = [int(v.rstrip(':')) for v in f[1:]]
        snaps.setdefault(f[0], []).append(dict(zip(vals[0::2], vals[1::2])))
out = []
for k, name, unit in (('P0', 'little', 1000), ('P3', 'mid', 1000), ('P7', 'prime', 1000), ('G', 'gpu', 1e6)):
    if len(snaps.get(k, [])) < 2:
        continue
    a, b = snaps[k][0], snaps[k][-1]
    dt = {fr: b[fr] - a.get(fr, 0) for fr in b}
    tot = sum(dt.values())
    if tot:
        out.append('%s %.0f' % (name, sum(fr * t for fr, t in dt.items()) / tot / unit))
S = [l.split() for l in raw if l.startswith('S ')]
busy = sum(int(s[1]) for s in S) / len(S)
ib = sum(int(s[2]) for s in S) / len(S) / 1000
iu = sum(int(s[3]) for s in S) / len(S) / 1000
cpu_t = max(int(l.split()[2]) for l in raw if l.startswith('T cpu')) / 1000
gpu_t = max(int(l.split()[2]) for l in raw if l.startswith('T gpu')) / 1000
log = open(sys.argv[2], errors='replace').read()
slow = [int(m.group(1)) for m in re.finditer(r'(\d+) over 20 ms', log)]
print('%-12s MHz: %s | gpu busy %.0f%% | battery %.0f mA, usb %.0f mA | max cpu %.1f C, gpu %.1f C | slow after 1800: %d'
      % (tag, ', '.join(out), busy, ib, iu, cpu_t, gpu_t, sum(slow[6:])))
