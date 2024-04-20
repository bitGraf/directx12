//***************************************************************************************
// color.hlsl by Frank Luna (C) 2015 All Rights Reserved.
//
// Transforms and colors geometry.
//***************************************************************************************

// #define BATCH_AMOUNT 1024 // <- set outside shader

cbuffer cbConstants : register(b0) {
    uint  batch_idx;
    uint  tex_diffuse;
    float alpha;
    float tex_scale;
};

cbuffer cbPerFrame : register(b1) {
    float4x4 r_Projection;
    float4x4 r_View; 
    float4   r_FogColor;
    float    r_FogStart;
    float    r_FogEnd;
};

cbuffer cbPerObject : register(b2) {
    float4x4 r_Model[BATCH_AMOUNT]; 
};

Texture2D    tex_map : register(t0);
SamplerState sam     : register(s0);

struct VertexIn
{
    float3 PosL     : POSITION;
    float3 Color    : COLOR;
    float2 TexCoord : TEXCOORD;
};

struct VertexOut
{
    float4 PosH     : SV_POSITION;
    float4 Color    : COLOR;
    float2 TexCoord : TEXCOORD;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;

    // Transform to homogeneous clip space.
    vout.PosH = mul(mul(mul(r_Projection, r_View), r_Model[batch_idx]), float4(vin.PosL, 1.0f));

    // Just pass vertex color into the pixel shader.
    vout.Color = float4(vin.Color, 1.0f);

    vout.TexCoord = vin.TexCoord * tex_scale;

    return vout;
}

float sRGB(float x) {
    if (x <= 0.0031308) {
        return 12.92 * x;
    } else {
        return 1.055*pow(x,(1.0 / 2.4) ) - 0.055;
    }
}

float4 sRGB_float4(float4 v) {
    // alpha is already linear?
    return float4(
        sRGB(v.r),
        sRGB(v.g),
        sRGB(v.b),
        v.a
    );
}

float4 PS(VertexOut pin) : SV_Target
{
    float4 tex_color = tex_map.Sample(sam, pin.TexCoord);
    clip(tex_color.a - 0.5f);
    tex_color = sRGB_float4(tex_color);
    //return pin.Color * tex_color;
    //return pin.Color;
    float4 mix_color = (pin.Color * (1 - tex_diffuse)) + (tex_color * tex_diffuse);

    return float4(mix_color.rgb, mix_color.a*alpha);
}


