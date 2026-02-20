#pragma once

#include "CinematicTimeline.h"

#include <DirectXMath.h>

#include <algorithm>

namespace RealSpace3 {

class CinematicPlayer {
public:
    bool Play(const RS3TimelineData& timeline, const RS3TimelinePlaybackOptions& options, std::string* outError = nullptr);
    void Stop();
    void Pause(bool paused);
    void Seek(float timeSec);
    void Update(float deltaTimeSec);
    bool EvaluateCameraPose(RS3CameraPose& outPose) const;

    bool IsPlaying() const { return m_playing; }
    bool HasTimeline() const { return m_hasTimeline; }
    float GetCurrentTime() const { return m_currentTimeSec; }
    float GetDuration() const { return m_durationSec; }
    int GetFps() const { return std::max(1, m_timeline.fps); }
    const RS3TimelineData& GetTimeline() const { return m_timeline; }

private:
    static float ApplyEase(float t, RS3TimelineEase ease);
    static DirectX::XMFLOAT3 CatmullRom(
        const DirectX::XMFLOAT3& p0,
        const DirectX::XMFLOAT3& p1,
        const DirectX::XMFLOAT3& p2,
        const DirectX::XMFLOAT3& p3,
        float t);
    static DirectX::XMVECTOR BuildCameraQuaternion(const RS3TimelineKeyframe& keyframe);

private:
    RS3TimelineData m_timeline;
    RS3TimelinePlaybackOptions m_options;
    bool m_hasTimeline = false;
    bool m_playing = false;
    bool m_paused = false;
    float m_currentTimeSec = 0.0f;
    float m_durationSec = 0.0f;
    float m_startTimeSec = 0.0f;
    float m_endTimeSec = 0.0f;
};

} // namespace RealSpace3
