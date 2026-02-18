#include "../Include/SceneManager.h"
#include "../Include/RScene.h"
#include "AppLogger.h"

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
    AppLogger::Log("[RS3] SceneManager::loadLobbyBasic -> basic scene loaded.");
}

void SceneManager::update(float deltaTime) {
    if (m_pCurrentScene) m_pCurrentScene->Update(deltaTime);
}

void SceneManager::draw(ID3D11DeviceContext* context) {
    if (!m_pCurrentScene) return;

    D3D11_VIEWPORT vp;
    vp.Width = static_cast<float>(m_width);
    vp.Height = static_cast<float>(m_height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    context->RSSetViewports(1, &vp);

    DirectX::XMFLOAT3 camPos = { 0.0f, -800.0f, 220.0f };
    DirectX::XMFLOAT3 camDir = { 0.0f, 1.0f, -0.2f };
    m_pCurrentScene->GetPreferredCamera(camPos, camDir);

    const DirectX::XMVECTOR eye = DirectX::XMVectorSet(camPos.x, camPos.y, camPos.z, 0.0f);
    DirectX::XMVECTOR dir = DirectX::XMVectorSet(camDir.x, camDir.y, camDir.z, 0.0f);
    dir = DirectX::XMVector3Normalize(dir);
    dir = DirectX::XMVectorScale(dir, 1000.0f);
    DirectX::XMVECTOR at = DirectX::XMVectorAdd(eye, dir);

    const DirectX::XMMATRIX view = DirectX::XMMatrixLookAtLH(
        eye,
        at,
        DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f)
    );
    const float aspect = static_cast<float>(m_width) / static_cast<float>(m_height);
    const DirectX::XMMATRIX proj =
        DirectX::XMMatrixPerspectiveFovLH(DirectX::XM_PI * 60.0f / 180.0f, aspect, 1.0f, 20000.0f);

    m_pCurrentScene->Draw(context, view * proj);
}

bool SceneManager::setCreationPreview(int sex, int face, int preset, int hair) {
    if (!m_pCurrentScene) return false;
    return m_pCurrentScene->SetCreationPreview(sex, face, preset, hair);
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

} // namespace RealSpace3
