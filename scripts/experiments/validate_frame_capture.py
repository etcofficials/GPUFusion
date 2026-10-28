# Validates GPUFusionFrameCapture against ground truth (docs/FRAME_TIMES.md). Needs administrator
# rights (event tracing); run it elevated. Changes no Windows setting.
#
# For each graphics API, GPUFusionFrameTest presents at a known pace and records its own present times;
# the capture of that process must show the same frame count and frame times. Also: a capture of every
# process (which programs present frames), the capture's overhead on an uncapped presenter, and GPU
# Fusion's own --measure-frames end to end.
#
#   python scripts/experiments/validate_frame_capture.py <result.json>
import json, statistics, subprocess, sys, time, os, tempfile

BIN = 'G:/GPUFusion/build/release/bin/'
DATA = 'G:/GPUFusion/build/release/bench-data'
OUT = sys.argv[1]
TMP = tempfile.mkdtemp(prefix='gf-frames-', dir='G:/GPUFusion/build')
results = {'started': time.strftime('%Y-%m-%d %H:%M:%S'), 'cases': [], 'errors': []}


def save():
    with open(OUT, 'w', encoding='utf-8') as f:
        json.dump(results, f, indent=1)


def luids():
    e = json.loads(subprocess.run([BIN + 'GPUFusionProbe.exe'], capture_output=True, text=True).stdout)['enumOrder']
    return next(a['luid'] for a in e if a['vendorId'] == 0x10DE), next(a['luid'] for a in e if a['vendorId'] == 0x8086)


def capture(seconds, pids=(), name='cap'):
    out = os.path.join(TMP, name + '.json')
    args = [BIN + 'GPUFusionFrameCapture.exe', '--out', out, '--seconds', str(seconds)]
    for p in pids:
        args += ['--pid', str(p)]
    return subprocess.Popen(args), out


def stats(t):
    ft = [b - a for a, b in zip(t, t[1:])]
    if not ft:
        return None
    s = sorted(ft)
    return {'presents': len(t), 'median': statistics.median(ft), 'mean': statistics.mean(ft), 'p99': s[min(len(s) - 1, int(0.99 * (len(s) - 1) + 0.5))],
            'max': s[-1], 'long': sum(1 for x in ft if x > 2 * statistics.median(ft) and x > statistics.median(ft) + 4)}


def case(name, test_args, seconds=6):
    test = subprocess.Popen([BIN + 'GPUFusionFrameTest.exe'] + test_args + ['--seconds', str(seconds + 3)], stdout=subprocess.PIPE, text=True)
    time.sleep(1.0)
    cap, out = capture(seconds, [test.pid], name)
    cap.wait()
    truth = json.loads(test.communicate()[0])
    c = json.load(open(out, encoding='utf-8'))
    entry = {'case': name, 'args': test_args, 'truth': truth, 'captureOk': c.get('ok'), 'error': c.get('error'), 'events': c.get('events'),
             'eventsLost': c.get('eventsLost'), 'overhead': c.get('overhead'), 'streams': []}
    for p in c.get('processes', []):
        if p['pid'] != test.pid:
            continue
        for s in p['streams']:
            entry['streams'].append({'runtime': s['runtime'], 'syncInterval': s['syncInterval'], 'discarded': s['discarded'], 'occluded': s['occluded'],
                                     'stats': stats(s['t'])})
    results['cases'].append(entry)
    save()
    st = entry['streams']
    print(name, 'truth median', truth.get('frameTimeMs', {}).get('median'), '| capture', [(s['runtime'], s['stats'] and round(s['stats']['median'], 3),
                                                                                                s['stats'] and s['stats']['presents']) for s in st], flush=True)


try:
    GT, INTEL = luids()
    case('d3d11 GT 10ms', ['--api', 'd3d11', '--interval-ms', '10'])
    case('d3d12 GT 10ms', ['--api', 'd3d12', '--interval-ms', '10'])
    case('d3d9 10ms', ['--api', 'd3d9', '--interval-ms', '10'])
    case('opengl 10ms', ['--api', 'opengl', '--interval-ms', '10'])
    case('d3d11 Intel 10ms', ['--api', 'd3d11', '--adapter', INTEL, '--interval-ms', '10'])
    case('d3d11 GT uncapped', ['--api', 'd3d11'])
    case('opengl uncapped', ['--api', 'opengl'])
    case('d3d9 uncapped', ['--api', 'd3d9'])
    case('d3d11 vsync', ['--api', 'd3d11', '--vsync'])
    case('opengl vsync', ['--api', 'opengl', '--vsync'])
    case('d3d11 stalls', ['--api', 'd3d11', '--interval-ms', '16.667', '--stall-every', '30', '--stall-ms', '50'])

    # Every process that presents (5 s): which programs draw frames right now.
    cap, out = capture(5, (), 'all')
    cap.wait()
    c = json.load(open(out, encoding='utf-8'))
    results['allProcesses'] = {'ok': c.get('ok'), 'error': c.get('error'), 'events': c.get('events'), 'overhead': c.get('overhead'),
                               'processes': [{'name': p['name'], 'streams': [{'runtime': s['runtime'], 'stats': stats(s['t'])} for s in p['streams']]}
                                             for p in c.get('processes', [])]}
    save()

    # Overhead on an uncapped presenter: presents in 8 s without and with a capture running (3 rounds).
    ov = {'without': [], 'with': [], 'captureOverhead': []}
    for r in range(3):
        for mode in ('without', 'with'):
            test = subprocess.Popen([BIN + 'GPUFusionFrameTest.exe', '--api', 'd3d11', '--seconds', '8'], stdout=subprocess.PIPE, text=True)
            cap = None
            if mode == 'with':
                cap, out = capture(10, [test.pid], f'ov{r}')
            truth = json.loads(test.communicate()[0])
            if cap:
                cap.wait()
                ov['captureOverhead'].append(json.load(open(out, encoding='utf-8')).get('overhead'))
            ov[mode].append(truth['presents'] / 8.0)
    results['overhead'] = ov
    save()

    # GPU Fusion end to end: --measure-frames on a running frame test (no prompt: already elevated).
    test = subprocess.Popen([BIN + 'GPUFusionFrameTest.exe', '--api', 'd3d11', '--interval-ms', '16.667', '--seconds', '14'], stdout=subprocess.PIPE, text=True)
    time.sleep(1)
    r = subprocess.run([BIN + 'GPUFusion.exe', '--data-dir', DATA, '--measure-frames', str(test.pid), '--frames-seconds', '8',
                        '--frames-label', 'validation'], capture_output=True, text=True)
    test.communicate()
    results['endToEnd'] = {'exitCode': r.returncode}
except Exception as e:  # keep what was measured
    results['errors'].append(repr(e))
results['finished'] = time.strftime('%Y-%m-%d %H:%M:%S')
save()
print('done')
