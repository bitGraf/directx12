//***************************************************************************************
// color.hlsl by Frank Luna (C) 2015 All Rights Reserved.
//
// Transforms and colors geometry.
//***************************************************************************************

// #define BATCH_AMOUNT 1024 // <- set outside shader

cbuffer cbConstants : register(b0) {
    uint batch_idx;
};

cbuffer cbPerFrame : register(b1) {
    float4x4 r_Projection;
    float4x4 r_View; 
};

cbuffer cbPerObject : register(b2) {
    float4x4 r_Model[BATCH_AMOUNT]; 
};

struct VertexIn
{
    float3 PosL  : POSITION;
    float4 Color : COLOR;
};

struct VertexOut
{
    float4 PosH  : SV_POSITION;
    float4 Color : COLOR;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;

    // Transform to homogeneous clip space.
    vout.PosH = mul(mul(mul(r_Projection, r_View), r_Model[batch_idx]), float4(vin.PosL, 1.0f));

    // Just pass vertex color into the pixel shader.
    vout.Color = vin.Color;

    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    return pin.Color;
}


