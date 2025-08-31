#ifndef COMMON_HLSLI_
#define COMMON_HLSLI_


// 0-4: +x -x +y -y +z
float3 viewDirFromFace(uint face, float2 uv)
{
    float2 face_pos = tan((uv - .5) * .5 * 3.1415926) * .5;
    float3 view_dir;
    switch (face) {
    case 0: // +x
        view_dir = float3(0.5, -face_pos);
        break;
    case 1: // -x
        view_dir = float3(-0.5, face_pos.x, -face_pos.y);
        break;
    case 2: // +y
        view_dir = float3(face_pos.x, 0.5, -face_pos.y);
        break;
    case 3: // -y
        view_dir = float3(-face_pos.x, -0.5, -face_pos.y);
        break;
    case 4: // +z
        view_dir = float3(-face_pos, 0.5);
        break;
    default:
        view_dir = float3(1, 0, 0);
        break;
    }
    return normalize(view_dir);
}

namespace Phase{
float HG(float cos_theta, float g)
{
    static const float scale = .25 / 3.1415926;
    const float g2 = g * g;

    float num = (1.0 - g2);
    float denom = pow(abs(1.0 + g2 - 2.0 * g * cos_theta), 1.5);

    return scale * num / denom;
}

float Draine(float cos_theta, float g, float alpha)
{
    static const float scale = .25 / 3.1415926;
    const float g2 = g * g;

    float num = (1.0 - g2) * (1.0 + alpha * cos_theta * cos_theta);
    float denom = pow(abs(1.0 + g2 - 2.0 * g * cos_theta), 1.5) * (1.0 + alpha * (1.0 + 2.0 * g2) / 3.0);

    return scale * num / denom;
}

float JendersieAt10um(float cos_theta)
{
    float g_hg = 0.9882,
          g_d = 0.5557,
          alpha = 21.9955,
          w_d = 0.4820;
    return lerp(HG(cos_theta, g_hg), Draine(cos_theta, g_d, alpha), w_d);
}

float MsHeuristic(float cos_theta, float tr)
{
    static const float scale = .25 / 3.1415926;
    static const float power = 0.5; // lower = less isotropic
    float w = 1 - pow(tr, power); // isotropic weight

    return lerp(JendersieAt10um(cos_theta), scale, w);
}
}

#endif