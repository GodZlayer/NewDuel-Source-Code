#pragma once

#include "ModelPackageLoader.h"
#include "SkeletonPlayer.h"

#include <string>
#include <vector>

namespace RealSpace3 {

struct CharacterVisualRequest {
    std::string baseModelId;
    std::vector<std::string> partModelIds;
    std::vector<std::string> weaponModelIds;
    std::string initialClip;
};

struct CharacterVisualInstance {
    std::vector<RS3ModelPackage> packages;
    SkeletonPlayer animation;
    bool valid = false;
};

class CharacterAssembler {
public:
    bool BuildCharacterVisual(const CharacterVisualRequest& request, CharacterVisualInstance& outInstance, std::string* outError = nullptr);
};

} // namespace RealSpace3
