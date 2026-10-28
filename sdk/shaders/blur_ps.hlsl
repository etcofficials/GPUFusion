// GPU Fusion SDK sample kernel: tent (triangle) blur of radius R over an RGBA8 image, in exact integer
// arithmetic, so the GPU's result equals the CPU reference (gpufusion::blurReference) bit for bit.
// Shader model 4.0 (feature level 10_0+): runs on the HD Graphics 2000 as well as the GT 730.
//
// Row-kernel convention (see gpufusion/rows.h):
//   t0  the input rows [rows.y, rows.y + texture height) of the whole image, DXGI_FORMAT_R8G8B8A8_UINT
//   b0  rows   = { first output row of this batch, rows.y = first input row in t0, width, height }
//       params = 4 × uint4 of the kernel's own parameters; here params[0].x = radius (0..15)
//   SV_Target: the output pixel of row (rows.x + SV_Position.y)
Texture2D<uint4> src : register(t0);
cbuffer Rows : register(b0)
{
    uint4 rows;
    uint4 params[4];
};

uint4 main(float4 pos : SV_Position) : SV_Target
{
    const int x = (int)pos.x;
    const int y = (int)pos.y + (int)rows.x;
    const int w = (int)rows.z;
    const int h = (int)rows.w;
    const int r = (int)min(params[0].x, 15u);
    uint4 sum = uint4(0, 0, 0, 0);
    uint total = 0;
    [loop]
    for (int dy = -r; dy <= r; ++dy) {
        const int yy = clamp(y + dy, 0, h - 1) - (int)rows.y;
        const uint wy = (uint)(r + 1 - abs(dy));
        [loop]
        for (int dx = -r; dx <= r; ++dx) {
            const uint wgt = wy * (uint)(r + 1 - abs(dx));
            sum += src.Load(int3(clamp(x + dx, 0, w - 1), yy, 0)) * wgt;
            total += wgt;
        }
    }
    return (sum + total / 2) / total;
}
