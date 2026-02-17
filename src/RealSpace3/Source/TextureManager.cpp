#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../Include/TextureManager.h"
#include "../Include/DDSLoader.h"
#include "AppLogger.h"
#include <algorithm>
#include <vector>
#include <wincodec.h>
#include <windows.h>

#pragma comment(lib, "windowscodecs.lib")

namespace RealSpace3 {

TextureManager::TextureManager(ID3D11Device* device) : m_pd3dDevice(device) {
    CreateDefaultTextures();
}

TextureManager::~TextureManager() {
    Clear();
}

std::string TextureManager::NormalizePath(const std::string& path) {
    std::string normalized = path;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::tolower);
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    // Remove leading/trailing whitespace
    while (!normalized.empty() && normalized.front() == ' ') normalized.erase(normalized.begin());
    while (!normalized.empty() && normalized.back() == ' ') normalized.pop_back();
    return normalized;
}

std::wstring TextureManager::ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int sz = MultiByteToWideChar(CP_ACP, 0, str.c_str(), (int)str.size(), nullptr, 0);
    std::wstring wstr(sz, 0);
    MultiByteToWideChar(CP_ACP, 0, str.c_str(), (int)str.size(), &wstr[0], sz);
    return wstr;
}

void TextureManager::CreateDefaultTextures() {
    // Magenta checkerboard (fallback = missing texture indicator)
    uint32_t magenta[4] = { 0xFFFF00FF, 0xFF000000, 0xFF000000, 0xFFFF00FF };
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 2; desc.Height = 2; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data = { magenta, 8, 0 };
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texM;
    m_pd3dDevice->CreateTexture2D(&desc, &data, &texM);
    m_pd3dDevice->CreateShaderResourceView(texM.Get(), nullptr, &m_fallbackSRV);

    uint32_t white[4] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
    data.pSysMem = white;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texW;
    m_pd3dDevice->CreateTexture2D(&desc, &data, &texW);
    m_pd3dDevice->CreateShaderResourceView(texW.Get(), nullptr, &m_whiteSRV);
}

bool TextureManager::TryLoadTexture(const std::string& path, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSRV) {
    std::wstring wpath = ToWide(path);
    
    // Check if file exists
    DWORD attrs = GetFileAttributesW(wpath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    
    HRESULT hr = DDSLoader::LoadFromFile(m_pd3dDevice, wpath, outSRV);
    if (SUCCEEDED(hr) && outSRV) {
        return true;
    }
    return false;
}

Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> TextureManager::CreateTextureFromMemory(const uint8_t* data, size_t size) {
    // TODO: Implement from-memory loading if needed
    return m_whiteSRV;
}

Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> TextureManager::GetTexture(const std::string& path) {
    if (path.empty()) return m_fallbackSRV;
    
    std::string key = NormalizePath(path);
    
    // Check cache
    auto it = m_textureCache.find(key);
    if (it != m_textureCache.end()) return it->second;
    
    // Build list of candidates
    std::vector<std::string> candidates;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    
    // Strategy 0: Direct path as provided (relative to app root)
    if (TryLoadTexture(path, srv)) {
        AppLogger::Log("[TextureManager] Loaded (Direct): " + path);
        m_textureCache[key] = srv;
        return srv;
    }
    candidates.push_back(path + ".dds");

    std::string fullPath = m_baseDirectory.empty() ? path : (m_baseDirectory + "/" + path);
    // Strategy 1: Map-relative path (e.g., "Maps/mansion/texture.bmp")
    candidates.push_back(fullPath);
    candidates.push_back(fullPath + ".dds");
    
    // Strategy 3: Just the filename in base directory
    std::string filename = path;
    auto lastSlash = filename.find_last_of("/\\");
    if (lastSlash != std::string::npos) filename = filename.substr(lastSlash + 1);
    if (!m_baseDirectory.empty()) {
        candidates.push_back(m_baseDirectory + "/" + filename);
        candidates.push_back(m_baseDirectory + "/" + filename + ".dds");
    }
    
    for (const auto& candidate : candidates) {
        if (TryLoadTexture(candidate, srv)) {
            AppLogger::Log("[TextureManager] Loaded: " + candidate);
            m_textureCache[key] = srv;
            return srv;
        }
    }
    
    // Failed to load — cache the fallback to avoid re-trying
    AppLogger::Log("[TextureManager] MISS: " + path + " (tried " + std::to_string(candidates.size()) + " paths)");
    m_textureCache[key] = m_fallbackSRV;
    return m_fallbackSRV;
}

void TextureManager::Clear() {
    m_textureCache.clear();
}

}
