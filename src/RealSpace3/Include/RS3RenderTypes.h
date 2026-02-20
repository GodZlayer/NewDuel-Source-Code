#pragma once

#include <DirectXMath.h>

#include <string>

namespace RealSpace3 {

enum class RS3RenderMode {
    MapOnlyCinematic = 0,
    ShowcaseOnly = 1,
    Gameplay = 2,
};

struct RS3CameraPose {
    DirectX::XMFLOAT3 position = { 0.0f, -800.0f, 220.0f };
    DirectX::XMFLOAT3 target = { 0.0f, 0.0f, 140.0f };
    DirectX::XMFLOAT3 up = { 0.0f, 0.0f, 1.0f };
    float fovDeg = 60.0f;
    float nearZ = 1.0f;
    float farZ = 20000.0f;
};

struct RS3TimelinePlaybackOptions {
    bool loop = false;
    float speed = 1.0f;
    float startTimeSec = 0.0f;
    float endTimeSec = -1.0f;
};

inline const char* ToRenderModeString(RS3RenderMode mode) {
    switch (mode) {
    case RS3RenderMode::MapOnlyCinematic: return "map_only";
    case RS3RenderMode::ShowcaseOnly: return "showcase_only";
    case RS3RenderMode::Gameplay: return "gameplay";
    default: return "gameplay";
    }
}

inline bool ParseRenderModeString(const std::string& value, RS3RenderMode& outMode) {
    if (value == "map_only" || value == "MapOnlyCinematic" || value == "map") {
        outMode = RS3RenderMode::MapOnlyCinematic;
        return true;
    }
    if (value == "showcase_only" || value == "ShowcaseOnly" || value == "showcase") {
        outMode = RS3RenderMode::ShowcaseOnly;
        return true;
    }
    if (value == "gameplay" || value == "Gameplay") {
        outMode = RS3RenderMode::Gameplay;
        return true;
    }
    return false;
}

} // namespace RealSpace3
