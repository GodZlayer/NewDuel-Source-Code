#include "RDeviceDX11.h"
#include "AppLogger.h"
#include <d3dcompiler.h>
#include <algorithm>

#pragma comment(lib, "d3dcompiler.lib")

namespace RealSpace3 {

RDeviceDX11::RDeviceDX11() : m_width(0), m_height(0) {}
RDeviceDX11::~RDeviceDX11() {}

bool RDeviceDX11::Initialize(HWND hWnd, int width, int height) {
    m_width = width; m_height = height;
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1; sd.BufferDesc.Width = width; sd.BufferDesc.Height = height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &m_pSwapChain, &m_pd3dDevice, NULL, &m_pd3dDeviceContext);
    if (FAILED(hr)) return false;

    Resize(width, height);

    D3D11_DEPTH_STENCIL_DESC dsDesc3D = {};
    dsDesc3D.DepthEnable = TRUE; dsDesc3D.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dsDesc3D.DepthFunc = D3D11_COMPARISON_LESS;
    m_pd3dDevice->CreateDepthStencilState(&dsDesc3D, &m_pDepthStencilState3D);

    D3D11_RASTERIZER_DESC rsDesc3D = {};
    rsDesc3D.FillMode = D3D11_FILL_SOLID; rsDesc3D.CullMode = D3D11_CULL_NONE; rsDesc3D.DepthClipEnable = TRUE;
    m_pd3dDevice->CreateRasterizerState(&rsDesc3D, &m_pRasterizerState3D);

    CreateUITexture(width, height);
    return true;
}

void RDeviceDX11::Resize(int width, int height) {
    if (!m_pSwapChain || width == 0 || height == 0) return;
    m_width = width; m_height = height;
    m_pRenderTargetView.Reset(); m_pDepthStencilView.Reset();
    m_pSwapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    ComPtr<ID3D11Texture2D> pBackBuffer;
    m_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    m_pd3dDevice->CreateRenderTargetView(pBackBuffer.Get(), NULL, &m_pRenderTargetView);
    D3D11_TEXTURE2D_DESC descDepth = { (uint32_t)width, (uint32_t)height, 1, 1, DXGI_FORMAT_D24_UNORM_S8_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL, 0, 0 };
    ComPtr<ID3D11Texture2D> pDepthStencil;
    m_pd3dDevice->CreateTexture2D(&descDepth, NULL, &pDepthStencil);
    m_pd3dDevice->CreateDepthStencilView(pDepthStencil.Get(), NULL, &m_pDepthStencilView);
    CreateUITexture(width, height);
}

void RDeviceDX11::SetStandard3DStates() {
    float blendFactor[4] = { 0, 0, 0, 0 };
    m_pd3dDeviceContext->OMSetBlendState(NULL, blendFactor, 0xffffffff);
    m_pd3dDeviceContext->OMSetDepthStencilState(m_pDepthStencilState3D.Get(), 0);
    m_pd3dDeviceContext->RSSetState(m_pRasterizerState3D.Get());
    m_pd3dDeviceContext->OMSetRenderTargets(1, m_pRenderTargetView.GetAddressOf(), m_pDepthStencilView.Get());
    D3D11_VIEWPORT vp = { 0, 0, (float)m_width, (float)m_height, 0.0f, 1.0f };
    m_pd3dDeviceContext->RSSetViewports(1, &vp);
    ID3D11ShaderResourceView* nullSRVs[8] = {nullptr};
    m_pd3dDeviceContext->PSSetShaderResources(0, 8, nullSRVs);
}

void RDeviceDX11::DrawAtomicProof() {
    if (!m_pProofVS) {
        const char* vsCode = "float4 VS(float3 pos : POSITION) : SV_POSITION { return float4(pos, 1.0f); }";
        const char* psCode = "float4 PS() : SV_Target { return float4(0, 0, 1, 1); }"; 
        ComPtr<ID3DBlob> vsBlob, psBlob;
        D3DCompile(vsCode, strlen(vsCode), NULL, NULL, NULL, "VS", "vs_4_0", 0, 0, &vsBlob, NULL);
        D3DCompile(psCode, strlen(psCode), NULL, NULL, NULL, "PS", "ps_4_0", 0, 0, &psBlob, NULL);
        m_pd3dDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_pProofVS);
        m_pd3dDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_pProofPS);
        D3D11_INPUT_ELEMENT_DESC ied[] = { { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
        m_pd3dDevice->CreateInputLayout(ied, 1, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_pProofLayout);
        float tri[] = { 0.0f, 0.5f, 0.5f, 0.5f, -0.5f, 0.5f, -0.5f, -0.5f, 0.5f };
        D3D11_BUFFER_DESC bd = { sizeof(tri), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
        D3D11_SUBRESOURCE_DATA sd = { tri };
        m_pd3dDevice->CreateBuffer(&bd, &sd, &m_pProofVB);
        D3D11_DEPTH_STENCIL_DESC dsd = { FALSE, D3D11_DEPTH_WRITE_MASK_ZERO, D3D11_COMPARISON_ALWAYS, FALSE };
        m_pd3dDevice->CreateDepthStencilState(&dsd, &m_pProofDS);
    }
    m_pd3dDeviceContext->OMSetRenderTargets(1, m_pRenderTargetView.GetAddressOf(), NULL);
    D3D11_VIEWPORT vp = { 0, 0, (float)m_width, (float)m_height, 0.0f, 1.0f };
    m_pd3dDeviceContext->RSSetViewports(1, &vp);
    m_pd3dDeviceContext->OMSetDepthStencilState(m_pProofDS.Get(), 0);
    m_pd3dDeviceContext->RSSetState(m_pRasterizerState3D.Get()); 
    m_pd3dDeviceContext->IASetInputLayout(m_pProofLayout.Get());
    m_pd3dDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_pd3dDeviceContext->VSSetShader(m_pProofVS.Get(), NULL, 0);
    m_pd3dDeviceContext->PSSetShader(m_pProofPS.Get(), NULL, 0);
    UINT stride = 12, offset = 0;
    m_pd3dDeviceContext->IASetVertexBuffers(0, 1, m_pProofVB.GetAddressOf(), &stride, &offset);
    m_pd3dDeviceContext->Draw(3, 0);
}

void RDeviceDX11::CreateUITexture(int width, int height) {
    m_pUITexture.Reset(); m_pUISRV.Reset();
    D3D11_TEXTURE2D_DESC td = { (uint32_t)width, (uint32_t)height, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE, D3D11_CPU_ACCESS_WRITE, 0 };
    m_pd3dDevice->CreateTexture2D(&td, NULL, &m_pUITexture);
    m_pd3dDevice->CreateShaderResourceView(m_pUITexture.Get(), NULL, &m_pUISRV);
    if (!m_pUIVS) {
        const char* vsCode = "struct VS_IN { float4 pos : POSITION; float2 uv : TEXCOORD; }; struct PS_IN { float4 pos : SV_POSITION; float2 uv : TEXCOORD; }; PS_IN VS(VS_IN input) { return (PS_IN)input; }";
        const char* psCode = "Texture2D tex : register(t0); SamplerState samp : register(s0); struct PS_IN { float4 pos : SV_POSITION; float2 uv : TEXCOORD; }; float4 PS(PS_IN input) : SV_Target { return tex.Sample(samp, input.uv); }";
        ComPtr<ID3DBlob> vsBlob, psBlob;
        D3DCompile(vsCode, strlen(vsCode), NULL, NULL, NULL, "VS", "vs_4_0", 0, 0, &vsBlob, NULL);
        D3DCompile(psCode, strlen(psCode), NULL, NULL, NULL, "PS", "ps_4_0", 0, 0, &psBlob, NULL);
        m_pd3dDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), NULL, &m_pUIVS);
        m_pd3dDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), NULL, &m_pUIPS);
        D3D11_INPUT_ELEMENT_DESC ied[] = { { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 }, { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
        m_pd3dDevice->CreateInputLayout(ied, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_pUIInputLayout);
        D3D11_SAMPLER_DESC sampDesc = {}; sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sampDesc.AddressU = sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        m_pd3dDevice->CreateSamplerState(&sampDesc, &m_pUISampler);
        D3D11_BLEND_DESC blendDesc = {};
        blendDesc.RenderTarget[0].BlendEnable = TRUE; blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD; blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD; blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        m_pd3dDevice->CreateBlendState(&blendDesc, &m_pUIBlendState);
        D3D11_RASTERIZER_DESC rasterDesc = { D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0.0f, 0.0f, TRUE, FALSE, FALSE, FALSE };
        m_pd3dDevice->CreateRasterizerState(&rasterDesc, &m_pUIRasterizerState);
        D3D11_DEPTH_STENCIL_DESC dsDesc = { FALSE, D3D11_DEPTH_WRITE_MASK_ZERO, D3D11_COMPARISON_ALWAYS, FALSE };
        m_pd3dDevice->CreateDepthStencilState(&dsDesc, &m_pUIDepthStencilState);
    }
    struct Vertex { float x, y, z, w, u, v; };
    Vertex vertices[] = { { -1, 1, 0, 1, 0, 0 }, { 1, 1, 0, 1, 1, 0 }, { -1, -1, 0, 1, 0, 1 }, { -1, -1, 0, 1, 0, 1 }, { 1, 1, 0, 1, 1, 0 }, { 1, -1, 0, 1, 1, 1 } };
    D3D11_BUFFER_DESC bd = { sizeof(vertices), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA initData = { vertices };
    m_pUIVB.Reset();
    m_pd3dDevice->CreateBuffer(&bd, &initData, &m_pUIVB);
}

void RDeviceDX11::UpdateUITexture(unsigned char* pixels, uint32_t rowPitch, uint32_t uiW, uint32_t uiH) {
    if (!pixels || !m_pUITexture) return;
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(m_pd3dDeviceContext->Map(m_pUITexture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        uint32_t copyW = (std::min)(uiW * 4, (uint32_t)mapped.RowPitch);
        uint32_t copyH = (std::min)((uint32_t)m_height, uiH);
        for (uint32_t y = 0; y < copyH; y++) memcpy((unsigned char*)mapped.pData + y * mapped.RowPitch, pixels + y * rowPitch, copyW);
        m_pd3dDeviceContext->Unmap(m_pUITexture.Get(), 0);
    }
}

void RDeviceDX11::DrawUI() {
    if (!m_pUISRV || !m_pUIInputLayout) return;
    m_pd3dDeviceContext->OMSetRenderTargets(1, m_pRenderTargetView.GetAddressOf(), nullptr);
    D3D11_VIEWPORT vp = { 0, 0, (float)m_width, (float)m_height, 0.0f, 1.0f };
    m_pd3dDeviceContext->RSSetViewports(1, &vp);
    unsigned int stride = sizeof(float) * 6, offset = 0;
    m_pd3dDeviceContext->IASetInputLayout(m_pUIInputLayout.Get());
    m_pd3dDeviceContext->IASetVertexBuffers(0, 1, m_pUIVB.GetAddressOf(), &stride, &offset);
    m_pd3dDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_pd3dDeviceContext->VSSetShader(m_pUIVS.Get(), NULL, 0);
    m_pd3dDeviceContext->PSSetShader(m_pUIPS.Get(), NULL, 0);
    m_pd3dDeviceContext->PSSetShaderResources(0, 1, m_pUISRV.GetAddressOf());
    m_pd3dDeviceContext->PSSetSamplers(0, 1, m_pUISampler.GetAddressOf());
    m_pd3dDeviceContext->RSSetState(m_pUIRasterizerState.Get());
    m_pd3dDeviceContext->OMSetDepthStencilState(m_pUIDepthStencilState.Get(), 0);
    float blendFactor[4] = { 0, 0, 0, 0 };
    m_pd3dDeviceContext->OMSetBlendState(m_pUIBlendState.Get(), blendFactor, 0xffffffff);
    m_pd3dDeviceContext->Draw(6, 0);
    m_pd3dDeviceContext->OMSetBlendState(NULL, blendFactor, 0xffffffff);
}

void RDeviceDX11::Clear(float r, float g, float b, float a) {
    const float color[] = { r, g, b, a };
    if (m_pRenderTargetView) m_pd3dDeviceContext->ClearRenderTargetView(m_pRenderTargetView.Get(), color);
    if (m_pDepthStencilView) m_pd3dDeviceContext->ClearDepthStencilView(m_pDepthStencilView.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
}

bool RDeviceDX11::ReadBackBufferBGRA(std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight) {
    outPixels.clear();
    outWidth = 0;
    outHeight = 0;
    if (!m_pSwapChain || !m_pd3dDevice || !m_pd3dDeviceContext) return false;

    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(m_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) || !backBuffer) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    backBuffer->GetDesc(&desc);
    if (desc.Width == 0 || desc.Height == 0) return false;

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(m_pd3dDevice->CreateTexture2D(&stagingDesc, nullptr, &staging)) || !staging) {
        return false;
    }

    m_pd3dDeviceContext->CopyResource(staging.Get(), backBuffer.Get());
    m_pd3dDeviceContext->Flush();

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(m_pd3dDeviceContext->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }

    outWidth = desc.Width;
    outHeight = desc.Height;
    outPixels.resize(static_cast<size_t>(outWidth) * static_cast<size_t>(outHeight) * 4u);

    const bool isRgba = (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const bool isBgra = (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);

    for (uint32_t y = 0; y < outHeight; ++y) {
        const uint8_t* srcRow = reinterpret_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(mapped.RowPitch) * y;
        uint8_t* dstRow = outPixels.data() + static_cast<size_t>(outWidth) * 4u * y;
        for (uint32_t x = 0; x < outWidth; ++x) {
            const uint8_t* src = srcRow + static_cast<size_t>(x) * 4u;
            uint8_t* dst = dstRow + static_cast<size_t>(x) * 4u;
            if (isRgba) {
                dst[0] = src[2];
                dst[1] = src[1];
                dst[2] = src[0];
                dst[3] = src[3];
            } else if (isBgra) {
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = src[3];
            } else {
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = src[3];
            }
        }
    }

    m_pd3dDeviceContext->Unmap(staging.Get(), 0);
    return true;
}

void RDeviceDX11::Present() { if (m_pSwapChain) m_pSwapChain->Present(1, 0); }

}
