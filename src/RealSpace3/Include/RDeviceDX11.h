#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace RealSpace3 {

class RDeviceDX11 {
public:
    RDeviceDX11();
    ~RDeviceDX11();

    bool Initialize(HWND hWnd, int width, int height);
    void Resize(int width, int height); 
    void Clear(float r, float g, float b, float a);
    void Present();

    void CreateUITexture(int width, int height);
    void UpdateUITexture(unsigned char* pixels, uint32_t rowPitch, uint32_t uiW, uint32_t uiH);
    void DrawUI();

    void SetStandard3DStates();
    void DrawAtomicProof(); // PROVA DE VIDA DO RASTERIZADOR
    bool ReadBackBufferBGRA(std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight);

    int GetWidth() { return m_width; }
    int GetHeight() { return m_height; }
    ID3D11Device* GetDevice() { return m_pd3dDevice.Get(); }
    ID3D11DeviceContext* GetContext() { return m_pd3dDeviceContext.Get(); }

private:
    ComPtr<ID3D11Device>            m_pd3dDevice;
    ComPtr<ID3D11DeviceContext>     m_pd3dDeviceContext;
    ComPtr<IDXGISwapChain>          m_pSwapChain;
    ComPtr<ID3D11RenderTargetView>  m_pRenderTargetView;
    ComPtr<ID3D11DepthStencilView>  m_pDepthStencilView;

    ComPtr<ID3D11DepthStencilState> m_pDepthStencilState3D;
    ComPtr<ID3D11RasterizerState>   m_pRasterizerState3D;

    // UI Resources
    ComPtr<ID3D11Texture2D>          m_pUITexture;
    ComPtr<ID3D11ShaderResourceView> m_pUISRV;
    ComPtr<ID3D11VertexShader>       m_pUIVS;
    ComPtr<ID3D11PixelShader>        m_pUIPS;
    ComPtr<ID3D11SamplerState>       m_pUISampler;
    ComPtr<ID3D11Buffer>             m_pUIVB;
    ComPtr<ID3D11BlendState>         m_pUIBlendState;
    ComPtr<ID3D11RasterizerState>    m_pUIRasterizerState;
    ComPtr<ID3D11DepthStencilState> m_pUIDepthStencilState;
    ComPtr<ID3D11InputLayout>       m_pUIInputLayout;

    // Proof Resources
    ComPtr<ID3D11Buffer>             m_pProofVB;
    ComPtr<ID3D11VertexShader>       m_pProofVS;
    ComPtr<ID3D11PixelShader>        m_pProofPS;
    ComPtr<ID3D11InputLayout>       m_pProofLayout;
    ComPtr<ID3D11DepthStencilState> m_pProofDS;

    int m_width;
    int m_height;
};

}
