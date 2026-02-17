#pragma once
#include <directxmath.h>
#include <vector>
#include <string>
#include <memory>
#include <cstdint>

namespace RealSpace3 {

using rvector = DirectX::XMFLOAT3;
using rmatrix = DirectX::XMFLOAT4X4;
using rplane = DirectX::XMFLOAT4;
using rquaternion = DirectX::XMFLOAT4;

// Flags de Material (RS2 Original) - REINTRODUZIDAS
constexpr uint32_t RM_FLAG_USEOPACITY   = 0x01;
constexpr uint32_t RM_FLAG_USEALPHATEST = 0x02;
constexpr uint32_t RM_FLAG_ADDITIVE     = 0x04;
constexpr uint32_t RM_FLAG_TWOSIDED     = 0x08;
constexpr uint32_t RM_FLAG_HIDE         = 0x10;

struct rboundingbox {
    rvector vmin;
    rvector vmax;
};

struct rfrustum {
    rplane planes[6];
};

struct RVertex {
    DirectX::XMFLOAT3 Pos;
    DirectX::XMFLOAT2 Tex;
    DirectX::XMFLOAT3 Normal;
};

// Estrutura de vértice para Runtime (DX11)
struct BSPVERTEX {
    float x, y, z;      // Position
    float nx, ny, nz;   // Normal
    uint32_t color;     // Diffuse Color (RGBA)
    float tu1, tv1;     // Diffuse UV
    float tu2, tv2;     // Lightmap UV
};

struct RPOLYGONINFO {
    rplane plane;
    int nMaterial;
    int nConvexPolygon;
    int nLightmapTexture;
    int nPolygonID;
    unsigned int dwFlags;
    int nVertices;
    int nIndicesPos;
};

struct RDrawInfo {
    int nIndicesOffset = 0;
    int nTriangleCount = 0;
};

// Limites RM_Skin
constexpr uint32_t MAX_BONES = 128;
constexpr uint32_t MAX_INFLUENCES = 4;

// Estrutura de vértice para Skinning (DX11) - Otimizada
struct SkinVertex {
    float x, y, z;          // Position
    float nx, ny, nz;       // Normal
    uint32_t color;         // RGBA
    float tu, tv;           // UV
    float weights[4];       // Blend Weights
    uint8_t indices[4];     // Blend Indices (MAX_BONES=128 cabe em uint8)
};

struct Bone {
    std::string name;
    int32_t parentIdx;
    DirectX::XMMATRIX localMatrix;
    DirectX::XMMATRIX offsetMatrix; // Inverse Bind Pose
    DirectX::XMMATRIX combinedMatrix;
};

// RS3 Animation frames and bone animation
struct AniFrame {
    rvector position;
    rquaternion rotation;
};

struct BoneAni {
    std::string boneName;
    std::vector<AniFrame> frames;
};

// RS3 State structures (server-authoritative)
struct AnimationState {
    int32_t animationId;
    float time;
    float blendWeight;
};

struct Transform {
    rvector position;
    rquaternion rotation;
    rvector scale = {1.0f, 1.0f, 1.0f};
};

struct EntityState {
    uint64_t entityId;
    Transform transform;
    AnimationState animation;
    int32_t meshId;
};

// Shader Constant Buffers (Aligned for HLSL)
struct ConstantBuffer {
    DirectX::XMMATRIX WorldViewProj;
    DirectX::XMFLOAT4 FogColor;
    float FogNear;
    float FogFar;
    uint32_t DebugMode;
    float AlphaRef;
    float LightmapScale;
    float OutlineThickness;
    DirectX::XMFLOAT2 UVScroll;
    float _padCB0; 
};
static_assert((sizeof(ConstantBuffer) % 16) == 0, "CB alinhamento inválido para HLSL");

struct SkinningConstantBuffer {
    DirectX::XMMATRIX boneMatrices[MAX_BONES];
};
static_assert((sizeof(SkinningConstantBuffer) % 16) == 0, "Skinning CB alignment invalid");

}
