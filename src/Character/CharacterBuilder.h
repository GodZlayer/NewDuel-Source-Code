#pragma once

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <array>
#include "RenderedCharacter.h"
#include "TextureManager.h"
#include <DirectXMath.h>

namespace Gunz {

// Gênero do personagem
enum class Gender {
    Male = 0,
    Female = 1
};

// Tipo de corpo (afeta scale/volume)
enum class BodyType {
    Thin = 0,
    Average = 1,
    Muscular = 2
};

// Slots de equipamento (categorias de partes legadas)
enum class EquipmentSlot {
    Face = 0,      // eq_face* (rosto)
    Head = 1,      // eq_head* (cabelo)
    Chest = 2,     // eq_chest* (torso superior)
    Hands = 3,     // eq_hands* (luvas/mãos)
    Legs = 4,      // eq_legs* (pernas)
    Feet = 5       // eq_feet* (botas/pés)
};

// Estado do modelo para preview - pode ser serializado para networking
struct ModelState {
    std::string name = "Preview Character";
    Gender gender = Gender::Male;

    // Aparência base
    BodyType body_type = BodyType::Average;
    float skin_tint_r = 1.0f;
    float skin_tint_g = 1.0f;
    float skin_tint_b = 1.0f;
    float skin_tint_a = 1.0f;

    // Seleções de partes (índices nas listas disponíveis)
    int face_idx = -1;   // índice em face options (kFaceMale/Female)
    int hair_idx = -1;   // índice em hair options (kHairMale/Female)

    // Equipamentos (item IDs do servidor/inventário)
    struct Equipment {
        int chest = -1;
        int hands = -1;
        int legs = -1;
        int feet = -1;
    } equipment;

    // Morph targets (opcional)
    std::unordered_map<std::string, float> morph_weights;

    bool IsValid() const {
        return gender != Gender::Male && gender != Gender::Female; // simplificado
        // Na prática: face_idx e hair_idx devem ser válidos para gênero
    }
};

class CharacterBuilder {
public:
    CharacterBuilder(ID3D11Device* device, RealSpace3::TextureManager* texMgr, const std::string& assetBasePath = ".");
    ~CharacterBuilder();

    // Inicialização
    bool Initialize();
    void Shutdown();

    // Carregamento de assets e catálogos
    bool LoadItemMapping();  // Carrega zitem.xml para mapear item_id -> mesh node
    void SetAssetBasePath(const std::string& path) { m_assetBasePath = path; }

    // Consulta de opções disponíveis (para UI)
    std::vector<std::string> GetFaceOptions() const;
    std::vector<std::string> GetHairOptions() const;
    // Equipment: lista de item IDs disponíveis por slot (filtrados por inventário se networking ativo)
    std::vector<int> GetAvailableItemsForSlot(EquipmentSlot slot) const;

    // Setters para customização
    void SetGender(Gender gender);
    void SetBodyType(BodyType type);
    void SetSkinTint(float r, float g, float b, float a = 1.0f);
    void SetFace(int faceIdx);  // índice em face options
    void SetHair(int hairIdx);  // índice em hair options

    // Equipamento
    bool EquipItem(EquipmentSlot slot, int item_id);  // usa mapeamento item_id -> node
    bool UnequipSlot(EquipmentSlot slot);
    void ClearEquipment();

    // Preview control
    void ApplyToScene();  // Reconstrói personagem com state atual (deve ser rápido)
    const ModelState& GetState() const { return m_state; }
    void SetState(const ModelState& state);

    // Transformação do personagem
    void SetWorldPosition(const DirectX::XMFLOAT3& pos);
    void SetWorldYaw(float yawRadians);
    void SetWorldTransform(const DirectX::XMFLOAT3& pos, float yawRadians);

    // Update/ Render
    void Update(float deltaTime);
    void Draw(ID3D11DeviceContext* context, DirectX::FXMMATRIX viewProj);

    // Animation
    void SetAnimation(const std::string& animFile);
    void SetAnimationLoop(bool loop);

    // Debug
    void SetDebugVisible(bool visible) { m_debugVisible = visible; }
    bool IsDebugVisible() const { return m_debugVisible; }

private:
    // Helper interno principal
    void RebuildCharacter();  // Reconstrói a partir de m_state

    // Helpers auxiliares
    void ResetSelections();
    bool ApplyLegacyPart(EquipmentSlot slot, const std::string& nodeName);
    std::string CategoryToLegacyString(EquipmentSlot slot) const;
    std::string GetMeshNameForItemId(int item_id) const;
    std::vector<std::string> GetMaleFaceList() const;
    std::vector<std::string> GetFemaleFaceList() const;
    std::vector<std::string> GetMaleHairList() const;
    std::vector<std::string> GetFemaleHairList() const;
    void LoadBaseModel();
    void AppendPartLibraries();
    void LoadIdleAnimation();
    void ApplyEquipmentFromState();

    // Membros de estado
    ModelState m_state;

    // Objeto de renderização
    std::unique_ptr<RenderedCharacter> m_character;

    // Device/Context (não próprios)
    ID3D11Device* m_device;
    RealSpace3::TextureManager* m_texMgr;

    // Asset paths
    std::string m_assetBasePath;

    // Mapeamento item_id -> mesh node name (carregado de zitem.xml)
    mutable std::unordered_map<int, std::string> m_itemMeshById;
    bool m_itemMappingLoaded = false;

    // Listas fixas de faces/cabelos (hardcoded por enquanto)
    static constexpr std::array<const char*, 20> kFaceMale = {
        "eq_face_01", "eq_face_02", "eq_face_04", "eq_face_05",
        "eq_face_a01", "eq_face_newface01", "eq_face_newface02",
        "eq_face_newface03", "eq_face_newface04", "eq_face_newface05",
        "eq_face_newface06", "eq_face_newface07", "eq_face_newface08",
        "eq_face_newface09", "eq_face_newface10", "eq_face_newface11",
        "eq_face_newface12", "eq_face_newface13", "eq_face_newface13",
        "eq_face_newface13"
    };

    static constexpr std::array<const char*, 20> kFaceFemale = {
        "eq_face_001", "eq_face_002", "eq_face_003", "eq_face_004",
        "eq_face_001", "eq_face_newface01", "eq_face_newface02",
        "eq_face_newface03", "eq_face_newface04", "eq_face_newface05",
        "eq_face_newface06", "eq_face_newface07", "eq_face_newface08",
        "eq_face_newface09", "eq_face_newface10", "eq_face_newface11",
        "eq_face_newface12", "eq_face_newface13", "eq_face_newface14",
        "eq_face_newface15"
    };

    static constexpr std::array<const char*, 5> kHairMale = {
        "eq_head_01", "eq_head_02", "eq_head_08", "eq_head_05", "eq_head_08"
    };

    static constexpr std::array<const char*, 5> kHairFemale = {
        "eq_head_pony", "eq_head_hair001", "eq_head_hair04",
        "eq_head_hair006", "eq_head_hair002"
    };

    // Listas de part libraries (ELU extras com equipamentos)
    static constexpr std::array<const char*, 4> kMalePartLibraries = {
        "Model/man/man-parts02.elu",   // base face/hair/legs
        "Model/man/man-parts03.elu",   // alternate base face/hair
        "Model/man/man-parts12.elu",   // default male chest
        "Model/man/man-parts_face.elu" // newface variants
    };

    static constexpr std::array<const char*, 5> kFemalePartLibraries = {
        "Model/woman/woman-parts02.elu",   // base face
        "Model/woman/woman-parts03.elu",   // base hair
        "Model/woman/woman-parts07.elu",   // default female legs
        "Model/woman/woman-parts11.elu",   // default female chest
        "Model/woman/woman-parts_face.elu" // newface variants
    };

    // Flags
    bool m_initialized = false;
    bool m_dirty = true;
    bool m_debugVisible = true;
};

} // namespace Gunz
