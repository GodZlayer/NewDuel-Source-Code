#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <memory>
#include <string>
#include "RScene.h"
#include "CinematicPlayer.h"

namespace RealSpace3 {

class SceneManager {
public:
    static SceneManager& getInstance() {
        static SceneManager instance;
        return instance;
    }

    bool init(ID3D11Device* device);
    void loadHangar();
    void loadLobbyBasic();
    void update(float deltaTime);
    void draw(ID3D11DeviceContext* context);
    void drawShowcaseOverlay(ID3D11DeviceContext* context);
    bool setRenderMode(RS3RenderMode mode);
    RS3RenderMode getRenderMode() const;
    bool loadScenePackage(const std::string& sceneId);
    bool playTimeline(const std::string& timelinePath, const RS3TimelinePlaybackOptions& opts);
    void stopTimeline();
    bool setCameraPose(const RS3CameraPose& pose, bool immediate);
    void setShowcaseViewport(int x, int y, int width, int height);
    bool setCreationPreview(int sex, int face, int preset, int hair);
    void setCreationPreviewVisible(bool visible);
    bool setShowcaseObjectModel(const std::string& modelId);
    bool adjustCreationCamera(float yawDeltaDeg, float pitchDeltaDeg, float zoomDelta);
    bool adjustCreationCharacterYaw(float yawDeltaDeg);
    bool setCreationCameraPose(float yawDeg, float pitchDeg, float distance, float focusHeight, bool autoOrbit);
    void setCreationCameraAutoOrbit(bool enabled);
    void resetCreationCamera();
    void setSize(int w, int h);
    bool shouldDrawShowcaseAfterUI() const;

private:
    SceneManager() = default;
    bool EnsureScene();
    ID3D11Device* m_pDevice = nullptr;
    std::unique_ptr<RScene> m_pCurrentScene;
    CinematicPlayer m_cinematicPlayer;
    bool m_hasCameraPoseOverride = false;
    RS3CameraPose m_cameraPoseOverride;
    int m_width = 1280;
    int m_height = 720;
};

} // namespace RealSpace3
