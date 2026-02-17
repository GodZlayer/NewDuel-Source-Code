#include "RenderManager.h"
#include "UIManager.h"
#include "LoadingManager.h"
#include "NakamaManager.h"
#include "RealSpace3/Include/SceneManager.h"

void RenderManager::init(RealSpace3::RDeviceDX11* device) {
    m_pDevice = device;
}

void RenderManager::render() {
    if (!m_pDevice) return;

    Nakama::NakamaManager::getInstance().tick();
    UIManager::getInstance().update();
    float dt = 0.016f;
    LoadingManager::getInstance().update(dt);
    RealSpace3::SceneManager::getInstance().update(dt);

    // 1. Background clear — dark scene color
    m_pDevice->Clear(0.02f, 0.02f, 0.05f, 1.0f);
    
    // 2. Renderização do Mundo 3D
    m_pDevice->SetStandard3DStates();
    RealSpace3::SceneManager::getInstance().draw(m_pDevice->GetContext());

    // 3. Renderização da UI (Overlay)
    UIManager::getInstance().render();
    uint32_t pitch = 0, uiW = 0, uiH = 0;
    unsigned char* pixels = UIManager::getInstance().getLockPixels(pitch, uiW, uiH);
    if (pixels && uiW > 0 && uiH > 0) {
        m_pDevice->UpdateUITexture(pixels, pitch, uiW, uiH);
        UIManager::getInstance().unlockPixels();
        m_pDevice->DrawUI();
    }

    // 4. Exibição
    m_pDevice->Present();
}
