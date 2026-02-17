#pragma once
#include <d3d11.h>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <wrl/client.h>
#include "RMeshNode.h"

using Microsoft::WRL::ComPtr;

namespace RealSpace3 {

class RMesh {
public:
    RMesh(ID3D11Device* device);
    ~RMesh() = default;

    bool LoadELU(const std::string& filename);
    void Update(float deltaTime);
    void Draw(ID3D11DeviceContext* context, ID3D11Buffer* constantBuffer);

    std::shared_ptr<RMeshNode> FindNode(const std::string& name);

private:
    void MapNodes(std::shared_ptr<RMeshNode> node);

    ID3D11Device* m_pd3dDevice;
    std::shared_ptr<RMeshNode> m_pRootNode;
    std::map<std::string, std::shared_ptr<RMeshNode>> m_nodeMap;
};

}