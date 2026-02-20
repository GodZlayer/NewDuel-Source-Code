#include "../Include/CinematicPlayer.h"

#include <algorithm>
#include <cmath>

namespace RealSpace3 {
namespace {

constexpr float kEpsilon = 1e-6f;

float Clamp01(float t) {
    return std::max(0.0f, std::min(1.0f, t));
}

float Length(const DirectX::XMFLOAT3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

DirectX::XMFLOAT3 Subtract(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b) {
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}

DirectX::XMFLOAT3 Lerp(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, float t) {
    return {
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t
    };
}

} // namespace

bool CinematicPlayer::Play(const RS3TimelineData& timeline, const RS3TimelinePlaybackOptions& options, std::string* outError) {
    if (timeline.keyframes.empty()) {
        if (outError) *outError = "Timeline has no camera keyframes.";
        return false;
    }

    m_timeline = timeline;
    std::sort(m_timeline.keyframes.begin(), m_timeline.keyframes.end(), [](const RS3TimelineKeyframe& a, const RS3TimelineKeyframe& b) {
        return a.t < b.t;
    });

    m_options = options;
    m_durationSec = std::max(0.0f, m_timeline.durationSec);
    if (m_durationSec <= 0.0f) {
        m_durationSec = std::max(0.0f, m_timeline.keyframes.back().t);
    }

    m_startTimeSec = std::max(0.0f, std::min(options.startTimeSec, m_durationSec));
    m_endTimeSec = options.endTimeSec > 0.0f ? std::min(options.endTimeSec, m_durationSec) : m_durationSec;
    if (m_endTimeSec <= m_startTimeSec) {
        m_endTimeSec = m_durationSec;
    }
    if (m_endTimeSec <= m_startTimeSec) {
        if (outError) *outError = "Timeline playback range is invalid.";
        return false;
    }

    m_currentTimeSec = m_startTimeSec;
    m_hasTimeline = true;
    m_playing = true;
    m_paused = false;
    return true;
}

void CinematicPlayer::Stop() {
    m_playing = false;
    m_paused = false;
    m_hasTimeline = false;
    m_currentTimeSec = 0.0f;
    m_durationSec = 0.0f;
    m_startTimeSec = 0.0f;
    m_endTimeSec = 0.0f;
    m_timeline = RS3TimelineData{};
}

void CinematicPlayer::Pause(bool paused) {
    m_paused = paused;
}

void CinematicPlayer::Seek(float timeSec) {
    if (!m_hasTimeline) return;
    m_currentTimeSec = std::max(m_startTimeSec, std::min(timeSec, m_endTimeSec));
}

void CinematicPlayer::Update(float deltaTimeSec) {
    if (!m_hasTimeline || !m_playing || m_paused) return;
    if (deltaTimeSec <= 0.0f) return;

    const float speed = std::max(0.0f, m_options.speed);
    m_currentTimeSec += deltaTimeSec * speed;

    const float range = std::max(kEpsilon, m_endTimeSec - m_startTimeSec);
    if (m_options.loop) {
        while (m_currentTimeSec > m_endTimeSec) {
            m_currentTimeSec -= range;
        }
        while (m_currentTimeSec < m_startTimeSec) {
            m_currentTimeSec += range;
        }
    } else {
        if (m_currentTimeSec >= m_endTimeSec) {
            m_currentTimeSec = m_endTimeSec;
            m_playing = false;
        }
    }
}

float CinematicPlayer::ApplyEase(float t, RS3TimelineEase ease) {
    const float x = Clamp01(t);
    if (ease == RS3TimelineEase::EaseInOutCubic) {
        if (x < 0.5f) {
            return 4.0f * x * x * x;
        }
        const float n = -2.0f * x + 2.0f;
        return 1.0f - (n * n * n) * 0.5f;
    }
    return x;
}

DirectX::XMFLOAT3 CinematicPlayer::CatmullRom(
    const DirectX::XMFLOAT3& p0,
    const DirectX::XMFLOAT3& p1,
    const DirectX::XMFLOAT3& p2,
    const DirectX::XMFLOAT3& p3,
    float t) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    return {
        0.5f * ((2.0f * p1.x) + (-p0.x + p2.x) * t + (2.0f * p0.x - 5.0f * p1.x + 4.0f * p2.x - p3.x) * t2 + (-p0.x + 3.0f * p1.x - 3.0f * p2.x + p3.x) * t3),
        0.5f * ((2.0f * p1.y) + (-p0.y + p2.y) * t + (2.0f * p0.y - 5.0f * p1.y + 4.0f * p2.y - p3.y) * t2 + (-p0.y + 3.0f * p1.y - 3.0f * p2.y + p3.y) * t3),
        0.5f * ((2.0f * p1.z) + (-p0.z + p2.z) * t + (2.0f * p0.z - 5.0f * p1.z + 4.0f * p2.z - p3.z) * t2 + (-p0.z + 3.0f * p1.z - 3.0f * p2.z + p3.z) * t3)
    };
}

DirectX::XMVECTOR CinematicPlayer::BuildCameraQuaternion(const RS3TimelineKeyframe& keyframe) {
    const DirectX::XMVECTOR baseForward = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    const DirectX::XMVECTOR baseUp = DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);

    const DirectX::XMFLOAT3 dir = Subtract(keyframe.target, keyframe.position);
    DirectX::XMVECTOR forward = DirectX::XMVectorSet(dir.x, dir.y, dir.z, 0.0f);
    if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(forward)) < kEpsilon) {
        forward = baseForward;
    } else {
        forward = DirectX::XMVector3Normalize(forward);
    }

    DirectX::XMVECTOR axis = DirectX::XMVector3Cross(baseForward, forward);
    float axisLenSq = DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(axis));
    const float dot = std::max(-1.0f, std::min(1.0f, DirectX::XMVectorGetX(DirectX::XMVector3Dot(baseForward, forward))));

    DirectX::XMVECTOR qDir = DirectX::XMQuaternionIdentity();
    if (axisLenSq > kEpsilon) {
        axis = DirectX::XMVector3Normalize(axis);
        const float angle = std::acos(dot);
        qDir = DirectX::XMQuaternionRotationAxis(axis, angle);
    } else if (dot < 0.0f) {
        qDir = DirectX::XMQuaternionRotationAxis(baseUp, DirectX::XM_PI);
    }

    const DirectX::XMVECTOR qRoll = DirectX::XMQuaternionRotationAxis(forward, DirectX::XMConvertToRadians(keyframe.rollDeg));
    return DirectX::XMQuaternionNormalize(DirectX::XMQuaternionMultiply(qRoll, qDir));
}

bool CinematicPlayer::EvaluateCameraPose(RS3CameraPose& outPose) const {
    if (!m_hasTimeline || m_timeline.keyframes.empty()) {
        return false;
    }

    const auto& keyframes = m_timeline.keyframes;
    const float t = std::max(0.0f, std::min(m_currentTimeSec, m_durationSec));

    if (keyframes.size() == 1) {
        const auto& k = keyframes.front();
        outPose.position = k.position;
        outPose.target = k.target;
        outPose.up = { 0.0f, 0.0f, 1.0f };
        outPose.fovDeg = std::max(1.0f, k.fovDeg);
        outPose.nearZ = 1.0f;
        outPose.farZ = 20000.0f;
        return true;
    }

    size_t seg = keyframes.size() - 2;
    for (size_t i = 0; i + 1 < keyframes.size(); ++i) {
        if (t <= keyframes[i + 1].t) {
            seg = i;
            break;
        }
    }

    const auto& k1 = keyframes[seg];
    const auto& k2 = keyframes[seg + 1];
    const auto& k0 = (seg == 0) ? keyframes[seg] : keyframes[seg - 1];
    const auto& k3 = (seg + 2 < keyframes.size()) ? keyframes[seg + 2] : keyframes[seg + 1];

    const float segmentDuration = std::max(kEpsilon, k2.t - k1.t);
    const float rawU = (t - k1.t) / segmentDuration;
    const float u = ApplyEase(rawU, k2.ease);

    const DirectX::XMFLOAT3 pos = CatmullRom(k0.position, k1.position, k2.position, k3.position, u);
    const float d1 = std::max(1.0f, Length(Subtract(k1.target, k1.position)));
    const float d2 = std::max(1.0f, Length(Subtract(k2.target, k2.position)));
    const float dist = d1 + (d2 - d1) * u;

    DirectX::XMVECTOR q1 = BuildCameraQuaternion(k1);
    DirectX::XMVECTOR q2 = BuildCameraQuaternion(k2);
    DirectX::XMVECTOR q = DirectX::XMQuaternionSlerp(q1, q2, u);
    q = DirectX::XMQuaternionNormalize(q);

    const DirectX::XMVECTOR baseForward = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    const DirectX::XMVECTOR baseUp = DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
    const DirectX::XMVECTOR forward = DirectX::XMVector3Normalize(DirectX::XMVector3Rotate(baseForward, q));
    const DirectX::XMVECTOR up = DirectX::XMVector3Normalize(DirectX::XMVector3Rotate(baseUp, q));

    DirectX::XMFLOAT3 forwardF;
    DirectX::XMFLOAT3 upF;
    DirectX::XMStoreFloat3(&forwardF, forward);
    DirectX::XMStoreFloat3(&upF, up);

    outPose.position = pos;
    outPose.target = {
        pos.x + forwardF.x * dist,
        pos.y + forwardF.y * dist,
        pos.z + forwardF.z * dist
    };
    outPose.up = upF;
    outPose.fovDeg = std::max(1.0f, k1.fovDeg + (k2.fovDeg - k1.fovDeg) * u);
    outPose.nearZ = 1.0f;
    outPose.farZ = 20000.0f;
    return true;
}

} // namespace RealSpace3
