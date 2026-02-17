#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include <map>
#include <memory>

namespace RealSpace3 {

class TextureManager {
public:
    TextureManager(ID3D11Device* device);
    ~TextureManager();

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> GetTexture(const std::string& path);
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> CreateTextureFromMemory(const uint8_t* data, size_t size);
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> GetWhiteTexture() { return m_whiteSRV; }
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> GetFallbackTexture() { return m_fallbackSRV; }
    
    void SetBaseDirectory(const std::string& dir) { m_baseDirectory = dir; }
    const std::string& GetBaseDirectory() const { return m_baseDirectory; }
    
    void Clear();

private:
    std::string NormalizePath(const std::string& path);
    std::wstring ToWide(const std::string& str);
    bool TryLoadTexture(const std::string& path, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSRV);
    void CreateDefaultTextures();

    ID3D11Device* m_pd3dDevice;
    std::map<std::string, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> m_textureCache;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_fallbackSRV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_whiteSRV;
    std::string m_baseDirectory;
};

}