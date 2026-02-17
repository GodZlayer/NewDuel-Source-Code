#include "Character/RenderedCharacter.h"

namespace Gunz {

RenderedCharacter::RenderedCharacter(ID3D11Device* device, RealSpace3::TextureManager* texMgr) {
    m_skin = std::make_unique<RealSpace3::RSkinObject>(device, texMgr);
}

RenderedCharacter::~RenderedCharacter() = default;

bool RenderedCharacter::LoadCharacter(bool female) {
    const char* model = female ?
        "Model/woman/woman-parts00.elu" :
        "Model/man/man-parts00.elu";

    if (!m_skin->LoadElu(model)) {
        return false;
    }

    const char* ani = female ?
        "Model/woman/woman_login_knife_idle.elu.ani" :
        "Model/man/man_login_knife_idle.elu.ani";

    m_skin->LoadAni(ani);
    m_skin->SetBindPoseOnly(false);
    return true;
}

void RenderedCharacter::Update(float deltaTime) {
    if (m_skin) m_skin->Update(deltaTime);
}

void RenderedCharacter::Draw(ID3D11DeviceContext* context, DirectX::FXMMATRIX viewProj) {
    if (m_skin) m_skin->Draw(context, viewProj, false);
}

void RenderedCharacter::SetWorldPosition(const DirectX::XMFLOAT3& pos) {
    if (m_skin) m_skin->SetWorldPosition(pos);
}

void RenderedCharacter::SetWorldYaw(float yawRadians) {
    if (m_skin) m_skin->SetWorldYaw(yawRadians);
}

} // namespace Gunz
