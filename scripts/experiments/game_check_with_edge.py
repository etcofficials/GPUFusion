# Capability "move other programs' GPU work off the game's GPU", end to end with a real application.
#
# Stand-in game: GPUFusionFrameTest --load 256 (Direct3D 11, GPU-bound on the GT 730, presents frames).
# Background: a temporary Microsoft Edge instance started here (its own profile folder under build\,
# so it never touches the user's browsers) drawing a WebGL page, first on its default GPU (the GT 730),
# then started on the Intel GPU with Chromium's --use-adapter-luid (the mechanism GPU Fusion verified).
# GPU Fusion's game check watches the game in each case; with --frames it also measures the game's
# frame times (needs administrator rights: run this script elevated). Only the Edge instance started
# here is closed afterwards (matched by its profile folder).
#
#   python scripts/experiments/game_check_with_edge.py [--frames] [--seconds N] [--rounds R]
import json, os, subprocess, sys, time

ROOT = 'G:/GPUFusion'
BIN = ROOT + '/build/release/bin/'
DATA = ROOT + '/build/release/bench-data'
EDGE = r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe'
PROFILE = ROOT + '/build/edge-test/profile'
PAGE = 'file:///' + ROOT + '/build/edge-test/webgl-load.html'
frames = '--frames' in sys.argv
seconds = int(sys.argv[sys.argv.index('--seconds') + 1]) if '--seconds' in sys.argv else 12
rounds = int(sys.argv[sys.argv.index('--rounds') + 1]) if '--rounds' in sys.argv else 1

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
    time.sleep(8)  # start, compile the shader, reach a steady load


def close_edge():
    # Only the processes of the instance started here: their command line names its profile folder.
    ps = ("Get-CimInstance Win32_Process -Filter \"Name='msedge.exe'\" | Where-Object { $_.CommandLine -like '*" + PROFILE.replace('/', '\\')
          + "*' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }")
    subprocess.run(['powershell', '-NoProfile', '-Command', ps], capture_output=True)
    time.sleep(3)


def check(game_pid, label):
    args = [BIN + 'GPUFusion.exe', '--data-dir', DATA, '--game-check', str(game_pid), '--game-seconds', str(seconds), '--game-label', label]
    if frames:
        args.append('--game-frames')
    subprocess.run(args, capture_output=True, text=True)
    newest = sorted(f for f in os.listdir(DATA + '/games') if f.startswith('game-'))[-1]
    return json.load(open(DATA + '/games/' + newest, encoding='utf-8'))


def summary(c):
    f = c.get('frames')
    fr = ''
    if f and not f.get('error'):
        ts = [s for p in f['capture']['processes'] if p['pid'] == c['pid'] for s in p['streams'] if s['runtime'] != 'Kernel']
        if ts:
            t = ts[0]['t']
            ft = sorted(b - a for a, b in zip(t, t[1:]))
            fr = f" | frames {len(t)} median {ft[len(ft) // 2]:.2f} ms ({1000 / ft[len(ft) // 2]:.1f} fps), p99 {ft[int(0.99 * (len(ft) - 1))]:.2f} ms"
    elif f:
        fr = ' | frames not measured: ' + f.get('error', '')[:80]
    comp = '; '.join(f"{k['name']} {k['meanPercent']:.0f}% on {k['adapter'][:14]} (move: {k['moveStatus']})" for k in c['competitors'][:4])
    return f"game {c['gameGpuMean']:.0f}% of {c['gameAdapter'][:14]}, others {c['othersOnGameGpuMean']:.0f}%{fr} | competitors: {comp or 'none'}"


results = []
game = subprocess.Popen([BIN + 'GPUFusionFrameTest.exe', '--api', 'd3d11', '--load', '256', '--seconds', str(rounds * 3 * (seconds + 30) + 60)],
                        stdout=subprocess.PIPE, text=True)
time.sleep(2)
try:
    for r in range(rounds):
        for case in ('alone', 'edge on GT 730', 'edge on Intel'):
            if case != 'alone':
                edge(case.endswith('Intel'))
            c = check(game.pid, case)
            close_edge()
            line = f"round {r + 1} {case:15s} {summary(c)}"
            print(line, flush=True)
            results.append({'round': r + 1, 'case': case, 'check': c['id'], 'gameGpuMean': c['gameGpuMean'], 'others': c['othersOnGameGpuMean'],
                            'summary': line})
finally:
    close_edge()
    game.kill()
json.dump(results, open(ROOT + '/build/game-check-edge.json', 'w'), indent=1)
