// Integer hash workload — must stay identical to gf::work::hashPixel (HashKernel.h).
// Shader model 4.0 (FL 10_0+), integer render target (R32_UINT), so the result is bit-exact.
cbuffer Params : register(b0)
{
    uint iterations;
    uint seed;
    uint originY;   // first row of this band in the full image
    uint pad;
};

uint hashInit(uint x, uint y, uint s0)
{
    uint s = (x * 73856093u) ^ (y * 19349663u) ^ (s0 * 83492791u);
    return s != 0u ? s : 0x9E3779B9u;
}

uint main(float4 pos : SV_Position) : SV_Target
{
    uint x = (uint)pos.x;
    uint y = (uint)pos.y + originY;
    uint s = hashInit(x, y, seed);
    [loop]
    for (uint i = 0; i < iterations; ++i) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        s *= 0x2545F491u;
    }
    return s;
}
