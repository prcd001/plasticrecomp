"""compare_shots.py - compare the screenshots of two auto_bootcamp runs (needs Pillow).

  python scripts/compare_shots.py before after [t t ...]

Compares build/screens/<before>_<t>s.png with <after>_<t>s.png for each time t (default: every time
that exists for both) and prints the size, the number of differing pixels (any channel off by more
than 8) and the largest channel difference. Moving elements (the pointer, animated units, movie frames)
make a few pixels differ even between two runs of the same build: compare against a repeat run."""
import glob, os, re, sys
from PIL import Image, ImageChops

d = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'screens')
a, b = sys.argv[1], sys.argv[2]
times = sys.argv[3:] or sorted({re.search(r'_(\d+)s\.png$', f).group(1) for f in glob.glob(os.path.join(d, a + '_*s.png'))})
for t in times:
    pa, pb = (os.path.join(d, f'{x}_{int(t):03d}s.png') for x in (a, b))
    if not (os.path.exists(pa) and os.path.exists(pb)):
        print(f't={t}: missing file'); continue
    ia, ib = Image.open(pa).convert('RGB'), Image.open(pb).convert('RGB')
    if ia.size != ib.size:
        print(f't={t}: size differs {ia.size} vs {ib.size}'); continue
    diff = ImageChops.difference(ia, ib)
    mx = max(x[1] for x in diff.getextrema())
    n = sum(1 for p in diff.getdata() if max(p) > 8)
    print(f't={t}: {ia.size[0]}x{ia.size[1]}  differing pixels {n} ({100.0 * n / (ia.size[0] * ia.size[1]):.3f}%)  max channel diff {mx}')
