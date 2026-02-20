#include "RenderManager.h"
#include "UIManager.h"
#include "LoadingManager.h"
#include "NakamaManager.h"
#include "RealSpace3/Include/SceneManager.h"
#include <algorithm>
#include <chrono>

void RenderManager::init(RealSpace3::RDeviceDX11* device) {
    m_pDevice = device;
}

void RenderManager::render() {
    if (!m_pDevice) return;

    static auto lastFrameTick = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - lastFrameTick).count();
    lastFrameTick = now;
    if (dt <= 0.0f || dt > 1.0f) dt = 1.0f / 60.0f;
    dt = std::clamp(dt, 1.0f / 240.0f, 1.0f / 20.0f);

    Nakama::NakamaManager::getInstance().tick();
    UIManager::getInstance().update();
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

    // 4. Renderiza o showcase por cima da UI
    m_pDevice->SetStandard3DStates();
    RealSpace3::SceneManager::getInstance().drawShowcaseOverlay(m_pDevice->GetContext());

    // 5. Exibição
    m_pDevice->Present();
}
