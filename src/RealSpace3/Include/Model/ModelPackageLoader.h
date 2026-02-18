#pragma once

#include <DirectXMath.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace RealSpace3 {

struct RS3ModelVertex {
    DirectX::XMFLOAT3 pos = { 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 normal = { 0.0f, 1.0f, 0.0f };
    DirectX::XMFLOAT2 uv = { 0.0f, 0.0f };
    uint16_t joints[4] = { 0, 0, 0, 0 };
    float weights[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
};

struct RS3ModelSubmesh {
    uint32_t materialIndex = 0;
    uint32_t nodeIndex = 0;
    uint32_t indexStart = 0;
    uint32_t indexCount = 0;
    DirectX::XMFLOAT4X4 nodeTransform = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
};

struct RS3Bone {
    std::string name;
    int32_t parentBone = -1;
    DirectX::XMFLOAT4X4 bind = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    DirectX::XMFLOAT4X4 invBind = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
};

struct RS3PosKey {
    float time = 0.0f;
    DirectX::XMFLOAT3 value = { 0.0f, 0.0f, 0.0f };
};

struct RS3RotKey {
    float time = 0.0f;
    DirectX::XMFLOAT4 value = { 0.0f, 0.0f, 0.0f, 1.0f };
};

struct RS3AnimationChannel {
    int32_t boneIndex = -1;
    std::vector<RS3PosKey> posKeys;
    std::vector<RS3RotKey> rotKeys;
};

struct RS3AnimationClip {
    std::string name;
    std::vector<RS3AnimationChannel> channels;
};

struct RS3Material {
    uint32_t legacyFlags = 0;
    uint32_t alphaMode = 0;
    float metallic = 0.0f;
    float roughness = 1.0f;

    std::string baseColorTexture;
    std::string normalTexture;
    std::string ormTexture;
    std::string emissiveTexture;
    std::string opacityTexture;
};

struct RS3AttachmentSocket {
    std::string name;
    int32_t nodeIndex = -1;
};

struct RS3ModelPackage {
    std::string modelId;
    std::string sourceGlb;
    std::filesystem::path baseDir;

    std::vector<RS3ModelVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<RS3ModelSubmesh> submeshes;

    std::vector<RS3Bone> bones;
    std::vector<RS3AnimationClip> clips;
    std::vector<RS3Material> materials;
    std::vector<RS3AttachmentSocket> sockets;
};

class ModelPackageLoader {
public:
    static bool LoadModelPackage(const std::string& modelId, RS3ModelPackage& outPackage, std::string* outError = nullptr);
};

} // namespace RealSpace3
