#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../Include/RSkinObject.h"
#include "AppLogger.h"
#include <fstream>
#include <d3dcompiler.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <set>

namespace RealSpace3 {

#pragma pack(push, 1)
namespace FileFormat {
    struct ELU_HEADER { uint32_t sig; uint32_t ver; int32_t nMaterial; int32_t nMeshCount; };
    struct ELU_MESH_DISK { char szName[40]; char szParentName[40]; float matLocal[16]; int32_t nVertices; int32_t nWeights; int32_t nIndices; };
    struct ELU_WEIGHT_DISK { char szBoneName[40]; float fWeight; uint32_t nVertexIdx; };
    struct ANI_HEADER { uint32_t sig; uint32_t ver; int32_t maxframe; int32_t model_num; int32_t ani_type; };
    struct ANI_BONE_INFO { char szName[40]; float matBase[16]; };
    struct ANI_FRAME_DISK { float pos[3]; float rot[4]; };
}
#pragma pack(pop)

constexpr uint32_t EXPORTER_SIG = 0x0107f060;

RSkinObject::RSkinObject(ID3D11Device* device, TextureManager* pTexMgr) 
    : m_pd3dDevice(device), m_pTextureManager(pTexMgr) {}
RSkinObject::~RSkinObject() {}

template<typename T>
const T* ReadFromBuffer(const std::vector<uint8_t>& buffer, size_t& offset, size_t count = 1) {
    if (offset + sizeof(T) * count > buffer.size()) return nullptr;
    const T* ptr = reinterpret_cast<const T*>(buffer.data() + offset);
    offset += sizeof(T) * count;
    return ptr;
}

DirectX::XMMATRIX RSkinObject::GetBoneMatrix(const std::string& boneName) {
    auto it = m_boneMap.find(boneName);
    if (it != m_boneMap.end()) return m_skeleton[it->second].combinedMatrix;
    return DirectX::XMMatrixIdentity();
}

RSkinObject::LegacyPartGroup RSkinObject::ClassifyLegacyPartNode(const std::string& nodeName) const {
    std::string n = nodeName;
    std::transform(n.begin(), n.end(), n.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    auto startsWith = [&](const char* prefix) {
        const size_t plen = std::strlen(prefix);
        return (n.size() >= plen) && (n.compare(0, plen, prefix) == 0);
    };

    if (startsWith("eq_face")) return LegacyPartGroup::Face;
    if (startsWith("eq_head")) return LegacyPartGroup::Head;
    if (startsWith("eq_chest")) return LegacyPartGroup::Chest;
    if (startsWith("eq_hands")) return LegacyPartGroup::Hands;
    if (startsWith("eq_legs")) return LegacyPartGroup::Legs;
    if (startsWith("eq_feet")) return LegacyPartGroup::Feet;
    return LegacyPartGroup::Count;
}

bool RSkinObject::IsLegacyPartNodeVisible(const std::string& nodeName) const {
    const auto group = ClassifyLegacyPartNode(nodeName);
    if (group == LegacyPartGroup::Count) return true;

    const auto idx = static_cast<size_t>(group);
    if (idx >= m_legacyPartSelection.size()) return false;
    const std::string& selected = m_legacyPartSelection[idx];
    if (selected.empty()) return false;

    std::string a = selected;
    std::string b = nodeName;
    std::transform(a.begin(), a.end(), a.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(b.begin(), b.end(), b.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return a == b;
}

void RSkinObject::ResetLegacyPartSelection() {
    for (auto& x : m_legacyPartSelection) x.clear();
    if (m_loadedLegacy5007) {
        RebuildLegacyCpuVertices();
        m_legacyCpuSkinDirty = true;
    }
}

bool RSkinObject::SetLegacyPart(const std::string& category, const std::string& nodeName) {
    if (!m_loadedLegacy5007 || nodeName.empty()) return false;

    std::string c = category;
    std::transform(c.begin(), c.end(), c.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    LegacyPartGroup target = LegacyPartGroup::Count;
    if (c == "face") target = LegacyPartGroup::Face;
    else if (c == "head" || c == "hair") target = LegacyPartGroup::Head;
    else if (c == "chest") target = LegacyPartGroup::Chest;
    else if (c == "hands") target = LegacyPartGroup::Hands;
    else if (c == "legs") target = LegacyPartGroup::Legs;
    else if (c == "feet") target = LegacyPartGroup::Feet;
    if (target == LegacyPartGroup::Count) return false;

    bool exists = false;
    for (const auto& node : m_legacyNodes) {
        if (ClassifyLegacyPartNode(node.name) != target) continue;
        std::string a = node.name;
        std::string b = nodeName;
        std::transform(a.begin(), a.end(), a.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        std::transform(b.begin(), b.end(), b.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (a == b) {
            exists = true;
            break;
        }
    }
    if (!exists) return false;

    m_legacyPartSelection[static_cast<size_t>(target)] = nodeName;
    RebuildLegacyCpuVertices();
    m_legacyCpuSkinDirty = true;
    return true;
}

bool RSkinObject::SetLegacyPartByIndex(const std::string& category, int index) {
    if (!m_loadedLegacy5007) return false;

    std::string c = category;
    std::transform(c.begin(), c.end(), c.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    LegacyPartGroup target = LegacyPartGroup::Count;
    if (c == "face") target = LegacyPartGroup::Face;
    else if (c == "head" || c == "hair") target = LegacyPartGroup::Head;
    else if (c == "chest") target = LegacyPartGroup::Chest;
    else if (c == "hands") target = LegacyPartGroup::Hands;
    else if (c == "legs") target = LegacyPartGroup::Legs;
    else if (c == "feet") target = LegacyPartGroup::Feet;
    if (target == LegacyPartGroup::Count) return false;

    std::vector<std::string> nodes;
    for (const auto& node : m_legacyNodes) {
        if (ClassifyLegacyPartNode(node.name) == target) {
            nodes.push_back(node.name);
        }
    }
    if (nodes.empty()) return false;

    std::sort(nodes.begin(), nodes.end(), [](const std::string& a, const std::string& b) {
        std::string la = a;
        std::string lb = b;
        std::transform(la.begin(), la.end(), la.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        std::transform(lb.begin(), lb.end(), lb.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return la < lb;
    });

    int wrapped = index;
    const int count = static_cast<int>(nodes.size());
    if (count > 0) {
        wrapped %= count;
        if (wrapped < 0) wrapped += count;
    }

    m_legacyPartSelection[static_cast<size_t>(target)] = nodes[static_cast<size_t>(wrapped)];
    RebuildLegacyCpuVertices();
    m_legacyCpuSkinDirty = true;
    return true;
}

struct TempWeight { float weight; uint8_t boneIdx; };

namespace {
bool ReadU32(std::ifstream& f, uint32_t& out) {
    f.read(reinterpret_cast<char*>(&out), sizeof(out));
    return static_cast<bool>(f);
}

std::string ReadLenString(std::ifstream& f) {
    uint32_t len = 0;
    if (!ReadU32(f, len) || len == 0 || len > 2048) return {};
    std::string s(len, '\0');
    f.read(s.data(), len);
    if (!f) return {};
    return s;
}

DirectX::XMFLOAT3 ConvertRS2Pos(const DirectX::XMFLOAT3& v) {
    // RS2 mesh basis to current DX11 scene basis.
    return DirectX::XMFLOAT3{ v.x, v.z, v.y };
}

bool IsZeroWeight(float w) {
    return std::fabs(w) <= 1e-6f;
}

bool IsWeaponLikeNodeName(const std::string& nodeName) {
    std::string n = nodeName;
    std::transform(n.begin(), n.end(), n.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const char* kWeaponTokens[] = {
        "weapon", "blade", "dagger", "katana", "sword",
        "pistol", "shotgun", "rifle", "smg", "rocket",
        "grenade", "muzzle", "cartridge", "medikit"
    };
    for (const char* tok : kWeaponTokens) {
        if (n.find(tok) != std::string::npos) return true;
    }
    return false;
}

void ClearSkinInfluences(SkinVertex& v) {
    for (int i = 0; i < 4; ++i) {
        v.weights[i] = 0.0f;
        v.indices[i] = 0;
    }
}

void FillTop4Influences(SkinVertex& v, const std::vector<TempWeight>& infl) {
    ClearSkinInfluences(v);
    float total = 0.0f;
    const int count = (std::min)(static_cast<int>(infl.size()), 4);
    for (int j = 0; j < count; ++j) {
        v.weights[j] = infl[j].weight;
        v.indices[j] = infl[j].boneIdx;
        total += infl[j].weight;
    }
    if (total > 0.0f) {
        for (int j = 0; j < 4; ++j) v.weights[j] /= total;
    } else {
        v.weights[0] = 1.0f;
    }
}

void FillOgzTripleInfluences(SkinVertex& v, const std::vector<TempWeight>& infl) {
    ClearSkinInfluences(v);
    if (infl.empty()) {
        v.weights[0] = 1.0f;
        return;
    }

    // OGZ-equivalent single influence: force one-hot on the source bone.
    if (infl.size() == 1) {
        v.weights[0] = 1.0f;
        v.indices[0] = infl[0].boneIdx;
        return;
    }

    const uint8_t idx0 = infl[0].boneIdx;
    const uint8_t idx1 = infl[1].boneIdx;
    const uint8_t idx2 = (infl.size() > 2) ? infl[2].boneIdx : idx1;

    float w1 = std::clamp(infl[0].weight, 0.0f, 1.0f);
    float w2 = std::clamp(infl[1].weight, 0.0f, 1.0f);
    const float pair = w1 + w2;
    if (pair > 1.0f) {
        const float invPair = 1.0f / pair;
        w1 *= invPair;
        w2 *= invPair;
    }

    float w3 = std::clamp(1.0f - (w1 + w2), 0.0f, 1.0f);
    float total = w1 + w2 + w3;
    if (total <= 1e-6f) {
        w1 = 1.0f;
        w2 = 0.0f;
        w3 = 0.0f;
        total = 1.0f;
    }

    const float invTotal = 1.0f / total;
    v.weights[0] = w1 * invTotal;
    v.weights[1] = w2 * invTotal;
    v.weights[2] = w3 * invTotal;
    v.weights[3] = 0.0f;
    v.indices[0] = idx0;
    v.indices[1] = idx1;
    v.indices[2] = idx2;
    v.indices[3] = 0;
}
}

bool RSkinObject::LoadElu(const std::string& filename) {
    m_loadedLegacy5007 = false;
    m_legacyCpuSkinDirty = false;
    m_legacyNodes.clear();
    for (auto& x : m_legacyPartSelection) x.clear();
    m_vertexBuffer.Reset();
    m_indexBuffer.Reset();
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return false;
    size_t size = file.tellg();
    if (size < sizeof(FileFormat::ELU_HEADER)) return false;
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(size);
    file.read((char*)buffer.data(), size);
    size_t offset = 0;
    auto header = ReadFromBuffer<FileFormat::ELU_HEADER>(buffer, offset);
    if (!header || header->sig != EXPORTER_SIG) return false;
    AppLogger::Log("[RS3_AUDIT] RSkinObject::LoadElu header sig=" + std::to_string(header->sig) +
                   " ver=" + std::to_string(header->ver) + " mats=" + std::to_string(header->nMaterial) +
                   " meshes=" + std::to_string(header->nMeshCount) + " file=" + filename);
    if (header->ver == 0x5007) return LoadEluLegacy5007(filename);
    if (header->nMaterial < 0 || header->nMaterial > 4096) return false;
    if (header->nMeshCount < 0 || header->nMeshCount > 4096) return false;
    
    // 1. Read Materials
    offset = sizeof(FileFormat::ELU_HEADER); 
    m_textureNames.clear();
    for(int i=0; i<header->nMaterial; ++i) {
        if (offset + 8 + 64 + 40 > buffer.size()) return false;
        offset += 8;
        offset += 64;
        char texName[40];
        std::memcpy(texName, buffer.data() + offset, 40);
        texName[39] = 0;
        m_textureNames.push_back(texName);
        offset += 128;
        if (offset > buffer.size()) return false;
    }

    m_vertices.clear(); m_indices.clear(); m_skeleton.clear(); m_boneMap.clear(); m_subsets.clear();
    std::vector<std::string> parentNames;
    std::vector<std::vector<TempWeight>> vertexInfluences;
    
    for (int m = 0; m < header->nMeshCount; ++m) {
        auto meshDisk = ReadFromBuffer<FileFormat::ELU_MESH_DISK>(buffer, offset);
        if (!meshDisk) return false;
        if (meshDisk->nVertices < 0 || meshDisk->nVertices > 200000) return false;
        if (meshDisk->nWeights < 0 || meshDisk->nWeights > 2000000) return false;
        if (meshDisk->nIndices < 0 || meshDisk->nIndices > 2000000) return false;
        Bone bone; bone.name = meshDisk->szName; bone.parentIdx = -1;
        bone.localMatrix = DirectX::XMLoadFloat4x4((DirectX::XMFLOAT4X4*)meshDisk->matLocal);
        m_skeleton.push_back(bone);
        parentNames.push_back(meshDisk->szParentName);
        m_boneMap[bone.name] = (int32_t)m_skeleton.size() - 1;
        
        size_t vStart = m_vertices.size();
        size_t iStart = m_indices.size(); // Absolute index start
        
        vertexInfluences.resize(vStart + meshDisk->nVertices);
        for (int v = 0; v < meshDisk->nVertices; ++v) {
            auto vData = ReadFromBuffer<float>(buffer, offset, 8);
            if (!vData) return false;
            SkinVertex sv = { vData[0], vData[1], vData[2], vData[3], vData[4], vData[5], 0xFFFFFFFF, vData[6], vData[7] };
            for(int i=0; i<4; ++i) { sv.weights[i] = 0.0f; sv.indices[i] = 0; }
            m_vertices.push_back(sv);
        }
        for (int w = 0; w < meshDisk->nWeights; ++w) {
            auto weightDisk = ReadFromBuffer<FileFormat::ELU_WEIGHT_DISK>(buffer, offset);
            if (!weightDisk) return false;
            uint32_t globalVIdx = (uint32_t)(vStart + weightDisk->nVertexIdx);
            if (globalVIdx < vertexInfluences.size()) {
                auto it = m_boneMap.find(weightDisk->szBoneName);
                if (it != m_boneMap.end()) vertexInfluences[globalVIdx].push_back({ weightDisk->fWeight, (uint8_t)it->second });
            }
        }
        
        // Read Indices
        auto iData = ReadFromBuffer<unsigned short>(buffer, offset, meshDisk->nIndices);
        if (!iData) return false;
        for(int i=0; i<meshDisk->nIndices; ++i) {
            // Indices in ELU are relative to the mesh's vertex start
            m_indices.push_back(iData[i] + (unsigned short)vStart);
        }

        // 2. Read Draw Props (Subsets)
        // Check if there is data left for this mesh properties?
        // While legacy ELU has phys info etc, we check for DrawProps which usually follow.
        // In some ELU versions, DrawProps are separate. 
        // Based on EluLoader.cpp: It reads Submesh Loop.
        // But `ELU_MESH_DISK` struct doesn't seem to account for it in the loop naturally?
        // Wait, EluLoader.cpp uses a completely different loading method (Chunk based?). 
        // The structure definition in RSkinObject.cpp seems to match a packed format.
        // Let's assume for now 1 Mesh = 1 Material if no props found, masking texture 0?
        // Actually, if we want multi-materials, we simply treat each Mesh as a subset if they are separated.
        // BUT, `ELU_MESH_DISK` implies one mesh node.
        // If an ELU mesh has multiple materials, it typically splits into sub-meshes or uses face-materials.
        // Let's default to: Each Mesh Node uses Material 0 (or try to find one).
        
        // Actually, most GunZ ELUs split same-material parts into different mesh nodes? 
        // No, `DrawProps` inside a mesh define index ranges.
        // Since we don't have the exact logic to parse variable-length DrawProps here without risking offset desync,
        // we will create ONE default subset for this mesh node using Material 0 (or based on Name?).
        // IMPROVEMENT: Map Material 0 to Texture 0.
        
        MeshSubset subset;
        subset.MaterialID = 0; // Default to first material
        subset.IndexStart = (uint32_t)iStart;
        subset.IndexCount = meshDisk->nIndices;
        m_subsets.push_back(subset);
    }
    
    for (size_t i = 0; i < m_vertices.size(); ++i) {
        auto& infl = vertexInfluences[i];
        std::sort(infl.begin(), infl.end(), [](auto& a, auto& b){ return a.weight > b.weight; });
        if (m_useOgzTripleWeights) {
            FillOgzTripleInfluences(m_vertices[i], infl);
        } else {
            FillTop4Influences(m_vertices[i], infl);
        }
    }
    BuildSkeleton(parentNames);
    return true;
}

bool RSkinObject::LoadEluLegacy5007(const std::string& filename) {
    m_loadedLegacy5007 = true;
    m_legacyCpuSkinDirty = false;
    m_legacyNodes.clear();
    for (auto& x : m_legacyPartSelection) x.clear();
    m_vertexBuffer.Reset();
    m_indexBuffer.Reset();
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) return false;

    uint32_t sig = 0, ver = 0, matCount = 0, meshCount = 0;
    if (!ReadU32(file, sig) || !ReadU32(file, ver) || !ReadU32(file, matCount) || !ReadU32(file, meshCount)) return false;
    if (sig != EXPORTER_SIG || ver != 0x5007 || meshCount == 0 || meshCount > 4096) return false;

    auto trimCstr = [](const char* s, size_t n) {
        size_t len = 0;
        while (len < n && s[len] != '\0') ++len;
        return std::string(s, len);
    };

    struct Color32 { float r, g, b, a; };
    struct FaceInfoDisk {
        int pointIndex[3];
        DirectX::XMFLOAT3 pointTex[3];
        int mtrlId;
        int sgId;
    };
    struct FaceNormalInfoDisk {
        DirectX::XMFLOAT3 normal;
        DirectX::XMFLOAT3 pointNormal[3];
    };
    struct PhysiqueInfoDisk {
        char parentName[4][40];
        float weight[4];
        int parentId[4];
        int num;
        DirectX::XMFLOAT3 offset[4];
    };
    struct MeshDiskData {
        std::string name;
        std::string parent;
        DirectX::XMFLOAT4X4 local{};
        int mtrlId = 0;
        std::vector<DirectX::XMFLOAT3> points;
        std::vector<FaceInfoDisk> faces;
        std::vector<FaceNormalInfoDisk> faceNormals;
        std::vector<PhysiqueInfoDisk> physique;
    };

    m_vertices.clear();
    m_indices.clear();
    m_subsets.clear();
    m_textureNames.clear();
    m_skeleton.clear();
    m_boneMap.clear();

    // Materials in exporter v8 layout.
    std::string eluDir = filename;
    const auto slash = eluDir.find_last_of("/\\");
    if (slash != std::string::npos) eluDir = eluDir.substr(0, slash);
    else eluDir.clear();

    for (uint32_t i = 0; i < matCount; ++i) {
        int mtrlId = 0;
        int subMtrlId = 0;
        Color32 ambient{}, diffuse{}, specular{};
        float power = 0.0f;
        int subMtrlNum = 0;
        char name[256] = {};
        char opaName[256] = {};
        int twoSided = 0;
        int additive = 0;
        int alphaTest = 0;

        file.read(reinterpret_cast<char*>(&mtrlId), sizeof(mtrlId));
        file.read(reinterpret_cast<char*>(&subMtrlId), sizeof(subMtrlId));
        file.read(reinterpret_cast<char*>(&ambient), sizeof(ambient));
        file.read(reinterpret_cast<char*>(&diffuse), sizeof(diffuse));
        file.read(reinterpret_cast<char*>(&specular), sizeof(specular));
        file.read(reinterpret_cast<char*>(&power), sizeof(power));
        file.read(reinterpret_cast<char*>(&subMtrlNum), sizeof(subMtrlNum));
        file.read(name, sizeof(name));
        file.read(opaName, sizeof(opaName));
        file.read(reinterpret_cast<char*>(&twoSided), sizeof(twoSided));
        file.read(reinterpret_cast<char*>(&additive), sizeof(additive));
        file.read(reinterpret_cast<char*>(&alphaTest), sizeof(alphaTest));
        if (!file) return false;

        std::string diffuseName = trimCstr(name, sizeof(name));
        if (!diffuseName.empty()) {
            const bool hasPath = (diffuseName.find('/') != std::string::npos) || (diffuseName.find('\\') != std::string::npos);
            if (!hasPath && !eluDir.empty()) {
                diffuseName = eluDir + "/" + diffuseName;
            }
        }
        if (!diffuseName.empty() && mtrlId >= 0) {
            if (static_cast<size_t>(mtrlId) >= m_textureNames.size()) m_textureNames.resize(mtrlId + 1);
            if (m_textureNames[mtrlId].empty()) m_textureNames[mtrlId] = diffuseName;
        }
    }

    std::vector<MeshDiskData> meshData;
    meshData.reserve(meshCount);
    std::vector<std::string> parentNames;
    parentNames.reserve(meshCount);

    for (uint32_t i = 0; i < meshCount; ++i) {
        MeshDiskData node;
        char nodeName[40] = {};
        char parentName[40] = {};
        DirectX::XMFLOAT3 apScale{};
        DirectX::XMFLOAT3 axisRot{};
        float axisRotAngle = 0.0f;
        DirectX::XMFLOAT3 axisScale{};
        float axisScaleAngle = 0.0f;
        DirectX::XMFLOAT4X4 matEtc{};

        file.read(nodeName, sizeof(nodeName));
        file.read(parentName, sizeof(parentName));
        file.read(reinterpret_cast<char*>(&node.local), sizeof(node.local));
        file.read(reinterpret_cast<char*>(&apScale), sizeof(apScale));
        file.read(reinterpret_cast<char*>(&axisRot), sizeof(axisRot));
        file.read(reinterpret_cast<char*>(&axisRotAngle), sizeof(axisRotAngle));
        file.read(reinterpret_cast<char*>(&axisScale), sizeof(axisScale));
        file.read(reinterpret_cast<char*>(&axisScaleAngle), sizeof(axisScaleAngle));
        file.read(reinterpret_cast<char*>(&matEtc), sizeof(matEtc));
        if (!file) return false;

        node.name = trimCstr(nodeName, sizeof(nodeName));
        node.parent = trimCstr(parentName, sizeof(parentName));

        Bone bone;
        bone.name = node.name;
        bone.parentIdx = -1;
        bone.localMatrix = DirectX::XMLoadFloat4x4(&node.local);
        bone.offsetMatrix = DirectX::XMMatrixIdentity();
        bone.combinedMatrix = DirectX::XMMatrixIdentity();
        m_boneMap[bone.name] = static_cast<int32_t>(m_skeleton.size());
        m_skeleton.push_back(bone);
        parentNames.push_back(node.parent);

        int pointCount = 0;
        file.read(reinterpret_cast<char*>(&pointCount), sizeof(pointCount));
        if (!file || pointCount < 0 || pointCount > 300000) return false;
        node.points.resize(static_cast<size_t>(pointCount));
        if (pointCount > 0) {
            file.read(reinterpret_cast<char*>(node.points.data()), sizeof(DirectX::XMFLOAT3) * node.points.size());
            if (!file) return false;
        }

        int faceCount = 0;
        file.read(reinterpret_cast<char*>(&faceCount), sizeof(faceCount));
        if (!file || faceCount < 0 || faceCount > 2000000) return false;

        node.faces.resize(static_cast<size_t>(faceCount));
        node.faceNormals.resize(static_cast<size_t>(faceCount));
        if (faceCount > 0) {
            file.read(reinterpret_cast<char*>(node.faces.data()), sizeof(FaceInfoDisk) * node.faces.size());
            file.read(reinterpret_cast<char*>(node.faceNormals.data()), sizeof(FaceNormalInfoDisk) * node.faceNormals.size());
            if (!file) return false;
        }

        int pointColorCount = 0;
        file.read(reinterpret_cast<char*>(&pointColorCount), sizeof(pointColorCount));
        if (!file || pointColorCount < 0 || pointColorCount > 300000) return false;
        if (pointColorCount > 0) {
            file.seekg(static_cast<std::streamoff>(pointColorCount) * sizeof(DirectX::XMFLOAT3), std::ios::cur);
            if (!file) return false;
        }

        file.read(reinterpret_cast<char*>(&node.mtrlId), sizeof(node.mtrlId));
        if (!file) return false;

        int physiqueCount = 0;
        file.read(reinterpret_cast<char*>(&physiqueCount), sizeof(physiqueCount));
        if (!file || physiqueCount < 0 || physiqueCount > 300000) return false;
        node.physique.resize(static_cast<size_t>(physiqueCount));
        if (physiqueCount > 0) {
            file.read(reinterpret_cast<char*>(node.physique.data()), sizeof(PhysiqueInfoDisk) * node.physique.size());
            if (!file) return false;
        }

        meshData.push_back(std::move(node));
    }

    BuildSkeleton(parentNames);
    for (auto& node : meshData) {
        for (auto& phys : node.physique) {
            const int influenceCount = (std::min)((std::max)(phys.num, 0), 4);
            for (int k = 0; k < influenceCount; ++k) {
                const std::string parentBoneName = trimCstr(phys.parentName[k], sizeof(phys.parentName[k]));
                auto itBone = m_boneMap.find(parentBoneName);
                if (itBone != m_boneMap.end()) {
                    phys.parentId[k] = itBone->second;
                } else {
                    const int pid = phys.parentId[k];
                    if (pid >= 0 && static_cast<size_t>(pid) < m_skeleton.size()) {
                        // Fallback only when name lookup fails.
                        phys.parentId[k] = pid;
                    } else {
                        phys.parentId[k] = -1;
                    }
                }
            }
        }
    }

    m_legacyNodes.clear();
    m_legacyNodes.reserve(meshData.size());
    for (const auto& node : meshData) {
        LegacyNode outNode{};
        outNode.name = node.name;
        outNode.mtrlId = (std::max)(0, node.mtrlId);
        auto itNodeBone = m_boneMap.find(node.name);
        if (itNodeBone != m_boneMap.end()) {
            outNode.nodeBoneId = itNodeBone->second;
        }
        outNode.points = node.points;
        outNode.faces.resize(node.faces.size());
        outNode.faceNormals.resize(node.faceNormals.size());
        outNode.physique.resize(node.physique.size());

        for (size_t i = 0; i < node.faces.size(); ++i) {
            LegacyFace f{};
            for (int c = 0; c < 3; ++c) {
                f.pointIndex[c] = node.faces[i].pointIndex[c];
                f.uv[c] = DirectX::XMFLOAT2(node.faces[i].pointTex[c].x, node.faces[i].pointTex[c].y);
            }
            outNode.faces[i] = f;
        }

        for (size_t i = 0; i < node.faceNormals.size(); ++i) {
            LegacyFaceNormal fn{};
            for (int c = 0; c < 3; ++c) {
                fn.pointNormal[c] = node.faceNormals[i].pointNormal[c];
            }
            outNode.faceNormals[i] = fn;
        }

        for (size_t i = 0; i < node.physique.size(); ++i) {
            LegacyPhysique p{};
            p.num = node.physique[i].num;
            for (int k = 0; k < 4; ++k) {
                p.parentId[k] = node.physique[i].parentId[k];
                p.weight[k] = node.physique[i].weight[k];
                p.offset[k] = node.physique[i].offset[k];
            }
            outNode.physique[i] = p;
        }

        m_legacyNodes.push_back(std::move(outNode));
    }

    // RS2 behavior: each equip group resolves to one active mesh node.
    for (const auto& node : m_legacyNodes) {
        const auto group = ClassifyLegacyPartNode(node.name);
        if (group == LegacyPartGroup::Count) continue;
        const size_t idx = static_cast<size_t>(group);
        if (idx >= m_legacyPartSelection.size()) continue;
        if (m_legacyPartSelection[idx].empty()) {
            m_legacyPartSelection[idx] = node.name;
        }
    }

    RebuildLegacyCpuVertices();
    m_legacyCpuSkinDirty = false;

    AppLogger::Log("[RS3_AUDIT] RSkinObject::LoadEluLegacy5007 -> loaded " + filename +
                   " verts=" + std::to_string(m_vertices.size()) +
                   " idx=" + std::to_string(m_indices.size()) +
                   " bones=" + std::to_string(m_skeleton.size()));
    return !m_vertices.empty() && !m_indices.empty() && !m_subsets.empty();
}

bool RSkinObject::AppendLegacyPartsFromElu5007(const std::string& filename, bool forceVisibleNonEquip) {
    if (!m_loadedLegacy5007) return false;

    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) return false;

    uint32_t sig = 0, ver = 0, matCount = 0, meshCount = 0;
    if (!ReadU32(file, sig) || !ReadU32(file, ver) || !ReadU32(file, matCount) || !ReadU32(file, meshCount)) return false;
    if (sig != EXPORTER_SIG || ver != 0x5007 || meshCount == 0 || meshCount > 4096) return false;

    auto trimCstr = [](const char* s, size_t n) {
        size_t len = 0;
        while (len < n && s[len] != '\0') ++len;
        return std::string(s, len);
    };

    struct Color32 { float r, g, b, a; };
    struct FaceInfoDisk {
        int pointIndex[3];
        DirectX::XMFLOAT3 pointTex[3];
        int mtrlId;
        int sgId;
    };
    struct FaceNormalInfoDisk {
        DirectX::XMFLOAT3 normal;
        DirectX::XMFLOAT3 pointNormal[3];
    };
    struct PhysiqueInfoDisk {
        char parentName[4][40];
        float weight[4];
        int parentId[4];
        int num;
        DirectX::XMFLOAT3 offset[4];
    };
    struct MeshDiskData {
        std::string name;
        std::string parent;
        int mtrlId = 0;
        std::vector<DirectX::XMFLOAT3> points;
        std::vector<FaceInfoDisk> faces;
        std::vector<FaceNormalInfoDisk> faceNormals;
        std::vector<PhysiqueInfoDisk> physique;
    };

    std::string eluDir = filename;
    const auto slash = eluDir.find_last_of("/\\");
    if (slash != std::string::npos) eluDir = eluDir.substr(0, slash);
    else eluDir.clear();

    const int mtrlBase = static_cast<int>(m_textureNames.size());
    for (uint32_t i = 0; i < matCount; ++i) {
        int mtrlId = 0;
        int subMtrlId = 0;
        Color32 ambient{}, diffuse{}, specular{};
        float power = 0.0f;
        int subMtrlNum = 0;
        char name[256] = {};
        char opaName[256] = {};
        int twoSided = 0;
        int additive = 0;
        int alphaTest = 0;

        file.read(reinterpret_cast<char*>(&mtrlId), sizeof(mtrlId));
        file.read(reinterpret_cast<char*>(&subMtrlId), sizeof(subMtrlId));
        file.read(reinterpret_cast<char*>(&ambient), sizeof(ambient));
        file.read(reinterpret_cast<char*>(&diffuse), sizeof(diffuse));
        file.read(reinterpret_cast<char*>(&specular), sizeof(specular));
        file.read(reinterpret_cast<char*>(&power), sizeof(power));
        file.read(reinterpret_cast<char*>(&subMtrlNum), sizeof(subMtrlNum));
        file.read(name, sizeof(name));
        file.read(opaName, sizeof(opaName));
        file.read(reinterpret_cast<char*>(&twoSided), sizeof(twoSided));
        file.read(reinterpret_cast<char*>(&additive), sizeof(additive));
        file.read(reinterpret_cast<char*>(&alphaTest), sizeof(alphaTest));
        if (!file) return false;

        std::string diffuseName = trimCstr(name, sizeof(name));
        if (!diffuseName.empty()) {
            const bool hasPath = (diffuseName.find('/') != std::string::npos) || (diffuseName.find('\\') != std::string::npos);
            if (!hasPath && !eluDir.empty()) diffuseName = eluDir + "/" + diffuseName;
        }

        if (!diffuseName.empty() && mtrlId >= 0) {
            const int outId = mtrlBase + mtrlId;
            if (static_cast<size_t>(outId) >= m_textureNames.size()) m_textureNames.resize(static_cast<size_t>(outId) + 1);
            if (m_textureNames[static_cast<size_t>(outId)].empty()) m_textureNames[static_cast<size_t>(outId)] = diffuseName;
        }
    }

    std::vector<MeshDiskData> meshData;
    meshData.reserve(meshCount);
    for (uint32_t i = 0; i < meshCount; ++i) {
        MeshDiskData node;
        char nodeName[40] = {};
        char parentName[40] = {};
        DirectX::XMFLOAT4X4 local{};
        DirectX::XMFLOAT3 apScale{};
        DirectX::XMFLOAT3 axisRot{};
        float axisRotAngle = 0.0f;
        DirectX::XMFLOAT3 axisScale{};
        float axisScaleAngle = 0.0f;
        DirectX::XMFLOAT4X4 matEtc{};

        file.read(nodeName, sizeof(nodeName));
        file.read(parentName, sizeof(parentName));
        file.read(reinterpret_cast<char*>(&local), sizeof(local));
        file.read(reinterpret_cast<char*>(&apScale), sizeof(apScale));
        file.read(reinterpret_cast<char*>(&axisRot), sizeof(axisRot));
        file.read(reinterpret_cast<char*>(&axisRotAngle), sizeof(axisRotAngle));
        file.read(reinterpret_cast<char*>(&axisScale), sizeof(axisScale));
        file.read(reinterpret_cast<char*>(&axisScaleAngle), sizeof(axisScaleAngle));
        file.read(reinterpret_cast<char*>(&matEtc), sizeof(matEtc));
        if (!file) return false;

        node.name = trimCstr(nodeName, sizeof(nodeName));
        node.parent = trimCstr(parentName, sizeof(parentName));

        int pointCount = 0;
        file.read(reinterpret_cast<char*>(&pointCount), sizeof(pointCount));
        if (!file || pointCount < 0 || pointCount > 300000) return false;
        node.points.resize(static_cast<size_t>(pointCount));
        if (pointCount > 0) {
            file.read(reinterpret_cast<char*>(node.points.data()), sizeof(DirectX::XMFLOAT3) * node.points.size());
            if (!file) return false;
        }

        int faceCount = 0;
        file.read(reinterpret_cast<char*>(&faceCount), sizeof(faceCount));
        if (!file || faceCount < 0 || faceCount > 2000000) return false;
        node.faces.resize(static_cast<size_t>(faceCount));
        node.faceNormals.resize(static_cast<size_t>(faceCount));
        if (faceCount > 0) {
            file.read(reinterpret_cast<char*>(node.faces.data()), sizeof(FaceInfoDisk) * node.faces.size());
            file.read(reinterpret_cast<char*>(node.faceNormals.data()), sizeof(FaceNormalInfoDisk) * node.faceNormals.size());
            if (!file) return false;
        }

        int pointColorCount = 0;
        file.read(reinterpret_cast<char*>(&pointColorCount), sizeof(pointColorCount));
        if (!file || pointColorCount < 0 || pointColorCount > 300000) return false;
        if (pointColorCount > 0) {
            file.seekg(static_cast<std::streamoff>(pointColorCount) * sizeof(DirectX::XMFLOAT3), std::ios::cur);
            if (!file) return false;
        }

        file.read(reinterpret_cast<char*>(&node.mtrlId), sizeof(node.mtrlId));
        if (!file) return false;

        int physiqueCount = 0;
        file.read(reinterpret_cast<char*>(&physiqueCount), sizeof(physiqueCount));
        if (!file || physiqueCount < 0 || physiqueCount > 300000) return false;
        node.physique.resize(static_cast<size_t>(physiqueCount));
        if (physiqueCount > 0) {
            file.read(reinterpret_cast<char*>(node.physique.data()), sizeof(PhysiqueInfoDisk) * node.physique.size());
            if (!file) return false;
        }

        meshData.push_back(std::move(node));
    }

    std::set<std::string> existing;
    for (const auto& n : m_legacyNodes) {
        std::string key = n.name;
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        existing.insert(key);
    }

    auto normalizeBoneName = [](std::string s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        size_t start = 0;
        while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
        if (start > 0) s.erase(0, start);
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return s;
    };

    auto findBoneId = [&](const std::string& boneName) -> int32_t {
        auto itExact = m_boneMap.find(boneName);
        if (itExact != m_boneMap.end()) return itExact->second;
        const std::string key = normalizeBoneName(boneName);
        for (const auto& kv : m_boneMap) {
            if (normalizeBoneName(kv.first) == key) return kv.second;
        }
        return -1;
    };

    int appended = 0;
    for (auto& node : meshData) {
        std::string key = node.name;
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (existing.count(key) != 0) continue;

        LegacyNode outNode{};
        outNode.name = node.name;
        outNode.mtrlId = (std::max)(0, mtrlBase + node.mtrlId);
        outNode.forceVisibleNonEquip = forceVisibleNonEquip;
        outNode.nodeBoneId = findBoneId(node.name);
        if (outNode.nodeBoneId < 0) {
            outNode.nodeBoneId = findBoneId(node.parent);
        }
        outNode.points = node.points;
        outNode.faces.resize(node.faces.size());
        outNode.faceNormals.resize(node.faceNormals.size());
        outNode.physique.resize(node.physique.size());

        for (size_t i = 0; i < node.faces.size(); ++i) {
            LegacyFace f{};
            for (int c = 0; c < 3; ++c) {
                f.pointIndex[c] = node.faces[i].pointIndex[c];
                f.uv[c] = DirectX::XMFLOAT2(node.faces[i].pointTex[c].x, node.faces[i].pointTex[c].y);
            }
            outNode.faces[i] = f;
        }
        for (size_t i = 0; i < node.faceNormals.size(); ++i) {
            LegacyFaceNormal fn{};
            for (int c = 0; c < 3; ++c) fn.pointNormal[c] = node.faceNormals[i].pointNormal[c];
            outNode.faceNormals[i] = fn;
        }
        for (size_t i = 0; i < node.physique.size(); ++i) {
            LegacyPhysique p{};
            p.num = node.physique[i].num;
            for (int k = 0; k < 4; ++k) {
                p.weight[k] = node.physique[i].weight[k];
                p.offset[k] = node.physique[i].offset[k];
                const std::string parentBoneName = trimCstr(node.physique[i].parentName[k], sizeof(node.physique[i].parentName[k]));
                p.parentId[k] = findBoneId(parentBoneName);
            }
            outNode.physique[i] = p;
        }

        m_legacyNodes.push_back(std::move(outNode));
        existing.insert(key);
        ++appended;
    }

    if (appended <= 0) return false;

    for (const auto& node : m_legacyNodes) {
        const auto group = ClassifyLegacyPartNode(node.name);
        if (group == LegacyPartGroup::Count) continue;
        const size_t idx = static_cast<size_t>(group);
        if (idx >= m_legacyPartSelection.size()) continue;
        if (m_legacyPartSelection[idx].empty()) m_legacyPartSelection[idx] = node.name;
    }

    RebuildLegacyCpuVertices();
    m_legacyCpuSkinDirty = true;
    AppLogger::Log("[RS3_AUDIT] RSkinObject::AppendLegacyPartsFromElu5007 -> appended " + std::to_string(appended) +
                   " nodes from " + filename);
    return true;
}

void RSkinObject::RebuildLegacyCpuVertices() {
    if (!m_loadedLegacy5007) return;

    m_vertices.clear();
    m_indices.clear();
    m_subsets.clear();

    bool hasEquipStyleNodes = false;
    for (const auto& node : m_legacyNodes) {
        if (ClassifyLegacyPartNode(node.name) != LegacyPartGroup::Count) {
            hasEquipStyleNodes = true;
            break;
        }
    }

    int rootId = -1;
    auto itRoot = m_boneMap.find("Bip01");
    if (itRoot != m_boneMap.end()) rootId = itRoot->second;
    if (rootId < 0) {
        itRoot = m_boneMap.find("Bip01 Pelvis");
        if (itRoot != m_boneMap.end()) rootId = itRoot->second;
    }

    DirectX::XMFLOAT3 rootConv{ 0.0f, 0.0f, 0.0f };
    if (rootId >= 0 && static_cast<size_t>(rootId) < m_skeleton.size()) {
        DirectX::XMFLOAT4X4 rootM{};
        DirectX::XMStoreFloat4x4(&rootM, m_skeleton[rootId].combinedMatrix);
        rootConv = ConvertRS2Pos(DirectX::XMFLOAT3{ rootM._41, rootM._42, rootM._43 });
    }

    for (const auto& node : m_legacyNodes) {
        if (IsWeaponLikeNodeName(node.name) && !node.forceVisibleNonEquip) {
            continue;
        }
        if (hasEquipStyleNodes) {
            const auto group = ClassifyLegacyPartNode(node.name);
            if (group == LegacyPartGroup::Count && !node.forceVisibleNonEquip) {
                continue;
            }
            if (group != LegacyPartGroup::Count && !IsLegacyPartNodeVisible(node.name)) {
                continue;
            }
        }

        const uint32_t indexStart = static_cast<uint32_t>(m_indices.size());
        DirectX::XMMATRIX nodeMat = DirectX::XMMatrixIdentity();
        if (node.nodeBoneId >= 0 && static_cast<size_t>(node.nodeBoneId) < m_skeleton.size()) {
            nodeMat = m_skeleton[node.nodeBoneId].combinedMatrix;
        } else if (rootId >= 0 && static_cast<size_t>(rootId) < m_skeleton.size()) {
            // Legacy ELUs with unresolved node-bone mapping should still follow root,
            // otherwise vertices can appear detached at world origin/top.
            nodeMat = m_skeleton[static_cast<size_t>(rootId)].combinedMatrix;
        }

        bool hasValidPhysique = false;
        if (node.physique.size() == node.points.size()) {
            for (const auto& ph : node.physique) {
                for (int i = 0; i < ph.num && i < 4; ++i) {
                    const int32_t boneId = ph.parentId[i];
                    const float w = ph.weight[i];
                    if (boneId >= 0 && static_cast<size_t>(boneId) < m_skeleton.size() && std::isfinite(w) && w > 0.0f) {
                        hasValidPhysique = true;
                        break;
                    }
                }
                if (hasValidPhysique) break;
            }
        }
        if (node.nodeBoneId < 0 && !hasValidPhysique) {
            continue;
        }

        const size_t faceCount = (std::min)(node.faces.size(), node.faceNormals.size());
        for (size_t f = 0; f < faceCount; ++f) {
            const auto& face = node.faces[f];
            const auto& fn = node.faceNormals[f];
            for (int c = 0; c < 3; ++c) {
                const int pidx = face.pointIndex[c];
                if (pidx < 0 || static_cast<size_t>(pidx) >= node.points.size()) continue;

                const auto& nrmSrc = fn.pointNormal[c];
                DirectX::XMVECTOR outPos = DirectX::XMVectorZero();
                DirectX::XMVECTOR outNrm = DirectX::XMVector3Normalize(
                    DirectX::XMVector3TransformNormal(
                        DirectX::XMVectorSet(nrmSrc.x, nrmSrc.y, nrmSrc.z, 0.0f), nodeMat));

                const auto& srcPos = node.points[static_cast<size_t>(pidx)];
                const bool usePhysique = (node.physique.size() == node.points.size());
                if (usePhysique) {
                    const auto& ph = node.physique[static_cast<size_t>(pidx)];
                    DirectX::XMVECTOR accum = DirectX::XMVectorZero();
                    float totalWeight = 0.0f;
                    for (int i = 0; i < ph.num && i < 4; ++i) {
                        const int32_t boneId = ph.parentId[i];
                        const float w = ph.weight[i];
                        if (boneId < 0 || static_cast<size_t>(boneId) >= m_skeleton.size() || !std::isfinite(w) || w <= 0.0f) {
                            continue;
                        }
                        const auto& off = ph.offset[i];
                        if (!std::isfinite(off.x) || !std::isfinite(off.y) || !std::isfinite(off.z)) {
                            continue;
                        }
                        const auto p = DirectX::XMVector3TransformCoord(
                            DirectX::XMVectorSet(off.x, off.y, off.z, 1.0f),
                            m_skeleton[static_cast<size_t>(boneId)].combinedMatrix);
                        accum = DirectX::XMVectorAdd(accum, DirectX::XMVectorScale(p, w));
                        totalWeight += w;
                    }
                    if (totalWeight > 1e-6f) {
                        outPos = DirectX::XMVectorScale(accum, 1.0f / totalWeight);
                    } else {
                        outPos = DirectX::XMVector3TransformCoord(
                            DirectX::XMVectorSet(srcPos.x, srcPos.y, srcPos.z, 1.0f), nodeMat);
                    }
                } else {
                    outPos = DirectX::XMVector3TransformCoord(
                        DirectX::XMVectorSet(srcPos.x, srcPos.y, srcPos.z, 1.0f), nodeMat);
                }

                DirectX::XMFLOAT3 outPosF{};
                DirectX::XMFLOAT3 outNrmF{};
                DirectX::XMStoreFloat3(&outPosF, outPos);
                DirectX::XMStoreFloat3(&outNrmF, outNrm);

                const auto posConv = ConvertRS2Pos(outPosF);
                const auto nrmConv = ConvertRS2Pos(outNrmF);

                SkinVertex sv{};
                sv.x = posConv.x - rootConv.x;
                sv.y = posConv.y - rootConv.y;
                sv.z = posConv.z - rootConv.z;
                sv.nx = nrmConv.x;
                sv.ny = nrmConv.y;
                sv.nz = nrmConv.z;
                sv.color = 0xFFFFFFFF;
                sv.tu = face.uv[c].x;
                sv.tv = face.uv[c].y;
                sv.weights[0] = 1.0f;
                sv.weights[1] = 0.0f;
                sv.weights[2] = 0.0f;
                sv.weights[3] = 0.0f;
                sv.indices[0] = 0;
                sv.indices[1] = 0;
                sv.indices[2] = 0;
                sv.indices[3] = 0;

                const uint32_t v = static_cast<uint32_t>(m_vertices.size());
                if (v >= 65535) break;
                m_vertices.push_back(sv);
                m_indices.push_back(static_cast<unsigned short>(v));
            }
        }

        MeshSubset subset{};
        subset.MaterialID = node.mtrlId;
        subset.IndexStart = indexStart;
        subset.IndexCount = static_cast<uint32_t>(m_indices.size() - indexStart);
        if (subset.IndexCount > 0) m_subsets.push_back(subset);
    }
}

void RSkinObject::UploadLegacyCpuVertices(ID3D11DeviceContext* context) {
    if (!context || !m_loadedLegacy5007 || !m_vertexBuffer || m_vertices.empty()) return;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context->Map(m_vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        std::memcpy(mapped.pData, m_vertices.data(), m_vertices.size() * sizeof(SkinVertex));
        context->Unmap(m_vertexBuffer.Get(), 0);
    }
}

void RSkinObject::BuildSkeleton(const std::vector<std::string>& parentNames) {
    for (size_t i = 0; i < m_skeleton.size(); ++i) {
        if (!parentNames[i].empty()) {
            auto it = m_boneMap.find(parentNames[i]);
            if (it != m_boneMap.end()) m_skeleton[i].parentIdx = it->second;
        }
    }
    for (size_t i = 0; i < m_skeleton.size(); ++i) {
        if (m_skeleton[i].parentIdx != -1)
            m_skeleton[i].combinedMatrix = m_skeleton[i].localMatrix * m_skeleton[m_skeleton[i].parentIdx].combinedMatrix;
        else
            m_skeleton[i].combinedMatrix = m_skeleton[i].localMatrix;
        m_skeleton[i].offsetMatrix = DirectX::XMMatrixInverse(nullptr, m_skeleton[i].combinedMatrix);
    }
}

bool RSkinObject::LoadAni(const std::string& filename) {
    if (m_loadedLegacy5007) {
        m_aniMap.clear();
        m_frameCount = 1;
        m_maxTime = 1.0f;
        return true;
    }

    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) return false;

    FileFormat::ANI_HEADER header{};
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!file || header.sig != EXPORTER_SIG) return false;
    if (header.model_num < 0 || header.model_num > 2048) return false;

    m_frameCount = (std::max)(1, (std::min)(header.maxframe + 1, 4096));
    m_maxTime = static_cast<float>(m_frameCount);
    m_aniMap.clear();

    struct PosKeyDisk { float x, y, z; int frame; };
    struct QuatKeyDisk { float x, y, z, w; int frame; };
    struct VisKeyDisk { float v; int frame; };

    auto samplePos = [](const std::vector<PosKeyDisk>& keys, int frame) {
        if (keys.empty()) return DirectX::XMFLOAT3{0, 0, 0};
        if (keys.size() == 1 || frame <= keys.front().frame) return DirectX::XMFLOAT3{keys.front().x, keys.front().y, keys.front().z};
        if (frame >= keys.back().frame) return DirectX::XMFLOAT3{keys.back().x, keys.back().y, keys.back().z};

        for (size_t i = 0; i + 1 < keys.size(); ++i) {
            const auto& a = keys[i];
            const auto& b = keys[i + 1];
            if (frame >= a.frame && frame <= b.frame) {
                const float span = static_cast<float>((std::max)(1, b.frame - a.frame));
                const float t = static_cast<float>(frame - a.frame) / span;
                return DirectX::XMFLOAT3{
                    a.x + (b.x - a.x) * t,
                    a.y + (b.y - a.y) * t,
                    a.z + (b.z - a.z) * t
                };
            }
        }
        return DirectX::XMFLOAT3{keys.back().x, keys.back().y, keys.back().z};
    };

    auto sampleRot = [](const std::vector<QuatKeyDisk>& keys, int frame) {
        if (keys.empty()) return DirectX::XMFLOAT4{0, 0, 0, 1};
        if (keys.size() == 1 || frame <= keys.front().frame) return DirectX::XMFLOAT4{keys.front().x, keys.front().y, keys.front().z, keys.front().w};
        if (frame >= keys.back().frame) return DirectX::XMFLOAT4{keys.back().x, keys.back().y, keys.back().z, keys.back().w};

        for (size_t i = 0; i + 1 < keys.size(); ++i) {
            const auto& a = keys[i];
            const auto& b = keys[i + 1];
            if (frame >= a.frame && frame <= b.frame) {
                const float span = static_cast<float>((std::max)(1, b.frame - a.frame));
                const float t = static_cast<float>(frame - a.frame) / span;
                DirectX::XMVECTOR qa = DirectX::XMQuaternionNormalize(DirectX::XMVectorSet(a.x, a.y, a.z, a.w));
                DirectX::XMVECTOR qb = DirectX::XMQuaternionNormalize(DirectX::XMVectorSet(b.x, b.y, b.z, b.w));
                DirectX::XMFLOAT4 out;
                DirectX::XMStoreFloat4(&out, DirectX::XMQuaternionSlerp(qa, qb, t));
                return out;
            }
        }
        return DirectX::XMFLOAT4{keys.back().x, keys.back().y, keys.back().z, keys.back().w};
    };

    for (int i = 0; i < header.model_num; ++i) {
        char boneName[40] = {};
        DirectX::XMFLOAT4X4 base{};
        int posCnt = 0;
        int rotCnt = 0;
        uint32_t visCnt = 0;

        file.read(boneName, sizeof(boneName));
        file.read(reinterpret_cast<char*>(&base), sizeof(base));
        file.read(reinterpret_cast<char*>(&posCnt), sizeof(posCnt));
        if (!file || posCnt < 0 || posCnt > 100000) return false;

        std::vector<PosKeyDisk> posKeys(static_cast<size_t>(posCnt));
        if (posCnt > 0) {
            file.read(reinterpret_cast<char*>(posKeys.data()), sizeof(PosKeyDisk) * posKeys.size());
            if (!file) return false;
        }

        file.read(reinterpret_cast<char*>(&rotCnt), sizeof(rotCnt));
        if (!file || rotCnt < 0 || rotCnt > 100000) return false;

        std::vector<QuatKeyDisk> rotKeys(static_cast<size_t>(rotCnt));
        if (rotCnt > 0) {
            file.read(reinterpret_cast<char*>(rotKeys.data()), sizeof(QuatKeyDisk) * rotKeys.size());
            if (!file) return false;
        }

        if (header.ver > 0x00000012) {
            file.read(reinterpret_cast<char*>(&visCnt), sizeof(visCnt));
            if (!file || visCnt > 100000) return false;
            if (visCnt > 0) {
                file.seekg(static_cast<std::streamoff>(visCnt) * sizeof(VisKeyDisk), std::ios::cur);
                if (!file) return false;
            }
        }

        size_t bnLen = 0;
        while (bnLen < sizeof(boneName) && boneName[bnLen] != '\0') ++bnLen;
        BoneAni ani;
        ani.boneName = std::string(boneName, bnLen);
        ani.frames.reserve(m_frameCount);

        for (int f = 0; f < m_frameCount; ++f) {
            AniFrame frame{};
            frame.position = samplePos(posKeys, f);
            frame.rotation = sampleRot(rotKeys, f);
            ani.frames.push_back(frame);
        }

        m_aniMap[ani.boneName] = std::move(ani);
    }

    return !m_aniMap.empty();
}

void RSkinObject::UpdateAnimation(float deltaTime) {
    if (!m_isFrozen) m_animTime += deltaTime * 30.0f; // 30 FPS default
    if (m_animTime >= m_maxTime) m_animTime = fmod(m_animTime, m_maxTime);
    float frameT = m_sampleBindPose ? 0 : m_animTime;
    if (m_frameCount <= 0) return;
    int f0 = (int)floor(frameT) % m_frameCount;
    int f1 = (f0 + 1) % m_frameCount;
    float t = frameT - floor(frameT);
    for (size_t i = 0; i < m_skeleton.size(); ++i) {
        Bone& bone = m_skeleton[i];
        DirectX::XMMATRIX local;
        if (m_sampleBindPose || m_aniMap.count(bone.name) == 0) {
            local = bone.localMatrix;
        } else {
            const auto& ani = m_aniMap[bone.name];
            DirectX::XMVECTOR p = DirectX::XMVectorLerp(DirectX::XMLoadFloat3(&ani.frames[f0].position), DirectX::XMLoadFloat3(&ani.frames[f1].position), t);
            DirectX::XMVECTOR r = DirectX::XMQuaternionSlerp(DirectX::XMLoadFloat4(&ani.frames[f0].rotation), DirectX::XMLoadFloat4(&ani.frames[f1].rotation), t);
            local = DirectX::XMMatrixRotationQuaternion(r) * DirectX::XMMatrixTranslationFromVector(p);
        }
        if (bone.parentIdx != -1)
            bone.combinedMatrix = local * m_skeleton[bone.parentIdx].combinedMatrix;
        else
            bone.combinedMatrix = local;
    }

    if (m_loadedLegacy5007) {
        RebuildLegacyCpuVertices();
        m_legacyCpuSkinDirty = true;
    }
}

void RSkinObject::Update(float deltaTime) { UpdateAnimation(deltaTime); }

void RSkinObject::Draw(ID3D11DeviceContext* context, DirectX::FXMMATRIX worldViewProj, bool bOutline) {
    if (!m_vertexBuffer) {
        if (!CreateDX11Resources()) return;
    }
    if (!m_skinVS || !m_skinningCB) return;

    if (m_loadedLegacy5007 && m_legacyCpuSkinDirty) {
        UploadLegacyCpuVertices(context);
        m_legacyCpuSkinDirty = false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr;

    // Update Slot 0 (Matrix/Alpha)
    if (SUCCEEDED(context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        ConstantBuffer* cb = (ConstantBuffer*)mapped.pData;
        DirectX::XMMATRIX world =
            DirectX::XMMatrixRotationZ(m_worldYaw) *
            DirectX::XMMatrixTranslation(m_worldPos.x, m_worldPos.y, m_worldPos.z);
        cb->WorldViewProj = DirectX::XMMatrixTranspose(world * worldViewProj);
        cb->AlphaRef = 0.0f; // Fix: Prevent discarding everything
        cb->LightmapScale = 1.0f;
        cb->DebugMode = 0;
        context->Unmap(m_constantBuffer.Get(), 0);
    }

    // Update Slot 1 (Bone Matrices)
    hr = context->Map(m_skinningCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        SkinningConstantBuffer* scb = (SkinningConstantBuffer*)mapped.pData;
        if (m_loadedLegacy5007) {
            for (size_t i = 0; i < (size_t)MAX_BONES; ++i) {
                scb->boneMatrices[i] = DirectX::XMMatrixIdentity();
            }
        } else {
            for (size_t i = 0; i < m_skeleton.size() && i < (size_t)MAX_BONES; ++i) {
                scb->boneMatrices[i] = DirectX::XMMatrixTranspose(m_skeleton[i].offsetMatrix * m_skeleton[i].combinedMatrix);
            }
            for (size_t i = m_skeleton.size(); i < (size_t)MAX_BONES; ++i) {
                scb->boneMatrices[i] = DirectX::XMMatrixIdentity();
            }
        }
        context->Unmap(m_skinningCB.Get(), 0);
    }

    context->VSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());
    context->VSSetConstantBuffers(1, 1, m_skinningCB.GetAddressOf());
    context->PSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());
    context->IASetInputLayout(m_skinLayout.Get());
    context->VSSetShader(m_skinVS.Get(), nullptr, 0);
    context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
    context->PSSetSamplers(0, 1, m_samplerState.GetAddressOf());

    UINT stride = sizeof(SkinVertex), offset = 0;
    context->IASetVertexBuffers(0, 1, m_vertexBuffer.GetAddressOf(), &stride, &offset);
    context->IASetIndexBuffer(m_indexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    
    // Draw Subsets
    for(const auto& subset : m_subsets) {
        ID3D11ShaderResourceView* srv = nullptr;
        if (m_pTextureManager) {
            srv = m_pTextureManager->GetFallbackTexture().Get();
        }

        if (subset.MaterialID < (int)m_textureNames.size() && m_pTextureManager) {
            auto texContainer = m_pTextureManager->GetTexture(m_textureNames[subset.MaterialID]);
            if (texContainer) {
                srv = texContainer.Get();
            }
        }

        context->PSSetShaderResources(0, 1, &srv);
        context->DrawIndexed(subset.IndexCount, subset.IndexStart, 0);
    }
}

bool RSkinObject::CreateDX11Resources() {
    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;
    
    // Vertex Shader
    HRESULT hr = D3DCompileFromFile(L"Mesh.hlsl", nullptr, nullptr, "VS_Skin", "vs_5_0", 0, 0, &vsBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) AppLogger::Log("SHADER ERROR (VS_Skin): " + std::string((char*)errorBlob->GetBufferPointer()));
        else AppLogger::Log("SHADER ERROR: Mesh.hlsl nao encontrado.");
        return false;
    }
    m_pd3dDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_skinVS);

    // Pixel Shader (explicit for characters)
    hr = D3DCompileFromFile(L"Mesh.hlsl", nullptr, nullptr, "PS_Main", "ps_5_0", 0, 0, &psBlob, &errorBlob);
    if (SUCCEEDED(hr)) {
        m_pd3dDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_pixelShader);
    }

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 52, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    m_pd3dDevice->CreateInputLayout(layout, ARRAYSIZE(layout), vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_skinLayout);
    
    D3D11_BUFFER_DESC vbd = {
        (UINT)(m_vertices.size() * sizeof(SkinVertex)),
        m_loadedLegacy5007 ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT,
        D3D11_BIND_VERTEX_BUFFER,
        m_loadedLegacy5007 ? D3D11_CPU_ACCESS_WRITE : 0u,
        0,
        0
    };
    D3D11_SUBRESOURCE_DATA vd = { m_vertices.data() };
    m_pd3dDevice->CreateBuffer(&vbd, &vd, &m_vertexBuffer);
    
    D3D11_BUFFER_DESC ibd = { (UINT)(m_indices.size() * sizeof(unsigned short)), D3D11_USAGE_DEFAULT, D3D11_BIND_INDEX_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA id = { m_indices.data() };
    m_pd3dDevice->CreateBuffer(&ibd, &id, &m_indexBuffer);
    
    D3D11_BUFFER_DESC cbd0 = { sizeof(ConstantBuffer), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
    m_pd3dDevice->CreateBuffer(&cbd0, nullptr, &m_constantBuffer);
    
    D3D11_BUFFER_DESC cbd1 = { sizeof(SkinningConstantBuffer), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
    m_pd3dDevice->CreateBuffer(&cbd1, nullptr, &m_skinningCB);

    // Sampler State
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD = 0;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    m_pd3dDevice->CreateSamplerState(&sd, &m_samplerState);

    return true;
}

}
