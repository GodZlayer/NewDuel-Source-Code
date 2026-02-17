#pragma once
#include "RBspObject.h"
#include "RSkinObject.h"
#include "StateManager.h"
#include <vector>
#include <unordered_map>

namespace RealSpace3 {

class RScene {
public:
    RScene(ID3D11Device* device, ID3D11DeviceContext* context);
    ~RScene();

    void LoadCharSelect();
    void LoadLobbyBasic();
    void Update(float deltaTime);
    void Draw(ID3D11DeviceContext* context, DirectX::FXMMATRIX viewProj);
    bool GetPreferredCamera(DirectX::XMFLOAT3& outPos, DirectX::XMFLOAT3& outDir) const;
    void SetCreationPreview(int sex, int face, int preset, int hair);
    void SetCreationPreviewVisible(bool visible);
    bool GetSpawnPos(DirectX::XMFLOAT3& outPos) const;

private:
    void BuildCreationVariants();
    void ApplyCreationPreview();
    bool FileExists(const std::string& path) const;
    void EnsureItemMeshMapLoaded();
    std::string GetMeshNameByItemId(uint32_t itemId) const;

    ID3D11Device* m_pd3dDevice;
    std::unique_ptr<RStateManager> m_stateManager;
    std::unique_ptr<TextureManager> m_textureManager;
    std::unique_ptr<RBspObject> m_map;
    std::unique_ptr<RSkinObject> m_character;
    DirectX::XMFLOAT3 m_cameraPos = { 0.0f, -500.0f, 200.0f };
    DirectX::XMFLOAT3 m_cameraDir = { 0.0f, 500.0f, -100.0f };
    bool m_hasCameraDummy = false;
    DirectX::XMFLOAT3 m_spawnPos = { 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 m_spawnDir = { 1.0f, 0.0f, 0.0f };
    bool m_hasSpawn = false;
    int m_previewSex = 0;
    int m_previewFace = 0;
    int m_previewPreset = 0;
    int m_previewHair = 0;
    bool m_previewVisible = false;
    std::vector<std::string> m_maleVariants;
    std::vector<std::string> m_femaleVariants;
    std::vector<std::string> m_malePartLibraries;
    std::vector<std::string> m_femalePartLibraries;
    std::unordered_map<uint32_t, std::string> m_itemMeshById;
    bool m_itemMeshMapLoaded = false;
    
    // Debug Attachment
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cubeVB;
    void CreateDebugCube();
    void DrawDebugCube(ID3D11DeviceContext* context, DirectX::FXMMATRIX worldViewProj);
};

}
