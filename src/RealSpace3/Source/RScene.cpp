#include "../Include/RScene.h"

#include "AppLogger.h"

#include <DirectXMath.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

namespace RealSpace3 {
namespace {

constexpr const char* kCharSelectSceneId = "char_creation_select";
constexpr const char* kShowcasePlatformModelId = "props/car_display_platform";
constexpr float kDefaultAlphaRef = 0.5f;
constexpr float kCreationCameraPitchMin = -0.75f;
constexpr float kCreationCameraPitchMax = 0.30f;
constexpr float kCreationCameraDistanceMin = 160.0f;
constexpr float kCreationCameraDistanceMax = 980.0f;
constexpr float kCreationCameraLerpSpeed = 10.0f;
constexpr float kCreationCameraAutoOrbitSpeed = 0.18f;
constexpr float kCreationShowroomPitch = 0.17f;
constexpr float kCreationShowroomDistance = 340.0f;
constexpr float kCreationShowroomFocusHeight = 92.0f;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;

void SetError(std::string* outError, const std::string& message) {
    if (outError) {
        *outError = message;
    }
}

float ClampFloat(float v, float lo, float hi) {
    return std::max(lo, std::min(v, hi));
}

float WrapAngle(float radians) {
    while (radians > kPi) radians -= kTwoPi;
    while (radians < -kPi) radians += kTwoPi;
    return radians;
}

float LerpAngle(float from, float to, float t) {
    const float delta = WrapAngle(to - from);
    return WrapAngle(from + delta * t);
}

DirectX::XMFLOAT4X4 Identity4x4() {
    return DirectX::XMFLOAT4X4(
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f);
}

int ClassifyPass(uint32_t legacyFlags, uint32_t alphaMode) {
    if ((legacyFlags & RM_FLAG_HIDE) != 0) {
        return -1;
    }
    if ((legacyFlags & RM_FLAG_ADDITIVE) != 0) {
        return 3;
    }
    if ((legacyFlags & RM_FLAG_USEOPACITY) != 0 || alphaMode == 2) {
        return 2;
    }
    if ((legacyFlags & RM_FLAG_USEALPHATEST) != 0 || alphaMode == 1) {
        return 1;
    }
    return 0;
}

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool ShouldSkipCharacterPreviewNode(const std::string& nodeName) {
    if (nodeName.empty()) {
        return false;
    }

    const std::string lower = ToLowerAscii(nodeName);
    if (lower.rfind("eq_w", 0) == 0) {
        // Character base ELU carries all weapon placeholders; keep them hidden in preview.
        return true;
    }
    if (lower.rfind("bip01", 0) == 0) {
        // Temporary OGZ parity fallback for char-select:
        // hide base bip geometry until cut-parts masking is ported.
        return true;
    }
    if (lower == "bip01 footsteps") {
        return true;
    }
    return false;
}

std::string ReplaceTextureFilename(const std::string& sourcePath, const char* replacementFilename) {
    if (!replacementFilename || !*replacementFilename) {
        return sourcePath;
    }
    const size_t slash = sourcePath.find_last_of("/\\");
    if (slash == std::string::npos) {
        return std::string(replacementFilename);
    }
    return sourcePath.substr(0, slash + 1) + replacementFilename;
}

void ApplyCreationTextureOverrides(CharacterVisualInstance& visual, int sex, int face, int hair) {
    if (visual.packages.empty()) {
        return;
    }

    auto& basePackage = visual.packages.front();
    if (basePackage.materials.empty()) {
        return;
    }

    static const std::array<const char*, 4> kMaleFaceTextures = {
        "gz_hum_face0001.bmp.dds",
        "gz_hum_face0002.bmp.dds",
        "gz_hum_face0003.bmp.dds",
        "gz_hum_face0004.bmp.dds"
    };

    static const std::array<const char*, 5> kMaleHairTextures = {
        "gz_hum_hair001.tga.dds",
        "gz_hum_hair002.tga.dds",
        "gz_hum_hair003.tga.dds",
        "gz_hum_hair004.tga.dds",
        "gz_hum_hair008.tga.dds"
    };

    static const std::array<const char*, 4> kFemaleFaceTextures = {
        "gz_huw_face001.bmp.dds",
        "gz_huw_face002.bmp.dds",
        "gz_huw_face003.bmp.dds",
        "gz_huw_face004.bmp.dds"
    };

    static const std::array<const char*, 5> kFemaleHairTextures = {
        "gz_huw_hair001.tga.dds",
        "gz_huw_hair002.tga.dds",
        "gz_huw_hair003.tga.dds",
        "gz_huw_hair005.tga.dds",
        "gz_huw_hair006.tga.dds"
    };

    const bool female = (sex == 1);
    const int safeFace = std::max(0, std::min(face, static_cast<int>(kMaleFaceTextures.size()) - 1));
    const int safeHair = std::max(0, std::min(hair, static_cast<int>(kMaleHairTextures.size()) - 1));
    const char* targetFaceTexture = female ? kFemaleFaceTextures[safeFace] : kMaleFaceTextures[safeFace];
    const char* targetHairTexture = female ? kFemaleHairTextures[safeHair] : kMaleHairTextures[safeHair];
    const std::string faceNeedle = female ? "gz_huw_face" : "gz_hum_face";
    const std::string hairNeedle = female ? "gz_huw_hair" : "gz_hum_hair";

    size_t replacedFace = 0;
    size_t replacedHair = 0;

    for (auto& material : basePackage.materials) {
        if (material.baseColorTexture.empty()) {
            continue;
        }

        const std::string lowered = ToLowerAscii(material.baseColorTexture);
        if (lowered.find(faceNeedle) != std::string::npos) {
            const std::string rewritten = ReplaceTextureFilename(material.baseColorTexture, targetFaceTexture);
            if (rewritten != material.baseColorTexture) {
                material.baseColorTexture = rewritten;
                ++replacedFace;
            }
            continue;
        }

        if (lowered.find(hairNeedle) != std::string::npos) {
            const std::string rewritten = ReplaceTextureFilename(material.baseColorTexture, targetHairTexture);
            if (rewritten != material.baseColorTexture) {
                material.baseColorTexture = rewritten;
                ++replacedHair;
            }
        }
    }

    AppLogger::Log("[RS3] Creation texture override: model='" + basePackage.modelId +
        "' sex=" + std::to_string(sex) +
        " face=" + std::to_string(safeFace) +
        " hair=" + std::to_string(safeHair) +
        " replaced(face=" + std::to_string(replacedFace) +
        ",hair=" + std::to_string(replacedHair) + ").");
}

bool CompileShader(const char* source,
    const char* entry,
    const char* target,
    Microsoft::WRL::ComPtr<ID3DBlob>& outBlob,
    std::string* outError = nullptr) {
    if (!source || !entry || !target) {
        SetError(outError, "CompileShader received invalid arguments.");
        return false;
    }

    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
    const HRESULT hr = D3DCompile(
        source,
        std::strlen(source),
        nullptr,
        nullptr,
        nullptr,
        entry,
        target,
        flags,
        0,
        &outBlob,
        &errorBlob);

    if (FAILED(hr)) {
        std::string message = "D3DCompile failed for entry='" + std::string(entry) + "' target='" + std::string(target) + "'.";
        if (errorBlob && errorBlob->GetBufferPointer() && errorBlob->GetBufferSize() > 0) {
            message += " compiler=";
            message.append(static_cast<const char*>(errorBlob->GetBufferPointer()), errorBlob->GetBufferSize());
        }
        SetError(outError, message);
        return false;
    }

    return true;
}

const char* kMapShaderSource = R"HLSL(
cbuffer PerFrame : register(b0) {
    row_major float4x4 gViewProj;
    float4 gLightDirIntensity;
    float4 gLightColorFogMin;
    float4 gFogColorFogMax;
    float4 gCameraPosFogEnabled;
    float4 gRenderParams;
};

Texture2D gDiffuse : register(t0);
SamplerState gSampler : register(s0);

struct VSIn {
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct VSOut {
    float4 pos : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float3 normalW : TEXCOORD1;
    float2 uv : TEXCOORD2;
};

VSOut VSMain(VSIn input) {
    VSOut o;
    float4 world = float4(input.pos, 1.0);
    o.pos = mul(world, gViewProj);
    o.worldPos = input.pos;
    o.normalW = normalize(input.normal);
    o.uv = input.uv;
    return o;
}

float4 PSMain(VSOut input) : SV_Target {
    float4 albedo = gDiffuse.Sample(gSampler, input.uv);

    int alphaMode = (int)gRenderParams.x;
    float alphaRef = gRenderParams.y;
    if (alphaMode == 1) {
        clip(albedo.a - alphaRef);
    }

    float3 N = normalize(input.normalW);
    float3 L = normalize(-gLightDirIntensity.xyz);
    float ndotl = saturate(dot(N, L));
    float diffuse = (0.25 + ndotl * gLightDirIntensity.w);
    float3 lit = albedo.rgb * diffuse * gLightColorFogMin.rgb;

    float fogEnabled = gCameraPosFogEnabled.w;
    float fogMin = gLightColorFogMin.w;
    float fogMax = gFogColorFogMax.w;
    float3 camPos = gCameraPosFogEnabled.xyz;
    float dist = distance(input.worldPos, camPos);

    float fogFactor = 1.0;
    if (fogEnabled > 0.5) {
        float span = max(fogMax - fogMin, 0.0001);
        fogFactor = saturate((fogMax - dist) / span);
    }

    float3 color = lerp(gFogColorFogMax.rgb, lit, fogFactor);

    if (alphaMode == 3) {
        return float4(color * albedo.a, albedo.a);
    }

    return float4(color, albedo.a);
}
)HLSL";

const char* kSkinShaderSource = R"HLSL(
cbuffer PerFrame : register(b0) {
    row_major float4x4 gWorld;
    row_major float4x4 gViewProj;
    float4 gLightDirIntensity;
    float4 gLightColorFogMin;
    float4 gFogColorFogMax;
    float4 gCameraPosFogEnabled;
    float4 gRenderParams;
};

cbuffer Bones : register(b1) {
    row_major float4x4 gBones[128];
};

Texture2D gDiffuse : register(t0);
SamplerState gSampler : register(s0);

struct VSIn {
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    uint4 joints : BLENDINDICES0;
    float4 weights : BLENDWEIGHT0;
};

struct VSOut {
    float4 pos : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float3 normalW : TEXCOORD1;
    float2 uv : TEXCOORD2;
};

VSOut VSMain(VSIn input) {
    float4 skinnedPos = float4(0.0, 0.0, 0.0, 0.0);
    float3 skinnedNrm = float3(0.0, 0.0, 0.0);
    float weightSum = 0.0;

    [unroll]
    for (int i = 0; i < 4; ++i) {
        float w = max(input.weights[i], 0.0);
        if (w <= 0.0) continue;

        uint idx = min(input.joints[i], 127u);
        row_major float4x4 B = gBones[idx];
        skinnedPos += mul(float4(input.pos, 1.0), B) * w;
        skinnedNrm += mul(float4(input.normal, 0.0), B).xyz * w;
        weightSum += w;
    }

    if (weightSum > 1e-6) {
        skinnedPos /= weightSum;
        skinnedNrm /= weightSum;
    } else {
        // Some legacy vertices are exported with zero influences.
        skinnedPos = float4(input.pos, 1.0);
        skinnedNrm = input.normal;
    }

    float4 worldPos = mul(skinnedPos, gWorld);

    VSOut o;
    o.pos = mul(worldPos, gViewProj);
    o.worldPos = worldPos.xyz;
    o.normalW = normalize(mul(float4(skinnedNrm, 0.0), gWorld).xyz);
    o.uv = input.uv;
    return o;
}

float4 PSMain(VSOut input) : SV_Target {
    float4 albedo = gDiffuse.Sample(gSampler, input.uv);

    int alphaMode = (int)gRenderParams.x;
    float alphaRef = gRenderParams.y;
    if (alphaMode == 1) {
        clip(albedo.a - alphaRef);
    }

    float3 N = normalize(input.normalW);
    float3 L = normalize(-gLightDirIntensity.xyz);
    float ndotl = saturate(dot(N, L));
    float diffuse = (0.25 + ndotl * gLightDirIntensity.w);
    float3 lit = albedo.rgb * diffuse * gLightColorFogMin.rgb;

    float fogEnabled = gCameraPosFogEnabled.w;
    float fogMin = gLightColorFogMin.w;
    float fogMax = gFogColorFogMax.w;
    float3 camPos = gCameraPosFogEnabled.xyz;
    float dist = distance(input.worldPos, camPos);

    float fogFactor = 1.0;
    if (fogEnabled > 0.5) {
        float span = max(fogMax - fogMin, 0.0001);
        fogFactor = saturate((fogMax - dist) / span);
    }

    float3 color = lerp(gFogColorFogMax.rgb, lit, fogFactor);

    if (alphaMode == 3) {
        return float4(color * albedo.a, albedo.a);
    }

    return float4(color, albedo.a);
}
)HLSL";

} // namespace

RScene::RScene(ID3D11Device* device, ID3D11DeviceContext* context)
    : m_pd3dDevice(device) {
    m_stateManager = std::make_unique<RStateManager>(device, context);
    m_textureManager = std::make_unique<TextureManager>(device);
    m_characterAssembler = std::make_unique<CharacterAssembler>();
    m_showcaseCharacter.debugName = "character";
    m_showcaseCharacter.animate = true;
    m_showcaseCharacter.skipCharacterNodeFilter = true;
    m_showcaseCharacter.faceCamera = true;
    m_showcaseCharacter.visible = false;

    m_showcasePlatform.debugName = "platform";
    m_showcasePlatform.animate = false;
    m_showcasePlatform.skipCharacterNodeFilter = false;
    m_showcasePlatform.faceCamera = false;
    m_showcasePlatform.visible = false;
    m_showcasePlatform.scale = 1.0f;
    m_showcasePlatform.localOffset = { 0.0f, 0.0f, -6.0f };
}

RScene::~RScene() {
    ReleaseCreationPreviewResources();
    ReleaseMapResources();
}

void RScene::LoadCharSelect() {
    ReleaseCreationPreviewResources();
    m_showcaseCharacter.visual = CharacterVisualInstance{};
    m_showcaseCharacter.visible = false;
    m_showcaseCharacter.gpuDirty = true;
    m_showcasePlatform.visual = CharacterVisualInstance{};
    m_showcasePlatform.visible = false;
    m_showcasePlatform.gpuDirty = true;
    m_creationShowroomMode = true;
    m_creationShowroomAnchor = { 0.0f, 0.0f, 0.0f };
    m_creationCharacterYaw = 0.0f;
    m_creationCameraRigReady = false;
    m_creationCameraAutoOrbit = true;

    if (!LoadCharSelectPackage(kCharSelectSceneId)) {
        AppLogger::Log("[RS3] LoadCharSelect -> package load failed, falling back to LoadLobbyBasic.");
        LoadLobbyBasic();
        m_creationShowroomMode = true;
        m_creationShowroomAnchor = { 0.0f, 0.0f, 0.0f };
        ResetCreationCameraRig();
        return;
    }

    CharacterVisualRequest platformReq;
    platformReq.baseModelId = kShowcasePlatformModelId;

    CharacterVisualInstance platformBuilt;
    std::string platformError;
    if (!m_characterAssembler->BuildCharacterVisual(platformReq, platformBuilt, &platformError)) {
        AppLogger::Log("[RS3] Showcase platform unavailable: " + platformError);
        m_showcasePlatform.visual = CharacterVisualInstance{};
        m_showcasePlatform.visible = false;
        m_showcasePlatform.gpuDirty = true;
    } else {
        m_showcasePlatform.visual = std::move(platformBuilt);
        m_showcasePlatform.visible = true;
        m_showcasePlatform.gpuDirty = true;
        std::string gpuError;
        if (!EnsureShowcaseGpuResources(m_showcasePlatform, &gpuError)) {
            AppLogger::Log("[RS3] Showcase platform GPU prepare failed: " + gpuError);
            m_showcasePlatform.visible = false;
        } else {
            AppLogger::Log("[RS3] Showcase platform ready: model='" + std::string(kShowcasePlatformModelId) + "'.");
        }
    }

    AppLogger::Log("[RS3] LoadCharSelect -> scene package active: char_creation_select (showroom mode enabled).");
}

bool RScene::LoadCharSelectPackage(const std::string& sceneId) {
    ScenePackageData package;
    std::string error;

    if (!ScenePackageLoader::Load(sceneId, package, &error)) {
        AppLogger::Log("[RS3] LoadCharSelectPackage failed: " + error);
        return false;
    }

    if (!EnsureMapPipeline()) {
        AppLogger::Log("[RS3] LoadCharSelectPackage failed: map pipeline initialization failed.");
        return false;
    }

    if (!BuildMapGpuResources(package, &error)) {
        AppLogger::Log("[RS3] LoadCharSelectPackage failed: " + error);
        return false;
    }

    std::ostringstream oss;
    oss << "[RS3] LoadCharSelectPackage success: sceneId='" << sceneId
        << "' verts=" << package.vertices.size()
        << " indices=" << package.indices.size()
        << " sections=" << package.sections.size()
        << " materials=" << package.materials.size();
    AppLogger::Log(oss.str());
    return true;
}

void RScene::LoadLobbyBasic() {
    ReleaseMapResources();
    ReleaseCreationPreviewResources();

    m_showcaseCharacter.visual = CharacterVisualInstance{};
    m_showcaseCharacter.visible = false;
    m_showcaseCharacter.gpuDirty = true;
    m_showcasePlatform.visual = CharacterVisualInstance{};
    m_showcasePlatform.visible = false;
    m_showcasePlatform.gpuDirty = true;
    m_creationShowroomMode = false;
    m_creationShowroomAnchor = { 0.0f, 0.0f, 0.0f };
    m_creationCharacterYaw = 0.0f;
    m_creationCameraRigReady = false;
    m_creationCameraAutoOrbit = true;

    m_cameraPos = { 0.0f, -800.0f, 220.0f };
    m_cameraDir = { 0.0f, 1.0f, -0.2f };

    m_hasSpawnPos = false;
    m_spawnPos = { 0.0f, 0.0f, 0.0f };
    m_spawnDir = { 0.0f, 1.0f, 0.0f };

    m_fogEnabled = false;
    m_fogMin = 1000.0f;
    m_fogMax = 7000.0f;
    m_fogColor = { 1.0f, 1.0f, 1.0f };

    m_sceneLightDir = { 0.0f, -1.0f, -0.3f };
    m_sceneLightColor = { 1.0f, 1.0f, 1.0f };
    m_sceneLightIntensity = 1.0f;

    AppLogger::Log("[RS3] LoadLobbyBasic -> basic offline fallback scene active.");
}

bool RScene::EnsureMapPipeline() {
    if (m_mapVS && m_mapPS && m_mapInputLayout && m_mapSampler && m_mapPerFrameCB) {
        return true;
    }

    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;
    std::string compileError;

    if (!CompileShader(kMapShaderSource, "VSMain", "vs_5_0", vsBlob, &compileError)) {
        AppLogger::Log("[RS3] EnsureMapPipeline VS compile failed: " + compileError);
        return false;
    }
    if (!CompileShader(kMapShaderSource, "PSMain", "ps_5_0", psBlob, &compileError)) {
        AppLogger::Log("[RS3] EnsureMapPipeline PS compile failed: " + compileError);
        return false;
    }

    if (FAILED(m_pd3dDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_mapVS))) {
        AppLogger::Log("[RS3] EnsureMapPipeline failed: CreateVertexShader.");
        return false;
    }
    if (FAILED(m_pd3dDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_mapPS))) {
        AppLogger::Log("[RS3] EnsureMapPipeline failed: CreatePixelShader.");
        return false;
    }

    const D3D11_INPUT_ELEMENT_DESC ied[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(MapGpuVertex, pos), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(MapGpuVertex, normal), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, offsetof(MapGpuVertex, uv), D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    if (FAILED(m_pd3dDevice->CreateInputLayout(ied, static_cast<UINT>(std::size(ied)), vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_mapInputLayout))) {
        AppLogger::Log("[RS3] EnsureMapPipeline failed: CreateInputLayout.");
        return false;
    }

    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(MapPerFrameCB);
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(m_pd3dDevice->CreateBuffer(&cbDesc, nullptr, &m_mapPerFrameCB))) {
        AppLogger::Log("[RS3] EnsureMapPipeline failed: CreateBuffer(map CB).");
        return false;
    }

    D3D11_SAMPLER_DESC samp = {};
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    samp.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    samp.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    samp.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(m_pd3dDevice->CreateSamplerState(&samp, &m_mapSampler))) {
        AppLogger::Log("[RS3] EnsureMapPipeline failed: CreateSamplerState.");
        return false;
    }

    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    blendDesc.RenderTarget[0].BlendEnable = FALSE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    if (FAILED(m_pd3dDevice->CreateBlendState(&blendDesc, &m_bsOpaque))) {
        AppLogger::Log("[RS3] EnsureMapPipeline failed: CreateBlendState(opaque).");
        return false;
    }

    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    if (FAILED(m_pd3dDevice->CreateBlendState(&blendDesc, &m_bsAlphaBlend))) {
        AppLogger::Log("[RS3] EnsureMapPipeline failed: CreateBlendState(alpha).");
        return false;
    }

    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    if (FAILED(m_pd3dDevice->CreateBlendState(&blendDesc, &m_bsAdditive))) {
        AppLogger::Log("[RS3] EnsureMapPipeline failed: CreateBlendState(additive).");
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC dsDesc = {};
    dsDesc.DepthEnable = TRUE;
    dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(m_pd3dDevice->CreateDepthStencilState(&dsDesc, &m_dsDepthWrite))) {
        AppLogger::Log("[RS3] EnsureMapPipeline failed: CreateDepthStencilState(depth-write).");
        return false;
    }

    dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    if (FAILED(m_pd3dDevice->CreateDepthStencilState(&dsDesc, &m_dsDepthRead))) {
        AppLogger::Log("[RS3] EnsureMapPipeline failed: CreateDepthStencilState(depth-read).");
        return false;
    }

    static bool logged = false;
    if (!logged) {
        AppLogger::Log("[RS3] EnsureMapPipeline -> ready.");
        logged = true;
    }

    return true;
}

bool RScene::BuildMapGpuResources(const ScenePackageData& package, std::string* outError) {
    ReleaseMapResources();

    if (package.vertices.empty() || package.indices.empty() || package.sections.empty()) {
        SetError(outError, "Scene package has no renderable map geometry.");
        return false;
    }

    std::vector<MapGpuVertex> gpuVertices;
    gpuVertices.reserve(package.vertices.size());
    for (const auto& v : package.vertices) {
        MapGpuVertex gv;
        gv.pos = v.pos;
        gv.normal = v.normal;
        gv.uv = v.uv;
        gpuVertices.push_back(gv);
    }

    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.ByteWidth = static_cast<UINT>(sizeof(MapGpuVertex) * gpuVertices.size());
    vbDesc.Usage = D3D11_USAGE_DEFAULT;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA vbData = {};
    vbData.pSysMem = gpuVertices.data();

    if (FAILED(m_pd3dDevice->CreateBuffer(&vbDesc, &vbData, &m_mapVB))) {
        SetError(outError, "Failed to create map vertex buffer.");
        return false;
    }

    D3D11_BUFFER_DESC ibDesc = {};
    ibDesc.ByteWidth = static_cast<UINT>(sizeof(uint32_t) * package.indices.size());
    ibDesc.Usage = D3D11_USAGE_DEFAULT;
    ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;

    D3D11_SUBRESOURCE_DATA ibData = {};
    ibData.pSysMem = package.indices.data();

    if (FAILED(m_pd3dDevice->CreateBuffer(&ibDesc, &ibData, &m_mapIB))) {
        SetError(outError, "Failed to create map index buffer.");
        return false;
    }

    m_mapSections.clear();
    m_mapSections.reserve(package.sections.size());

    m_textureManager->SetBaseDirectory(package.baseDir);

    for (const auto& section : package.sections) {
        if (section.indexCount == 0) continue;

        MapSectionRuntime runtime;
        runtime.indexStart = section.indexStart;
        runtime.indexCount = section.indexCount;

        if (section.materialIndex < package.materials.size()) {
            const auto& mat = package.materials[section.materialIndex];
            runtime.materialFlags = mat.flags;
            if (!mat.diffuseMap.empty()) {
                runtime.diffuseSRV = m_textureManager->GetTexture(mat.diffuseMap);
            }
        }

        if (!runtime.diffuseSRV) {
            runtime.diffuseSRV = m_textureManager->GetWhiteTexture();
        }

        m_mapSections.push_back(std::move(runtime));
    }

    if (m_mapSections.empty()) {
        SetError(outError, "Map sections are empty after runtime build.");
        return false;
    }

    if (package.hasCamera02) {
        m_cameraPos = package.cameraPos02;
        m_cameraDir = package.cameraDir02;
    } else if (package.hasCamera01) {
        m_cameraPos = package.cameraPos01;
        m_cameraDir = package.cameraDir01;
    }

    m_hasSpawnPos = package.hasSpawn;
    m_spawnPos = package.spawnPos;
    m_spawnDir = package.spawnDir;
    if (m_creationShowroomMode) {
        m_creationShowroomAnchor = m_hasSpawnPos ? m_spawnPos : DirectX::XMFLOAT3{ 0.0f, 0.0f, 0.0f };
    }

    m_fogEnabled = package.fogEnabled;
    m_fogMin = package.fogMin;
    m_fogMax = package.fogMax;
    m_fogColor = package.fogColor;

    m_sceneLightDir = { 0.0f, -1.0f, -0.3f };
    m_sceneLightColor = { 1.0f, 1.0f, 1.0f };
    m_sceneLightIntensity = 1.0f;

    if (!package.lights.empty()) {
        const auto& l = package.lights.front();
        DirectX::XMVECTOR lightPos = DirectX::XMVectorSet(l.position.x, l.position.y, l.position.z, 0.0f);
        if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(lightPos)) > 0.00001f) {
            lightPos = DirectX::XMVector3Normalize(DirectX::XMVectorNegate(lightPos));
            DirectX::XMStoreFloat3(&m_sceneLightDir, lightPos);
        }
        m_sceneLightColor = l.color;
        m_sceneLightIntensity = std::max(0.1f, l.intensity);
    }

    m_hasMapGeometry = true;
    ResetCreationCameraRig();
    return true;
}

void RScene::ReleaseMapResources() {
    m_mapVB.Reset();
    m_mapIB.Reset();
    m_mapSections.clear();
    m_hasMapGeometry = false;
}

bool RScene::EnsureSkinPipeline() {
    if (m_skinVS && m_skinPS && m_skinInputLayout && m_skinSampler && m_skinPerFrameCB && m_skinBonesCB &&
        m_skinBsOpaque && m_skinBsAlphaBlend && m_skinBsAdditive && m_skinDsDepthWrite && m_skinDsDepthRead && m_skinDsNoDepth) {
        return true;
    }

    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;
    std::string compileError;

    if (!CompileShader(kSkinShaderSource, "VSMain", "vs_5_0", vsBlob, &compileError)) {
        AppLogger::Log("[RS3] EnsureSkinPipeline VS compile failed: " + compileError);
        return false;
    }
    if (!CompileShader(kSkinShaderSource, "PSMain", "ps_5_0", psBlob, &compileError)) {
        AppLogger::Log("[RS3] EnsureSkinPipeline PS compile failed: " + compileError);
        return false;
    }

    if (FAILED(m_pd3dDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_skinVS))) {
        AppLogger::Log("[RS3] EnsureSkinPipeline failed: CreateVertexShader.");
        return false;
    }
    if (FAILED(m_pd3dDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_skinPS))) {
        AppLogger::Log("[RS3] EnsureSkinPipeline failed: CreatePixelShader.");
        return false;
    }

    const D3D11_INPUT_ELEMENT_DESC ied[] = {
        { "POSITION",      0, DXGI_FORMAT_R32G32B32_FLOAT,       0, offsetof(SkinGpuVertex, pos), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",        0, DXGI_FORMAT_R32G32B32_FLOAT,       0, offsetof(SkinGpuVertex, normal), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD",      0, DXGI_FORMAT_R32G32_FLOAT,          0, offsetof(SkinGpuVertex, uv), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "BLENDINDICES",  0, DXGI_FORMAT_R16G16B16A16_UINT,     0, offsetof(SkinGpuVertex, joints), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "BLENDWEIGHT",   0, DXGI_FORMAT_R32G32B32A32_FLOAT,    0, offsetof(SkinGpuVertex, weights), D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    if (FAILED(m_pd3dDevice->CreateInputLayout(ied, static_cast<UINT>(std::size(ied)), vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_skinInputLayout))) {
        AppLogger::Log("[RS3] EnsureSkinPipeline failed: CreateInputLayout.");
        return false;
    }

    D3D11_BUFFER_DESC cbFrameDesc = {};
    cbFrameDesc.ByteWidth = sizeof(SkinPerFrameCB);
    cbFrameDesc.Usage = D3D11_USAGE_DEFAULT;
    cbFrameDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(m_pd3dDevice->CreateBuffer(&cbFrameDesc, nullptr, &m_skinPerFrameCB))) {
        AppLogger::Log("[RS3] EnsureSkinPipeline failed: CreateBuffer(skin frame CB).");
        return false;
    }

    D3D11_BUFFER_DESC cbBonesDesc = {};
    cbBonesDesc.ByteWidth = sizeof(SkinBonesCB);
    cbBonesDesc.Usage = D3D11_USAGE_DEFAULT;
    cbBonesDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(m_pd3dDevice->CreateBuffer(&cbBonesDesc, nullptr, &m_skinBonesCB))) {
        AppLogger::Log("[RS3] EnsureSkinPipeline failed: CreateBuffer(skin bones CB).");
        return false;
    }

    D3D11_SAMPLER_DESC samp = {};
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    samp.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    samp.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    samp.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(m_pd3dDevice->CreateSamplerState(&samp, &m_skinSampler))) {
        AppLogger::Log("[RS3] EnsureSkinPipeline failed: CreateSamplerState.");
        return false;
    }

    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    blendDesc.RenderTarget[0].BlendEnable = FALSE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    if (FAILED(m_pd3dDevice->CreateBlendState(&blendDesc, &m_skinBsOpaque))) {
        AppLogger::Log("[RS3] EnsureSkinPipeline failed: CreateBlendState(opaque).");
        return false;
    }

    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    if (FAILED(m_pd3dDevice->CreateBlendState(&blendDesc, &m_skinBsAlphaBlend))) {
        AppLogger::Log("[RS3] EnsureSkinPipeline failed: CreateBlendState(alpha).");
        return false;
    }

    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    if (FAILED(m_pd3dDevice->CreateBlendState(&blendDesc, &m_skinBsAdditive))) {
        AppLogger::Log("[RS3] EnsureSkinPipeline failed: CreateBlendState(additive).");
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC dsDesc = {};
    dsDesc.DepthEnable = TRUE;
    dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(m_pd3dDevice->CreateDepthStencilState(&dsDesc, &m_skinDsDepthWrite))) {
        AppLogger::Log("[RS3] EnsureSkinPipeline failed: CreateDepthStencilState(depth-write).");
        return false;
    }

    dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    if (FAILED(m_pd3dDevice->CreateDepthStencilState(&dsDesc, &m_skinDsDepthRead))) {
        AppLogger::Log("[RS3] EnsureSkinPipeline failed: CreateDepthStencilState(depth-read).");
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC dsOverlay = {};
    dsOverlay.DepthEnable = FALSE;
    dsOverlay.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsOverlay.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (FAILED(m_pd3dDevice->CreateDepthStencilState(&dsOverlay, &m_skinDsNoDepth))) {
        AppLogger::Log("[RS3] EnsureSkinPipeline failed: CreateDepthStencilState(no-depth).");
        return false;
    }

    static bool logged = false;
    if (!logged) {
        AppLogger::Log("[RS3] EnsureSkinPipeline -> ready.");
        logged = true;
    }

    return true;
}

bool RScene::EnsureShowcaseGpuResources(ShowcaseRenderable& renderable, std::string* outError) {
    if (!renderable.gpuDirty) {
        return !renderable.gpu.empty();
    }

    renderable.gpu.clear();
    renderable.gpuDirty = true;

    if (!renderable.visual.valid || renderable.visual.packages.empty()) {
        SetError(outError, "Showcase '" + renderable.debugName + "' is not valid or has no packages.");
        return false;
    }

    renderable.gpu.reserve(renderable.visual.packages.size());

    for (const auto& package : renderable.visual.packages) {
        if (package.vertices.empty() || package.indices.empty() || package.submeshes.empty()) {
            continue;
        }

        SkinPackageRuntime runtime;
        runtime.modelId = package.modelId;
        runtime.boneCount = static_cast<uint32_t>(std::min<size_t>(package.bones.size(), MAX_BONES));

        std::vector<SkinGpuVertex> gpuVertices;
        gpuVertices.reserve(package.vertices.size());
        size_t zeroInfluenceCount = 0;
        for (const auto& v : package.vertices) {
            SkinGpuVertex sv;
            sv.pos = v.pos;
            sv.normal = v.normal;
            sv.uv = v.uv;
            sv.joints[0] = v.joints[0];
            sv.joints[1] = v.joints[1];
            sv.joints[2] = v.joints[2];
            sv.joints[3] = v.joints[3];
            sv.weights[0] = v.weights[0];
            sv.weights[1] = v.weights[1];
            sv.weights[2] = v.weights[2];
            sv.weights[3] = v.weights[3];
            const float ws = std::max(0.0f, sv.weights[0]) + std::max(0.0f, sv.weights[1]) +
                std::max(0.0f, sv.weights[2]) + std::max(0.0f, sv.weights[3]);
            if (ws <= 0.000001f) {
                ++zeroInfluenceCount;
            }
            gpuVertices.push_back(sv);
        }

        if (zeroInfluenceCount > 0) {
            AppLogger::Log("[RS3] Skin vertex audit: model='" + package.modelId + "' zeroInfluence=" +
                std::to_string(zeroInfluenceCount) + "/" + std::to_string(gpuVertices.size()));
        }

        D3D11_BUFFER_DESC vbDesc = {};
        vbDesc.ByteWidth = static_cast<UINT>(sizeof(SkinGpuVertex) * gpuVertices.size());
        vbDesc.Usage = D3D11_USAGE_DEFAULT;
        vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

        D3D11_SUBRESOURCE_DATA vbData = {};
        vbData.pSysMem = gpuVertices.data();

        if (FAILED(m_pd3dDevice->CreateBuffer(&vbDesc, &vbData, &runtime.vb))) {
            SetError(outError, "Failed to create preview skin vertex buffer for modelId='" + package.modelId + "'.");
            return false;
        }

        D3D11_BUFFER_DESC ibDesc = {};
        ibDesc.ByteWidth = static_cast<UINT>(sizeof(uint32_t) * package.indices.size());
        ibDesc.Usage = D3D11_USAGE_DEFAULT;
        ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;

        D3D11_SUBRESOURCE_DATA ibData = {};
        ibData.pSysMem = package.indices.data();

        if (FAILED(m_pd3dDevice->CreateBuffer(&ibDesc, &ibData, &runtime.ib))) {
            SetError(outError, "Failed to create preview skin index buffer for modelId='" + package.modelId + "'.");
            return false;
        }

        runtime.submeshes.reserve(package.submeshes.size());
        size_t nonIdentityNodeTransformCount = 0;

        const std::string baseDir = package.baseDir.generic_string();
        m_textureManager->SetBaseDirectory(baseDir);

        for (const auto& sub : package.submeshes) {
            if (sub.indexCount == 0) continue;

            SkinSubmeshRuntime s;
            s.indexStart = sub.indexStart;
            s.indexCount = sub.indexCount;
            s.nodeIndex = sub.nodeIndex;
            s.nodeTransform = sub.nodeTransform;
            const float* nt = reinterpret_cast<const float*>(&s.nodeTransform);
            const float identity[16] = {
                1,0,0,0,
                0,1,0,0,
                0,0,1,0,
                0,0,0,1
            };
            bool nodeIsIdentity = true;
            for (int k = 0; k < 16; ++k) {
                if (std::fabs(nt[k] - identity[k]) > 0.0001f) {
                    nodeIsIdentity = false;
                    break;
                }
            }
            if (!nodeIsIdentity) {
                ++nonIdentityNodeTransformCount;
            }

            if (sub.materialIndex < package.materials.size()) {
                const auto& material = package.materials[sub.materialIndex];
                s.legacyFlags = material.legacyFlags;
                s.alphaMode = material.alphaMode;

                if (!material.baseColorTexture.empty()) {
                    s.diffuseSRV = m_textureManager->GetTexture(material.baseColorTexture);
                }
            }

            if (!s.diffuseSRV) {
                s.diffuseSRV = m_textureManager->GetWhiteTexture();
            }

            runtime.submeshes.push_back(std::move(s));
        }

        if (!runtime.submeshes.empty()) {
            if (nonIdentityNodeTransformCount > 0) {
                AppLogger::Log("[RS3] Creation preview node transforms: model='" + package.modelId +
                    "' nonIdentitySubmeshes=" + std::to_string(nonIdentityNodeTransformCount) +
                    "/" + std::to_string(runtime.submeshes.size()));
            }
            renderable.gpu.push_back(std::move(runtime));
        }
    }

    if (renderable.gpu.empty()) {
        SetError(outError, "Showcase '" + renderable.debugName + "' GPU cache is empty.");
        return false;
    }

    renderable.gpuDirty = false;

    size_t totalSubmeshes = 0;
    for (const auto& p : renderable.gpu) {
        totalSubmeshes += p.submeshes.size();
    }

    std::ostringstream oss;
    oss << "[RS3] Showcase GPU cache ready: name='" << renderable.debugName << "' packages=" << renderable.gpu.size()
        << " submeshes=" << totalSubmeshes;
    AppLogger::Log(oss.str());

    return true;
}

void RScene::ReleaseCreationPreviewResources() {
    m_showcaseCharacter.gpu.clear();
    m_showcaseCharacter.gpuDirty = true;
    m_showcasePlatform.gpu.clear();
    m_showcasePlatform.gpuDirty = true;
}

DirectX::XMFLOAT3 RScene::GetCreationCameraFocus() const {
    DirectX::XMFLOAT3 focus = m_creationShowroomMode
        ? m_creationShowroomAnchor
        : (m_hasSpawnPos ? m_spawnPos : DirectX::XMFLOAT3{ 0.0f, 0.0f, 0.0f });
    focus.z += m_creationCameraFocusHeight;
    return focus;
}

void RScene::ResetCreationCameraRig() {
    if (m_creationShowroomMode) {
        m_creationCameraYaw = 0.0f;
        m_creationCameraPitch = ClampFloat(kCreationShowroomPitch, kCreationCameraPitchMin, kCreationCameraPitchMax);
        m_creationCameraDistance = ClampFloat(kCreationShowroomDistance, kCreationCameraDistanceMin, kCreationCameraDistanceMax);
        m_creationCameraFocusHeight = kCreationShowroomFocusHeight;

        m_creationCameraYawTarget = m_creationCameraYaw;
        m_creationCameraPitchTarget = m_creationCameraPitch;
        m_creationCameraDistanceTarget = m_creationCameraDistance;
        m_creationCameraFocusHeightTarget = m_creationCameraFocusHeight;
        m_creationCameraRigReady = true;
        UpdateCreationCameraFromRig();
        return;
    }

    const float defaultDistance = 360.0f;
    const float defaultPitch = 0.16f;
    const float defaultFocusHeight = 90.0f;

    const DirectX::XMFLOAT3 focusBase = m_hasSpawnPos ? m_spawnPos : DirectX::XMFLOAT3{ 0.0f, 0.0f, 0.0f };
    const DirectX::XMFLOAT3 focus = { focusBase.x, focusBase.y, focusBase.z + defaultFocusHeight };

    DirectX::XMVECTOR offset = DirectX::XMVectorSet(
        m_cameraPos.x - focus.x,
        m_cameraPos.y - focus.y,
        m_cameraPos.z - focus.z,
        0.0f);

    const float offsetLenSq = DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(offset));
    if (offsetLenSq < 1.0f) {
        offset = DirectX::XMVectorSet(0.0f, -defaultDistance, defaultDistance * std::sin(defaultPitch), 0.0f);
    }

    DirectX::XMFLOAT3 offF = {};
    DirectX::XMStoreFloat3(&offF, offset);
    const float horizontal = std::max(0.001f, std::sqrt(offF.x * offF.x + offF.y * offF.y));
    const float distance = std::max(0.001f, std::sqrt(offF.x * offF.x + offF.y * offF.y + offF.z * offF.z));
    const float yaw = std::atan2(offF.x, -offF.y);
    const float pitch = std::atan2(offF.z, horizontal);

    m_creationCameraYaw = WrapAngle(yaw);
    m_creationCameraPitch = ClampFloat(pitch, kCreationCameraPitchMin, kCreationCameraPitchMax);
    m_creationCameraDistance = ClampFloat(distance, kCreationCameraDistanceMin, kCreationCameraDistanceMax);
    m_creationCameraFocusHeight = defaultFocusHeight;

    m_creationCameraYawTarget = m_creationCameraYaw;
    m_creationCameraPitchTarget = m_creationCameraPitch;
    m_creationCameraDistanceTarget = m_creationCameraDistance;
    m_creationCameraFocusHeightTarget = m_creationCameraFocusHeight;
    m_creationCameraRigReady = true;

    UpdateCreationCameraFromRig();
}

void RScene::UpdateCreationCameraFromRig() {
    if (!m_creationCameraRigReady) {
        return;
    }

    const DirectX::XMFLOAT3 focus = GetCreationCameraFocus();
    const float cosPitch = std::cos(m_creationCameraPitch);
    const float sinPitch = std::sin(m_creationCameraPitch);
    const float sinYaw = std::sin(m_creationCameraYaw);
    const float cosYaw = std::cos(m_creationCameraYaw);

    const DirectX::XMFLOAT3 offset = {
        sinYaw * m_creationCameraDistance * cosPitch,
        -cosYaw * m_creationCameraDistance * cosPitch,
        sinPitch * m_creationCameraDistance
    };

    m_cameraPos = { focus.x + offset.x, focus.y + offset.y, focus.z + offset.z };

    DirectX::XMVECTOR dir = DirectX::XMVectorSet(
        focus.x - m_cameraPos.x,
        focus.y - m_cameraPos.y,
        focus.z - m_cameraPos.z,
        0.0f);
    if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(dir)) < 0.000001f) {
        dir = DirectX::XMVectorSet(0.0f, 1.0f, -0.2f, 0.0f);
    } else {
        dir = DirectX::XMVector3Normalize(dir);
    }

    DirectX::XMStoreFloat3(&m_cameraDir, dir);
}

bool RScene::AdjustCreationCamera(float yawDeltaDeg, float pitchDeltaDeg, float zoomDelta) {
    if (!m_showcaseCharacter.visual.valid && !m_creationCameraRigReady) {
        return false;
    }

    if (!m_creationCameraRigReady) {
        ResetCreationCameraRig();
    }

    m_creationCameraYawTarget = WrapAngle(m_creationCameraYawTarget + DirectX::XMConvertToRadians(yawDeltaDeg));
    m_creationCameraPitchTarget = ClampFloat(
        m_creationCameraPitchTarget + DirectX::XMConvertToRadians(pitchDeltaDeg),
        kCreationCameraPitchMin,
        kCreationCameraPitchMax);
    m_creationCameraDistanceTarget = ClampFloat(
        m_creationCameraDistanceTarget + zoomDelta,
        kCreationCameraDistanceMin,
        kCreationCameraDistanceMax);
    m_creationCameraAutoOrbit = false;
    return true;
}

bool RScene::AdjustCreationCharacterYaw(float yawDeltaDeg) {
    if (!m_showcaseCharacter.visual.valid && !m_creationCameraRigReady) {
        return false;
    }

    m_creationCharacterYaw = WrapAngle(m_creationCharacterYaw + DirectX::XMConvertToRadians(yawDeltaDeg));
    return true;
}

bool RScene::SetCreationCameraPose(float yawDeg, float pitchDeg, float distance, float focusHeight, bool autoOrbit) {
    if (!m_showcaseCharacter.visual.valid && !m_creationCameraRigReady) {
        return false;
    }

    if (!m_creationCameraRigReady) {
        ResetCreationCameraRig();
    }

    m_creationCameraYawTarget = WrapAngle(DirectX::XMConvertToRadians(yawDeg));
    m_creationCameraPitchTarget = ClampFloat(
        DirectX::XMConvertToRadians(pitchDeg),
        kCreationCameraPitchMin,
        kCreationCameraPitchMax);
    m_creationCameraDistanceTarget = ClampFloat(
        distance,
        kCreationCameraDistanceMin,
        kCreationCameraDistanceMax);
    m_creationCameraFocusHeightTarget = ClampFloat(focusHeight, 20.0f, 260.0f);
    m_creationCameraAutoOrbit = autoOrbit;
    return true;
}

void RScene::SetCreationCameraAutoOrbit(bool enabled) {
    m_creationCameraAutoOrbit = enabled;
}

void RScene::ResetCreationCamera() {
    ResetCreationCameraRig();
    m_creationCharacterYaw = 0.0f;
    m_creationCameraAutoOrbit = true;
}

bool RScene::BuildShowcaseWorldMatrix(const ShowcaseRenderable& renderable, bool applyCreationOrientation, DirectX::XMFLOAT4X4& outWorld) const {
    DirectX::XMFLOAT3 pos = m_creationShowroomMode
        ? m_creationShowroomAnchor
        : (m_hasSpawnPos ? m_spawnPos : DirectX::XMFLOAT3{ 0.0f, 0.0f, 0.0f });
    pos.x += renderable.localOffset.x;
    pos.y += renderable.localOffset.y;
    pos.z += renderable.localOffset.z;

    DirectX::XMVECTOR forward = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    if (applyCreationOrientation || renderable.faceCamera) {
        if (m_creationShowroomMode) {
            // Showroom mode keeps the character decoupled from map spawn/orientation.
            DirectX::XMVECTOR toCamera = DirectX::XMVectorSet(
                m_cameraPos.x - pos.x,
                m_cameraPos.y - pos.y,
                0.0f,
                0.0f);
            if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(toCamera)) < 0.000001f) {
                toCamera = DirectX::XMVectorSet(0.0f, -1.0f, 0.0f, 0.0f);
            } else {
                toCamera = DirectX::XMVector3Normalize(toCamera);
            }

            // Converted character packages have opposite local forward.
            forward = DirectX::XMVectorNegate(toCamera);
            if (std::abs(m_creationCharacterYaw) > 0.00001f) {
                const DirectX::XMMATRIX yawRot = DirectX::XMMatrixRotationZ(m_creationCharacterYaw);
                forward = DirectX::XMVector3Normalize(DirectX::XMVector3TransformNormal(forward, yawRot));
            }
            forward = DirectX::XMVectorNegate(forward);
        } else {
            DirectX::XMFLOAT3 dir = m_hasSpawnPos ? m_spawnDir : DirectX::XMFLOAT3{ 0.0f, 1.0f, 0.0f };

            // Prefer map spawn direction for OGZ parity, but keep auto-flip to avoid back-facing.
            forward = DirectX::XMVectorSet(dir.x, dir.y, 0.0f, 0.0f);
            DirectX::XMVECTOR toCamera = DirectX::XMVectorSet(
                m_cameraPos.x - pos.x,
                m_cameraPos.y - pos.y,
                0.0f,
                0.0f);
            if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(forward)) < 0.000001f) {
                forward = toCamera;
            }
            if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(forward)) < 0.000001f) {
                forward = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
            }

            if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(toCamera)) >= 0.000001f) {
                const DirectX::XMVECTOR forwardN = DirectX::XMVector3Normalize(forward);
                const DirectX::XMVECTOR toCameraN = DirectX::XMVector3Normalize(toCamera);
                if (DirectX::XMVectorGetX(DirectX::XMVector3Dot(forwardN, toCameraN)) < 0.0f) {
                    forward = DirectX::XMVectorNegate(forward);
                }
            }

            forward = DirectX::XMVector3Normalize(forward);

            // Converted character packages are authored with opposite local forward
            // versus map spawn forward; compensate here for char-select preview.
            forward = DirectX::XMVectorNegate(forward);

            if (std::abs(m_creationCharacterYaw) > 0.00001f) {
                const DirectX::XMMATRIX yawRot = DirectX::XMMatrixRotationZ(m_creationCharacterYaw);
                forward = DirectX::XMVector3Normalize(DirectX::XMVector3TransformNormal(forward, yawRot));
            }
        }
    } else {
        const float yaw = DirectX::XMConvertToRadians(renderable.yawOffsetDeg);
        const DirectX::XMMATRIX yawRot = DirectX::XMMatrixRotationZ(yaw);
        forward = DirectX::XMVector3Normalize(
            DirectX::XMVector3TransformNormal(DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), yawRot));
    }

    forward = DirectX::XMVector3Normalize(forward);

    DirectX::XMVECTOR up = DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
    DirectX::XMVECTOR right = DirectX::XMVector3Cross(up, forward);
    if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(right)) < 0.000001f) {
        right = DirectX::XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
    }
    right = DirectX::XMVector3Normalize(right);
    up = DirectX::XMVector3Normalize(DirectX::XMVector3Cross(forward, right));

    const float safeScale = (renderable.scale <= 0.0001f) ? 1.0f : renderable.scale;
    right = DirectX::XMVectorScale(right, safeScale);
    up = DirectX::XMVectorScale(up, safeScale);
    forward = DirectX::XMVectorScale(forward, safeScale);

    DirectX::XMFLOAT3 rightF;
    DirectX::XMFLOAT3 upF;
    DirectX::XMFLOAT3 forwardF;
    DirectX::XMStoreFloat3(&rightF, right);
    DirectX::XMStoreFloat3(&upF, up);
    DirectX::XMStoreFloat3(&forwardF, forward);

    outWorld = DirectX::XMFLOAT4X4(
        rightF.x, rightF.y, rightF.z, 0.0f,
        upF.x, upF.y, upF.z, 0.0f,
        forwardF.x, forwardF.y, forwardF.z, 0.0f,
        pos.x, pos.y, pos.z, 1.0f);

    return true;
}

void RScene::BuildBindPoseSkinMatrices(const RS3ModelPackage& package, std::vector<DirectX::XMFLOAT4X4>& outMatrices) const {
    const size_t count = std::min<size_t>(package.bones.size(), MAX_BONES);
    outMatrices.assign(count, Identity4x4());
}

void RScene::Update(float deltaTime) {
    if (deltaTime <= 0.0f) {
        return;
    }

    if (m_creationCameraRigReady) {
        if (m_creationCameraAutoOrbit && m_showcaseCharacter.visible) {
            m_creationCameraYawTarget = WrapAngle(m_creationCameraYawTarget + kCreationCameraAutoOrbitSpeed * deltaTime);
        }

        const float t = ClampFloat(1.0f - std::exp(-kCreationCameraLerpSpeed * deltaTime), 0.0f, 1.0f);
        m_creationCameraYaw = LerpAngle(m_creationCameraYaw, m_creationCameraYawTarget, t);
        m_creationCameraPitch += (m_creationCameraPitchTarget - m_creationCameraPitch) * t;
        m_creationCameraDistance += (m_creationCameraDistanceTarget - m_creationCameraDistance) * t;
        m_creationCameraFocusHeight += (m_creationCameraFocusHeightTarget - m_creationCameraFocusHeight) * t;

        UpdateCreationCameraFromRig();
    }

    if (m_showcaseCharacter.visible && m_showcaseCharacter.visual.valid && m_showcaseCharacter.animate) {
        m_showcaseCharacter.visual.animation.Update(deltaTime);
    }
    if (m_showcasePlatform.visible && m_showcasePlatform.visual.valid && m_showcasePlatform.animate) {
        m_showcasePlatform.visual.animation.Update(deltaTime);
    }
}

void RScene::DrawWorld(ID3D11DeviceContext* context, DirectX::FXMMATRIX viewProj) {
    if (!context) {
        return;
    }

    m_stateManager->ClearSRVs();

    // Char select showcase mode: hide world map and render only showcase layers.
    if (m_creationShowroomMode) {
        m_stateManager->Reset();
        return;
    }

    if (m_hasMapGeometry && m_mapVB && m_mapIB && EnsureMapPipeline()) {
        const UINT stride = sizeof(MapGpuVertex);
        const UINT offset = 0;

        context->IASetInputLayout(m_mapInputLayout.Get());
        context->IASetVertexBuffers(0, 1, m_mapVB.GetAddressOf(), &stride, &offset);
        context->IASetIndexBuffer(m_mapIB.Get(), DXGI_FORMAT_R32_UINT, 0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        context->VSSetShader(m_mapVS.Get(), nullptr, 0);
        context->PSSetShader(m_mapPS.Get(), nullptr, 0);
        context->VSSetConstantBuffers(0, 1, m_mapPerFrameCB.GetAddressOf());
        context->PSSetConstantBuffers(0, 1, m_mapPerFrameCB.GetAddressOf());
        context->PSSetSamplers(0, 1, m_mapSampler.GetAddressOf());

        const auto drawPass = [&](int passId, ID3D11BlendState* blendState, ID3D11DepthStencilState* depthState) {
            const float blendFactor[4] = { 0, 0, 0, 0 };
            context->OMSetBlendState(blendState, blendFactor, 0xffffffff);
            context->OMSetDepthStencilState(depthState, 0);

            for (const auto& sec : m_mapSections) {
                if (ClassifyPass(sec.materialFlags, 0) != passId) {
                    continue;
                }

                MapPerFrameCB cb = {};
                DirectX::XMStoreFloat4x4(&cb.viewProj, viewProj);
                cb.lightDirIntensity = { m_sceneLightDir.x, m_sceneLightDir.y, m_sceneLightDir.z, m_sceneLightIntensity };
                cb.lightColorFogMin = { m_sceneLightColor.x, m_sceneLightColor.y, m_sceneLightColor.z, m_fogMin };
                cb.fogColorFogMax = { m_fogColor.x, m_fogColor.y, m_fogColor.z, m_fogMax };
                cb.cameraPosFogEnabled = { m_cameraPos.x, m_cameraPos.y, m_cameraPos.z, m_fogEnabled ? 1.0f : 0.0f };
                cb.renderParams = { static_cast<float>(passId), kDefaultAlphaRef, 0.0f, 0.0f };

                context->UpdateSubresource(m_mapPerFrameCB.Get(), 0, nullptr, &cb, 0, 0);

                ID3D11ShaderResourceView* srv = sec.diffuseSRV ? sec.diffuseSRV.Get() : m_textureManager->GetWhiteTexture().Get();
                context->PSSetShaderResources(0, 1, &srv);
                context->DrawIndexed(sec.indexCount, sec.indexStart, 0);
            }
        };

        m_stateManager->ApplyPass(RenderPass::Map);
        drawPass(0, m_bsOpaque.Get(), m_dsDepthWrite.Get());
        drawPass(1, m_bsOpaque.Get(), m_dsDepthWrite.Get());
        drawPass(2, m_bsAlphaBlend.Get(), m_dsDepthRead.Get());
        drawPass(3, m_bsAdditive.Get(), m_dsDepthRead.Get());
    }

    m_stateManager->Reset();
}

void RScene::DrawShowcase(ID3D11DeviceContext* context, DirectX::FXMMATRIX viewProj, bool forceNoDepthTest) {
    if (!context) {
        return;
    }

    m_stateManager->ClearSRVs();

    // Overlay showcase only runs when UI provides an explicit stage rect.
    if (!m_showcaseViewportEnabled) {
        m_stateManager->Reset();
        return;
    }

    if (!EnsureSkinPipeline()) {
        m_stateManager->Reset();
        return;
    }

    D3D11_VIEWPORT savedViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT savedViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    context->RSGetViewports(&savedViewportCount, savedViewports);

    if (m_showcaseViewportEnabled) {
        context->RSSetViewports(1, &m_showcaseViewport);
    }

    auto drawRenderable = [&](ShowcaseRenderable& renderable, bool applyCreationOrientation) -> size_t {
        if (!renderable.visible || !renderable.visual.valid) {
            return 0;
        }

        std::string gpuError;
        if (!EnsureShowcaseGpuResources(renderable, &gpuError)) {
            AppLogger::Log("[RS3] Draw showcase skipped: name='" + renderable.debugName + "' reason='" + gpuError + "'");
            return 0;
        }

        DirectX::XMFLOAT4X4 world;
        BuildShowcaseWorldMatrix(renderable, applyCreationOrientation, world);

        std::vector<DirectX::XMFLOAT4X4> animatedMatrices;
        if (renderable.animate && !renderable.visual.animation.BuildSkinMatrices(animatedMatrices) &&
            !renderable.visual.packages.empty()) {
            BuildBindPoseSkinMatrices(renderable.visual.packages.front(), animatedMatrices);
        }

        size_t drawCount = 0;
        ID3D11DepthStencilState* depthOpaque = forceNoDepthTest ? m_skinDsNoDepth.Get() : m_skinDsDepthWrite.Get();
        ID3D11DepthStencilState* depthAlpha = forceNoDepthTest ? m_skinDsNoDepth.Get() : m_skinDsDepthRead.Get();

        for (size_t packageIndex = 0; packageIndex < renderable.gpu.size(); ++packageIndex) {
            auto& runtime = renderable.gpu[packageIndex];
            const auto& sourcePackage = renderable.visual.packages[packageIndex];

            std::vector<DirectX::XMFLOAT4X4> skinMatrices;
            if (renderable.animate && packageIndex == 0 && !animatedMatrices.empty()) {
                skinMatrices = animatedMatrices;
            } else {
                BuildBindPoseSkinMatrices(sourcePackage, skinMatrices);
            }

            const UINT stride = sizeof(SkinGpuVertex);
            const UINT offset = 0;
            context->IASetInputLayout(m_skinInputLayout.Get());
            context->IASetVertexBuffers(0, 1, runtime.vb.GetAddressOf(), &stride, &offset);
            context->IASetIndexBuffer(runtime.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            context->VSSetShader(m_skinVS.Get(), nullptr, 0);
            context->PSSetShader(m_skinPS.Get(), nullptr, 0);
            context->VSSetConstantBuffers(0, 1, m_skinPerFrameCB.GetAddressOf());
            context->VSSetConstantBuffers(1, 1, m_skinBonesCB.GetAddressOf());
            context->PSSetConstantBuffers(0, 1, m_skinPerFrameCB.GetAddressOf());
            context->PSSetSamplers(0, 1, m_skinSampler.GetAddressOf());

            const auto drawSkinPass = [&](int passId, ID3D11BlendState* blendState, ID3D11DepthStencilState* depthState) {
                const float blendFactor[4] = { 0, 0, 0, 0 };
                context->OMSetBlendState(blendState, blendFactor, 0xffffffff);
                context->OMSetDepthStencilState(depthState, 0);

                for (const auto& sub : runtime.submeshes) {
                    if (renderable.skipCharacterNodeFilter) {
                        const std::string nodeName = (sub.nodeIndex < sourcePackage.bones.size())
                            ? sourcePackage.bones[sub.nodeIndex].name
                            : std::string();
                        if (ShouldSkipCharacterPreviewNode(nodeName)) {
                            continue;
                        }
                    }

                    if (ClassifyPass(sub.legacyFlags, sub.alphaMode) != passId) {
                        continue;
                    }

                    SkinBonesCB bonesCB = {};
                    for (auto& m : bonesCB.bones) {
                        m = Identity4x4();
                    }
                    const size_t copyCount = std::min<size_t>(skinMatrices.size(), MAX_BONES);
                    const DirectX::XMMATRIX subRef = DirectX::XMLoadFloat4x4(&sub.nodeTransform);
                    for (size_t i = 0; i < copyCount; ++i) {
                        const DirectX::XMMATRIX skin = DirectX::XMLoadFloat4x4(&skinMatrices[i]);
                        const DirectX::XMMATRIX combined = DirectX::XMMatrixMultiply(subRef, skin);
                        DirectX::XMStoreFloat4x4(&bonesCB.bones[i], combined);
                    }
                    context->UpdateSubresource(m_skinBonesCB.Get(), 0, nullptr, &bonesCB, 0, 0);

                    SkinPerFrameCB cb = {};
                    cb.world = world;
                    DirectX::XMStoreFloat4x4(&cb.viewProj, viewProj);
                    cb.lightDirIntensity = { m_sceneLightDir.x, m_sceneLightDir.y, m_sceneLightDir.z, m_sceneLightIntensity };
                    cb.lightColorFogMin = { m_sceneLightColor.x, m_sceneLightColor.y, m_sceneLightColor.z, m_fogMin };
                    cb.fogColorFogMax = { m_fogColor.x, m_fogColor.y, m_fogColor.z, m_fogMax };
                    cb.cameraPosFogEnabled = { m_cameraPos.x, m_cameraPos.y, m_cameraPos.z, m_fogEnabled ? 1.0f : 0.0f };
                    cb.renderParams = { static_cast<float>(passId), kDefaultAlphaRef, 0.0f, 0.0f };

                    context->UpdateSubresource(m_skinPerFrameCB.Get(), 0, nullptr, &cb, 0, 0);

                    ID3D11ShaderResourceView* srv = sub.diffuseSRV ? sub.diffuseSRV.Get() : m_textureManager->GetWhiteTexture().Get();
                    context->PSSetShaderResources(0, 1, &srv);
                    context->DrawIndexed(sub.indexCount, sub.indexStart, 0);
                    ++drawCount;
                }
            };

            m_stateManager->ApplyPass(RenderPass::Skin_Base);
            drawSkinPass(0, m_skinBsOpaque.Get(), depthOpaque);
            drawSkinPass(1, m_skinBsOpaque.Get(), depthOpaque);
            drawSkinPass(2, m_skinBsAlphaBlend.Get(), depthAlpha);
            drawSkinPass(3, m_skinBsAdditive.Get(), depthAlpha);
        }

        return drawCount;
    };

    const size_t platformDrawCount = drawRenderable(m_showcasePlatform, false);
    const size_t characterDrawCount = drawRenderable(m_showcaseCharacter, true);

    static int logTick = 0;
    if ((logTick++ % 180) == 0) {
        const RS3AnimationClip* clip = m_showcaseCharacter.visual.animation.GetCurrentClip();
        const std::string clipName = clip ? clip->name : std::string("<none>");
        AppLogger::Log("[RS3] Showcase draw stats: platform_calls=" + std::to_string(platformDrawCount) +
            " character_calls=" + std::to_string(characterDrawCount) +
            " viewport=" + std::to_string(static_cast<int>(m_showcaseViewport.TopLeftX)) + "," +
            std::to_string(static_cast<int>(m_showcaseViewport.TopLeftY)) + "," +
            std::to_string(static_cast<int>(m_showcaseViewport.Width)) + "x" +
            std::to_string(static_cast<int>(m_showcaseViewport.Height)) +
            " clip='" + clipName + "' t=" + std::to_string(m_showcaseCharacter.visual.animation.GetCurrentTimeSeconds()));
    }

    if (m_showcaseViewportEnabled && savedViewportCount > 0) {
        context->RSSetViewports(savedViewportCount, savedViewports);
    }

    m_stateManager->Reset();
}

void RScene::Draw(ID3D11DeviceContext* context, DirectX::FXMMATRIX viewProj) {
    DrawWorld(context, viewProj);
    DrawShowcase(context, viewProj, false);
}

void RScene::SetShowcaseViewportPixels(int x, int y, int width, int height) {
    if (width <= 1 || height <= 1) {
        m_showcaseViewportEnabled = false;
        return;
    }

    m_showcaseViewportEnabled = true;
    m_showcaseViewport.TopLeftX = static_cast<float>(std::max(0, x));
    m_showcaseViewport.TopLeftY = static_cast<float>(std::max(0, y));
    m_showcaseViewport.Width = static_cast<float>(std::max(1, width));
    m_showcaseViewport.Height = static_cast<float>(std::max(1, height));
    m_showcaseViewport.MinDepth = 0.0f;
    m_showcaseViewport.MaxDepth = 1.0f;

    static int rectLogTick = 0;
    if ((rectLogTick++ % 120) == 0) {
        AppLogger::Log("[RS3] Showcase viewport updated: x=" + std::to_string(static_cast<int>(m_showcaseViewport.TopLeftX)) +
            " y=" + std::to_string(static_cast<int>(m_showcaseViewport.TopLeftY)) +
            " w=" + std::to_string(static_cast<int>(m_showcaseViewport.Width)) +
            " h=" + std::to_string(static_cast<int>(m_showcaseViewport.Height)));
    }
}

bool RScene::GetPreferredCamera(DirectX::XMFLOAT3& outPos, DirectX::XMFLOAT3& outDir) const {
    outPos = m_cameraPos;
    outDir = m_cameraDir;
    return m_hasMapGeometry || m_creationShowroomMode;
}

bool RScene::SetCreationPreview(int sex, int face, int preset, int hair) {
    (void)preset;

    m_creationSex = sex;
    m_creationFace = face;
    m_creationPreset = preset;
    m_creationHair = hair;

    CharacterVisualRequest req;
    req.baseModelId = (sex == 1) ? "character/herowoman1" : "character/heroman1";
    req.initialClip = "login_idle#m2";

    CharacterVisualInstance built;
    std::string error;
    if (!m_characterAssembler->BuildCharacterVisual(req, built, &error)) {
        m_showcaseCharacter.visual = CharacterVisualInstance{};
        m_showcaseCharacter.visible = false;
        m_showcaseCharacter.gpuDirty = true;
        ReleaseCreationPreviewResources();

        AppLogger::Log("[RS3] SetCreationPreview failed for sex=" + std::to_string(sex) + ": " + error);
        return false;
    }

    ApplyCreationTextureOverrides(built, sex, face, hair);

    bool clipSet = false;
    static const std::array<const char*, 4> kClipFallback = {
        "login_idle#m2",
        "login_idle",
        "idle#m2",
        "idle"
    };

    for (const char* clipName : kClipFallback) {
        if (built.animation.SetAnimationClipByName(clipName, 0.15f)) {
            clipSet = true;
            AppLogger::Log(std::string("[RS3] SetCreationPreview clip='") + clipName + "'.");
            break;
        }
    }

    if (!clipSet && !built.packages.empty() && !built.packages.front().clips.empty()) {
        const std::string firstClip = built.packages.front().clips.front().name;
        if (built.animation.SetAnimationClipByName(firstClip, 0.15f)) {
            AppLogger::Log("[RS3] SetCreationPreview fallback clip='" + firstClip + "'.");
        }
    }

    m_showcaseCharacter.visual = std::move(built);
    m_showcaseCharacter.visible = true;
    m_showcaseCharacter.gpuDirty = true;
    if (!m_creationCameraRigReady) {
        ResetCreationCameraRig();
    } else {
        UpdateCreationCameraFromRig();
    }

    std::string gpuError;
    if (!EnsureShowcaseGpuResources(m_showcaseCharacter, &gpuError)) {
        AppLogger::Log("[RS3] SetCreationPreview GPU prepare failed: " + gpuError);
        return false;
    }

    AppLogger::Log("[RS3] SetCreationPreview success: model='" + req.baseModelId + "'.");
    return true;
}

void RScene::SetCreationPreviewVisible(bool visible) {
    m_showcaseCharacter.visible = visible;
    if (visible && m_showcasePlatform.visual.valid) {
        m_showcasePlatform.visible = true;
    }
}

bool RScene::GetSpawnPos(DirectX::XMFLOAT3& outPos) const {
    if (!m_hasSpawnPos) {
        return false;
    }

    outPos = m_spawnPos;
    return true;
}

} // namespace RealSpace3
