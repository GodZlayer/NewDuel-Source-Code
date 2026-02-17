#pragma once

#include <memory>
#include "RSkinObject.h"
#include "TextureManager.h"

namespace Gunz {

class RenderedCharacter {
public:
    RenderedCharacter(ID3D11Device* device, RealSpace3::TextureManager* texMgr);
    ~RenderedCharacter();

    bool LoadCharacter(bool female);
    void Update(float deltaTime);
    void Draw(ID3D11DeviceContext* context, DirectX::FXMMATRIX viewProj);
    void SetWorldPosition(const DirectX::XMFLOAT3& pos);
    void SetWorldYaw(float yawRadians);

    // Access to internal RSkinObject for advanced control (CharacterBuilder)
    RealSpace3::RSkinObject* GetSkinObject() const { return m_skin.get(); }

private:
    std::unique_ptr<RealSpace3::RSkinObject> m_skin;
};

} // namespace Gunz
