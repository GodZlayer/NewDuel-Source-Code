cbuffer ConstantBuffer : register(b0)
{
    matrix WorldViewProj;
    float4 FogColor;
    float FogNear;
    float FogFar;
    uint DebugMode;
    float AlphaRef;
    float LightmapScale;
    float OutlineThickness;
    float2 UVScroll;
    float _pad;
};

// Bone matrix buffer for characters
cbuffer SkinningConstantBuffer : register(b1)
{
    matrix BoneMatrices[128];
};

Texture2D texDiffuse : register(t0);
SamplerState samLinear : register(s0);

struct VS_INPUT
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float4 color : COLOR;
    float2 tex0 : TEXCOORD0;
    float2 tex1 : TEXCOORD1;
};

struct VS_SKIN_INPUT
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float4 color : COLOR;
    float2 tex0 : TEXCOORD0;
    float4 blendWeights : BLENDWEIGHT;
    uint4  blendIndices : BLENDINDICES;
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float3 normal : NORMAL;
    float4 color : COLOR;
    float2 tex0 : TEXCOORD0;
};

PS_INPUT VS_Main(VS_INPUT input)
{
    PS_INPUT output;
    output.pos = mul(float4(input.pos, 1.0f), WorldViewProj);
    output.color = input.color;
    output.tex0 = input.tex0;
    output.normal = input.normal;
    return output;
}

PS_INPUT VS_Skin(VS_SKIN_INPUT input)
{
    float4 pos = float4(0, 0, 0, 1);
    float3 normal = float3(0, 0, 0);

    for (int i = 0; i < 4; i++)
    {
        uint idx = input.blendIndices[i];
        float weight = input.blendWeights[i];
        
        pos.xyz += mul(float4(input.pos, 1.0f), BoneMatrices[idx]).xyz * weight;
        normal += mul(input.normal, (float3x3)BoneMatrices[idx]) * weight;
    }

    PS_INPUT output;
    output.pos = mul(float4(pos.xyz, 1.0f), WorldViewProj);
    output.color = input.color;
    output.tex0 = input.tex0;
    output.normal = normalize(normal);
    return output;
}

float4 PS_Main(PS_INPUT input) : SV_Target
{
    float4 texColor = texDiffuse.Sample(samLinear, input.tex0);
    
    // Alpha Test
    if (texColor.a < AlphaRef) discard;
    
    // Simple Lighting
    float3 lightDir = normalize(float3(1, 1, -1));
    float diff = max(dot(input.normal, lightDir), 0.4f);
    
    return texColor * float4(diff, diff, diff, 1.0f);
}
