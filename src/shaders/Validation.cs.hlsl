#include "SH_Lite.hlsli"

cbuffer CBData : register(b0)
{
    float3 light_dir;
    float weight;
};

Texture2D<float3> TexSHCoeff1;
Texture2D<float3> TexSHCoeff2;
Texture2D<float3> TexSHCoeff3;
RWTexture2D<float> RWTexOut;

[numthreads(8, 8, 1)] 
void main(uint2 tid : SV_DispatchThreadID)
{
    float3 sh0 = TexSHCoeff1[tid.xy];
    float3 sh1 = TexSHCoeff2[tid.xy];
    float3 sh2 = TexSHCoeff3[tid.xy];
    
    SH::L2 sh;
    sh.C[0] = sh0.x;
    sh.C[1] = sh0.y;
    sh.C[2] = sh0.z;
    sh.C[3] = sh1.x;
    sh.C[4] = sh1.y;
    sh.C[5] = sh1.z;
    sh.C[6] = sh2.x;
    sh.C[7] = sh2.y;
    sh.C[8] = sh2.z;

    RWTexOut[tid.xy] = SH::Evaluate(sh, light_dir);
}