#include "SH_Lite.hlsli"

cbuffer CBData : register(b0)
{
    float3 light_dir;
    float weight;
};

Texture2D<float> TexRadiance;
RWTexture2D<float3> RWTexSHCoeff1;
RWTexture2D<float3> RWTexSHCoeff2;
RWTexture2D<float3> RWTexSHCoeff3;

[numthreads(8, 8, 1)] 
void main(uint2 tid : SV_DispatchThreadID)
{
    float color = TexRadiance[tid];
    SH::L2 sh = SH::ProjectOntoL2(light_dir, color * weight);

    RWTexSHCoeff1[tid.xy] += float3(sh.C[0], sh.C[1], sh.C[2]);
    RWTexSHCoeff2[tid.xy] += float3(sh.C[3], sh.C[4], sh.C[5]);
    RWTexSHCoeff3[tid.xy] += float3(sh.C[6], sh.C[7], sh.C[8]);
}