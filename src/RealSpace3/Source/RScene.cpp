#include "../Include/RScene.h"
#include "AppLogger.h"
#include <array>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <cmath>
#include <regex>
#include <sstream>
#include <cstdint>
#include <iterator>
#include <filesystem>

namespace RealSpace3 {
namespace {
std::string ToLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

int WrapIndex(int value, int count) {
    if (count <= 0) return 0;
    int x = value % count;
    if (x < 0) x += count;
    return x;
}

// DEV toggles for OGZ->NDG migration (manual/reversible).
constexpr bool kForceCharacterDrawOutsidePreview = true; // false restores preview-only draw gate
constexpr bool kUseOgzTripleWeightsForOgzAssets = true;  // manual toggle for OGZ triple-weight path
constexpr bool kUseCreationSpawnTransformFix = true;       // false restores original spawn preview transform

constexpr std::array<const char*, 5> kHairMale = {
    "eq_head_01",
    "eq_head_02",
    "eq_head_08",
    "eq_head_05",
    "eq_head_08"
};

constexpr std::array<const char*, 5> kHairFemale = {
    "eq_head_pony",
    "eq_head_hair001",
    "eq_head_hair04",
    "eq_head_hair006",
    "eq_head_hair002"
};

constexpr std::array<const char*, 20> kFaceMale = {
    "eq_face_01",
    "eq_face_02",
    "eq_face_04",
    "eq_face_05",
    "eq_face_a01",
    "eq_face_newface01",
    "eq_face_newface02",
    "eq_face_newface03",
    "eq_face_newface04",
    "eq_face_newface05",
    "eq_face_newface06",
    "eq_face_newface07",
    "eq_face_newface08",
    "eq_face_newface09",
    "eq_face_newface10",
    "eq_face_newface11",
    "eq_face_newface12",
    "eq_face_newface13",
    "eq_face_newface13",
    "eq_face_newface13"
};

constexpr std::array<const char*, 20> kFaceFemale = {
    "eq_face_001",
    "eq_face_002",
    "eq_face_003",
    "eq_face_004",
    "eq_face_001",
    "eq_face_newface01",
    "eq_face_newface02",
    "eq_face_newface03",
    "eq_face_newface04",
    "eq_face_newface05",
    "eq_face_newface06",
    "eq_face_newface07",
    "eq_face_newface08",
    "eq_face_newface09",
    "eq_face_newface10",
    "eq_face_newface11",
    "eq_face_newface12",
    "eq_face_newface13",
    "eq_face_newface14",
    "eq_face_newface15"
};

struct InitialCostumePreset {
    uint32_t meleeItemID;
    uint32_t primaryItemID;
    uint32_t secondaryItemID;
    uint32_t custom1ItemID;
    uint32_t custom2ItemID;
    uint32_t chestItemID;
    uint32_t handsItemID;
    uint32_t legsItemID;
    uint32_t feetItemID;
};

constexpr int kMaxCostumeTemplate = 6;
constexpr std::array<std::array<InitialCostumePreset, 2>, kMaxCostumeTemplate> kInitialCostume = {{
    {{
        { 1, 5001, 4001, 30301, 0, 21001, 0, 23001, 0 },
        { 1, 5001, 4001, 30301, 0, 21501, 0, 23501, 0 }
    }},
    {{
        { 2, 5002, 0, 30301, 0, 21001, 0, 23001, 0 },
        { 2, 5002, 0, 30301, 0, 21501, 0, 23501, 0 }
    }},
    {{
        { 1, 4005, 5001, 30401, 0, 21001, 0, 23001, 0 },
        { 1, 4005, 5001, 30401, 0, 21501, 0, 23501, 0 }
    }},
    {{
        { 2, 4001, 0, 30401, 0, 21001, 0, 23001, 0 },
        { 2, 4001, 0, 30401, 0, 21501, 0, 23501, 0 }
    }},
    {{
        { 2, 4002, 0, 30401, 30001, 21001, 0, 23001, 0 },
        { 2, 4002, 0, 30401, 30001, 21501, 0, 23501, 0 }
    }},
    {{
        { 1, 4006, 0, 30101, 30001, 21001, 0, 23001, 0 },
        { 1, 4006, 4006, 30101, 30001, 21501, 0, 23501, 0 }
    }}
}};
}

RScene::RScene(ID3D11Device* device, ID3D11DeviceContext* context) 
    : m_pd3dDevice(device) 
{
    m_stateManager = std::make_unique<RStateManager>(device, context);
    m_textureManager = std::make_unique<TextureManager>(device);
}

RScene::~RScene() {}

bool RScene::FileExists(const std::string& path) const {
    std::ifstream file(path, std::ios::binary);
    return file.is_open();
}

void RScene::EnsureItemMeshMapLoaded() {
    if (m_itemMeshMapLoaded) return;
    m_itemMeshMapLoaded = true;
    m_itemMeshById.clear();

    const std::array<std::string, 6> candidates = {
        "ogz-client-master/system/zitem.xml",
        "ogz-client-master/system/zitem_cleaned.xml",
        "OpenGunZ-Client/system/zitem.xml",
        "OpenGunZ-Client/system/zitem_cleaned.xml",
        "system/zitem.xml",
        "system/zitem_cleaned.xml"
    };

    std::string sourcePath;
    for (const auto& path : candidates) {
        if (FileExists(path)) {
            sourcePath = path;
            break;
        }
    }

    if (sourcePath.empty()) {
        AppLogger::Log("[RS3_AUDIT] RScene::EnsureItemMeshMapLoaded -> zitem not found.");
        return;
    }

    std::ifstream file(sourcePath, std::ios::binary);
    if (!file.is_open()) {
        AppLogger::Log("[RS3_AUDIT] RScene::EnsureItemMeshMapLoaded -> failed to open: " + sourcePath);
        return;
    }

    const std::string xml((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const std::regex itemTagRe("<[^>]*ITEM\\b[^>]*>");
    const std::regex idRe("\\bid\\s*=\\s*\"([0-9]+)\"");
    const std::regex meshRe("\\bmesh_name\\s*=\\s*\"([^\"]+)\"");
    for (std::sregex_iterator it(xml.begin(), xml.end(), itemTagRe), end; it != end; ++it) {
        const std::string tag = it->str();
        std::smatch idMatch;
        std::smatch meshMatch;
        if (!std::regex_search(tag, idMatch, idRe) || !std::regex_search(tag, meshMatch, meshRe)) continue;
        if (idMatch.size() < 2 || meshMatch.size() < 2) continue;

        uint32_t id = 0;
        std::istringstream iss(idMatch[1].str());
        iss >> id;
        if (!iss || id == 0) continue;

        std::string mesh = meshMatch[1].str();
        if (mesh.empty()) continue;
        m_itemMeshById[id] = mesh;
    }

    AppLogger::Log("[RS3_AUDIT] RScene::EnsureItemMeshMapLoaded -> source=" + sourcePath +
        " entries=" + std::to_string(m_itemMeshById.size()));
}

std::string RScene::GetMeshNameByItemId(uint32_t itemId) const {
    auto it = m_itemMeshById.find(itemId);
    if (it == m_itemMeshById.end()) return {};
    return it->second;
}

void RScene::BuildCreationVariants() {
    m_maleVariants.clear();
    m_femaleVariants.clear();
    m_malePartLibraries.clear();
    m_femalePartLibraries.clear();

    // Presets represent initial weapon/clothes package in original flow.
    // Face is separate metadata and should not swap the whole body mesh.
    const std::array<const char*, 1> male = { "Model/man/man-parts00.elu" };
    const std::array<const char*, 1> female = { "Model/woman/woman-parts01.elu" };

    for (auto* x : male) if (FileExists(x)) m_maleVariants.emplace_back(x);
    for (auto* x : female) if (FileExists(x)) m_femaleVariants.emplace_back(x);
    // OGZ-safe whitelist: avoids mixing incompatible themed rigs that distort preview vertices.
    const std::array<const char*, 4> maleParts = {
        "Model/man/man-parts02.elu",   // base face/hair/legs
        "Model/man/man-parts03.elu",   // alternate base face/hair
        "Model/man/man-parts12.elu",   // default male chest
        "Model/man/man-parts_face.elu" // newface variants
    };
    const std::array<const char*, 5> femaleParts = {
        "Model/woman/woman-parts02.elu",   // base face
        "Model/woman/woman-parts03.elu",   // base hair
        "Model/woman/woman-parts07.elu",   // default female legs
        "Model/woman/woman-parts11.elu",   // default female chest
        "Model/woman/woman-parts_face.elu" // newface variants
    };
    for (auto* x : maleParts) if (FileExists(x)) m_malePartLibraries.emplace_back(x);
    for (auto* x : femaleParts) if (FileExists(x)) m_femalePartLibraries.emplace_back(x);

    AppLogger::Log("[RS3_AUDIT] BuildCreationVariants: maleVariants=" + std::to_string(m_maleVariants.size()) +
        " femaleVariants=" + std::to_string(m_femaleVariants.size()) +
        " malePartLibs=" + std::to_string(m_malePartLibraries.size()) +
        " femalePartLibs=" + std::to_string(m_femalePartLibraries.size()));
}

void RScene::ApplyCreationPreview() {
    AppLogger::Log("[RS3_AUDIT] ApplyCreationPreview called. m_character=" + std::to_string((uintptr_t)m_character.get()) + " m_previewVisible=" + std::to_string(m_previewVisible));
    if (!m_character) {
        AppLogger::Log("[RS3_AUDIT] ApplyCreationPreview: m_character is null!");
        return;
    }

    const bool female = (m_previewSex != 0);
    const auto& variants = female ? m_femaleVariants : m_maleVariants;
    if (variants.empty()) return;
    const int preset = (m_previewPreset < 0) ? -m_previewPreset : m_previewPreset;
    // OGZ creation logic: sex selects the visual base; preset is for initial weapon package.
    const int idx = 0;
    const std::string elu = variants[idx];
    const std::string ani = female ? "Model/woman/woman_login_knife_idle.elu.ani"
                                   : "Model/man/man_login_knife_idle.elu.ani";

    if (!FileExists(ani)) {
        AppLogger::Log("[RS3_AUDIT] RScene::ApplyCreationPreview -> Missing ANI: " + ani);
        return;
    }

    // Heuristica de caminho OGZ (DEV-only): pode ser ajustada manualmente pelo toggle acima.
    const auto eluLower = ToLowerCopy(elu);
    const bool isLikelyOgzPath =
        (eluLower.find("model/man/") == 0) ||
        (eluLower.find("model/woman/") == 0);
    m_character->SetUseOgzTripleWeights(kUseOgzTripleWeightsForOgzAssets && isLikelyOgzPath);

    const bool eluOk = m_character->LoadElu(elu);
    if (!eluOk) {
        AppLogger::Log("[RS3_AUDIT] RScene::ApplyCreationPreview -> ELU load failed. ELU=" + elu);
        return;
    }

    const auto& partLibs = female ? m_femalePartLibraries : m_malePartLibraries;
    for (const auto& partElu : partLibs) {
        if (ToLowerCopy(partElu) == ToLowerCopy(elu)) continue;
        if (FileExists(partElu)) {
            m_character->AppendLegacyPartsFromElu5007(partElu);
        }
    }

    EnsureItemMeshMapLoaded();

    const int costumeIdx = WrapIndex(preset, kMaxCostumeTemplate);
    const int sexIdx = female ? 1 : 0;
    const auto& costume = kInitialCostume[static_cast<size_t>(costumeIdx)][static_cast<size_t>(sexIdx)];

    const int faceIdx = WrapIndex(m_previewFace, static_cast<int>(kFaceMale.size()));
    const int hairIdx = 0;
    const char* faceNode = female ? kFaceFemale[faceIdx] : kFaceMale[faceIdx];
    const char* hairNode = female ? kHairFemale[hairIdx] : kHairMale[hairIdx];

    std::string chestNode = GetMeshNameByItemId(costume.chestItemID);
    std::string handsNode = GetMeshNameByItemId(costume.handsItemID);
    std::string legsNode = GetMeshNameByItemId(costume.legsItemID);
    std::string feetNode = GetMeshNameByItemId(costume.feetItemID);
    const bool chestOk = !chestNode.empty() && m_character->SetLegacyPart("chest", chestNode);
    const bool handsOk = !handsNode.empty() && m_character->SetLegacyPart("hands", handsNode);
    const bool legsOk = !legsNode.empty() && m_character->SetLegacyPart("legs", legsNode);
    const bool feetOk = !feetNode.empty() && m_character->SetLegacyPart("feet", feetNode);

    bool faceOk = m_character->SetLegacyPart("face", faceNode);
    if (!faceOk) {
        faceOk = m_character->SetLegacyPartByIndex("face", faceIdx);
    }
    const bool headOk = m_character->SetLegacyPart("head", hairNode);

    AppLogger::Log("[RS3_AUDIT] RScene::ApplyCreationPreview -> Legacy parts face=" + std::string(faceNode) +
        " (" + (faceOk ? "ok" : "miss") + "), head=" + std::string(hairNode) +
        " (" + (headOk ? "ok" : "miss") + "), chest=" + (chestNode.empty() ? std::string("none") : chestNode) +
        " (" + (chestOk ? "ok" : "miss") + "), hands=" + (handsNode.empty() ? std::string("none") : handsNode) +
        " (" + (handsOk ? "ok" : "miss") + "), legs=" + (legsNode.empty() ? std::string("none") : legsNode) +
        " (" + (legsOk ? "ok" : "miss") + "), feet=" + (feetNode.empty() ? std::string("none") : feetNode) +
        " (" + (feetOk ? "ok" : "miss") + ")");

    const bool aniOk = m_character->LoadAni(ani);
    if (!aniOk) {
        AppLogger::Log("[RS3_AUDIT] RScene::ApplyCreationPreview -> ANI load failed, using bind-pose fallback. ANI=" + ani);
        m_character->SetBindPoseOnly(true);
    } else {
        m_character->SetBindPoseOnly(false);
    }

    DirectX::XMFLOAT3 previewPos = m_spawnPos;
    float previewYaw = 0.0f;
    if (m_hasSpawn) {
        // Original tuning kept for rollback.
        constexpr float kSpawnHeightOffsetLegacy = 98.0f;
        constexpr float kSpawnYawBiasLegacy = -0.62f;
        constexpr float kExtraTurnLegacy = 1.91986218f;

        previewPos = m_spawnPos;
        const float dx = m_spawnDir.x;
        const float dy = m_spawnDir.y;

        if (kUseCreationSpawnTransformFix) {
            // DEV-only heuristic for OGZ->NDG preview framing (rollback available via toggle).
            // Scene uses Z-up (see LookAtLH up=(0,0,1)); yaw is on XY plane.
            // Keep spawn anchor and remove legacy yaw bias/extra turn to avoid off-screen framing.
            constexpr float kSpawnHeightOffsetFix = 0.0f;
            previewPos.z += kSpawnHeightOffsetFix;
            if (std::fabs(dx) > 1e-4f || std::fabs(dy) > 1e-4f) {
                previewYaw = std::atan2(dy, dx);
            }
        } else {
            // Legacy behavior.
            previewPos.z += kSpawnHeightOffsetLegacy;
            if (std::fabs(dx) > 1e-4f || std::fabs(dy) > 1e-4f) {
                previewYaw = std::atan2(dy, dx) + kSpawnYawBiasLegacy + kExtraTurnLegacy;
            }
        }
    } else if (m_hasCameraDummy) {
        const DirectX::XMVECTOR eye = DirectX::XMVectorSet(m_cameraPos.x, m_cameraPos.y, m_cameraPos.z, 0.0f);
        DirectX::XMVECTOR dir = DirectX::XMVectorSet(m_cameraDir.x, m_cameraDir.y, 0.0f, 0.0f);
        dir = DirectX::XMVector3Normalize(dir);
        const DirectX::XMVECTOR up = DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
        DirectX::XMVECTOR right = DirectX::XMVector3Normalize(DirectX::XMVector3Cross(up, dir));

        // Fallback only when map has no spawn dummy.
        const float kForward = 390.0f;
        const float kRight = 220.0f;
        const float kHeight = 230.0f;
        const float kYawBias = 0.841799f;

        DirectX::XMVECTOR p = DirectX::XMVectorAdd(eye, DirectX::XMVectorScale(dir, kForward));
        p = DirectX::XMVectorAdd(p, DirectX::XMVectorScale(right, kRight));
        DirectX::XMStoreFloat3(&previewPos, p);
        previewPos.z = m_cameraPos.z - kHeight;

        DirectX::XMFLOAT3 d{};
        DirectX::XMStoreFloat3(&d, dir);
        // Face towards camera direction (z-up world).
        previewYaw = std::atan2(-d.y, -d.x) + kYawBias;
    }
    m_character->SetWorldPosition(previewPos);
    m_character->SetWorldYaw(previewYaw);

    AppLogger::Log("[RS3_AUDIT] RScene::ApplyCreationPreview -> Preview applied. Sex=" + std::to_string(m_previewSex) +
        " Face=" + std::to_string(m_previewFace) + " Preset=" + std::to_string(m_previewPreset) +
        " Hair=" + std::to_string(m_previewHair) +
        " SpawnPos=(" + std::to_string(m_spawnPos.x) + "," + std::to_string(m_spawnPos.y) + "," + std::to_string(m_spawnPos.z) + ")" +
        " SpawnDir=(" + std::to_string(m_spawnDir.x) + "," + std::to_string(m_spawnDir.y) + "," + std::to_string(m_spawnDir.z) + ")" +
        " PreviewPos=(" + std::to_string(previewPos.x) + "," + std::to_string(previewPos.y) + "," + std::to_string(previewPos.z) + ")" +
        " PreviewYaw=" + std::to_string(previewYaw) +
        " Ani=" + std::string(aniOk ? "ok" : "fallback"));
}

void RScene::LoadCharSelect() {
    AppLogger::Log("[RS3_AUDIT] LoadCharSelect start");
    m_map = std::make_unique<RBspObject>(m_pd3dDevice, m_textureManager.get());

    bool ok = false;
    const std::array<std::string, 3> mapCandidates = {
        "ui/Char-Creation-Select/login.RS",
        "ui/Char-Creation-Select/login.rs",
        "ui/Char-Creation-Select/char_select.rs"
    };

    for (const auto& candidate : mapCandidates) {
        if (m_map->Open(candidate)) {
            ok = true;
            AppLogger::Log("[RS3_AUDIT] RScene::LoadCharSelect -> Loaded map candidate: " + candidate);
            break;
        }
    }

    if (!ok) {
        AppLogger::Log("[RS3_AUDIT] RScene::LoadCharSelect -> Map FAILED. Reason: " + std::string(m_map->GetLastOpenError()));
    } else {
        AppLogger::Log("[RS3_AUDIT] RScene::LoadCharSelect -> Map Loaded OK.");

        bool hasCam02 = false;
        bool hasCam01 = false;
        DirectX::XMFLOAT3 cam01Pos = {};
        DirectX::XMFLOAT3 cam01Dir = {};

        auto* dummies = m_map->GetDummyList();
        if (dummies) {
            for (const auto& dummy : *dummies) {
                const std::string name = ToLowerCopy(dummy.Name);
                if (name == "camera_pos 02") {
                    m_cameraPos = dummy.Position;
                    m_cameraDir = dummy.Direction;
                    m_hasCameraDummy = true;
                    hasCam02 = true;
                } else if (name == "camera_pos 01") {
                    cam01Pos = dummy.Position;
                    cam01Dir = dummy.Direction;
                    hasCam01 = true;
                } else if (name == "spawn_solo_101") {
                    m_spawnPos = dummy.Position;
                    m_spawnDir = dummy.Direction;
                    m_hasSpawn = true;
                }
            }
        }

        if (!hasCam02 && hasCam01) {
            m_cameraPos = cam01Pos;
            m_cameraDir = cam01Dir;
            m_hasCameraDummy = true;
        }

        if (m_hasCameraDummy) {
            AppLogger::Log("[RS3_AUDIT] RScene::LoadCharSelect -> Camera from dummy: Pos(" +
                std::to_string(m_cameraPos.x) + "," + std::to_string(m_cameraPos.y) + "," + std::to_string(m_cameraPos.z) +
                ") Dir(" + std::to_string(m_cameraDir.x) + "," + std::to_string(m_cameraDir.y) + "," + std::to_string(m_cameraDir.z) + ")");
        }

    }

    m_character = std::make_unique<RSkinObject>(m_pd3dDevice, m_textureManager.get());
    BuildCreationVariants();
    m_previewSex = 0;
    m_previewFace = 0;
    m_previewPreset = 0;
    m_previewVisible = false;
    ApplyCreationPreview();

    if (m_character) {
        ApplyCreationPreview();
    }
}

void RScene::LoadLobbyBasic() {
    m_map.reset();
    m_character.reset();
    m_previewVisible = false;
    m_hasCameraDummy = false;
    m_hasSpawn = false;

    // Camera fixa para cena de lobby basica (sem assets).
    m_cameraPos = { 0.0f, -800.0f, 220.0f };
    m_cameraDir = { 0.0f, 1.0f, -0.2f };

    AppLogger::Log("[RS3_AUDIT] RScene::LoadLobbyBasic -> empty lobby scene configured.");
}

void RScene::Update(float deltaTime) {
    if (m_map) m_map->Update(deltaTime);
    if (m_character) m_character->Update(deltaTime);
}

void RScene::Draw(ID3D11DeviceContext* context, DirectX::FXMMATRIX viewProj) {
    m_stateManager->ClearSRVs();

    // 1. Renderizar Mapa
    m_stateManager->ApplyPass(RenderPass::Map);
    if (m_map) {
        rfrustum dummy; 
        m_map->Draw(context, viewProj, dummy, RenderMode::Baseline); // Baseline para garantir visibilidade inicial
    }

    // 2. Renderizar Personagem
    m_stateManager->ApplyPass(RenderPass::Skin_Base);
    static bool loggedSkinBasePass = false;
    if (!loggedSkinBasePass) {
        AppLogger::Log("[RS3_AUDIT] RScene::Draw -> Skin_Base pass applied.");
        loggedSkinBasePass = true;
    }
    const bool shouldDrawCharacter = (m_character != nullptr) &&
        (m_previewVisible || kForceCharacterDrawOutsidePreview);
    if (shouldDrawCharacter) {
        static bool loggedForcedPreviewBypass = false;
        if (!m_previewVisible && kForceCharacterDrawOutsidePreview && !loggedForcedPreviewBypass) {
            AppLogger::Log("[RS3_AUDIT] RScene::Draw -> character render forced outside preview gate.");
            loggedForcedPreviewBypass = true;
        }
        AppLogger::Log("[RS3_AUDIT] RScene::Draw -> Calling m_character->Draw (ptr=" + std::to_string((uintptr_t)m_character.get()) + ")");
        m_character->Draw(context, viewProj, false);
    } else {
        AppLogger::Log("[RS3_AUDIT] RScene::Draw -> shouldDrawCharacter FALSE. m_character=" + std::to_string((uintptr_t)m_character.get()) + " m_previewVisible=" + std::to_string(m_previewVisible) + " kForce=" + std::to_string(kForceCharacterDrawOutsidePreview));
    }

    m_stateManager->Reset();
}

bool RScene::GetPreferredCamera(DirectX::XMFLOAT3& outPos, DirectX::XMFLOAT3& outDir) const {
    outPos = m_cameraPos;
    outDir = m_cameraDir;
    return m_hasCameraDummy;
}

bool RScene::GetSpawnPos(DirectX::XMFLOAT3& outPos) const {
    if (m_hasSpawn) {
        outPos = m_spawnPos;
        return true;
    }
    return false;
}

void RScene::SetCreationPreview(int sex, int face, int preset, int hair) {
    m_previewSex = sex;
    m_previewFace = face;
    m_previewPreset = preset;
    m_previewHair = hair;
    m_previewVisible = true;
    ApplyCreationPreview();
}

void RScene::SetCreationPreviewVisible(bool visible) {
    m_previewVisible = visible;
}

}
