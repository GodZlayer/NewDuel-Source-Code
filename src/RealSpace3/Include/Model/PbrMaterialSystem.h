#pragma once

#include "ModelPackageLoader.h"

#include <DirectXMath.h>

namespace RealSpace3 {

struct PbrMaterialGpuParams {
    DirectX::XMFLOAT4 baseColorFactor = { 1.0f, 1.0f, 1.0f, 1.0f };
    float metallic = 0.0f;
    float roughness = 1.0f;
    float alphaCutoff = 0.5f;
    uint32_t alphaMode = 0;
    uint32_t legacyFlags = 0;
};

class PbrMaterialSystem {
public:
    static PbrMaterialGpuParams BuildParams(const RS3Material& material);
};

} // namespace RealSpace3
