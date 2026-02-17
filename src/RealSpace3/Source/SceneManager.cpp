#include "../Include/SceneManager.h"
#include "../Include/RBspObject.h"
#include "../Include/RScene.h"
#include "AppLogger.h"
#include <d3dcompiler.h>
#include <algorithm>

namespace RealSpace3 {

bool SceneManager::init(ID3D11Device* device) {
    m_pDevice = device;
    return true;
}

void SceneManager::loadHangar() {
    if (!m_pDevice) return;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    m_pDevice->GetImmediateContext(&context);
    m_pCurrentScene = std::make_unique<RScene>(m_pDevice, context.Get());
    m_pCurrentScene->LoadCharSelect();
}

void SceneManager::loadLobbyBasic() {
    if (!m_pDevice) return;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    m_pDevice->GetImmediateContext(&context);
    m_pCurrentScene = std::make_unique<RScene>(m_pDevice, context.Get());
    m_pCurrentScene->LoadLobbyBasic();
    AppLogger::Log("[RS3_AUDIT] SceneManager::loadLobbyBasic -> lobby scene loaded.");
}

void SceneManager::update(float deltaTime) {
    if (m_pCurrentScene) m_pCurrentScene->Update(deltaTime);
}

void SceneManager::draw(ID3D11DeviceContext* context) {
    if (!m_pCurrentScene) return;

    static int frameCheck = 0;
    bool shouldLog = (++frameCheck % 60 == 0);

    // 1. Configurar viewport para janela atual
    D3D11_VIEWPORT vp;
    vp.Width = static_cast<float>(m_width);
    vp.Height = static_cast<float>(m_height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    context->RSSetViewports(1, &vp);

    // 2. Carregar câmera do dummy
    DirectX::XMFLOAT3 camPos = { 0.0f, -500.0f, 200.0f };
    DirectX::XMFLOAT3 camDir = { 0.0f, 500.0f, -100.0f };
    m_pCurrentScene->GetPreferredCamera(camPos, camDir);

    // 3. Look-at: preferir spawn do mapa (onde personagem nasce)
    const DirectX::XMVECTOR eye = DirectX::XMVectorSet(camPos.x, camPos.y, camPos.z, 0.0f);
    DirectX::XMVECTOR at;
    DirectX::XMFLOAT3 spawnPos;
    if (m_pCurrentScene->GetSpawnPos(spawnPos)) {
        at = DirectX::XMVectorSet(spawnPos.x, spawnPos.y, spawnPos.z, 0.0f);
    } else {
        // Fallback: usar camDir normalizado e escalado
        DirectX::XMVECTOR dir = DirectX::XMVectorSet(camDir.x, camDir.y, camDir.z, 0.0f);
        dir = DirectX::XMVector3Normalize(dir);
        dir = DirectX::XMVectorScale(dir, 1000.0f);
        at = DirectX::XMVectorAdd(eye, dir);
    }

    DirectX::XMMATRIX view = DirectX::XMMatrixLookAtLH(
        eye,
        at,
        DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f)
    );
    float aspect = static_cast<float>(m_width) / static_cast<float>(m_height);
    DirectX::XMMATRIX proj = DirectX::XMMatrixPerspectiveFovLH(DirectX::XM_PI * 60.0f / 180.0f, aspect, 1.0f, 20000.0f);
    DirectX::XMMATRIX viewProj = view * proj;

    if (shouldLog) AppLogger::Log("[RS3_AUDIT] SceneManager::draw calling RScene::Draw");
    m_pCurrentScene->Draw(context, viewProj);
}

void SceneManager::setCreationPreview(int sex, int face, int preset, int hair) {
    if (!m_pCurrentScene) return;
    m_pCurrentScene->SetCreationPreview(sex, face, preset, hair);
}

void SceneManager::setCreationPreviewVisible(bool visible) {
    if (!m_pCurrentScene) return;
    m_pCurrentScene->SetCreationPreviewVisible(visible);
}

void SceneManager::setSize(int w, int h) {
    if (w > 0 && h > 0) {
        m_width = w;
        m_height = h;
    }
}

}
