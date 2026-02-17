#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../Include/RBspObject.h"
#include "AppLogger.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <map>
#include <cstring>
#include <windows.h>
#include <direct.h>
#include <functional>
#include <d3dcompiler.h>
#include <filesystem>
#include <array>
#include <cctype>

namespace RealSpace3 {
namespace {
std::string ToLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool StartsWithInsensitive(const std::string& value, const std::string& prefix) {
    if (value.size() < prefix.size()) return false;
    return ToLowerCopy(value.substr(0, prefix.size())) == ToLowerCopy(prefix);
}

std::string ResolveCaseInsensitivePath(const std::string& rawPath) {
    namespace fs = std::filesystem;

    fs::path input(rawPath);
    if (fs::exists(input)) {
        return input.generic_string();
    }

    fs::path current = input.is_absolute() ? input.root_path() : fs::current_path();
    fs::path rel = input.is_absolute() ? input.relative_path() : input;

    for (const auto& part : rel) {
        if (part == ".") continue;
        if (part == "..") {
            current /= part;
            continue;
        }

        fs::path exact = current / part;
        if (fs::exists(exact)) {
            current = exact;
            continue;
        }

        if (!fs::exists(current) || !fs::is_directory(current)) {
            return {};
        }

        std::string wanted = ToLowerCopy(part.string());
        bool found = false;
        for (const auto& entry : fs::directory_iterator(current)) {
            if (ToLowerCopy(entry.path().filename().string()) == wanted) {
                current = entry.path();
                found = true;
                break;
            }
        }

        if (!found) {
            return {};
        }
    }

    if (!fs::exists(current)) {
        return {};
    }

    return current.generic_string();
}

std::vector<std::string> BuildMapCandidates(const std::string& requested) {
    namespace fs = std::filesystem;

    std::vector<std::string> candidates;
    candidates.push_back(requested);

    fs::path requestedPath(requested);
    std::string ext = ToLowerCopy(requestedPath.extension().string());
    if (ext != ".rs") {
        candidates.push_back(requested + ".rs");
    }

    const std::string normalized = requestedPath.generic_string();
    const bool hasPath = normalized.find('/') != std::string::npos || normalized.find('\\') != std::string::npos;
    const bool hasMapsPrefix = StartsWithInsensitive(normalized, "Maps/");
    const bool hasInterfacePrefix = StartsWithInsensitive(normalized, "Interface/");

    if (hasMapsPrefix) {
        candidates.push_back(normalized.substr(5));
    }
    else if (hasPath && !hasInterfacePrefix) {
        candidates.push_back("Maps/" + normalized);
    }

    if (!hasPath) {
        const std::string name = requestedPath.stem().string();
        candidates.push_back("Maps/" + name + "/" + name + ".rs");
    }

    return candidates;
}

std::string FindMapByStem(const std::string& requested) {
    namespace fs = std::filesystem;

    std::string stem = fs::path(requested).stem().string();
    if (stem.empty()) stem = requested;
    if (stem.empty()) return {};

    std::string wanted = ToLowerCopy(stem);
    const std::array<std::string, 2> roots = { "Interface", "Maps" };

    for (const auto& root : roots) {
        std::error_code ec;
        if (!fs::exists(root, ec)) continue;

        for (fs::recursive_directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec)) {
            if (ec || !it->is_regular_file()) continue;

            const auto path = it->path();
            if (ToLowerCopy(path.extension().string()) != ".rs") continue;
            if (ToLowerCopy(path.stem().string()) == wanted) {
                return path.generic_string();
            }
        }
    }

    return {};
}
}

#pragma pack(push, 1)
namespace FileFormat {
    struct RHEADER { uint32_t dwID; uint32_t dwVersion; };
    struct RS_DISK_COUNTS { int32_t nNodes; int32_t nPolygons; int32_t nVertices; int32_t nIndices; };
    struct BSPVERTEX_DISK { 
        float x, y, z; 
        float nx, ny, nz; 
        float tu1, tv1; 
        float tu2, tv2; 
    };
    struct RPOLYGONINFO_DISK {
        int32_t nMaterial;
        int32_t nConvexPolygon;
        uint32_t dwFlags;
        int32_t nVertices;
    };
}
#pragma pack(pop)

#define RS_ID 0x12345678

RSBspNode::RSBspNode() : nPolygon(0), nSubsetIdx(0), nSubsetCount(0), nPositiveIdx(NODE_INDEX_NULL), 
    nNegativeIdx(NODE_INDEX_NULL), nFrameCount(-1), dwFlags(0) {
    plane = {0,0,0,0}; bbTree = {{0,0,0}, {0,0,0}};
}
RSBspNode::~RSBspNode() {}

template<typename T>
static bool Read(const std::vector<uint8_t>& b, size_t& off, T& out) {
    if (off + sizeof(T) > b.size()) return false;
    std::memcpy(&out, b.data() + off, sizeof(T));
    off += sizeof(T);
    return true;
}

template<typename T>
static bool ReadArray(const std::vector<uint8_t>& b, size_t& off, T* out, size_t count) {
    size_t sz = sizeof(T) * count;
    if (off + sz > b.size()) return false;
    std::memcpy(out, b.data() + off, sz);
    off += sz;
    return true;
}

static bool ReadCString(const std::vector<uint8_t>& b, size_t& off, std::string& out) {
    size_t start = off;
    while (off < b.size() && b[off] != 0) off++;
    if (off >= b.size()) return false;
    out.assign(reinterpret_cast<const char*>(b.data() + start), off - start);
    off++; 
    return true;
}

// Minimal XML helper — extracts text content between <TAG> and </TAG>
static std::string ExtractXMLTag(const std::string& xml, const std::string& tag, size_t startPos = 0, size_t* foundPos = nullptr) {
    std::string openTag = "<" + tag + ">";
    std::string closeTag = "</" + tag + ">";
    
    size_t start = xml.find(openTag, startPos);
    if (start == std::string::npos) {
        // Try with attributes: <TAG ...>
        std::string openTagAttr = "<" + tag + " ";
        start = xml.find(openTagAttr, startPos);
        if (start == std::string::npos) {
            if (foundPos) *foundPos = std::string::npos;
            return "";
        }
        start = xml.find(">", start);
        if (start == std::string::npos) {
            if (foundPos) *foundPos = std::string::npos;
            return "";
        }
        start += 1;
    } else {
        start += openTag.length();
    }
    
    size_t end = xml.find(closeTag, start);
    if (end == std::string::npos) {
        if (foundPos) *foundPos = std::string::npos;
        return "";
    }
    
    if (foundPos) *foundPos = end + closeTag.length();
    return xml.substr(start, end - start);
}

static bool ParseFloat3(const std::string& text, DirectX::XMFLOAT3& out) {
    std::istringstream iss(text);
    return static_cast<bool>(iss >> out.x >> out.y >> out.z);
}

static void ParseDummiesFromXML(const std::string& xmlContent, std::vector<RDummy>& outDummies) {
    outDummies.clear();

    std::string dummyList = ExtractXMLTag(xmlContent, "DUMMYLIST");
    if (dummyList.empty()) return;

    size_t searchPos = 0;
    while (true) {
        size_t dummyStart = dummyList.find("<DUMMY", searchPos);
        if (dummyStart == std::string::npos) break;

        size_t dummyEnd = dummyList.find("</DUMMY>", dummyStart);
        if (dummyEnd == std::string::npos) break;
        dummyEnd += 8; // strlen("</DUMMY>")

        std::string block = dummyList.substr(dummyStart, dummyEnd - dummyStart);
        searchPos = dummyEnd;

        size_t nameAttr = block.find("name=\"");
        if (nameAttr == std::string::npos) continue;
        nameAttr += 6;
        size_t nameEnd = block.find('"', nameAttr);
        if (nameEnd == std::string::npos) continue;

        RDummy dummy;
        dummy.Name = block.substr(nameAttr, nameEnd - nameAttr);

        const std::string posText = ExtractXMLTag(block, "POSITION");
        const std::string dirText = ExtractXMLTag(block, "DIRECTION");
        if (!ParseFloat3(posText, dummy.Position) || !ParseFloat3(dirText, dummy.Direction)) continue;

        outDummies.push_back(dummy);
    }
}

RBspObject::RBspObject(ID3D11Device* device, TextureManager* pTexMgr) : m_pd3dDevice(device), m_pTextureManager(pTexMgr) {}
RBspObject::~RBspObject() {}

int RBspObject::Open_Nodes(const std::vector<uint8_t>& buffer, size_t& offset, int depth, std::map<int, std::vector<unsigned short>>& materialMap) {
    if (depth > (int)MAX_BSP_DEPTH) return NODE_INDEX_NULL;
    static uint32_t s_poolIdx = 0; if (depth == 0) s_poolIdx = 0;
    
    uint32_t currentIdx = s_poolIdx++;
    if (currentIdx >= m_stats.nNodes) return NODE_INDEX_NULL;
    
    RSBspNode& node = m_OcRoot[currentIdx];
    float p[4], vmin[3], vmax[3];
    Read(buffer, offset, p); Read(buffer, offset, vmin); Read(buffer, offset, vmax);
    node.plane = {p[0], p[1], p[2], p[3]};
    node.bbTree.vmin = {vmin[0], vmin[1], vmin[2]}; node.bbTree.vmax = {vmax[0], vmax[1], vmax[2]};

    uint8_t hasP, hasN;
    Read(buffer, offset, hasP); if (hasP) node.nPositiveIdx = Open_Nodes(buffer, offset, depth + 1, materialMap);
    Read(buffer, offset, hasN); if (hasN) node.nNegativeIdx = Open_Nodes(buffer, offset, depth + 1, materialMap);

    Read(buffer, offset, node.nPolygon);
    
    for (int i = 0; i < node.nPolygon; ++i) {
        FileFormat::RPOLYGONINFO_DISK poly; Read(buffer, offset, poly);
        
        int matID = poly.nMaterial + 1; // Legacy mat numbering offset
        if (matID < 0 || matID >= (int)m_materialSRVs.size()) matID = 0;

        uint32_t startVert = (uint32_t)m_OcVertices.size();

        for (int j = 0; j < poly.nVertices; j++) {
            FileFormat::BSPVERTEX_DISK v; Read(buffer, offset, v);
            BSPVERTEX bv;
            bv.x = v.x; bv.y = v.y; bv.z = v.z;
            bv.nx = v.nx; bv.ny = v.ny; bv.nz = v.nz;
            bv.color = 0xFFFFFFFF;
            bv.tu1 = v.tu1; bv.tv1 = v.tv1;
            bv.tu2 = v.tu2; bv.tv2 = v.tv2;
            m_OcVertices.push_back(bv);
        }

        // Generate indices for triangle fan
        for (int k = 0; k < poly.nVertices - 2; k++) {
            materialMap[matID].push_back((unsigned short)(startVert));
            materialMap[matID].push_back((unsigned short)(startVert + k + 1));
            materialMap[matID].push_back((unsigned short)(startVert + k + 2));
        }

        // READ MISSING SURFACE NORMAL (12 bytes)
        float surfaceNormal[3];
        Read(buffer, offset, surfaceNormal);
    }
    return (int)currentIdx;
}

bool RBspObject::LoadXMLMaterials(const std::string& xmlPath) {
    std::ifstream file(xmlPath);
    if (!file.is_open()) {
        AppLogger::Log("[RBspObject] XML not found: " + xmlPath);
        return false;
    }
    
    std::string xmlContent((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    
    AppLogger::Log("[RBspObject] Parsing XML materials from: " + xmlPath);
    
    // Set TextureManager base directory to map folder
    if (m_pTextureManager) {
        m_pTextureManager->SetBaseDirectory(m_baseDirectory);
    }

    ParseDummiesFromXML(xmlContent, m_dummies);
    AppLogger::Log("[RBspObject] Dummies parsed: " + std::to_string(m_dummies.size()));
    
    // Find <MATERIALLIST>
    std::string matListContent = ExtractXMLTag(xmlContent, "MATERIALLIST");
    if (matListContent.empty()) {
        AppLogger::Log("[RBspObject] No MATERIALLIST found in XML");
        return false;
    }
    
    // Clear and rebuild material SRVs
    m_materialSRVs.clear();
    
    // Index 0 = fallback material (no texture)
    if (m_pTextureManager)
        m_materialSRVs.push_back(m_pTextureManager->GetWhiteTexture());
    else
        m_materialSRVs.push_back(nullptr);
    
    // Parse each <MATERIAL> block
    size_t searchPos = 0;
    int matIndex = 0;
    while (true) {
        // Find next <MATERIAL
        size_t matStart = matListContent.find("<MATERIAL", searchPos);
        if (matStart == std::string::npos) break;
        
        size_t matEnd = matListContent.find("</MATERIAL>", matStart);
        if (matEnd == std::string::npos) break;
        matEnd += 11; // strlen("</MATERIAL>")
        
        std::string matBlock = matListContent.substr(matStart, matEnd - matStart);
        searchPos = matEnd;
        
        // Extract DIFFUSEMAP
        std::string diffuseMap = ExtractXMLTag(matBlock, "DIFFUSEMAP");
        
        // Trim whitespace
        while (!diffuseMap.empty() && (diffuseMap.front() == ' ' || diffuseMap.front() == '\t' || diffuseMap.front() == '\r' || diffuseMap.front() == '\n'))
            diffuseMap.erase(diffuseMap.begin());
        while (!diffuseMap.empty() && (diffuseMap.back() == ' ' || diffuseMap.back() == '\t' || diffuseMap.back() == '\r' || diffuseMap.back() == '\n'))
            diffuseMap.pop_back();
        
        matIndex++;
        
        if (diffuseMap.empty()) {
            // No texture — use white
            if (m_pTextureManager)
                m_materialSRVs.push_back(m_pTextureManager->GetWhiteTexture());
            else
                m_materialSRVs.push_back(nullptr);
            continue;
        }
        
        // Load via TextureManager (which handles path resolution and DDS loading)
        if (m_pTextureManager) {
            auto srv = m_pTextureManager->GetTexture(diffuseMap);
            m_materialSRVs.push_back(srv);
        } else {
            m_materialSRVs.push_back(nullptr);
        }
    }
    
    AppLogger::Log("[RBspObject] Loaded " + std::to_string(matIndex) + " materials from XML (" + std::to_string(m_materialSRVs.size()) + " total with fallback)");
    return true;
}

bool RBspObject::Open(const std::string& filename) {
    m_lastError = "None";
    std::string resolvedPath;

    for (const auto& candidate : BuildMapCandidates(filename)) {
        resolvedPath = ResolveCaseInsensitivePath(candidate);
        if (!resolvedPath.empty()) break;
    }
    if (resolvedPath.empty()) {
        resolvedPath = FindMapByStem(filename);
    }
    if (resolvedPath.empty()) {
        m_lastError = "Could not resolve map path: " + filename;
        AppLogger::Log("[RBspObject] Open FAILED: " + m_lastError);
        return false;
    }

    m_filename = resolvedPath;
    AppLogger::Log("[RBspObject] Opening: " + m_filename);
    
    // Extract base directory for texture lookup
    m_baseDirectory = m_filename;
    auto lastSlash = m_baseDirectory.find_last_of("/\\");
    if (lastSlash != std::string::npos) {
        m_baseDirectory = m_baseDirectory.substr(0, lastSlash);
    } else {
        m_baseDirectory = ".";
    }
    AppLogger::Log("[RBspObject] Base directory: " + m_baseDirectory);
    
    // Load XML materials first (before BSP, so m_materialSRVs is populated)
    std::string xmlPath = m_filename + ".xml";
    // Try case-insensitive: Mansion.RS.xml vs mansion.rs.xml
    {
        WIN32_FIND_DATAA fd;
        std::string pattern = m_baseDirectory + "/*.xml";
        HANDLE hFind = FindFirstFileA(pattern.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                std::string found = fd.cFileName;
                std::string foundLower = found;
                std::transform(foundLower.begin(), foundLower.end(), foundLower.begin(), ::tolower);
                
                // Extract the base name of our .RS file
                std::string rsBase = m_filename;
                auto rsSlash = rsBase.find_last_of("/\\");
                if (rsSlash != std::string::npos) rsBase = rsBase.substr(rsSlash + 1);
                std::string rsBaseLower = rsBase;
                std::transform(rsBaseLower.begin(), rsBaseLower.end(), rsBaseLower.begin(), ::tolower);
                
                std::string expectedXml = rsBaseLower + ".xml";
                
                if (foundLower == expectedXml) {
                    xmlPath = m_baseDirectory + "/" + found;
                    AppLogger::Log("[RBspObject] Found XML (case-insensitive): " + xmlPath);
                    break;
                }
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
    }
    
    LoadXMLMaterials(xmlPath);
    
    // If XML loading didn't populate materials, use a minimal fallback
    if (m_materialSRVs.empty()) {
        m_materialSRVs.push_back(m_pTextureManager ? m_pTextureManager->GetWhiteTexture() : nullptr);
    }
    
    std::ifstream file(m_filename, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        m_lastError = "Could not open file: " + m_filename;
        return false;
    }
    
    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(fileSize);
    file.read(reinterpret_cast<char*>(buffer.data()), fileSize);
    file.close();

    size_t offset = 0;
    uint32_t dwID, dwVer; 
    if (!Read(buffer, offset, dwID) || !Read(buffer, offset, dwVer)) {
        m_lastError = "Failed to read header";
        return false;
    }
    
    AppLogger::Log("[RBspObject] Header: 0x" + std::to_string(dwID) + " Ver: " + std::to_string(dwVer));
    
    // 1. Skip Materials in RS binary (we loaded from XML instead)
    int32_t nMat; 
    if (!Read(buffer, offset, nMat)) { m_lastError = "Failed to read nMat"; return false; }
    AppLogger::Log("[RBspObject] Binary materials (skipping): " + std::to_string(nMat));
    
    for (int i = 0; i < nMat; i++) { 
        std::string s; if (!ReadCString(buffer, offset, s)) break;
    }

    // Skip Convex Polygons
    int32_t nCP, cpExt; 
    if (!Read(buffer, offset, nCP) || !Read(buffer, offset, cpExt)) { m_lastError = "Failed to read CP counts"; return false; }
    AppLogger::Log("[RBspObject] ConvexPolys: " + std::to_string(nCP));
    
    for (int i = 0; i < nCP; i++) {
        offset += 28; // plane(16) + flags(4) + mat(4) + area(4)
        int32_t nV; if (!Read(buffer, offset, nV)) break;
        offset += (size_t)nV * 24; 
    }

    // RS2 format has 4 ints for counts, then ANOTHER 4 ints (total 8)
    FileFormat::RS_DISK_COUNTS actual; 
    if (!Read(buffer, offset, actual)) { m_lastError = "Failed to read actual counts"; return false; }
    
    // Skip the redundant 4 ints
    offset += 16; 
    
    m_stats.nNodes = actual.nNodes;
    m_stats.nPolygons = actual.nPolygons;
    
    AppLogger::Log("[RBspObject] Nodes: " + std::to_string(actual.nNodes) + " Polys: " + std::to_string(actual.nPolygons));

    // 2. Load Nodes & Interleaved Geometries
    m_OcRoot.clear(); m_OcRoot.resize(actual.nNodes);
    m_OcVertices.clear();
    m_OcIndices.clear();
    m_drawSubsetsVector.clear();

    std::map<int, std::vector<unsigned short>> materialMap;
    Open_Nodes(buffer, offset, 0, materialMap);

    // Flatten Map to Subsets
    for(auto& pair : materialMap) {
        DrawSubset subset;
        subset.MaterialID = (uint32_t)pair.first;
        subset.IndexStart = (uint32_t)m_OcIndices.size();
        subset.IndexCount = (uint32_t)pair.second.size();
        m_OcIndices.insert(m_OcIndices.end(), pair.second.begin(), pair.second.end());
        m_drawSubsetsVector.push_back(subset);
    }

    // Bounding Box
    DirectX::XMFLOAT3 vMin = {1e9f,1e9f,1e9f}, vMax = {-1e9f,-1e9f,-1e9f};
    if (m_OcVertices.empty()) {
        vMin = {0,0,0}; vMax = {0,0,0};
    } else {
        for(const auto& v : m_OcVertices) {
            vMin.x = (std::min)(vMin.x, v.x); vMin.y = (std::min)(vMin.y, v.y); vMin.z = (std::min)(vMin.z, v.z);
            vMax.x = (std::max)(vMax.x, v.x); vMax.y = (std::max)(vMax.y, v.y); vMax.z = (std::max)(vMax.z, v.z);
        }
    }
    m_stats.vMin = {vMin.x, vMin.y, vMin.z};
    m_stats.vMax = {vMax.x, vMax.y, vMax.z};
    
    AppLogger::Log("[RBspObject] BBox: Min(" + std::to_string(vMin.x) + "," + std::to_string(vMin.y) + "," + std::to_string(vMin.z) + ") Max(" + std::to_string(vMax.x) + "," + std::to_string(vMax.y) + "," + std::to_string(vMax.z) + ")");
    AppLogger::Log("[RBspObject] Verts: " + std::to_string(m_OcVertices.size()) + " Indices: " + std::to_string(m_OcIndices.size()) + " Subsets: " + std::to_string(m_drawSubsetsVector.size()));

    if (!CreateDX11Resources()) { m_lastError = "CreateDX11Resources failed"; return false; }

    if (!m_OcVertices.empty()) {
        D3D11_BUFFER_DESC vbd = { (UINT)(m_OcVertices.size() * sizeof(BSPVERTEX)), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
        D3D11_SUBRESOURCE_DATA vd = { m_OcVertices.data() };
        m_pd3dDevice->CreateBuffer(&vbd, &vd, &m_vertexBuffer);
    }
    
    if (!m_OcIndices.empty()) {
        D3D11_BUFFER_DESC ibd = { (UINT)(m_OcIndices.size() * sizeof(unsigned short)), D3D11_USAGE_DEFAULT, D3D11_BIND_INDEX_BUFFER, 0, 0, 0 };
        D3D11_SUBRESOURCE_DATA id = { m_OcIndices.data() };
        m_pd3dDevice->CreateBuffer(&ibd, &id, &m_indexBuffer);
    }

    return true;
}

void RBspObject::Draw(ID3D11DeviceContext* context, const DirectX::FXMMATRIX& viewProj, const rfrustum& frustum, RenderMode mode, bool bWireframe) {
    if (!m_vertexBuffer || !m_indexBuffer) return;
    
    // Update Constant Buffer
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        ConstantBuffer* cb = (ConstantBuffer*)mapped.pData;
        cb->WorldViewProj = DirectX::XMMatrixTranspose(viewProj); 
        cb->AlphaRef = 0.0f;
        cb->LightmapScale = 1.0f;
        cb->DebugMode = 0; 
        context->Unmap(m_constantBuffer.Get(), 0);
    }

    // Set Shaders
    context->IASetInputLayout(m_inputLayout.Get());
    context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
    
    // Bind Constant Buffer to BOTH VS and PS
    context->VSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());
    context->PSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());
    
    // Bind Sampler State to PS
    context->PSSetSamplers(0, 1, m_samplerState.GetAddressOf());
    
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    UINT stride = sizeof(BSPVERTEX), offset = 0;
    context->IASetVertexBuffers(0, 1, m_vertexBuffer.GetAddressOf(), &stride, &offset);
    context->IASetIndexBuffer(m_indexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);

    // Draw Subsets
    for (const auto& subset : m_drawSubsetsVector) {
        // Bind Texture
        ID3D11ShaderResourceView* pSRV = nullptr;
        if (subset.MaterialID < m_materialSRVs.size() && m_materialSRVs[subset.MaterialID]) {
            pSRV = m_materialSRVs[subset.MaterialID].Get();
        } else if (m_pTextureManager) {
            pSRV = m_pTextureManager->GetWhiteTexture().Get();
        }
        
        if (pSRV) {
            context->PSSetShaderResources(0, 1, &pSRV);
        }
        
        context->DrawIndexed(subset.IndexCount, subset.IndexStart, 0);
    }
}

void RBspObject::Update(float dt) {}
bool RBspObject::IsVisible(const rboundingbox& b, const rfrustum& f) { return true; }
void RBspObject::RenderNode(ID3D11DeviceContext* c, int32_t n, const rfrustum& f) {}
void RBspObject::UpdateTraversalChecksum(const RSBspNode& n) {}
bool RBspObject::LoadXML(const std::string& f) { return true; }
bool RBspObject::OpenLightmap(const std::string& f) { return true; }
void RBspObject::DrawSubsets(ID3D11DeviceContext* c, const std::vector<uint32_t>& s) {}

bool RBspObject::CreateDX11Resources() {
    if (m_vertexShader && m_pixelShader) return true;

    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;
    DWORD dwShaderFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    dwShaderFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    // Vertex Shader
    HRESULT hr = D3DCompileFromFile(L"Mesh.hlsl", nullptr, nullptr, "VS_Main", "vs_5_0", dwShaderFlags, 0, &vsBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) OutputDebugStringA((char*)errorBlob->GetBufferPointer());
        AppLogger::Log("[RBspObject] Failed to compile VS_Main");
        return false;
    }
    m_pd3dDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vertexShader);

    // Pixel Shader
    hr = D3DCompileFromFile(L"Mesh.hlsl", nullptr, nullptr, "PS_Main", "ps_5_0", dwShaderFlags, 0, &psBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) OutputDebugStringA((char*)errorBlob->GetBufferPointer());
        AppLogger::Log("[RBspObject] Failed to compile PS_Main");
        return false;
    }
    m_pd3dDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_pixelShader);

    // Input Layout
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,    0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    m_pd3dDevice->CreateInputLayout(layout, ARRAYSIZE(layout), vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_inputLayout);

    // Constant Buffer
    D3D11_BUFFER_DESC cbd = { sizeof(ConstantBuffer), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
    m_pd3dDevice->CreateBuffer(&cbd, nullptr, &m_constantBuffer);

    // Rasterizer States
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE; 
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable = TRUE;
    m_pd3dDevice->CreateRasterizerState(&rd, &m_rsSolid);

    rd.FillMode = D3D11_FILL_WIREFRAME;
    rd.CullMode = D3D11_CULL_NONE;
    m_pd3dDevice->CreateRasterizerState(&rd, &m_rsWireframe);

    // Sampler State
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD = 0;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    m_pd3dDevice->CreateSamplerState(&sd, &m_samplerState);

    return true;
}

}
