#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include <cstdint>

namespace RealSpace3 {

// Minimal DDS loader for GunZ texture formats (DXT1, DXT3, DXT5, uncompressed BGRA)
// Also supports BMP/PNG/TGA/JPG via WIC fallback
class DDSLoader {
public:
    static HRESULT LoadFromFile(
        ID3D11Device* device,
        const std::wstring& filePath,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSRV
    );

    static HRESULT LoadWICFromFile(
        ID3D11Device* device,
        const std::wstring& filePath,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSRV
    );

private:
    static DXGI_FORMAT GetDXGIFormat(uint32_t fourCC, uint32_t rgbBitCount, uint32_t rMask, uint32_t gMask, uint32_t bMask, uint32_t aMask);
    static size_t BitsPerPixel(DXGI_FORMAT fmt);
    static void GetSurfaceInfo(size_t width, size_t height, DXGI_FORMAT fmt, size_t& numBytes, size_t& rowBytes, size_t& numRows);
};

}
