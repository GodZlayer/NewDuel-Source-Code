#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <map>
#include <unordered_map>
#include <array>
#include <DirectXMath.h>
#include "Types.h"
#include "TextureManager.h"

namespace RealSpace3 {

class RSkinObject {
public:
    RSkinObject(ID3D11Device* device, TextureManager* pTexMgr);
    ~RSkinObject();

    bool LoadElu(const std::string& filename);
    bool ParseAniFile(const std::string& filename, std::map<std::string, BoneAni>& outAniMap, int& outFrameCount, float& outMaxTime);
    bool AppendLegacyPartsFromElu5007(const std::string& filename, bool forceVisibleNonEquip = false);
    bool LoadAni(const std::string& filename);
    
    void Update(float deltaTime); 
    void Draw(ID3D11DeviceContext* context, DirectX::FXMMATRIX worldViewProj, bool bOutline = false);
    void SetWorldPosition(const DirectX::XMFLOAT3& pos) { m_worldPos = pos; }
    void SetWorldYaw(float yawRadians) { m_worldYaw = yawRadians; }

    // RS3: Server-authoritative state
    void SetServerTransform(const Transform& transform);
    void SetAnimationState(const AnimationState& state);

    // RS3: Animation loading by ID
    bool LoadAnimation(int32_t animId, const std::string& filename);

    // Controles de Debug e Attachment
    void SetFreeze(bool b) { m_isFrozen = b; }
    void Step(float dt) { m_animTime += dt; }
    void SetBindPoseOnly(bool b) { m_sampleBindPose = b; }
    void SetUseOgzTripleWeights(bool enabled) { m_useOgzTripleWeights = enabled; }
    DirectX::XMMATRIX GetBoneMatrix(const std::string& boneName);
    bool SetLegacyPart(const std::string& category, const std::string& nodeName);
    bool SetLegacyPartByIndex(const std::string& category, int index);
    void ResetLegacyPartSelection();

private:
    struct LegacyPhysique {
        int32_t parentId[4] = { -1, -1, -1, -1 };
        float weight[4] = { 0, 0, 0, 0 };
        int32_t num = 0;
        DirectX::XMFLOAT3 offset[4] = {};
    };

    struct LegacyFace {
        int32_t pointIndex[3] = { 0, 0, 0 };
        DirectX::XMFLOAT2 uv[3] = {};
    };

    struct LegacyFaceNormal {
        DirectX::XMFLOAT3 pointNormal[3] = {};
    };

    struct LegacyNode {
        std::string name;
        int32_t mtrlId = 0;
        int32_t nodeBoneId = -1;
        bool forceVisibleNonEquip = false;
        std::vector<DirectX::XMFLOAT3> points;
        std::vector<LegacyFace> faces;
        std::vector<LegacyFaceNormal> faceNormals;
        std::vector<LegacyPhysique> physique;
    };

    bool LoadEluLegacy5007(const std::string& filename);
    void RebuildLegacyCpuVertices();
    void UploadLegacyCpuVertices(ID3D11DeviceContext* context);
    ID3D11Device* m_pd3dDevice;
    TextureManager* m_pTextureManager;
    
    // Skeleton
    std::vector<Bone> m_skeleton;
    std::map<std::string, int32_t> m_boneMap;
    
    struct MeshSubset {
        int32_t MaterialID;
        uint32_t IndexStart;
        uint32_t IndexCount;
    };

    // Mesh Data
    std::vector<SkinVertex> m_vertices;
    std::vector<unsigned short> m_indices;
    std::vector<MeshSubset> m_subsets;
    std::vector<std::string> m_textureNames;
    std::vector<LegacyNode> m_legacyNodes;

    enum class LegacyPartGroup : int {
        Face = 0,
        Head,
        Chest,
        Hands,
        Legs,
        Feet,
        Count
    };
    std::array<std::string, static_cast<size_t>(LegacyPartGroup::Count)> m_legacyPartSelection;
    
    // Animation Data

    std::map<std::string, BoneAni> m_aniMap;
    float m_animTime = 0.0f;
    float m_maxTime = 0.0f;
    int32_t m_frameCount = 0;

    // Flags de Controle
    bool m_isFrozen = false;
    bool m_sampleBindPose = false;
    bool m_useOgzTripleWeights = false;
    bool m_loadedLegacy5007 = false;
    bool m_legacyCpuSkinDirty = false;
    DirectX::XMFLOAT3 m_worldPos = { 0.0f, 0.0f, 0.0f };
    float m_worldYaw = 0.0f;

    // DX11
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_vertexBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_indexBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_constantBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_skinningCB;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_skinVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>  m_pixelShader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout>  m_skinLayout;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_samplerState;

    void BuildSkeleton(const std::vector<std::string>& parentNames);
    bool CreateDX11Resources();
    void UpdateAnimation(float deltaTime);
    LegacyPartGroup ClassifyLegacyPartNode(const std::string& nodeName) const;
    bool IsLegacyPartNodeVisible(const std::string& nodeName) const;
};

}
