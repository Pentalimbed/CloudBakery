#include "Common.hlsli"
#include "SH_Lite.hlsli"

cbuffer CBData : register(b0)
{
    float3 light_dir;
    float weight;
    uint face;
    float3 _pad;
};

Texture2D<float> TexRadiance : register(t0);
Texture2D<float> TexTr : register(t1);
RWTexture2DArray<float3> RWTexSHCoeffs : register(u0);

[numthreads(8, 8, 1)] 
void main(uint2 tid : SV_DispatchThreadID)
{
    float color = TexRadiance[tid];

    uint2 dims;
    TexTr.GetDimensions(dims.x, dims.y);
    float2 uv = (tid.xy + .5) / dims;
    float3 view_dir = viewDirFromFace(face, uv);
    float u = dot(-view_dir, light_dir);
    float phase = Phase::MsHeuristic(u, TexTr[tid.xy]);

    color /= phase;

    SH::L2 sh = SH::ProjectOntoL2(light_dir, color * weight * 4 * 3.1415926);

    RWTexSHCoeffs[uint3(tid.xy, 0)] += float3(sh.C[0], sh.C[1], sh.C[2]);
    RWTexSHCoeffs[uint3(tid.xy, 1)] += float3(sh.C[3], sh.C[4], sh.C[5]);
    RWTexSHCoeffs[uint3(tid.xy, 2)] += float3(sh.C[6], sh.C[7], sh.C[8]);
}