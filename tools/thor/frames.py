# frames.py <log>: per frame (present to present) from LAA_TRACE_SYNC lines: submissions, GPU waits, main-thread waits
import re, sys
ev = []
for line in open(sys.argv[1], errors='replace'):
    m = re.match(r'sync: ([\d.]+) (.*)', line.strip())
    if m: ev.append((float(m.group(1)), m.group(2)))
ev.sort()
pres = [t for t, s in ev if s.startswith('presented')]
rows = []
for a, b in zip(pres, pres[1:]):
    sub = gpu = mainw = 0.0; nsub = 0; vs = 0
    for t, s in ev:
        if not (a < t <= b): continue
        if s.startswith('submitted'): sub += float(re.search(r'took ([\d.]+)', s).group(1)); nsub += 1
        elif 'wait for' in s: gpu += float(re.search(r': ([\d.]+) ms', s).group(1))
        elif s.startswith('thread 81 waited'): mainw += float(re.search(r'waited ([\d.]+)', s).group(1))
        elif s == 'vsync': vs += 1
    rows.append((b - a, sub, nsub, gpu, mainw, vs))
fast = [r for r in rows if r[0] < 20]; slow = [r for r in rows if r[0] >= 20]
def avg(rs, i): return sum(r[i] for r in rs) / max(1, len(rs))
print(f'{len(rows)} frames, {len(slow)} slow')
for name, rs in (('ok  ', fast), ('slow', slow)):
    print(f'{name}: frame {avg(rs,0):5.1f} ms  submit {avg(rs,1):4.1f} ({avg(rs,2):.1f} lists)  gpu wait {avg(rs,3):4.1f}  main thread waits {avg(rs,4):4.1f}  other {avg(rs,0)-avg(rs,1)-avg(rs,3)-avg(rs,4):4.1f}')
print('slow frames:', [round(r[0],1) for r in slow][:20])
idx = [i for i, r in enumerate(rows) if r[0] >= 20]; print('at frame offsets', idx[:30])
