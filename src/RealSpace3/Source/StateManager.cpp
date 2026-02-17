#include "../Include/StateManager.h"

namespace RealSpace3 {

RStateManager::RStateManager(ID3D11Device* device, ID3D11DeviceContext* context)
    : m_pd3dDevice(device), m_pContext(context)
{
    // Map pass states.
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable = TRUE;
    device->CreateRasterizerState(&rd, &m_rsMap);

    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    device->CreateDepthStencilState(&dsd, &m_dsMap);

    // Skin base pass states (separate objects to avoid pass coupling).
    D3D11_RASTERIZER_DESC skinRD = {};
    skinRD.FillMode = D3D11_FILL_SOLID;
    skinRD.CullMode = D3D11_CULL_NONE;
    skinRD.FrontCounterClockwise = FALSE;
    skinRD.DepthClipEnable = TRUE;
    device->CreateRasterizerState(&skinRD, &m_rsSkinBase);

    D3D11_DEPTH_STENCIL_DESC skinDS = {};
    skinDS.DepthEnable = TRUE;
    skinDS.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    skinDS.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    device->CreateDepthStencilState(&skinDS, &m_dsSkinBase);

    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    auto& rt0 = blendDesc.RenderTarget[0];
    rt0.BlendEnable = FALSE;
    rt0.SrcBlend = D3D11_BLEND_ONE;
    rt0.DestBlend = D3D11_BLEND_ZERO;
    rt0.BlendOp = D3D11_BLEND_OP_ADD;
    rt0.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt0.DestBlendAlpha = D3D11_BLEND_ZERO;
    rt0.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt0.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&blendDesc, &m_bsSkinBaseOpaque);
}

RStateManager::~RStateManager() {}

void RStateManager::ApplyPass(RenderPass pass) {
    switch (pass) {
    case RenderPass::Map:
        m_pContext->RSSetState(m_rsMap.Get());
        m_pContext->OMSetDepthStencilState(m_dsMap.Get(), 0);
        break;
    case RenderPass::Skin_Base: {
        const float blendFactor[4] = {0, 0, 0, 0};
        m_pContext->RSSetState(m_rsSkinBase.Get());
        m_pContext->OMSetDepthStencilState(m_dsSkinBase.Get(), 0);
        m_pContext->OMSetBlendState(m_bsSkinBaseOpaque.Get(), blendFactor, 0xffffffff);
        break;
    }
    default:
        break;
    }
}

void RStateManager::ClearSRVs() {
    ID3D11ShaderResourceView* nullSRVs[8] = {nullptr};
    m_pContext->PSSetShaderResources(0, 8, nullSRVs);
}

void RStateManager::Reset() {
    const float blendFactor[4] = {0, 0, 0, 0};
    m_pContext->RSSetState(nullptr);
    m_pContext->OMSetDepthStencilState(nullptr, 0);
    m_pContext->OMSetBlendState(nullptr, blendFactor, 0xffffffff);
}

}
