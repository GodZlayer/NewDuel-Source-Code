#include "Character/CharacterBuilder.h"

#include "RenderedCharacter.h"  // Para GetSkinObject
#include "AppLogger.h"

#include <fstream>
#include <regex>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace fs = std::filesystem;

namespace Gunz {

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
} // anonymous namespace

// ==================== PUBLIC ====================

CharacterBuilder::CharacterBuilder(ID3D11Device* device, RealSpace3::TextureManager* texMgr, const std::string& assetBasePath)
    : m_device(device)
    , m_texMgr(texMgr)
    , m_assetBasePath(assetBasePath)
    , m_state()
    , m_character(nullptr)
    , m_itemMappingLoaded(false) {
}

CharacterBuilder::~CharacterBuilder() {
    Shutdown();
}

bool CharacterBuilder::Initialize() {
    if (m_initialized) return true;

    m_character = std::make_unique<RenderedCharacter>(m_device, m_texMgr);
    if (!m_character) {
        AppLogger::Log("[CharacterBuilder] Failed to create RenderedCharacter");
        return false;
    }

    // Estado padrão
    m_state = ModelState();
    m_state.gender = Gender::Male;
    m_state.face_idx = 0;
    m_state.hair_idx = 0;

    // Carrega mapeamento de itens
    if (!LoadItemMapping()) {
        AppLogger::Log("[CharacterBuilder] Warning: Failed to load item mapping (zitem.xml)");
    }

    m_initialized = true;
    AppLogger::Log("[CharacterBuilder] Initialized");
    return true;
}

void CharacterBuilder::Shutdown() {
    m_character.reset();
    m_itemMeshById.clear();
    m_itemMappingLoaded = false;
    m_initialized = false;
}

// ==================== LOAD ITEM MAPPING ====================

// Copiado e adaptado de RScene::EnsureItemMeshMapLoaded
bool CharacterBuilder::LoadItemMapping() {
    if (m_itemMappingLoaded) return true;

    // Procurar zitem.xml em vários locais
    std::vector<std::string> candidates = {
        m_assetBasePath + "/system/zitem.xml",
        m_assetBasePath + "/system/zitem_cleaned.xml",
        m_assetBasePath + "/zitem.xml",
        m_assetBasePath + "/zitem_cleaned.xml",
        // Caminhos absolutos comuns
        "system/zitem.xml",
        "system/zitem_cleaned.xml",
        "ogz-client-master/system/zitem.xml",
        "ogz-client-master/system/zitem_cleaned.xml",
        "OpenGunZ-Client/system/zitem.xml",
        "OpenGunZ-Client/system/zitem_cleaned.xml"
    };

    std::string sourcePath;
    for (const auto& path : candidates) {
        if (fs::exists(path)) {
            sourcePath = path;
            break;
        }
    }

    if (sourcePath.empty()) {
        AppLogger::Log("[CharacterBuilder] LoadItemMapping -> zitem.xml not found");
        return false;
    }

    std::ifstream file(sourcePath);
    if (!file.is_open()) {
        AppLogger::Log("[CharacterBuilder] LoadItemMapping -> failed to open: " + sourcePath);
        return false;
    }

    // Ler XML inteiro
    std::string xml((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    const std::regex itemTagRe("<[^>]*ITEM\\b[^>]*>");
    const std::regex idRe("\\bid\\s*=\\s*\"([0-9]+)\"");
    const std::regex meshRe("\\bmesh_name\\s*=\\s*\"([^\"]+)\"");

    m_itemMeshById.clear();
    for (std::sregex_iterator it(xml.begin(), xml.end(), itemTagRe), end; it != end; ++it) {
        const std::string tag = it->str();
        std::smatch idMatch, meshMatch;
        if (!std::regex_search(tag, idMatch, idRe) || !std::regex_search(tag, meshMatch, meshRe)) continue;
        if (idMatch.size() < 2 || meshMatch.size() < 2) continue;

        uint32_t id = 0;
        std::istringstream iss(idMatch[1].str());
        iss >> id;
        if (!iss || id == 0) continue;

        std::string mesh = meshMatch[1].str();
        if (mesh.empty()) continue;

        m_itemMeshById[static_cast<int>(id)] = mesh;
    }

    m_itemMappingLoaded = true;
    AppLogger::Log("[CharacterBuilder] LoadItemMapping -> loaded " + std::to_string(m_itemMeshById.size()) + " entries");
    return true;
}

// ==================== GETTERS FOR UI ====================

std::vector<std::string> CharacterBuilder::GetFaceOptions() const {
    if (m_state.gender == Gender::Female) {
        return { kFaceFemale.begin(), kFaceFemale.end() };
    }
    return { kFaceMale.begin(), kFaceMale.end() };
}

std::vector<std::string> CharacterBuilder::GetHairOptions() const {
    if (m_state.gender == Gender::Female) {
        return { kHairFemale.begin(), kHairFemale.end() };
    }
    return { kHairMale.begin(), kHairMale.end() };
}

std::vector<int> CharacterBuilder::GetAvailableItemsForSlot(EquipmentSlot slot) const {
    // Por enquanto retorna todos IDs conhecidos (futuro: filtrar por inventário e por slot)
    std::vector<int> ids;
    for (const auto& kv : m_itemMeshById) {
        ids.push_back(kv.first);
    }
    return ids;
}

// ==================== SETTERS ====================

void CharacterBuilder::SetGender(Gender gender) {
    if (m_state.gender == gender) return;
    m_state.gender = gender;
    // Reset choices para padrões do novo gênero
    m_state.face_idx = 0;
    m_state.hair_idx = 0;
    ClearEquipment();
    m_dirty = true;
}

void CharacterBuilder::SetBodyType(BodyType type) {
    m_state.body_type = type;
    //TODO: Implementar escala de bones conforme body_type
    m_dirty = true;
}

void CharacterBuilder::SetSkinTint(float r, float g, float b, float a) {
    m_state.skin_tint_r = r;
    m_state.skin_tint_g = g;
    m_state.skin_tint_b = b;
    m_state.skin_tint_a = a;
    m_dirty = true;
}

void CharacterBuilder::SetFace(int faceIdx) {
    auto faces = GetFaceOptions();
    int wrapped = WrapIndex(faceIdx, static_cast<int>(faces.size()));
    if (wrapped < 0) wrapped = 0;
    m_state.face_idx = wrapped;
    m_dirty = true;
}

void CharacterBuilder::SetHair(int hairIdx) {
    auto hairs = GetHairOptions();
    int wrapped = WrapIndex(hairIdx, static_cast<int>(hairs.size()));
    if (wrapped < 0) wrapped = 0;
    m_state.hair_idx = wrapped;
    m_dirty = true;
}

bool CharacterBuilder::EquipItem(EquipmentSlot slot, int item_id) {
    if (item_id < 0) return false;
    switch (slot) {
        case EquipmentSlot::Chest: m_state.equipment.chest = item_id; break;
        case EquipmentSlot::Hands: m_state.equipment.hands = item_id; break;
        case EquipmentSlot::Legs: m_state.equipment.legs = item_id; break;
        case EquipmentSlot::Feet: m_state.equipment.feet = item_id; break;
        default:
            // Face e Head usam SetFace/SetHair
            return false;
    }
    m_dirty = true;
    return true;
}

bool CharacterBuilder::UnequipSlot(EquipmentSlot slot) {
    switch (slot) {
        case EquipmentSlot::Chest: m_state.equipment.chest = -1; break;
        case EquipmentSlot::Hands: m_state.equipment.hands = -1; break;
        case EquipmentSlot::Legs: m_state.equipment.legs = -1; break;
        case EquipmentSlot::Feet: m_state.equipment.feet = -1; break;
        default: return false;
    }
    m_dirty = true;
    return true;
}

void CharacterBuilder::ClearEquipment() {
    m_state.equipment = ModelState::Equipment();
    m_dirty = true;
}

// ==================== PREVIEW CONTROL ====================

void CharacterBuilder::ApplyToScene() {
    if (!m_initialized) {
        if (!Initialize()) {
            AppLogger::Log("[CharacterBuilder] ApplyToScene: Falha na inicialização");
            return;
        }
    }

    if (!m_dirty) return;

    AppLogger::Log("[CharacterBuilder] ApplyToScene: Reconstruindo personagem");
    RebuildCharacter();

    m_dirty = false;
}

void CharacterBuilder::SetState(const ModelState& state) {
    m_state = state;
    m_dirty = true;
    ApplyToScene();
}

// ==================== TRANSFORM ====================

void CharacterBuilder::SetWorldPosition(const DirectX::XMFLOAT3& pos) {
    if (m_character) {
        m_character->SetWorldPosition(pos);
    }
}

void CharacterBuilder::SetWorldYaw(float yawRadians) {
    if (m_character) {
        m_character->SetWorldYaw(yawRadians);
    }
}

// ==================== UPDATE / DRAW ====================

void CharacterBuilder::Update(float deltaTime) {
    if (m_character) {
        m_character->Update(deltaTime);
    }
}

void CharacterBuilder::Draw(ID3D11DeviceContext* context, DirectX::FXMMATRIX viewProj) {
    if (m_character) {
        m_character->Draw(context, viewProj);
    }
}

void CharacterBuilder::SetAnimation(const std::string& animFile) {
    // TODO: Implementar mudança de animação via RSkinObject
    AppLogger::Log("[CharacterBuilder] SetAnimation: não implementado");
}

void CharacterBuilder::SetAnimationLoop(bool loop) {
    // TODO
}

// ==================== PRIVATE HELPERS ====================

void CharacterBuilder::ResetSelections() {
    if (m_character) {
        if (RealSpace3::RSkinObject* skin = m_character->GetSkinObject()) {
            skin->ResetLegacyPartSelection();
        }
    }
}

bool CharacterBuilder::ApplyLegacyPart(EquipmentSlot slot, const std::string& nodeName) {
    if (nodeName.empty() || !m_character) return false;
    RealSpace3::RSkinObject* skin = m_character->GetSkinObject();
    if (!skin) return false;
    std::string category = CategoryToLegacyString(slot);
    if (category.empty()) return false;
    return skin->SetLegacyPart(category, nodeName);
}

std::string CharacterBuilder::CategoryToLegacyString(EquipmentSlot slot) const {
    switch (slot) {
        case EquipmentSlot::Face: return "face";
        case EquipmentSlot::Head: return "head";
        case EquipmentSlot::Chest: return "chest";
        case EquipmentSlot::Hands: return "hands";
        case EquipmentSlot::Legs: return "legs";
        case EquipmentSlot::Feet: return "feet";
        default: return "";
    }
}

std::string CharacterBuilder::GetMeshNameForItemId(int item_id) const {
    if (!m_itemMappingLoaded) return {};
    auto it = m_itemMeshById.find(item_id);
    if (it == m_itemMeshById.end()) return {};
    return it->second;
}

void CharacterBuilder::LoadBaseModel() {
    if (!m_character) return;
    bool female = (m_state.gender == Gender::Female);
    if (!m_character->LoadCharacter(female)) {
        AppLogger::Log("[CharacterBuilder] LoadBaseModel falhou (female=" + std::to_string(female) + ")");
    }
}

void CharacterBuilder::AppendPartLibraries() {
    if (!m_character) return;
    RealSpace3::RSkinObject* skin = m_character->GetSkinObject();
    if (!skin) return;

    bool female = (m_state.gender == Gender::Female);
    const auto* libs = female ? kFemalePartLibraries.data() : kMalePartLibraries.data();
    size_t count = female ? kFemalePartLibraries.size() : kMalePartLibraries.size();

    for (size_t i = 0; i < count; ++i) {
        std::string path = m_assetBasePath + "/" + libs[i];
        std::replace(path.begin(), path.end(), '\\', '/');

        if (!fs::exists(path)) {
            AppLogger::Log("[CharacterBuilder] Part library não encontrado: " + path);
            continue;
        }

        if (!skin->AppendLegacyPartsFromElu5007(path)) {
            AppLogger::Log("[CharacterBuilder] Falha ao anexar part library: " + path);
        }
    }
}

void CharacterBuilder::ApplyEquipmentFromState() {
    if (!m_character) return;
    RealSpace3::RSkinObject* skin = m_character->GetSkinObject();
    if (!skin) return;

    // Face
    if (m_state.face_idx >= 0) {
        const auto& faces = (m_state.gender == Gender::Female) ? kFaceFemale : kFaceMale;
        int idx = WrapIndex(m_state.face_idx, static_cast<int>(faces.size()));
        if (idx >= 0 && static_cast<size_t>(idx) < faces.size()) {
            std::string node = faces[static_cast<size_t>(idx)];
            skin->SetLegacyPart("face", node);
        }
    }

    // Hair (Head slot)
    if (m_state.hair_idx >= 0) {
        const auto& hairs = (m_state.gender == Gender::Female) ? kHairFemale : kHairMale;
        int idx = WrapIndex(m_state.hair_idx, static_cast<int>(hairs.size()));
        if (idx >= 0 && static_cast<size_t>(idx) < hairs.size()) {
            std::string node = hairs[static_cast<size_t>(idx)];
            skin->SetLegacyPart("head", node);
        }
    }

    // Equipment slots via item IDs
    auto equip = [&](EquipmentSlot slot, int item_id) {
        if (item_id < 0) return;
        std::string mesh = GetMeshNameForItemId(item_id);
        if (mesh.empty()) return;
        std::string cat = CategoryToLegacyString(slot);
        if (cat.empty()) return;
        skin->SetLegacyPart(cat, mesh);
    };

    equip(EquipmentSlot::Chest, m_state.equipment.chest);
    equip(EquipmentSlot::Hands, m_state.equipment.hands);
    equip(EquipmentSlot::Legs, m_state.equipment.legs);
    equip(EquipmentSlot::Feet, m_state.equipment.feet);
}

// ==================== REBUILD CHARACTER ====================

void CharacterBuilder::RebuildCharacter() {
    // Garante inicialização
    if (!m_initialized) {
        if (!Initialize()) return;
    }

    if (!m_character) {
        AppLogger::Log("[CharacterBuilder] RebuildCharacter: m_character null");
        return;
    }

    RealSpace3::RSkinObject* skin = m_character->GetSkinObject();
    if (!skin) {
        AppLogger::Log("[CharacterBuilder] RebuildCharacter: GetSkinObject null");
        return;
    }

    // 1. Reset seleções atuais
    ResetSelections();

    // 2. Carrega modelo base (homem ou mulher)
    LoadBaseModel();

    // 3. Anexa bibliotecas de partes (part libraries)
    AppendPartLibraries();

    // 4. Aplica face, cabelo e equipamentos
    ApplyEquipmentFromState();

    // Nota: A animação já foi carregada por LoadCharacter. Transformações
    // de posição/yaw devem ser setadas externamente após rebuild.
    AppLogger::Log("[CharacterBuilder] RebuildCharacter concluído");
}

} // namespace Gunz
