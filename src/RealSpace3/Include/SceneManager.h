#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <memory>
#include "RScene.h"

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
    void setShowcaseViewport(int x, int y, int width, int height);
    bool setCreationPreview(int sex, int face, int preset, int hair);
    void setCreationPreviewVisible(bool visible);
    bool adjustCreationCamera(float yawDeltaDeg, float pitchDeltaDeg, float zoomDelta);
    bool adjustCreationCharacterYaw(float yawDeltaDeg);
    bool setCreationCameraPose(float yawDeg, float pitchDeg, float distance, float focusHeight, bool autoOrbit);
    void setCreationCameraAutoOrbit(bool enabled);
    void resetCreationCamera();
    void setSize(int w, int h);

private:
    SceneManager() = default;
    ID3D11Device* m_pDevice = nullptr;
    std::unique_ptr<RScene> m_pCurrentScene;
    int m_width = 1280;
    int m_height = 720;
};

} // namespace RealSpace3
