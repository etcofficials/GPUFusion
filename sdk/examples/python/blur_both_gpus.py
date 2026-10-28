# GPU Fusion SDK from Python (ctypes, standard library only): blurs an image on every GPU with the
# SDK's adaptive scheduler, checks the result against the CPU reference, and prints how the rows were
# divided. The same calls work from C#, Java (FFM), Rust, ... through GPUFusionSDK.dll.
#
#   python blur_both_gpus.py [path\to\GPUFusionSDK.dll] [runs]
import ctypes as C
import os
import sys
import time

dll_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'GPUFusionSDK.dll')
runs = int(sys.argv[2]) if len(sys.argv) > 2 else 10
sdk = C.CDLL(dll_path)

MAX_DEVICES = 8


class Adapter(C.Structure):
    _fields_ = [('name', C.c_char * 128), ('luid', C.c_char * 24), ('vendor_id', C.c_uint), ('feature_level', C.c_uint),
                ('compute_shaders', C.c_int), ('dedicated_mb', C.c_ulonglong)]


class Report(C.Structure):
    _fields_ = [('ok', C.c_int), ('multi', C.c_int), ('wall_ms', C.c_double), ('overhead_ms', C.c_double), ('devices', C.c_int),
                ('rows', C.c_uint * MAX_DEVICES), ('work_ms', C.c_double * MAX_DEVICES), ('transfer_ms', C.c_double * MAX_DEVICES),
                ('plan', C.c_char * 256)]


sdk.gf_create.restype = C.c_void_p
sdk.gf_create.argtypes = [C.c_int, C.c_int]
sdk.gf_destroy.argtypes = [C.c_void_p]
sdk.gf_last_error.restype = C.c_char_p
sdk.gf_last_error.argtypes = [C.c_void_p]
sdk.gf_adapter_count.argtypes = [C.c_void_p]
sdk.gf_adapter_info.argtypes = [C.c_void_p, C.c_int, C.POINTER(Adapter)]
sdk.gf_kernel_blur.argtypes = [C.c_void_p]
sdk.gf_run.argtypes = [C.c_void_p, C.c_int, C.c_char_p, C.c_size_t, C.c_char_p, C.c_size_t, C.c_uint, C.c_uint, C.POINTER(C.c_uint * 16),
                       C.POINTER(Report)]
sdk.gf_learned.restype = C.c_char_p
sdk.gf_learned.argtypes = [C.c_void_p, C.c_int]
sdk.gf_blur_reference.argtypes = [C.c_char_p, C.c_size_t, C.c_char_p, C.c_size_t, C.c_uint, C.c_uint, C.c_uint]

assert sdk.gf_abi_version() == 1, 'this example is written for ABI version 1'
gf = sdk.gf_create(1, 0)  # every hardware GPU, conservative policy
if not gf:
    sys.exit('no GPU can run the SDK kernels')
for i in range(sdk.gf_adapter_count(gf)):
    a = Adapter()
    sdk.gf_adapter_info(gf, i, C.byref(a))
    print(f'GPU {i}: {a.name.decode()} feature level {a.feature_level >> 12}_{(a.feature_level >> 8) & 15}, {a.dedicated_mb} MB')

# A deterministic 1920 x 1080 test image (RGBA).
w, h, radius = 1920, 1080, 6
img = bytearray(w * h * 4)
for y in range(h):
    row = y * w * 4
    for x in range(0, w, 1):
        o = row + x * 4
        img[o] = (x * 7 + y * 3) & 255
        img[o + 1] = (x ^ y) & 255
        img[o + 2] = (x * y >> 4) & 255
        img[o + 3] = 255
src = bytes(img)
ref = C.create_string_buffer(w * h * 4)
sdk.gf_blur_reference(src, w * 4, ref, w * 4, w, h, radius)

kernel = sdk.gf_kernel_blur(gf)
params = (C.c_uint * 16)(radius)
out = C.create_string_buffer(w * h * 4)
times = []
for k in range(runs):
    r = Report()
    t = time.perf_counter()
    if sdk.gf_run(gf, kernel, src, w * 4, out, w * 4, w, h, C.byref(params), C.byref(r)) != 0:
        sys.exit('run failed: ' + sdk.gf_last_error(gf).decode())
    times.append((time.perf_counter() - t) * 1000)
    assert out.raw == ref.raw, 'the GPU result differs from the CPU reference'
    print(f'run {k + 1}: {r.wall_ms:7.2f} ms, rows per GPU {list(r.rows[:r.devices])}, both GPUs: {bool(r.multi)} - {r.plan.decode()}')
print('every result equal to the CPU reference; median', sorted(times)[len(times) // 2], 'ms')
print('learned state (keep it to start with these shares next time):', sdk.gf_learned(gf, kernel).decode())
sdk.gf_destroy(gf)
