#pragma once

#include "RS3RenderTypes.h"

#include <DirectXMath.h>

#include <string>
#include <vector>

namespace RealSpace3 {

enum class RS3TimelineEase {
    Linear = 0,
    EaseInOutCubic = 1,
};

struct RS3TimelineKeyframe {
    float t = 0.0f;
    DirectX::XMFLOAT3 position = { 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 target = { 0.0f, 1.0f, 0.0f };
    float rollDeg = 0.0f;
    float fovDeg = 60.0f;
    RS3TimelineEase ease = RS3TimelineEase::Linear;
};

struct RS3TimelineAudio {
    bool enabled = false;
    std::string file;
    float offsetSec = 0.0f;
    float gainDb = 0.0f;
};

struct RS3TimelineData {
    std::string version = "ndg_cine_v1";
    std::string sceneId;
    RS3RenderMode mode = RS3RenderMode::MapOnlyCinematic;
    float durationSec = 0.0f;
    int fps = 60;
    std::vector<RS3TimelineKeyframe> keyframes;
    RS3TimelineAudio audio;
};

bool LoadTimelineFromFile(const std::string& timelinePath, RS3TimelineData& outTimeline, std::string* outError = nullptr);

} // namespace RealSpace3
