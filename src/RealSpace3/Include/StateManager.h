#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include "Types.h"

namespace RealSpace3 {

enum class RenderPass {
    Map,
    Skin_Base,
    Skin_Outline,
    Alpha,
    Additive,
    UI
};

class RStateManager {
public:
    RStateManager(ID3D11Device* device, ID3D11DeviceContext* context);
    ~RStateManager();

    void ApplyPass(RenderPass pass);
    void ClearSRVs();
    void Reset();

private:
    ID3D11Device* m_pd3dDevice;
    ID3D11DeviceContext* m_pContext;

    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rsMap;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_dsMap;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rsSkinBase;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_dsSkinBase;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_bsSkinBaseOpaque;
};

}
