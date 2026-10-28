# Measured frame times of a GPU-bound stand-in game while a real background program (a temporary Edge
# instance with a WebGL page, started here) runs on the game's GPU, and after it is started on the
# Intel GPU instead (Chromium's --use-adapter-luid). One frame-time capture for the whole sequence:
# only GPUFusionFrameCapture.exe is elevated (one administrator prompt); Edge and the game are not.
#
#   python scripts/experiments/frames_with_edge.py [phase-seconds] [rounds]
import json, os, statistics, subprocess, sys, time

ROOT = 'G:/GPUFusion'
BIN = ROOT + '/build/release/bin/'
EDGE = r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe'
PROFILE = ROOT + '/build/edge-test/profile'
PAGE = 'file:///' + ROOT + '/build/edge-test/webgl-load.html'
PHASE = int(sys.argv[1]) if len(sys.argv) > 1 else 30
ROUNDS = int(sys.argv[2]) if len(sys.argv) > 2 else 2
SETTLE = 8
OUT = ROOT + '/build/frames-edge-capture.json'

probe = json.loads(subprocess.run([BIN + 'GPUFusionProbe.exe'], capture_output=True, text=True).stdout)['enumOrder']
intel = next(a for a in probe if a['vendorId'] == 0x8086)['luid']
high, low = (int(x, 16) for x in intel.split('_'))


def edge(on_intel):
    args = [EDGE, '--user-data-dir=' + PROFILE.replace('/', '\\'), '--no-first-run', '--no-default-browser-check', '--new-window',
            '--disable-features=CalculateNativeWinOcclusion', '--disable-backgrounding-occluded-windows', '--disable-renderer-backgrounding',
            '--window-size=900,700', '--window-position=700,40']
    if on_intel:
        args.append(f'--use-adapter-luid={high},{low}')
    subprocess.Popen(args + [PAGE])


def close_edge():
    ps = ("Get-CimInstance Win32_Process -Filter \"Name='msedge.exe'\" | Where-Object { $_.CommandLine -like '*" + PROFILE.replace('/', '\\')
          + "*' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }")
    subprocess.run(['powershell', '-NoProfile', '-Command', ps], capture_output=True)


phases = [c for _ in range(ROUNDS) for c in ('alone', 'edge on GT 730', 'edge on Intel')]
total = len(phases) * (PHASE + SETTLE + 4) + 20
if os.path.exists(OUT):
    os.remove(OUT)
game = subprocess.Popen([BIN + 'GPUFusionFrameTest.exe', '--api', 'd3d11', '--load', '256', '--seconds', str(total + 30)], stdout=subprocess.PIPE, text=True)
time.sleep(2)
# The only elevated step: the capture helper (the Windows administrator prompt appears once).
r = subprocess.run(['powershell', '-NoProfile', '-Command',
                    f"Start-Process -FilePath '{BIN}GPUFusionFrameCapture.exe' -ArgumentList '--out','{OUT}','--seconds','{total}','--pid','{game.pid}' -Verb RunAs"],
                   capture_output=True, text=True)
if r.returncode != 0:
    game.kill()
    sys.exit('the administrator prompt was declined or failed: ' + r.stderr.strip()[:200])
t0 = time.time()  # the helper starts right after the prompt is approved (its own clock starts within ~0.3 s)
marks = []
try:
    for name in phases:
        if name != 'alone':
            edge(name.endswith('Intel'))
        time.sleep(SETTLE)
        start = time.time() - t0
        time.sleep(PHASE)
        marks.append((name, start, time.time() - t0))
        close_edge()
        time.sleep(4)
    while not os.path.exists(OUT):
        time.sleep(2)
finally:
    close_edge()
    game.kill()

cap = json.load(open(OUT, encoding='utf-8'))
proc = next(p for p in cap['processes'] if p['pid'] == game.pid)
t = next(s for s in proc['streams'] if s['runtime'] == 'DXGI')['t']
results = []
for name, a, b in marks:
    ta = [x for x in t if a * 1000 + 1500 <= x <= b * 1000 - 500]  # inside the phase, away from its edges
    ft = [y - x for x, y in zip(ta, ta[1:])]
    per_second = []
    for s in range(int(a) + 2, int(b)):
        w = [y - x for x, y in zip(ta, ta[1:]) if s * 1000 <= y < (s + 1) * 1000]
        if w:
            per_second.append(statistics.mean(w))
    sf = sorted(ft)
    results.append({'phase': name, 'frames': len(ta), 'medianMs': statistics.median(ft), 'meanMs': statistics.mean(ft), 'p99Ms': sf[int(0.99 * (len(sf) - 1))],
                    'fps': 1000 / statistics.mean(ft), 'perSecondMeans': per_second})
    print(f"{name:15s} frames {len(ta):5d}  median {statistics.median(ft):6.2f} ms  mean {statistics.mean(ft):6.2f} ms  p99 {sf[int(0.99 * (len(sf) - 1))]:6.2f} ms  "
          f"{1000 / statistics.mean(ft):5.1f} fps", flush=True)
json.dump({'capture': {k: cap[k] for k in ('events', 'eventsLost', 'overhead', 'wallMs')}, 'phases': results}, open(ROOT + '/build/frames-edge-result.json', 'w'), indent=1)
