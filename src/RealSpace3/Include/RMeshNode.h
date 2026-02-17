#pragma once
#include <d3d11.h>
#include <directxmath.h>
#include <string>
#include <vector>
#include <memory>
#include <wrl/client.h>
#include "Types.h" // Incluir definicao unica

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace RealSpace3 {

class RMesh;

class RMeshNode {
public:
    RMeshNode(const std::string& name);
    ~RMeshNode() = default;

    void UpdateMatrices(const XMMATRIX& parentMatrix);
    void Draw(ID3D11DeviceContext* context, ID3D11Buffer* constantBuffer);

    std::string m_name;
    XMMATRIX m_localMatrix;
    XMMATRIX m_combinedMatrix;
    XMFLOAT3 m_position = { 0,0,0 };
    XMFLOAT3 m_rotation = { 0,0,0 };
    XMFLOAT3 m_scale = { 1,1,1 };
    
    bool m_isDirty = true;
    
    ComPtr<ID3D11Buffer> m_vertexBuffer;
    ComPtr<ID3D11Buffer> m_indexBuffer;
    UINT m_indexCount = 0;
    
    ID3D11ShaderResourceView* m_pTextureSRV = nullptr;

    std::shared_ptr<RMesh> m_pMesh; 
    std::vector<std::shared_ptr<RMeshNode>> m_children;
};

}