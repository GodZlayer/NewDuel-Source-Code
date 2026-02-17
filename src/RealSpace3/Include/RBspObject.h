#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <cstdint>
#include <map>
#include <memory>
#include <DirectXMath.h>
#include "Types.h"
#include "TextureManager.h"

namespace RealSpace3 {

inline constexpr uint32_t MAX_BSP_DEPTH = 1024;
inline constexpr int32_t NODE_INDEX_NULL = -1;

enum class RenderMode {
    Baseline,
    BSP_Traversal
};

struct RDummy {
    std::string Name;
    DirectX::XMFLOAT3 Position;
    DirectX::XMFLOAT3 Direction;
};

struct RBspMaterialMeta {
    uint32_t dwFlags = 0;
    float uSpeed = 0.0f;
    float vSpeed = 0.0f;
    float alphaRef = 0.5f;
};

struct RBspSubset {
    uint32_t materialId;
    uint32_t lightmapId;
    uint32_t startIndex;
    uint32_t indexCount;
    uint32_t dwFlags;
    float uSpeed; 
    float vSpeed;
};

struct RSBspNode {
    int32_t nPolygon;
    uint32_t nSubsetIdx;
    uint32_t nSubsetCount;
    int32_t nPositiveIdx;
    int32_t nNegativeIdx;
    int nFrameCount;
    rplane plane;
    rboundingbox bbTree;
    uint32_t dwFlags;

    RSBspNode();
    ~RSBspNode();
};

struct MaterialRuntime {
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> diffuseSRV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> lightmapSRV;
    uint32_t dwFlags;
    float alphaRef;
};

struct MapStats {
    uint32_t nNodes = 0;
    uint32_t nLeafNodes = 0;
    uint32_t nPolygons = 0;
    uint32_t nVertices = 0;
    uint32_t nIndices = 0;
    uint32_t nMaterials = 0;
    uint32_t nLightmaps = 0;
    uint32_t maxDepth = 0;
    uint64_t traversalChecksum = 0;
    rvector vMin = {0,0,0}, vMax = {0,0,0};
};

struct RenderStats {
    uint32_t nDrawCalls = 0;
    uint32_t nVisiblePolygons = 0;
    uint32_t nVisitedNodes = 0;
    uint32_t nVisibleLeaves = 0;
    uint32_t nVisibleSubsets = 0;
    uint32_t nTrianglesDrawn = 0;
};

class RBspObject {
public:
    RBspObject(ID3D11Device* device, TextureManager* pTexMgr);
    ~RBspObject();

    bool Open(const std::string& filename);
    void Draw(ID3D11DeviceContext* context, const DirectX::FXMMATRIX& viewProj, const rfrustum& frustum, RenderMode mode = RenderMode::BSP_Traversal, bool bWireframe = false);
    
    std::vector<RDummy>* GetDummyList() { return &m_dummies; }
    void Update(float dt);

    const MapStats& GetStats() const { return m_stats; }
    const RenderStats& GetRenderStats() const { return m_renderStats; }
    const char* GetLastOpenError() const { return m_lastError.c_str(); }

private:
    bool OpenRs(const std::vector<uint8_t>& buffer);
    bool OpenLightmap(const std::string& filename);
    bool LoadXML(const std::string& filename);
    bool LoadXMLMaterials(const std::string& xmlPath);
    bool CreateDX11Resources();
    
    int Open_Nodes(const std::vector<uint8_t>& buffer, size_t& offset, int depth, std::map<int, std::vector<unsigned short>>& materialMap);
    void UpdateTraversalChecksum(const RSBspNode& node);

    bool IsVisible(const rboundingbox& bb, const rfrustum& frustum);
    void RenderNode(ID3D11DeviceContext* context, int32_t nodeIdx, const rfrustum& frustum);
    void DrawSubsets(ID3D11DeviceContext* context, const std::vector<uint32_t>& subsetIndices);

    ID3D11Device* m_pd3dDevice;
    TextureManager* m_pTextureManager;
    std::map<int32_t, MaterialRuntime> m_materials;
    std::map<std::string, RBspMaterialMeta> m_materialMeta;
    std::vector<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> m_lightmaps;
    std::vector<RDummy> m_dummies;

    std::vector<RSBspNode> m_OcRoot;
    std::vector<RPOLYGONINFO> m_OcInfo;
    std::vector<BSPVERTEX> m_OcVertices;
    std::vector<unsigned short> m_OcIndices;
    std::vector<RBspSubset> m_Subsets;

    std::vector<uint32_t> m_opaqueQueue;
    std::vector<uint32_t> m_alphaTestQueue;
    std::vector<uint32_t> m_alphaBlendQueue;
    std::vector<uint32_t> m_additiveQueue;

    struct DrawSubset {
        uint32_t MaterialID;
        uint32_t IndexStart;
        uint32_t IndexCount;
    };

    MapStats m_stats;
    RenderStats m_renderStats;

    Microsoft::WRL::ComPtr<ID3D11Buffer> m_vertexBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_indexBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_constantBuffer;
    
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixelShader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_inputLayout;

    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rsSolid;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rsWireframe;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_blendState;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_dsLessEqual;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_dsNoWrite;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_samplerState;

    // Multi-Material Support
    std::vector<DrawSubset> m_drawSubsetsVector;
    std::vector<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> m_materialSRVs;
    
    std::string m_lastError;
    std::string m_filename;
    std::string m_baseDirectory; // directory containing the .RS file
    float m_globalTime = 0.0f;
};

}