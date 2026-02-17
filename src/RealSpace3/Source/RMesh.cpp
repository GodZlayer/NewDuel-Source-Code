#include "../Include/RMesh.h"
#include "../Include/RMeshNode.h"
#include "../Include/TextureManager.h"
#include "AppLogger.h"
#include <fstream>
#include <vector>

namespace RealSpace3 {

// Funcoes de baixo nivel para garantir alinhamento de 4 bytes (Padrao RS2)
bool ReadInt(std::ifstream& f, uint32_t& i) { return (bool)f.read((char*)&i, 4); }
bool ReadFloat(std::ifstream& f, float& fl) { return (bool)f.read((char*)&fl, 4); }

std::string ReadString(std::ifstream& f) {
    uint32_t len = 0;
    if (!ReadInt(f, len) || len == 0 || len > 1024) return "";
    std::vector<char> buf(len);
    f.read(buf.data(), len);
    return std::string(buf.begin(), buf.end());
}

RMesh::RMesh(ID3D11Device* device) : m_pd3dDevice(device) {
    m_pRootNode = std::make_shared<RMeshNode>("root");
}

bool RMesh::LoadELU(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) return false;

    uint32_t magic, version, matCount, meshCount;
    ReadInt(file, magic);   // 0x50455352 (RSPE)
    ReadInt(file, version); // 0x5007
    ReadInt(file, matCount);
    ReadInt(file, meshCount);

    AppLogger::Log("RS3: Carregando ELU v" + std::to_string(version) + " [" + filename + "]");

    // 1. Materiais (RS2 Parity)
    for (uint32_t i = 0; i < matCount; i++) {
        uint32_t id, subCount;
        ReadInt(file, id);
        ReadInt(file, subCount);
        for (uint32_t j = 0; j < subCount; j++) {
            file.seekg(40, std::ios::cur); // Ambient, Diffuse, Specular, Power (40 bytes)
            ReadString(file); // Diffuse Map
            ReadString(file); // Alpha Map
        }
    }

    // 2. Meshes
    for (uint32_t i = 0; i < meshCount; i++) {
        std::string name = ReadString(file);
        std::string parent = ReadString(file);
        auto node = std::make_shared<RMeshNode>(name);
        
        file.read((char*)&node->m_localMatrix, 64);
        node->m_isDirty = false;

        // --- VERTEX DATA (O Coracao do RS2) ---
        uint32_t posCount; ReadInt(file, posCount);
        std::vector<XMFLOAT3> pos(posCount);
        if (posCount > 0) file.read((char*)pos.data(), posCount * 12);

        uint32_t normCount; ReadInt(file, normCount);
        std::vector<XMFLOAT3> norm(normCount);
        if (normCount > 0) file.read((char*)norm.data(), normCount * 12);

        // NOVIDADE: Algumas versoes 5007 tem Tangentes ou Pesos de Bone aqui.
        // O original pula se nao for usado. Vamos tentar detectar o desalinhamento.
        // Se o proximo int for muito grande, precisamos saltar o bloco de tangentes.
        uint32_t nextBlock; ReadInt(file, nextBlock);
        if (nextBlock > 100000) { // Provavel desalinhamento
             file.seekg(-4, std::ios::cur); 
        } else {
             file.seekg(nextBlock * 16, std::ios::cur); // Pula Tangentes
        }

        uint32_t uvCount; ReadInt(file, uvCount);
        std::vector<XMFLOAT2> uvs(uvCount);
        if (uvCount > 0) file.read((char*)uvs.data(), uvCount * 8);

        // --- FACE DATA ---
        uint32_t faceCount; ReadInt(file, faceCount);
        if (faceCount > 0 && !pos.empty()) {
            std::vector<WORD> indices;
            for (uint32_t j = 0; j < faceCount; j++) {
                uint32_t idx[3];
                file.read((char*)idx, 12);
                indices.push_back((WORD)idx[0]); 
                indices.push_back((WORD)idx[2]); 
                indices.push_back((WORD)idx[1]);
                file.seekg(28, std::ios::cur); // Atributos RS2 (Lightmap, ID, etc)
            }

            // Reconstrucao para DX11
            std::vector<RVertex> finalVerts;
            for (size_t j = 0; j < pos.size(); j++) {
                RVertex v;
                v.Pos = { pos[j].x * 0.01f, pos[j].z * 0.01f, pos[j].y * 0.01f };
                v.Normal = (j < norm.size()) ? XMFLOAT3(norm[j].x, norm[j].z, norm[j].y) : XMFLOAT3(0,1,0);
                v.Tex = (j < uvs.size()) ? uvs[j] : XMFLOAT2(0,0);
                finalVerts.push_back(v);
            }

            D3D11_BUFFER_DESC vbd = { (UINT)(finalVerts.size() * sizeof(RVertex)), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
            D3D11_SUBRESOURCE_DATA vd = { finalVerts.data() };
            m_pd3dDevice->CreateBuffer(&vbd, &vd, node->m_vertexBuffer.GetAddressOf());

            D3D11_BUFFER_DESC ibd = { (UINT)(indices.size() * sizeof(WORD)), D3D11_USAGE_DEFAULT, D3D11_BIND_INDEX_BUFFER, 0, 0, 0 };
            D3D11_SUBRESOURCE_DATA id = { indices.data() };
            m_pd3dDevice->CreateBuffer(&ibd, &id, node->m_indexBuffer.GetAddressOf());
            node->m_indexCount = (UINT)indices.size();
        }
        m_pRootNode->m_children.push_back(std::move(node));
    }

    MapNodes(m_pRootNode);
    return true;
}

void RMesh::MapNodes(std::shared_ptr<RMeshNode> node) { if (!node) return; m_nodeMap[node->m_name] = node; for (auto& child : node->m_children) MapNodes(child); }
std::shared_ptr<RMeshNode> RMesh::FindNode(const std::string& name) { auto it = m_nodeMap.find(name); return (it != m_nodeMap.end()) ? it->second : nullptr; }
void RMesh::Update(float dt) { if (m_pRootNode) m_pRootNode->UpdateMatrices(XMMatrixIdentity()); }
void RMesh::Draw(ID3D11DeviceContext* ctx, ID3D11Buffer* cb) { if (m_pRootNode) m_pRootNode->Draw(ctx, cb); }

}
