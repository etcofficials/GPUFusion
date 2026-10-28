# Does other GPU work slow a GPU-bound game on the GT 730, and does moving that work to the Intel GPU
# remove the cost? (docs/GAME_MULTI_GPU_FEASIBILITY.md, experiment E1)
#
# Stand-in game: GPUFusionSplitDemo on the default adapter (the GT 730 here), hash image at standard
# size, median time per image over 20 runs, output checked. Background load, one at a time:
#   - a continuously rendering 3D program (GPUFusionProbe --render) on the GT 730 or on the Intel GPU,
#   - continuous H.264 decoding (GPU Fusion's job dispatcher) on the GT 730 or on the Intel GPU.
# Conditions alternate in each round. Changes no Windows setting. Needs a release build and the test
# clips that `GPUFusion.exe --run-lab decode` creates in <data>\clips.
#
#   python scripts/experiments/game_contention.py [rounds]
import json, statistics, subprocess, sys, threading, time

BIN = 'G:/GPUFusion/build/release/bin/'
DATA = 'G:/GPUFusion/build/release/bench-data'
ROUNDS = int(sys.argv[1]) if len(sys.argv) > 1 else 3

# LUIDs change at every restart: read them from the probe.
enum = json.loads(subprocess.run([BIN + 'GPUFusionProbe.exe'], capture_output=True, text=True).stdout)['enumOrder']
GT = next(a['luid'] for a in enum if a['vendorId'] == 0x10DE)
INTEL = next(a['luid'] for a in enum if a['vendorId'] == 0x8086)


def game():
    out = subprocess.run([BIN + 'GPUFusionSplitDemo.exe', '--size', 'standard', '--runs', '20'], capture_output=True, text=True)
    j = json.loads(out.stdout)
    return j['single'][0]['wallMs']['median'], j['allOutputCorrect']


def decode(gpus, stop):
    while not stop.is_set():
        subprocess.run([BIN + 'GPUFusion.exe', '--data-dir', DATA, '--dispatch', 'decode', '--dispatch-inputs', DATA + '/clips',
                        '--dispatch-gpus', gpus], capture_output=True)


conds = ['none', '3D app on GT 730', '3D app on Intel', 'decode on GT 730', 'decode on Intel']
res = {c: [] for c in conds}
for rnd in range(ROUNDS):
    for c in conds:
        bg, stop, t = None, threading.Event(), None
        if c.startswith('3D'):
            bg = subprocess.Popen([BIN + 'GPUFusionProbe.exe', '--render', '25', '--adapter', GT if 'GT' in c else INTEL],
                                  stdout=subprocess.DEVNULL)
        elif c.startswith('decode'):
            t = threading.Thread(target=decode, args=('discrete' if 'GT' in c else 'integrated', stop))
            t.start()
        if c != 'none':
            time.sleep(2)  # the background load is running before the game starts
        ms, ok = game()
        if bg:
            bg.kill()
            bg.wait()
        if t:
            stop.set()
            t.join()
        res[c].append(ms)
        print(f'round {rnd + 1} {c:17s} game {ms:7.2f} ms per image (correct {ok})', flush=True)
base = statistics.median(res['none'])
for c, v in res.items():
    print(f'{c:17s} median {statistics.median(v):7.2f} ms ({100 * (statistics.median(v) / base - 1):+.1f} % time) runs {[round(x, 2) for x in v]}')
