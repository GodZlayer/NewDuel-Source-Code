#pragma once

#include "ScenePackageLoader.h"
#include "StateManager.h"
#include "TextureManager.h"
#include "Types.h"
#include "Model/CharacterAssembler.h"

#include <DirectXMath.h>
#include <d3d11.h>
#include <array>
#include <memory>
#include <string>
#include <vector>
#include <wrl/client.h>

namespace RealSpace3 {

class RScene {
public:
    RScene(ID3D11Device* device, ID3D11DeviceContext* context);
    ~RScene();

    void LoadCharSelect();
    bool LoadCharSelectPackage(const std::string& sceneId);
    void LoadLobbyBasic();
    void Update(float deltaTime);
    void DrawWorld(ID3D11DeviceContext* context, DirectX::FXMMATRIX viewProj);
    void DrawShowcase(ID3D11DeviceContext* context, DirectX::FXMMATRIX viewProj, bool forceNoDepthTest);
    void Draw(ID3D11DeviceContext* context, DirectX::FXMMATRIX viewProj);
    void SetShowcaseViewportPixels(int x, int y, int width, int height);
    bool GetPreferredCamera(DirectX::XMFLOAT3& outPos, DirectX::XMFLOAT3& outDir) const;
    bool SetCreationPreview(int sex, int face, int preset, int hair);
    void SetCreationPreviewVisible(bool visible);
    bool AdjustCreationCamera(float yawDeltaDeg, float pitchDeltaDeg, float zoomDelta);
    bool AdjustCreationCharacterYaw(float yawDeltaDeg);
    bool SetCreationCameraPose(float yawDeg, float pitchDeg, float distance, float focusHeight, bool autoOrbit);
    void SetCreationCameraAutoOrbit(bool enabled);
    void ResetCreationCamera();
    bool GetSpawnPos(DirectX::XMFLOAT3& outPos) const;

private:
    struct MapGpuVertex {
        DirectX::XMFLOAT3 pos;
        DirectX::XMFLOAT3 normal;
        DirectX::XMFLOAT2 uv;
    };

    struct MapSectionRuntime {
        uint32_t indexStart = 0;
        uint32_t indexCount = 0;
        uint32_t materialFlags = 0;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> diffuseSRV;
    };

    struct MapPerFrameCB {
        DirectX::XMFLOAT4X4 viewProj;
        DirectX::XMFLOAT4 lightDirIntensity;
        DirectX::XMFLOAT4 lightColorFogMin;
        DirectX::XMFLOAT4 fogColorFogMax;
        DirectX::XMFLOAT4 cameraPosFogEnabled;
        DirectX::XMFLOAT4 renderParams;
    };

    struct SkinGpuVertex {
        DirectX::XMFLOAT3 pos;
        DirectX::XMFLOAT3 normal;
        DirectX::XMFLOAT2 uv;
        uint16_t joints[4];
        float weights[4];
    };

    struct SkinSubmeshRuntime {
        uint32_t indexStart = 0;
        uint32_t indexCount = 0;
        uint32_t nodeIndex = 0;
        uint32_t legacyFlags = 0;
        uint32_t alphaMode = 0;
        DirectX::XMFLOAT4X4 nodeTransform = {
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1
        };
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> diffuseSRV;
    };

    struct SkinPackageRuntime {
        std::string modelId;
        uint32_t boneCount = 0;
        Microsoft::WRL::ComPtr<ID3D11Buffer> vb;
        Microsoft::WRL::ComPtr<ID3D11Buffer> ib;
        std::vector<SkinSubmeshRuntime> submeshes;
    };

    struct ShowcaseRenderable {
        std::string debugName;
        CharacterVisualInstance visual;
        std::vector<SkinPackageRuntime> gpu;
        bool visible = false;
        bool gpuDirty = true;
        bool animate = false;
        bool skipCharacterNodeFilter = false;
        bool faceCamera = false;
        float yawOffsetDeg = 0.0f;
        float scale = 1.0f;
        DirectX::XMFLOAT3 localOffset = { 0.0f, 0.0f, 0.0f };
    };

    struct SkinPerFrameCB {
        DirectX::XMFLOAT4X4 world;
        DirectX::XMFLOAT4X4 viewProj;
        DirectX::XMFLOAT4 lightDirIntensity;
        DirectX::XMFLOAT4 lightColorFogMin;
        DirectX::XMFLOAT4 fogColorFogMax;
        DirectX::XMFLOAT4 cameraPosFogEnabled;
        DirectX::XMFLOAT4 renderParams;
    };

    struct SkinBonesCB {
        std::array<DirectX::XMFLOAT4X4, MAX_BONES> bones;
    };

    bool EnsureMapPipeline();
    bool BuildMapGpuResources(const ScenePackageData& package, std::string* outError = nullptr);
    void ReleaseMapResources();
    bool EnsureSkinPipeline();
    bool EnsureShowcaseGpuResources(ShowcaseRenderable& renderable, std::string* outError = nullptr);
    void ReleaseCreationPreviewResources();
    bool BuildShowcaseWorldMatrix(const ShowcaseRenderable& renderable, bool applyCreationOrientation, DirectX::XMFLOAT4X4& outWorld) const;
    void BuildBindPoseSkinMatrices(const RS3ModelPackage& package, std::vector<DirectX::XMFLOAT4X4>& outMatrices) const;
    void ResetCreationCameraRig();
    void UpdateCreationCameraFromRig();
    DirectX::XMFLOAT3 GetCreationCameraFocus() const;

private:
    ID3D11Device* m_pd3dDevice = nullptr;
    std::unique_ptr<RStateManager> m_stateManager;
    std::unique_ptr<TextureManager> m_textureManager;
    std::unique_ptr<CharacterAssembler> m_characterAssembler;

    ShowcaseRenderable m_showcaseCharacter;
    ShowcaseRenderable m_showcasePlatform;
    bool m_creationShowroomMode = false;
    DirectX::XMFLOAT3 m_creationShowroomAnchor = { 0.0f, 0.0f, 0.0f };
    int m_creationSex = 0;
    int m_creationFace = 0;
    int m_creationPreset = 0;
    int m_creationHair = 0;

    bool m_creationCameraRigReady = false;
    bool m_creationCameraAutoOrbit = true;
    float m_creationCameraYaw = 0.0f;
    float m_creationCameraPitch = 0.16f;
    float m_creationCameraDistance = 360.0f;
    float m_creationCameraFocusHeight = 90.0f;
    float m_creationCameraYawTarget = 0.0f;
    float m_creationCameraPitchTarget = 0.16f;
    float m_creationCameraDistanceTarget = 360.0f;
    float m_creationCameraFocusHeightTarget = 90.0f;
    float m_creationCharacterYaw = 0.0f;

    DirectX::XMFLOAT3 m_cameraPos = { 0.0f, -800.0f, 220.0f };
    DirectX::XMFLOAT3 m_cameraDir = { 0.0f, 1.0f, -0.2f };

    bool m_hasSpawnPos = false;
    DirectX::XMFLOAT3 m_spawnPos = { 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 m_spawnDir = { 0.0f, 1.0f, 0.0f };

    bool m_hasMapGeometry = false;
    bool m_fogEnabled = false;
    float m_fogMin = 1000.0f;
    float m_fogMax = 7000.0f;
    DirectX::XMFLOAT3 m_fogColor = { 1.0f, 1.0f, 1.0f };

    DirectX::XMFLOAT3 m_sceneLightDir = { 0.0f, -1.0f, -0.3f };
    DirectX::XMFLOAT3 m_sceneLightColor = { 1.0f, 1.0f, 1.0f };
    float m_sceneLightIntensity = 1.0f;

    std::vector<MapSectionRuntime> m_mapSections;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_mapVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_mapPS;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_mapInputLayout;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_mapSampler;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_mapVB;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_mapIB;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_mapPerFrameCB;

    Microsoft::WRL::ComPtr<ID3D11BlendState> m_bsOpaque;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_bsAlphaBlend;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_bsAdditive;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_dsDepthWrite;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_dsDepthRead;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_skinVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_skinPS;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_skinInputLayout;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_skinSampler;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_skinPerFrameCB;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_skinBonesCB;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_skinBsOpaque;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_skinBsAlphaBlend;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_skinBsAdditive;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_skinDsDepthWrite;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_skinDsDepthRead;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_skinDsNoDepth;

    bool m_showcaseViewportEnabled = false;
    D3D11_VIEWPORT m_showcaseViewport = { 0.0f, 0.0f, 1280.0f, 720.0f, 0.0f, 1.0f };
};

} // namespace RealSpace3
