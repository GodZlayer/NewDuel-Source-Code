#pragma once

#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <vector>

namespace RealSpace3 {

struct ScenePackageVertex {
    DirectX::XMFLOAT3 pos;
    DirectX::XMFLOAT3 normal;
    DirectX::XMFLOAT2 uv;
};

struct ScenePackageMaterial {
    uint32_t flags = 0;
    std::string diffuseMap;
};

struct ScenePackageLight {
    DirectX::XMFLOAT3 position = { 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 color = { 1.0f, 1.0f, 1.0f };
    float intensity = 1.0f;
    float attenuationStart = 0.0f;
    float attenuationEnd = 1000.0f;
};

struct ScenePackageSection {
    uint32_t materialIndex = 0;
    uint32_t indexStart = 0;
    uint32_t indexCount = 0;
};

struct ScenePackageCollisionNode {
    DirectX::XMFLOAT4 plane = { 0.0f, 0.0f, 1.0f, 0.0f };
    bool solid = false;
    int32_t posChild = -1;
    int32_t negChild = -1;
};

struct ScenePackageCollision {
    int32_t rootIndex = -1;
    std::vector<ScenePackageCollisionNode> nodes;
};

struct ScenePackageData {
    std::string sceneId;
    std::string baseDir;

    DirectX::XMFLOAT3 cameraPos01 = { 0.0f, -800.0f, 220.0f };
    DirectX::XMFLOAT3 cameraDir01 = { 0.0f, 1.0f, -0.2f };
    DirectX::XMFLOAT3 cameraPos02 = { 0.0f, -800.0f, 220.0f };
    DirectX::XMFLOAT3 cameraDir02 = { 0.0f, 1.0f, -0.2f };

    DirectX::XMFLOAT3 spawnPos = { 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 spawnDir = { 0.0f, 1.0f, 0.0f };

    float fogMin = 1000.0f;
    float fogMax = 7000.0f;
    DirectX::XMFLOAT3 fogColor = { 1.0f, 1.0f, 1.0f };
    bool fogEnabled = false;

    DirectX::XMFLOAT3 boundsMin = { 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 boundsMax = { 0.0f, 0.0f, 0.0f };

    std::vector<ScenePackageMaterial> materials;
    std::vector<ScenePackageLight> lights;
    std::vector<ScenePackageSection> sections;
    std::vector<ScenePackageVertex> vertices;
    std::vector<uint32_t> indices;

    ScenePackageCollision collision;

    bool hasCamera01 = false;
    bool hasCamera02 = false;
    bool hasSpawn = false;
};

class ScenePackageLoader {
public:
    static bool Load(const std::string& sceneId, ScenePackageData& outData, std::string* outError = nullptr);
};

} // namespace RealSpace3
