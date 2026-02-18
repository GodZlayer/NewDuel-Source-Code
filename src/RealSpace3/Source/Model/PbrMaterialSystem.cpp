#include "../../Include/Model/PbrMaterialSystem.h"

namespace RealSpace3 {

PbrMaterialGpuParams PbrMaterialSystem::BuildParams(const RS3Material& material) {
    PbrMaterialGpuParams out;

    out.metallic = material.metallic;
    out.roughness = material.roughness;
    out.alphaMode = material.alphaMode;
    out.legacyFlags = material.legacyFlags;

    if (material.alphaMode == 1) {
        out.alphaCutoff = 0.5f;
    } else {
        out.alphaCutoff = 0.0f;
    }

    return out;
}

} // namespace RealSpace3
