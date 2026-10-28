/* GPU Fusion SDK — C interface (GPUFusionSDK.dll). MIT License.
 *
 * The same engine as the C++ headers (adapters, row kernels, the adaptive scheduler), behind a plain C
 * ABI, so that programs in other languages can use it: Unity and other engines' native plugins, C#
 * (P/Invoke), Java (Foreign Function & Memory API, JDK 22+), Python (ctypes), Rust, Go...
 *
 * A context holds one Direct3D 11 device per GPU. A kernel is an SM 4.0 pixel shader following the
 * row-kernel convention of gpufusion/rows.h (the SDK ships one: a tent blur, gf_kernel_blur). Each run
 * divides an image's rows between the GPUs: the primary GPU (the discrete one) does most of the work,
 * others help only as far as it measurably pays off, failed helper batches are re-run on the primary,
 * and every row is computed exactly once. Nothing here makes an unmodified program use two GPUs: a
 * program calls this library for work it chooses to hand over.
 *
 *     gf_context* gf = gf_create(GF_ADAPTERS_ALL, GF_POLICY_CONSERVATIVE);
 *     int k = gf_kernel_blur(gf);
 *     unsigned params[16] = { 6 };              // radius
 *     gf_report r;
 *     if (gf_run(gf, k, in, w * 4, out, w * 4, w, h, params, &r) != 0) puts(gf_last_error(gf));
 *     gf_destroy(gf);
 *
 * Thread safety: one thread at a time per context. Errors: functions return a negative value (or
 * NULL) and gf_last_error() says why.
 */
#ifndef GPUFUSION_C_H
#define GPUFUSION_C_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(GPUFUSION_SDK_BUILD)
#define GF_API __declspec(dllexport)
#else
#define GF_API __declspec(dllimport)
#endif

#define GF_ABI_VERSION 1          /* this header; gf_abi_version() must match */
#define GF_MAX_DEVICES 8

enum { GF_ADAPTERS_OFFERED = 0,   /* the GPUs GPU Fusion offered (GPUFUSION_ADAPTERS), else the default one */
       GF_ADAPTERS_ALL = 1 };     /* every hardware GPU, the one with the most dedicated memory first */
enum { GF_POLICY_CONSERVATIVE = 0, GF_POLICY_BALANCED = 1, GF_POLICY_PERFORMANCE = 2 };

typedef struct gf_context gf_context;

typedef struct gf_adapter {
    char name[128];               /* UTF-8 */
    char luid[24];                /* "HHHHHHHH_LLLLLLLL" */
    unsigned vendor_id;
    unsigned feature_level;       /* e.g. 0xb000 for 11_0, 0xa100 for 10_1 */
    int compute_shaders;          /* 0/1 */
    unsigned long long dedicated_mb;
} gf_adapter;

typedef struct gf_report {
    int ok;                       /* every row computed exactly once */
    int multi;                    /* more than one GPU took part */
    double wall_ms;               /* the whole run, transfers included */
    double overhead_ms;           /* time on the critical path that was not work */
    int devices;
    unsigned rows[GF_MAX_DEVICES];        /* rows each GPU computed (re-runs included) */
    double work_ms[GF_MAX_DEVICES];       /* time in each GPU's batches */
    double transfer_ms[GF_MAX_DEVICES];   /* upload + readback (GPU timestamps) + mapping */
    char plan[256];               /* why the work was divided this way */
} gf_report;

GF_API int gf_abi_version(void);
GF_API int gf_sdk_version(void);

GF_API gf_context* gf_create(int adapters, int policy);
GF_API void gf_destroy(gf_context* gf);
GF_API const char* gf_last_error(gf_context* gf);   /* "" if none; valid until the next call */

GF_API int gf_adapter_count(gf_context* gf);
GF_API int gf_adapter_info(gf_context* gf, int index, gf_adapter* out);

/* A kernel from compiled shader bytecode (fxc /T ps_4_0). halo: input rows needed above and below
 * each output row. Returns the kernel id (>= 0). */
GF_API int gf_kernel_create(gf_context* gf, const void* bytecode, size_t size, unsigned halo);
/* The SDK's tent blur (params[0] = radius 0..15). */
GF_API int gf_kernel_blur(gf_context* gf);

/* Runs a kernel over a width x height RGBA8 image. Returns 0 on success. */
GF_API int gf_run(gf_context* gf, int kernel, const unsigned char* in, size_t in_pitch, unsigned char* out, size_t out_pitch, unsigned width,
                  unsigned height, const unsigned params[16], gf_report* report);

/* What the kernel's scheduler has learned, to keep between program runs (NUL-terminated text, valid
 * until the next call), and to restore it. gf_restore returns 0 if the text was accepted. */
GF_API const char* gf_learned(gf_context* gf, int kernel);
GF_API int gf_restore(gf_context* gf, int kernel, const char* learned);

/* The CPU reference of gf_kernel_blur, for checking results. */
GF_API void gf_blur_reference(const unsigned char* in, size_t in_pitch, unsigned char* out, size_t out_pitch, unsigned width, unsigned height,
                              unsigned radius);

#ifdef __cplusplus
}
#endif

#endif /* GPUFUSION_C_H */
