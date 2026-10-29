import sys, re
lines = open(sys.argv[1]).read().split('\n')
start = next(i for i, l in enumerate(lines) if 'GPU time of frame' in l)
out, prev, n, fss = [], None, 0, []
def flush():
    if prev: out.append('%4dx  %s  fs %s' % (n, prev, ' '.join(sorted(set(fss)))[:60]))
for l in lines[start:]:
    if 'presented' in l and 'frame' in l: break
    m = re.match(r'render: draw vs \S+ fs (\S+) -> (.*?) vp (\S+ \S+)', l)
    if m:
        key = m.group(2) + ' vp ' + m.group(3)
        if key != prev: flush(); prev, n, fss = key, 0, []
        n += 1; fss.append(m.group(1)); continue
    m = re.match(r'render: (copy|clear|dispatch|blit|resolve|update|fill)(.*)', l)
    if m and not l.startswith('render:    '):
        flush(); prev = None; out.append('       ' + m.group(1) + m.group(2)[:110])
flush()
print('\n'.join(out))
