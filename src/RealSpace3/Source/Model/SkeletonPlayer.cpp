#include "../../Include/Model/SkeletonPlayer.h"
#include "AppLogger.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

namespace RealSpace3 {
namespace {

float ComputeClipDuration(const RS3AnimationClip& clip) {
    float duration = 0.0f;
    for (const auto& channel : clip.channels) {
        if (!channel.posKeys.empty()) {
            duration = std::max(duration, channel.posKeys.back().time);
        }
        if (!channel.rotKeys.empty()) {
            duration = std::max(duration, channel.rotKeys.back().time);
        }
    }
    return duration;
}

float WrapTime(float time, float duration) {
    if (duration <= 0.0f) return 0.0f;
    time = std::fmod(time, duration);
    if (time < 0.0f) time += duration;
    return time;
}

DirectX::XMFLOAT3 SamplePosition(const std::vector<RS3PosKey>& keys, float time, const DirectX::XMFLOAT3& fallback) {
    if (keys.empty()) return fallback;
    if (keys.size() == 1) return keys.front().value;
    if (time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time) return keys.back().value;

    for (size_t i = 1; i < keys.size(); ++i) {
        if (time <= keys[i].time) {
            const auto& a = keys[i - 1];
            const auto& b = keys[i];
            const float span = b.time - a.time;
            const float t = (span > 0.0f) ? ((time - a.time) / span) : 0.0f;
            const DirectX::XMVECTOR va = DirectX::XMLoadFloat3(&a.value);
            const DirectX::XMVECTOR vb = DirectX::XMLoadFloat3(&b.value);
            DirectX::XMFLOAT3 out;
            DirectX::XMStoreFloat3(&out, DirectX::XMVectorLerp(va, vb, t));
            return out;
        }
    }

    return keys.back().value;
}

DirectX::XMFLOAT4 SampleRotation(const std::vector<RS3RotKey>& keys, float time, const DirectX::XMFLOAT4& fallback) {
    if (keys.empty()) return fallback;
    if (keys.size() == 1) return keys.front().value;
    if (time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time) return keys.back().value;

    for (size_t i = 1; i < keys.size(); ++i) {
        if (time <= keys[i].time) {
            const auto& a = keys[i - 1];
            const auto& b = keys[i];
            const float span = b.time - a.time;
            const float t = (span > 0.0f) ? ((time - a.time) / span) : 0.0f;
            DirectX::XMVECTOR qa = DirectX::XMQuaternionNormalize(DirectX::XMLoadFloat4(&a.value));
            DirectX::XMVECTOR qb = DirectX::XMQuaternionNormalize(DirectX::XMLoadFloat4(&b.value));
            DirectX::XMFLOAT4 out;
            DirectX::XMStoreFloat4(&out, DirectX::XMQuaternionSlerp(qa, qb, t));
            return out;
        }
    }

    return keys.back().value;
}

const RS3AnimationChannel* FindChannelForBone(const RS3AnimationClip& clip, int32_t boneIndex) {
    for (const auto& channel : clip.channels) {
        if (channel.boneIndex == boneIndex) return &channel;
    }
    return nullptr;
}

bool MatrixIsFiniteAndReasonable(const DirectX::XMFLOAT4X4& m, float* outMaxAbs = nullptr, float* outMaxTranslate = nullptr) {
    const float* v = reinterpret_cast<const float*>(&m);
    float maxAbs = 0.0f;
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(v[i])) {
            if (outMaxAbs) *outMaxAbs = std::numeric_limits<float>::infinity();
            if (outMaxTranslate) *outMaxTranslate = std::numeric_limits<float>::infinity();
            return false;
        }
        maxAbs = std::max(maxAbs, std::fabs(v[i]));
    }

    const float tx = m._41;
    const float ty = m._42;
    const float tz = m._43;
    const float tmag = std::sqrt(tx * tx + ty * ty + tz * tz);

    if (outMaxAbs) *outMaxAbs = maxAbs;
    if (outMaxTranslate) *outMaxTranslate = tmag;

    if (maxAbs > 1000.0f) return false;
    if (tmag > 500.0f) return false;
    return true;
}

DirectX::XMFLOAT3 ExtractTranslationFromMatrix(const DirectX::XMFLOAT4X4& m) {
    return DirectX::XMFLOAT3(m._41, m._42, m._43);
}

DirectX::XMFLOAT4 ExtractRotationFromMatrix(const DirectX::XMFLOAT4X4& m) {
    DirectX::XMFLOAT4 out = { 0.0f, 0.0f, 0.0f, 1.0f };
    DirectX::XMVECTOR scale = DirectX::XMVectorSet(1.0f, 1.0f, 1.0f, 0.0f);
    DirectX::XMVECTOR rot = DirectX::XMQuaternionIdentity();
    DirectX::XMVECTOR pos = DirectX::XMVectorZero();
    if (DirectX::XMMatrixDecompose(&scale, &rot, &pos, DirectX::XMLoadFloat4x4(&m))) {
        DirectX::XMStoreFloat4(&out, rot);
        return out;
    }

    // Fallback: strip translation and extract a normalized rotation best-effort.
    DirectX::XMFLOAT4X4 noT = m;
    noT._41 = noT._42 = noT._43 = 0.0f;
    noT._44 = 1.0f;
    rot = DirectX::XMQuaternionNormalize(DirectX::XMQuaternionRotationMatrix(DirectX::XMLoadFloat4x4(&noT)));
    DirectX::XMStoreFloat4(&out, rot);
    return out;
}

float ComputeOrderError(const RS3ModelPackage* package, bool localFirstOrder) {
    if (!package || package->bones.empty()) return 0.0f;

    std::vector<DirectX::XMMATRIX> global(package->bones.size(), DirectX::XMMatrixIdentity());
    float error = 0.0f;

    for (size_t i = 0; i < package->bones.size(); ++i) {
        const auto& bone = package->bones[i];
        const DirectX::XMMATRIX local = DirectX::XMLoadFloat4x4(&bone.bind);
        if (bone.parentBone >= 0 && static_cast<size_t>(bone.parentBone) < global.size()) {
            const DirectX::XMMATRIX parent = global[static_cast<size_t>(bone.parentBone)];
            global[i] = localFirstOrder ? DirectX::XMMatrixMultiply(local, parent) : DirectX::XMMatrixMultiply(parent, local);
        } else {
            global[i] = local;
        }

        const DirectX::XMMATRIX invBind = DirectX::XMLoadFloat4x4(&bone.invBind);
        const DirectX::XMMATRIX skin = DirectX::XMMatrixMultiply(invBind, global[i]);
        const DirectX::XMMATRIX identity = DirectX::XMMatrixIdentity();
        for (int r = 0; r < 4; ++r) {
            const DirectX::XMVECTOR diff = DirectX::XMVectorAbs(DirectX::XMVectorSubtract(skin.r[r], identity.r[r]));
            DirectX::XMFLOAT4 row;
            DirectX::XMStoreFloat4(&row, diff);
            error += row.x + row.y + row.z + row.w;
        }
    }

    return error;
}

} // namespace

void SkeletonPlayer::SetPackage(const RS3ModelPackage* package) {
    m_package = package;
    m_clipIndex = -1;
    m_blendSeconds = 0.0f;
    m_timeSeconds = 0.0f;
    m_clipDuration = 0.0f;
    m_parentOrderResolved = false;
    m_localFirstOrder = true;
    m_loggedOrderDiagnostics = false;
    m_loggedDecomposeWarning = false;
    m_loggedSkinFallbackWarning = false;
}

bool SkeletonPlayer::SetAnimationClipByName(const std::string& clipName, float blendSeconds) {
    if (!m_package) return false;

    for (size_t i = 0; i < m_package->clips.size(); ++i) {
        if (m_package->clips[i].name == clipName) {
            m_clipIndex = static_cast<int32_t>(i);
            m_blendSeconds = blendSeconds;
            m_timeSeconds = 0.0f;
            m_clipDuration = ComputeClipDuration(m_package->clips[i]);
            return true;
        }
    }

    return false;
}

const RS3AnimationClip* SkeletonPlayer::GetCurrentClip() const {
    if (!m_package) return nullptr;
    if (m_clipIndex < 0 || static_cast<size_t>(m_clipIndex) >= m_package->clips.size()) return nullptr;
    return &m_package->clips[static_cast<size_t>(m_clipIndex)];
}

float SkeletonPlayer::GetBlendSeconds() const {
    return m_blendSeconds;
}

void SkeletonPlayer::Update(float deltaSeconds) {
    if (deltaSeconds <= 0.0f) return;
    if (!m_package) return;
    if (m_clipIndex < 0 || static_cast<size_t>(m_clipIndex) >= m_package->clips.size()) return;

    if (m_clipDuration <= 0.0f) {
        m_clipDuration = ComputeClipDuration(m_package->clips[static_cast<size_t>(m_clipIndex)]);
    }

    m_timeSeconds += deltaSeconds;
    if (m_clipDuration > 0.0f) {
        m_timeSeconds = WrapTime(m_timeSeconds, m_clipDuration);
    }
}

bool SkeletonPlayer::BuildSkinMatrices(std::vector<DirectX::XMFLOAT4X4>& outMatrices) const {
    outMatrices.clear();
    if (!m_package) return false;

    const auto& bones = m_package->bones;
    if (bones.empty()) return true;

    if (!m_parentOrderResolved) {
        const float localFirstError = ComputeOrderError(m_package, true);
        const float parentFirstError = ComputeOrderError(m_package, false);
        m_localFirstOrder = localFirstError <= parentFirstError;
        m_parentOrderResolved = true;

        if (!m_loggedOrderDiagnostics) {
            std::ostringstream oss;
            oss << "[RS3] SkeletonPlayer order resolve: localFirstError=" << localFirstError
                << " parentFirstError=" << parentFirstError
                << " selected=" << (m_localFirstOrder ? "localFirst" : "parentFirst");
            AppLogger::Log(oss.str());
            m_loggedOrderDiagnostics = true;
        }
    }

    const RS3AnimationClip* clip = GetCurrentClip();
    const float sampleTime = (m_clipDuration > 0.0f) ? WrapTime(m_timeSeconds, m_clipDuration) : 0.0f;

    std::vector<DirectX::XMMATRIX> localMats;
    std::vector<DirectX::XMMATRIX> globalMats;
    localMats.resize(bones.size(), DirectX::XMMatrixIdentity());
    globalMats.resize(bones.size(), DirectX::XMMatrixIdentity());

    size_t decomposeFailCount = 0;
    for (size_t i = 0; i < bones.size(); ++i) {
        const auto& bone = bones[i];
        const DirectX::XMMATRIX bindMatrix = DirectX::XMLoadFloat4x4(&bone.bind);
        const RS3AnimationChannel* channel = clip ? FindChannelForBone(*clip, static_cast<int32_t>(i)) : nullptr;
        const bool hasAnimatedChannel = channel && (!channel->posKeys.empty() || !channel->rotKeys.empty());

        if (!hasAnimatedChannel) {
            localMats[i] = bindMatrix;
        } else {
            DirectX::XMVECTOR bindScale = DirectX::XMVectorSet(1.0f, 1.0f, 1.0f, 0.0f);
            DirectX::XMVECTOR bindRot = DirectX::XMQuaternionIdentity();
            DirectX::XMVECTOR bindPos = DirectX::XMVectorZero();

            bool decomposeOk = DirectX::XMMatrixDecompose(&bindScale, &bindRot, &bindPos, bindMatrix);
            if (!decomposeOk) {
                ++decomposeFailCount;
                const DirectX::XMFLOAT3 pos = ExtractTranslationFromMatrix(bone.bind);
                const DirectX::XMFLOAT4 rot = ExtractRotationFromMatrix(bone.bind);
                bindScale = DirectX::XMVectorSet(1.0f, 1.0f, 1.0f, 0.0f);
                bindPos = DirectX::XMVectorSet(pos.x, pos.y, pos.z, 1.0f);
                bindRot = DirectX::XMQuaternionNormalize(DirectX::XMLoadFloat4(&rot));
            }

            DirectX::XMFLOAT3 bindPosF;
            DirectX::XMStoreFloat3(&bindPosF, bindPos);
            DirectX::XMFLOAT4 bindRotF;
            DirectX::XMStoreFloat4(&bindRotF, bindRot);

            DirectX::XMFLOAT3 sampledPos = SamplePosition(channel->posKeys, sampleTime, bindPosF);
            DirectX::XMFLOAT4 sampledRot = SampleRotation(channel->rotKeys, sampleTime, bindRotF);

            const DirectX::XMVECTOR posV = DirectX::XMLoadFloat3(&sampledPos);
            const DirectX::XMVECTOR rotV = DirectX::XMQuaternionNormalize(DirectX::XMLoadFloat4(&sampledRot));
            const DirectX::XMMATRIX local = DirectX::XMMatrixAffineTransformation(bindScale, DirectX::XMVectorZero(), rotV, posV);
            localMats[i] = local;
        }

        if (bone.parentBone >= 0 && static_cast<size_t>(bone.parentBone) < globalMats.size()) {
            const DirectX::XMMATRIX parent = globalMats[static_cast<size_t>(bone.parentBone)];
            globalMats[i] = m_localFirstOrder ? DirectX::XMMatrixMultiply(localMats[i], parent) : DirectX::XMMatrixMultiply(parent, localMats[i]);
        } else {
            globalMats[i] = localMats[i];
        }
    }

    outMatrices.resize(bones.size());
    bool invalidSkinMatrix = false;
    float worstAbs = 0.0f;
    float worstTranslate = 0.0f;
    for (size_t i = 0; i < bones.size(); ++i) {
        const DirectX::XMMATRIX invBind = DirectX::XMLoadFloat4x4(&bones[i].invBind);
        const DirectX::XMMATRIX skin = DirectX::XMMatrixMultiply(invBind, globalMats[i]);
        DirectX::XMStoreFloat4x4(&outMatrices[i], skin);

        float maxAbs = 0.0f;
        float maxTranslate = 0.0f;
        if (!MatrixIsFiniteAndReasonable(outMatrices[i], &maxAbs, &maxTranslate)) {
            invalidSkinMatrix = true;
            worstAbs = std::max(worstAbs, maxAbs);
            worstTranslate = std::max(worstTranslate, maxTranslate);
        }
    }

    if (decomposeFailCount > 0 && !m_loggedDecomposeWarning) {
        AppLogger::Log("[RS3] SkeletonPlayer: bind decompose fallback count=" + std::to_string(decomposeFailCount));
        m_loggedDecomposeWarning = true;
    }

    if (invalidSkinMatrix) {
        if (!m_loggedSkinFallbackWarning) {
            std::ostringstream oss;
            oss << "[RS3] SkeletonPlayer: invalid skin matrices detected; fallback to bind pose. worstAbs=" << worstAbs
                << " worstTranslate=" << worstTranslate;
            AppLogger::Log(oss.str());
            m_loggedSkinFallbackWarning = true;
        }
        return false;
    }

    return true;
}

float SkeletonPlayer::GetCurrentTimeSeconds() const {
    return m_timeSeconds;
}

} // namespace RealSpace3
