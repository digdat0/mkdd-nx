"""CPU cost per game frame from a bench run with MKDD_PROF=<s>: the profiler's per-thread
Mcycles/s divided by the game frame rate over the same window (more stable than fps, which the
VI pacing caps). usage: py -3 pc/per_frame.py pc/run/bench_<label>.txt [prof start s]"""
import re, sys
text = open(sys.argv[1]).read()
start = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0
rows = [(float(t), int(f)) for t, f in re.findall(r"\[t=\s*([\d.]+)s\] frames (\d+)", text)]
win = [r for r in rows if r[0] >= start]
if len(win) < 2:
    sys.exit("not enough samples")
fps = (win[-1][1] - win[0][1]) / (win[-1][0] - win[0][0])
cpu = {}
for name, v in re.findall(r"cpu: (\S+)\s+([\d.]+) Mcycles/s", text):
    cpu[name] = cpu.get(name, 0) + float(v)
print(f"{sys.argv[1]}: {fps:.1f} frames/s in window; " + ", ".join(
    f"{k} {cpu[k] / fps:.2f} Mcyc/frame" for k in ("game", "mkdd-gpu") if k in cpu))
