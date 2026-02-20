#include "../Include/SceneManager.h"
#include "../Include/RScene.h"
#include "../Include/CinematicTimeline.h"

#include "AppLogger.h"

#include <algorithm>

namespace RealSpace3 {

bool SceneManager::init(ID3D11Device* device) {
    m_pDevice = device;
    return true;
}

bool SceneManager::EnsureScene() {
    if (!m_pDevice) return false;
    if (m_pCurrentScene) return true;

    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    m_pDevice->GetImmediateContext(&context);
    m_pCurrentScene = std::make_unique<RScene>(m_pDevice, context.Get());
    m_pCurrentScene->SetRenderMode(RS3RenderMode::Gameplay);
    return true;
}

void SceneManager::loadHangar() {
    if (!EnsureScene()) return;
    stopTimeline();
    m_pCurrentScene->LoadCharSelect();
    m_hasCameraPoseOverride = false;
    m_pCurrentScene->ClearCameraPose();
}

void SceneManager::loadLobbyBasic() {
    if (!EnsureScene()) return;
    stopTimeline();
    m_pCurrentScene->LoadLobbyBasic();
    m_hasCameraPoseOverride = false;
    m_pCurrentScene->ClearCameraPose();
    AppLogger::Log("[RS3] SceneManager::loadLobbyBasic -> basic scene loaded.");
}

bool SceneManager::setRenderMode(RS3RenderMode mode) {
    if (!EnsureScene()) return false;
    m_pCurrentScene->SetRenderMode(mode);
    return true;
}

RS3RenderMode SceneManager::getRenderMode() const {
    if (!m_pCurrentScene) return RS3RenderMode::Gameplay;
    return m_pCurrentScene->GetRenderMode();
}

bool SceneManager::loadScenePackage(const std::string& sceneId) {
    if (!EnsureScene()) return false;
    const bool ok = m_pCurrentScene->LoadScenePackage(sceneId);
    if (ok) {
        m_pCurrentScene->SetRenderMode(RS3RenderMode::MapOnlyCinematic);
    }
    return ok;
}

bool SceneManager::playTimeline(const std::string& timelinePath, const RS3TimelinePlaybackOptions& opts) {
    if (!EnsureScene()) return false;

    RS3TimelineData timeline;
    std::string timelineError;
    if (!LoadTimelineFromFile(timelinePath, timeline, &timelineError)) {
        AppLogger::Log("[RS3] playTimeline failed: " + timelineError);
        return false;
    }

    if (!m_pCurrentScene->LoadScenePackage(timeline.sceneId)) {
        AppLogger::Log("[RS3] playTimeline failed: could not load scene package '" + timeline.sceneId + "'.");
        return false;
    }

    m_pCurrentScene->SetRenderMode(timeline.mode);

    std::string playError;
    if (!m_cinematicPlayer.Play(timeline, opts, &playError)) {
        AppLogger::Log("[RS3] playTimeline failed: " + playError);
        return false;
    }

    RS3CameraPose pose;
    if (m_cinematicPlayer.EvaluateCameraPose(pose)) {
        setCameraPose(pose, true);
    }

    AppLogger::Log("[RS3] playTimeline success: sceneId='" + timeline.sceneId + "' mode='" + std::string(ToRenderModeString(timeline.mode)) + "'.");
    return true;
}

void SceneManager::stopTimeline() {
    m_cinematicPlayer.Stop();
    m_hasCameraPoseOverride = false;
    if (m_pCurrentScene) {
        m_pCurrentScene->ClearCameraPose();
    }
}

bool SceneManager::setCameraPose(const RS3CameraPose& pose, bool immediate) {
    if (!EnsureScene()) return false;
    m_cameraPoseOverride = pose;
    m_hasCameraPoseOverride = true;
    return m_pCurrentScene->SetCameraPose(pose, immediate);
}

void SceneManager::update(float deltaTime) {
    if (!m_pCurrentScene) return;

    m_pCurrentScene->Update(deltaTime);

    if (m_cinematicPlayer.HasTimeline()) {
        m_cinematicPlayer.Update(deltaTime);
        RS3CameraPose pose;
        if (m_cinematicPlayer.EvaluateCameraPose(pose)) {
            setCameraPose(pose, true);
        }
    }
}

void SceneManager::draw(ID3D11DeviceContext* context) {
    if (!m_pCurrentScene || !context) return;

    D3D11_VIEWPORT vp;
    vp.Width = static_cast<float>(m_width);
    vp.Height = static_cast<float>(m_height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    context->RSSetViewports(1, &vp);

    RS3CameraPose pose;
    if (m_hasCameraPoseOverride) {
        pose = m_cameraPoseOverride;
    } else {
        m_pCurrentScene->GetPreferredCameraPose(pose);
    }

    const DirectX::XMVECTOR eye = DirectX::XMVectorSet(pose.position.x, pose.position.y, pose.position.z, 0.0f);
    DirectX::XMVECTOR at = DirectX::XMVectorSet(pose.target.x, pose.target.y, pose.target.z, 0.0f);
    const DirectX::XMVECTOR up = DirectX::XMVectorSet(pose.up.x, pose.up.y, pose.up.z, 0.0f);
    if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(DirectX::XMVectorSubtract(at, eye))) < 0.000001f) {
        at = DirectX::XMVectorSet(pose.position.x, pose.position.y + 1000.0f, pose.position.z, 0.0f);
    }

    const DirectX::XMMATRIX view = DirectX::XMMatrixLookAtLH(eye, at, up);
    const float aspect = static_cast<float>(m_width) / static_cast<float>(m_height);
    const float nearZ = std::max(0.01f, pose.nearZ);
    const float farZ = std::max(nearZ + 0.1f, pose.farZ);
    const DirectX::XMMATRIX proj =
        DirectX::XMMatrixPerspectiveFovLH(DirectX::XM_PI * std::clamp(pose.fovDeg, 1.0f, 170.0f) / 180.0f, aspect, nearZ, farZ);

    m_pCurrentScene->DrawWorld(context, view * proj);
}

void SceneManager::drawShowcaseOverlay(ID3D11DeviceContext* context) {
    if (!m_pCurrentScene || !context) return;

    if (m_pCurrentScene->GetRenderMode() != RS3RenderMode::ShowcaseOnly) {
        return;
    }

    D3D11_VIEWPORT vp;
    vp.Width = static_cast<float>(m_width);
    vp.Height = static_cast<float>(m_height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    context->RSSetViewports(1, &vp);

    RS3CameraPose pose;
    if (m_hasCameraPoseOverride) {
        pose = m_cameraPoseOverride;
    } else {
        m_pCurrentScene->GetPreferredCameraPose(pose);
    }

    const DirectX::XMVECTOR eye = DirectX::XMVectorSet(pose.position.x, pose.position.y, pose.position.z, 0.0f);
    DirectX::XMVECTOR at = DirectX::XMVectorSet(pose.target.x, pose.target.y, pose.target.z, 0.0f);
    const DirectX::XMVECTOR up = DirectX::XMVectorSet(pose.up.x, pose.up.y, pose.up.z, 0.0f);
    if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(DirectX::XMVectorSubtract(at, eye))) < 0.000001f) {
        at = DirectX::XMVectorSet(pose.position.x, pose.position.y + 1000.0f, pose.position.z, 0.0f);
    }

    const DirectX::XMMATRIX view = DirectX::XMMatrixLookAtLH(eye, at, up);
    const float aspect = static_cast<float>(m_width) / static_cast<float>(m_height);
    const float nearZ = std::max(0.01f, pose.nearZ);
    const float farZ = std::max(nearZ + 0.1f, pose.farZ);
    const DirectX::XMMATRIX proj =
        DirectX::XMMatrixPerspectiveFovLH(DirectX::XM_PI * std::clamp(pose.fovDeg, 1.0f, 170.0f) / 180.0f, aspect, nearZ, farZ);

    m_pCurrentScene->DrawShowcase(context, view * proj, false);
}

void SceneManager::setShowcaseViewport(int x, int y, int width, int height) {
    if (!m_pCurrentScene) return;
    if (m_width <= 0 || m_height <= 0) return;

    if (width <= 1 || height <= 1) {
        m_pCurrentScene->SetShowcaseViewportPixels(0, 0, 0, 0);
        return;
    }

    const int x0 = std::clamp(x, 0, m_width - 1);
    const int y0 = std::clamp(y, 0, m_height - 1);
    const int w = std::max(1, std::min(width, m_width - x0));
    const int h = std::max(1, std::min(height, m_height - y0));
    m_pCurrentScene->SetShowcaseViewportPixels(x0, y0, w, h);
}

bool SceneManager::setCreationPreview(int sex, int face, int preset, int hair) {
    if (!m_pCurrentScene) return false;
    return m_pCurrentScene->SetCreationPreview(sex, face, preset, hair);
}

void SceneManager::setCreationPreviewVisible(bool visible) {
    if (!m_pCurrentScene) return;
    m_pCurrentScene->SetCreationPreviewVisible(visible);
}

bool SceneManager::setShowcaseObjectModel(const std::string& modelId) {
    if (!EnsureScene()) return false;
    return m_pCurrentScene->SetShowcaseObjectModel(modelId);
}

bool SceneManager::adjustCreationCamera(float yawDeltaDeg, float pitchDeltaDeg, float zoomDelta) {
    if (!m_pCurrentScene) return false;
    m_hasCameraPoseOverride = false;
    m_pCurrentScene->ClearCameraPose();
    return m_pCurrentScene->AdjustCreationCamera(yawDeltaDeg, pitchDeltaDeg, zoomDelta);
}

bool SceneManager::adjustCreationCharacterYaw(float yawDeltaDeg) {
    if (!m_pCurrentScene) return false;
    m_hasCameraPoseOverride = false;
    m_pCurrentScene->ClearCameraPose();
    return m_pCurrentScene->AdjustCreationCharacterYaw(yawDeltaDeg);
}

bool SceneManager::setCreationCameraPose(float yawDeg, float pitchDeg, float distance, float focusHeight, bool autoOrbit) {
    if (!m_pCurrentScene) return false;
    m_hasCameraPoseOverride = false;
    m_pCurrentScene->ClearCameraPose();
    return m_pCurrentScene->SetCreationCameraPose(yawDeg, pitchDeg, distance, focusHeight, autoOrbit);
}

void SceneManager::setCreationCameraAutoOrbit(bool enabled) {
    if (!m_pCurrentScene) return;
    m_pCurrentScene->SetCreationCameraAutoOrbit(enabled);
}

void SceneManager::resetCreationCamera() {
    if (!m_pCurrentScene) return;
    m_hasCameraPoseOverride = false;
    m_pCurrentScene->ClearCameraPose();
    m_pCurrentScene->ResetCreationCamera();
}

void SceneManager::setSize(int w, int h) {
    if (w > 0 && h > 0) {
        m_width = w;
        m_height = h;
    }
}

bool SceneManager::shouldDrawShowcaseAfterUI() const {
    return m_pCurrentScene && m_pCurrentScene->GetRenderMode() == RS3RenderMode::ShowcaseOnly;
}

} // namespace RealSpace3
