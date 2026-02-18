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
    bool setCreationPreview(int sex, int face, int preset, int hair);
    void setCreationPreviewVisible(bool visible);
    void setSize(int w, int h);

private:
    SceneManager() = default;
    ID3D11Device* m_pDevice = nullptr;
    std::unique_ptr<RScene> m_pCurrentScene;
    int m_width = 1280;
    int m_height = 720;
};

} // namespace RealSpace3
