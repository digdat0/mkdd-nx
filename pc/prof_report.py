"""Summarise prof.txt from mkdd_pc.exe (MKDD_PROF=<s>): per thread, busy share and top functions.
usage: python pc/prof_report.py pc/build/mkdd_pc.exe prof.txt [top]"""
import bisect, collections, subprocess, sys
exe, prof = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 25
NM = 'nm'
syms = []
for line in subprocess.run([NM, '-n', '--defined-only', exe], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3 and p[1] in 'tT':
        syms.append((int(p[0], 16), p[2]))
syms.sort()
addrs = [a for a, _ in syms]
BASE = 0x140000000
def name(rva):
    if rva >> 63: return '<dll+%#x>' % (rva & ((1 << 63) - 1))
    i = bisect.bisect_right(addrs, rva + BASE) - 1
    return syms[i][1] if i >= 0 else '?'
per = collections.defaultdict(collections.Counter)
for line in open(prof):
    t, rva, n = line.split()
    if '@' in t:
        th, mod = t.split('@'); per[th]['<' + mod + '>'] += int(n); continue
    per[t][name(int(rva, 16))] += int(n)
for t, c in sorted(per.items(), key=lambda kv: -sum(kv[1].values())):
    total = sum(c.values()); out = sum(v for k, v in c.items() if k.endswith('.dll>') or k == '<?>')
    print(f'== {t}: {total} samples, {100*(total-out)/total:.1f}% in exe')
    for fn, n in c.most_common(top):
        print(f'  {100*n/total:5.1f}%  {fn}')
