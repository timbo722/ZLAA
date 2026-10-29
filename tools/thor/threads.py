import sys, re
a, b = {}, {}
for line in open(sys.argv[1]):
    m = re.match(r'([AB]) (\d+) \((.*)\) (.*)', line.strip())
    if not m: continue
    f = m.group(4).split()          # fields from state (field 3) on
    ut, st, cpu = int(f[11]), int(f[12]), int(f[36])
    (a if m.group(1) == 'A' else b)[m.group(2)] = (m.group(3), ut + st, cpu)
rows = sorted(((b[t][1] - a[t][1], t, b[t][0], b[t][2]) for t in b if t in a), reverse=True)
tot = sum(r[0] for r in rows)
print('total %.0f%% of one core over 5 s' % (tot / 5))     # ticks are 1/100 s
for d, t, n, c in rows[:16]: print('%5.1f%%  tid %s  cpu%d  %s' % (d / 5, t, c, n))
