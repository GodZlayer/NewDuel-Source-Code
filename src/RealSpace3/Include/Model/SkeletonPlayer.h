#pragma once

#include "ModelPackageLoader.h"

#include <string>

namespace RealSpace3 {

class SkeletonPlayer {
public:
    void SetPackage(const RS3ModelPackage* package);
    bool SetAnimationClipByName(const std::string& clipName, float blendSeconds);
    const RS3AnimationClip* GetCurrentClip() const;
    float GetBlendSeconds() const;
    void Update(float deltaSeconds);
    bool BuildSkinMatrices(std::vector<DirectX::XMFLOAT4X4>& outMatrices) const;
    float GetCurrentTimeSeconds() const;

private:
    const RS3ModelPackage* m_package = nullptr;
    int32_t m_clipIndex = -1;
    float m_blendSeconds = 0.0f;
    float m_timeSeconds = 0.0f;
    float m_clipDuration = 0.0f;
    mutable bool m_parentOrderResolved = false;
    mutable bool m_localFirstOrder = true;
    mutable bool m_loggedOrderDiagnostics = false;
    mutable bool m_loggedDecomposeWarning = false;
    mutable bool m_loggedSkinFallbackWarning = false;
};

} // namespace RealSpace3
