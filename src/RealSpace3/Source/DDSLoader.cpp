#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../Include/DDSLoader.h"
#include "AppLogger.h"
#include <fstream>
#include <vector>
#include <algorithm>
#include <wincodec.h>

#pragma comment(lib, "windowscodecs.lib")

namespace RealSpace3 {

// DDS file structures
#pragma pack(push, 1)
struct DDS_PIXELFORMAT {
    uint32_t dwSize;
    uint32_t dwFlags;
    uint32_t dwFourCC;
    uint32_t dwRGBBitCount;
    uint32_t dwRBitMask;
    uint32_t dwGBitMask;
    uint32_t dwBBitMask;
    uint32_t dwABitMask;
};

struct DDS_HEADER {
    uint32_t dwSize;
    uint32_t dwFlags;
    uint32_t dwHeight;
    uint32_t dwWidth;
    uint32_t dwPitchOrLinearSize;
    uint32_t dwDepth;
    uint32_t dwMipMapCount;
    uint32_t dwReserved1[11];
    DDS_PIXELFORMAT ddspf;
    uint32_t dwCaps;
    uint32_t dwCaps2;
    uint32_t dwCaps3;
    uint32_t dwCaps4;
    uint32_t dwReserved2;
};
#pragma pack(pop)

constexpr uint32_t DDS_MAGIC = 0x20534444; // "DDS "
constexpr uint32_t DDPF_FOURCC = 0x4;
constexpr uint32_t DDPF_RGB = 0x40;
constexpr uint32_t DDPF_ALPHAPIXELS = 0x1;

#ifndef MAKEFOURCC
#define MAKEFOURCC(a, b, c, d) ((uint32_t)(uint8_t)(a) | ((uint32_t)(uint8_t)(b) << 8) | ((uint32_t)(uint8_t)(c) << 16) | ((uint32_t)(uint8_t)(d) << 24))
#endif

DXGI_FORMAT DDSLoader::GetDXGIFormat(uint32_t fourCC, uint32_t rgbBitCount, uint32_t rMask, uint32_t gMask, uint32_t bMask, uint32_t aMask) {
    if (fourCC == MAKEFOURCC('D','X','T','1')) return DXGI_FORMAT_BC1_UNORM;
    if (fourCC == MAKEFOURCC('D','X','T','2') || fourCC == MAKEFOURCC('D','X','T','3')) return DXGI_FORMAT_BC2_UNORM;
    if (fourCC == MAKEFOURCC('D','X','T','4') || fourCC == MAKEFOURCC('D','X','T','5')) return DXGI_FORMAT_BC3_UNORM;

    // Uncompressed
    if (rgbBitCount == 32) {
        if (rMask == 0x00FF0000 && gMask == 0x0000FF00 && bMask == 0x000000FF && aMask == 0xFF000000)
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        if (rMask == 0x000000FF && gMask == 0x0000FF00 && bMask == 0x00FF0000 && aMask == 0xFF000000)
            return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    if (rgbBitCount == 24) {
        // 24-bit BGR — we'll need to convert, but for now use BGRA
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    }

    return DXGI_FORMAT_UNKNOWN;
}

size_t DDSLoader::BitsPerPixel(DXGI_FORMAT fmt) {
    switch(fmt) {
    case DXGI_FORMAT_BC1_UNORM: return 4;
    case DXGI_FORMAT_BC2_UNORM: return 8;
    case DXGI_FORMAT_BC3_UNORM: return 8;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM: return 32;
    default: return 32;
    }
}

void DDSLoader::GetSurfaceInfo(size_t width, size_t height, DXGI_FORMAT fmt, size_t& numBytes, size_t& rowBytes, size_t& numRows) {
    bool bc = false;
    size_t bcnumBytesPerBlock = 0;

    switch(fmt) {
    case DXGI_FORMAT_BC1_UNORM: bc = true; bcnumBytesPerBlock = 8; break;
    case DXGI_FORMAT_BC2_UNORM: bc = true; bcnumBytesPerBlock = 16; break;
    case DXGI_FORMAT_BC3_UNORM: bc = true; bcnumBytesPerBlock = 16; break;
    default: break;
    }

    if (bc) {
        size_t numBlocksWide = (std::max)(size_t(1), (width + 3) / 4);
        size_t numBlocksHigh = (std::max)(size_t(1), (height + 3) / 4);
        rowBytes = numBlocksWide * bcnumBytesPerBlock;
        numRows = numBlocksHigh;
        numBytes = rowBytes * numBlocksHigh;
    } else {
        size_t bpp = BitsPerPixel(fmt);
        rowBytes = (width * bpp + 7) / 8;
        numRows = height;
        numBytes = rowBytes * height;
    }
}

HRESULT DDSLoader::LoadFromFile(ID3D11Device* device, const std::wstring& filePath, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSRV) {
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return E_FAIL;

    size_t fileSize = (size_t)file.tellg();
    file.seekg(0, std::ios::beg);

    if (fileSize < sizeof(uint32_t) + sizeof(DDS_HEADER)) return E_FAIL;

    std::vector<uint8_t> data(fileSize);
    file.read(reinterpret_cast<char*>(data.data()), fileSize);
    file.close();

    // Verify magic
    uint32_t magic = *reinterpret_cast<uint32_t*>(data.data());
    if (magic != DDS_MAGIC) {
        // Not a DDS file, try WIC
        return LoadWICFromFile(device, filePath, outSRV);
    }

    const DDS_HEADER* header = reinterpret_cast<const DDS_HEADER*>(data.data() + 4);
    if (header->dwSize != 124) return E_FAIL;

    uint32_t width = header->dwWidth;
    uint32_t height = header->dwHeight;
    uint32_t mipCount = (std::max)(header->dwMipMapCount, 1u);

    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

    if (header->ddspf.dwFlags & DDPF_FOURCC) {
        format = GetDXGIFormat(header->ddspf.dwFourCC, 0, 0, 0, 0, 0);
    } else if (header->ddspf.dwFlags & (DDPF_RGB | DDPF_ALPHAPIXELS)) {
        format = GetDXGIFormat(0, header->ddspf.dwRGBBitCount, header->ddspf.dwRBitMask, header->ddspf.dwGBitMask, header->ddspf.dwBBitMask, header->ddspf.dwABitMask);
    }

    if (format == DXGI_FORMAT_UNKNOWN) {
        // Fallback: try WIC
        return LoadWICFromFile(device, filePath, outSRV);
    }

    size_t dataOffset = 4 + sizeof(DDS_HEADER);
    // Check for DX10 extended header
    if (header->ddspf.dwFourCC == MAKEFOURCC('D','X','1','0')) {
        dataOffset += 20; // DDS_HEADER_DXT10
    }

    const uint8_t* pixelData = data.data() + dataOffset;
    size_t pixelDataSize = fileSize - dataOffset;

    // Handle 24-bit BGR by converting to 32-bit BGRA
    std::vector<uint8_t> convertedData;
    if (header->ddspf.dwRGBBitCount == 24 && !(header->ddspf.dwFlags & DDPF_FOURCC)) {
        size_t pixelCount = width * height;
        convertedData.resize(pixelCount * 4);
        for (size_t i = 0; i < pixelCount && i * 3 + 2 < pixelDataSize; ++i) {
            convertedData[i * 4 + 0] = pixelData[i * 3 + 0]; // B
            convertedData[i * 4 + 1] = pixelData[i * 3 + 1]; // G
            convertedData[i * 4 + 2] = pixelData[i * 3 + 2]; // R
            convertedData[i * 4 + 3] = 255;                    // A
        }
        pixelData = convertedData.data();
        pixelDataSize = convertedData.size();
        format = DXGI_FORMAT_B8G8R8A8_UNORM;
        mipCount = 1; // Only base level for converted
    }

    // Create texture with first mip level only for simplicity
    size_t numBytes, rowBytes, numRows;
    GetSurfaceInfo(width, height, format, numBytes, rowBytes, numRows);

    if (numBytes > pixelDataSize) {
        return E_FAIL;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1; // Only top mip
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = pixelData;
    initData.SysMemPitch = (UINT)rowBytes;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    HRESULT hr = device->CreateTexture2D(&desc, &initData, &tex);
    if (FAILED(hr)) return hr;

    hr = device->CreateShaderResourceView(tex.Get(), nullptr, &outSRV);
    return hr;
}

HRESULT DDSLoader::LoadWICFromFile(ID3D11Device* device, const std::wstring& filePath, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSRV) {
    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory));
    if (FAILED(hr)) return hr;

    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    hr = wicFactory->CreateDecoderFromFilename(filePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) return hr;

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) return hr;

    UINT width, height;
    frame->GetSize(&width, &height);

    // Convert to BGRA
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    hr = wicFactory->CreateFormatConverter(&converter);
    if (FAILED(hr)) return hr;

    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return hr;

    std::vector<uint8_t> pixels(width * height * 4);
    hr = converter->CopyPixels(nullptr, width * 4, (UINT)pixels.size(), pixels.data());
    if (FAILED(hr)) return hr;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = pixels.data();
    initData.SysMemPitch = width * 4;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    hr = device->CreateTexture2D(&desc, &initData, &tex);
    if (FAILED(hr)) return hr;

    hr = device->CreateShaderResourceView(tex.Get(), nullptr, &outSRV);
    return hr;
}

}
