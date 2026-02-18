#include "../../Include/Model/CharacterAssembler.h"

namespace RealSpace3 {

namespace {

void SetError(std::string* outError, const std::string& msg) {
    if (outError) {
        *outError = msg;
    }
}

bool LoadPackage(const std::string& modelId, CharacterVisualInstance& outInstance, std::string* outError) {
    RS3ModelPackage pkg;
    std::string err;
    if (!ModelPackageLoader::LoadModelPackage(modelId, pkg, &err)) {
        SetError(outError, "LoadModelPackage failed for '" + modelId + "': " + err);
        return false;
    }

    outInstance.packages.push_back(std::move(pkg));
    return true;
}

} // namespace

bool CharacterAssembler::BuildCharacterVisual(const CharacterVisualRequest& request, CharacterVisualInstance& outInstance, std::string* outError) {
    outInstance.packages.clear();
    outInstance.animation.SetPackage(nullptr);
    outInstance.valid = false;

    if (request.baseModelId.empty()) {
        SetError(outError, "CharacterVisualRequest.baseModelId is empty.");
        return false;
    }

    if (!LoadPackage(request.baseModelId, outInstance, outError)) {
        return false;
    }

    for (const auto& partId : request.partModelIds) {
        if (partId.empty()) continue;
        if (!LoadPackage(partId, outInstance, outError)) {
            return false;
        }
    }

    for (const auto& weaponId : request.weaponModelIds) {
        if (weaponId.empty()) continue;
        if (!LoadPackage(weaponId, outInstance, outError)) {
            return false;
        }
    }

    outInstance.animation.SetPackage(&outInstance.packages.front());

    if (!request.initialClip.empty()) {
        (void)outInstance.animation.SetAnimationClipByName(request.initialClip, 0.15f);
    }

    outInstance.valid = true;
    return true;
}

} // namespace RealSpace3
