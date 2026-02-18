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
constexpr float kDefaultAlphaRef = 0.5f;

void SetError(std::string* outError, const std::string& message) {
    if (outError) {
        *outError = message;
    }
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
}

RScene::~RScene() {
    ReleaseCreationPreviewResources();
    ReleaseMapResources();
}

void RScene::LoadCharSelect() {
    ReleaseCreationPreviewResources();
    m_creationPreview = CharacterVisualInstance{};
    m_creationPreviewVisible = false;
    m_creationPreviewGpuDirty = true;

    if (!LoadCharSelectPackage(kCharSelectSceneId)) {
        AppLogger::Log("[RS3] LoadCharSelect -> package load failed, falling back to LoadLobbyBasic.");
        LoadLobbyBasic();
        return;
    }

    AppLogger::Log("[RS3] LoadCharSelect -> scene package active: char_creation_select.");
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

    m_creationPreview = CharacterVisualInstance{};
    m_creationPreviewVisible = false;
    m_creationPreviewGpuDirty = true;

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
    return true;
}

void RScene::ReleaseMapResources() {
    m_mapVB.Reset();
    m_mapIB.Reset();
    m_mapSections.clear();
    m_hasMapGeometry = false;
}

bool RScene::EnsureSkinPipeline() {
    if (m_skinVS && m_skinPS && m_skinInputLayout && m_skinSampler && m_skinPerFrameCB && m_skinBonesCB) {
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

    static bool logged = false;
    if (!logged) {
        AppLogger::Log("[RS3] EnsureSkinPipeline -> ready.");
        logged = true;
    }

    return true;
}

bool RScene::EnsureCreationPreviewGpuResources(std::string* outError) {
    if (!m_creationPreviewGpuDirty) {
        return !m_creationPreviewGpu.empty();
    }

    ReleaseCreationPreviewResources();

    if (!m_creationPreview.valid || m_creationPreview.packages.empty()) {
        SetError(outError, "Creation preview is not valid or has no packages.");
        return false;
    }

    m_creationPreviewGpu.reserve(m_creationPreview.packages.size());

    for (const auto& package : m_creationPreview.packages) {
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
            m_creationPreviewGpu.push_back(std::move(runtime));
        }
    }

    if (m_creationPreviewGpu.empty()) {
        SetError(outError, "Creation preview GPU cache is empty.");
        return false;
    }

    m_creationPreviewGpuDirty = false;

    size_t totalSubmeshes = 0;
    for (const auto& p : m_creationPreviewGpu) {
        totalSubmeshes += p.submeshes.size();
    }

    std::ostringstream oss;
    oss << "[RS3] Creation preview GPU cache ready: packages=" << m_creationPreviewGpu.size()
        << " submeshes=" << totalSubmeshes;
    AppLogger::Log(oss.str());

    return true;
}

void RScene::ReleaseCreationPreviewResources() {
    m_creationPreviewGpu.clear();
    m_creationPreviewGpuDirty = true;
}

bool RScene::BuildPreviewWorldMatrix(DirectX::XMFLOAT4X4& outWorld) const {
    DirectX::XMFLOAT3 pos = m_hasSpawnPos ? m_spawnPos : DirectX::XMFLOAT3{ 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 dir = m_hasSpawnPos ? m_spawnDir : DirectX::XMFLOAT3{ 0.0f, 1.0f, 0.0f };

    // Prefer map spawn direction for OGZ parity, but keep auto-flip to avoid back-facing.
    DirectX::XMVECTOR forward = DirectX::XMVectorSet(dir.x, dir.y, 0.0f, 0.0f);
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

    DirectX::XMVECTOR up = DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
    DirectX::XMVECTOR right = DirectX::XMVector3Cross(up, forward);
    if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(right)) < 0.000001f) {
        right = DirectX::XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
    }
    right = DirectX::XMVector3Normalize(right);

    up = DirectX::XMVector3Normalize(DirectX::XMVector3Cross(forward, right));

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

    if (m_creationPreviewVisible && m_creationPreview.valid) {
        m_creationPreview.animation.Update(deltaTime);
    }
}

void RScene::Draw(ID3D11DeviceContext* context, DirectX::FXMMATRIX viewProj) {
    if (!context) {
        return;
    }

    m_stateManager->ClearSRVs();

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

    if (m_creationPreviewVisible && m_creationPreview.valid && EnsureSkinPipeline()) {
        std::string gpuError;
        if (!EnsureCreationPreviewGpuResources(&gpuError)) {
            AppLogger::Log("[RS3] Draw preview skipped: " + gpuError);
        } else {
            DirectX::XMFLOAT4X4 world;
            BuildPreviewWorldMatrix(world);

            std::vector<DirectX::XMFLOAT4X4> animatedMatrices;
            if (!m_creationPreview.animation.BuildSkinMatrices(animatedMatrices) && !m_creationPreview.packages.empty()) {
                BuildBindPoseSkinMatrices(m_creationPreview.packages.front(), animatedMatrices);
            }

            size_t drawCount = 0;

            for (size_t packageIndex = 0; packageIndex < m_creationPreviewGpu.size(); ++packageIndex) {
                auto& runtime = m_creationPreviewGpu[packageIndex];
                const auto& sourcePackage = m_creationPreview.packages[packageIndex];

                std::vector<DirectX::XMFLOAT4X4> skinMatrices;
                if (packageIndex == 0 && !animatedMatrices.empty()) {
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
                        const std::string nodeName = (sub.nodeIndex < sourcePackage.bones.size())
                            ? sourcePackage.bones[sub.nodeIndex].name
                            : std::string();
                        if (ShouldSkipCharacterPreviewNode(nodeName)) {
                            continue;
                        }

                        if (ClassifyPass(sub.legacyFlags, sub.alphaMode) != passId) {
                            continue;
                        }

                        // OGZ order for skinned character parts is: v * partRef * invBind * current.
                        // We fold `partRef` (sub.nodeTransform) into each bone matrix per submesh.
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
                drawSkinPass(0, m_skinBsOpaque.Get(), m_skinDsDepthWrite.Get());
                drawSkinPass(1, m_skinBsOpaque.Get(), m_skinDsDepthWrite.Get());
                drawSkinPass(2, m_skinBsAlphaBlend.Get(), m_skinDsDepthRead.Get());
                drawSkinPass(3, m_skinBsAdditive.Get(), m_skinDsDepthRead.Get());
            }

            static int logTick = 0;
            if ((logTick++ % 180) == 0) {
                const RS3AnimationClip* clip = m_creationPreview.animation.GetCurrentClip();
                const std::string clipName = clip ? clip->name : std::string("<none>");
                AppLogger::Log("[RS3] Preview draw stats: drawCalls=" + std::to_string(drawCount) +
                    " clip='" + clipName + "' t=" + std::to_string(m_creationPreview.animation.GetCurrentTimeSeconds()));
            }
        }
    }

    m_stateManager->Reset();
}

bool RScene::GetPreferredCamera(DirectX::XMFLOAT3& outPos, DirectX::XMFLOAT3& outDir) const {
    outPos = m_cameraPos;
    outDir = m_cameraDir;
    return m_hasMapGeometry;
}

bool RScene::SetCreationPreview(int sex, int face, int preset, int hair) {
    (void)face;
    (void)preset;
    (void)hair;

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
        m_creationPreview = CharacterVisualInstance{};
        m_creationPreviewVisible = false;
        m_creationPreviewGpuDirty = true;
        ReleaseCreationPreviewResources();

        AppLogger::Log("[RS3] SetCreationPreview failed for sex=" + std::to_string(sex) + ": " + error);
        return false;
    }

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

    m_creationPreview = std::move(built);
    m_creationPreviewVisible = true;
    m_creationPreviewGpuDirty = true;

    std::string gpuError;
    if (!EnsureCreationPreviewGpuResources(&gpuError)) {
        AppLogger::Log("[RS3] SetCreationPreview GPU prepare failed: " + gpuError);
        return false;
    }

    AppLogger::Log("[RS3] SetCreationPreview success: model='" + req.baseModelId + "'.");
    return true;
}

void RScene::SetCreationPreviewVisible(bool visible) {
    m_creationPreviewVisible = visible;
}

bool RScene::GetSpawnPos(DirectX::XMFLOAT3& outPos) const {
    if (!m_hasSpawnPos) {
        return false;
    }

    outPos = m_spawnPos;
    return true;
}

} // namespace RealSpace3
