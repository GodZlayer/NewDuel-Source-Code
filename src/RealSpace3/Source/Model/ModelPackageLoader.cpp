#include "../../Include/Model/ModelPackageLoader.h"
#include "AppLogger.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

namespace RealSpace3 {
namespace {

namespace fs = std::filesystem;

class BinReader {
public:
    explicit BinReader(const std::vector<uint8_t>& data)
        : m_data(data) {
    }

    size_t Remaining() const {
        return (m_off <= m_data.size()) ? (m_data.size() - m_off) : 0;
    }

    bool ReadBytes(void* dst, size_t size) {
        if (Remaining() < size) return false;
        std::memcpy(dst, m_data.data() + m_off, size);
        m_off += size;
        return true;
    }

    bool ReadU16(uint16_t& outValue) {
        return ReadBytes(&outValue, sizeof(outValue));
    }

    bool ReadI32(int32_t& outValue) {
        return ReadBytes(&outValue, sizeof(outValue));
    }

    bool ReadU32(uint32_t& outValue) {
        return ReadBytes(&outValue, sizeof(outValue));
    }

    bool ReadF32(float& outValue) {
        return ReadBytes(&outValue, sizeof(outValue));
    }

    bool ReadString(std::string& outValue) {
        uint32_t len = 0;
        if (!ReadU32(len)) return false;
        if (Remaining() < len) return false;

        outValue.clear();
        if (len == 0) return true;

        outValue.assign(
            reinterpret_cast<const char*>(m_data.data() + m_off),
            reinterpret_cast<const char*>(m_data.data() + m_off + len));
        m_off += len;
        return true;
    }

private:
    const std::vector<uint8_t>& m_data;
    size_t m_off = 0;
};

bool ReadFileBytes(const fs::path& filePath, std::vector<uint8_t>& outBytes) {
    std::ifstream in(filePath, std::ios::binary);
    if (!in.is_open()) return false;

    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0) return false;
    in.seekg(0, std::ios::beg);

    outBytes.resize(static_cast<size_t>(size));
    if (!outBytes.empty()) {
        in.read(reinterpret_cast<char*>(outBytes.data()), size);
        if (!in.good() && !in.eof()) return false;
    }

    return true;
}

bool ReadTextFile(const fs::path& filePath, std::string& outText) {
    std::ifstream in(filePath, std::ios::binary);
    if (!in.is_open()) return false;

    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!text.empty() && static_cast<unsigned char>(text[0]) == 0xEF && text.size() >= 3) {
        if (static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
            text = text.substr(3);
        }
    }

    outText = std::move(text);
    return true;
}

void SetError(std::string* outError, const std::string& msg) {
    if (outError) {
        *outError = msg;
    }
}

float TranslationMagnitudeRowMajor(const DirectX::XMFLOAT4X4& m) {
    return std::fabs(m._41) + std::fabs(m._42) + std::fabs(m._43);
}

float TranslationMagnitudeColumnMajor(const DirectX::XMFLOAT4X4& m) {
    return std::fabs(m._14) + std::fabs(m._24) + std::fabs(m._34);
}

bool LooksLikeColumnMajorMatrix(const DirectX::XMFLOAT4X4& m) {
    const float rowT = TranslationMagnitudeRowMajor(m);
    const float colT = TranslationMagnitudeColumnMajor(m);
    if (colT <= 0.0001f) {
        return false;
    }
    return rowT <= (colT * 0.35f);
}

void TransposeMatrixInPlace(DirectX::XMFLOAT4X4& matrix) {
    const DirectX::XMMATRIX xm = DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&matrix));
    DirectX::XMStoreFloat4x4(&matrix, xm);
}

enum class BindHierarchyMode {
    LocalFirst,
    ParentFirst,
    Global
};

DirectX::XMFLOAT4X4 MultiplyMatrices(const DirectX::XMFLOAT4X4& a, const DirectX::XMFLOAT4X4& b) {
    DirectX::XMFLOAT4X4 out{};
    const DirectX::XMMATRIX xm = DirectX::XMMatrixMultiply(DirectX::XMLoadFloat4x4(&a), DirectX::XMLoadFloat4x4(&b));
    DirectX::XMStoreFloat4x4(&out, xm);
    return out;
}

DirectX::XMFLOAT4X4 InverseMatrix(const DirectX::XMFLOAT4X4& m) {
    DirectX::XMFLOAT4X4 out{};
    DirectX::XMVECTOR det{};
    const DirectX::XMMATRIX inv = DirectX::XMMatrixInverse(&det, DirectX::XMLoadFloat4x4(&m));
    DirectX::XMStoreFloat4x4(&out, inv);
    return out;
}

float MatrixDistanceToIdentity(const DirectX::XMFLOAT4X4& m) {
    const DirectX::XMMATRIX xm = DirectX::XMLoadFloat4x4(&m);
    const DirectX::XMMATRIX id = DirectX::XMMatrixIdentity();

    float error = 0.0f;
    for (int r = 0; r < 4; ++r) {
        const DirectX::XMVECTOR diff = DirectX::XMVectorAbs(DirectX::XMVectorSubtract(xm.r[r], id.r[r]));
        DirectX::XMFLOAT4 row{};
        DirectX::XMStoreFloat4(&row, diff);
        error += row.x + row.y + row.z + row.w;
    }
    return error;
}

void BuildGlobalBindMatrices(const std::vector<RS3Bone>& bones, BindHierarchyMode mode, std::vector<DirectX::XMFLOAT4X4>& outGlobal) {
    outGlobal.assign(bones.size(), DirectX::XMFLOAT4X4(
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1));

    if (mode == BindHierarchyMode::Global) {
        for (size_t i = 0; i < bones.size(); ++i) {
            outGlobal[i] = bones[i].bind;
        }
        return;
    }

    std::vector<uint8_t> state(bones.size(), 0); // 0=unvisited,1=visiting,2=done
    auto eval = [&](auto&& self, size_t idx) -> void {
        if (state[idx] == 2) return;
        if (state[idx] == 1) {
            outGlobal[idx] = bones[idx].bind;
            state[idx] = 2;
            return;
        }

        state[idx] = 1;
        const int32_t parent = bones[idx].parentBone;
        if (parent >= 0 && static_cast<size_t>(parent) < bones.size()) {
            self(self, static_cast<size_t>(parent));
            outGlobal[idx] = (mode == BindHierarchyMode::LocalFirst)
                ? MultiplyMatrices(bones[idx].bind, outGlobal[static_cast<size_t>(parent)])
                : MultiplyMatrices(outGlobal[static_cast<size_t>(parent)], bones[idx].bind);
        } else {
            outGlobal[idx] = bones[idx].bind;
        }
        state[idx] = 2;
    };

    for (size_t i = 0; i < bones.size(); ++i) {
        eval(eval, i);
    }
}

float ComputeSkinIdentityError(const std::vector<RS3Bone>& bones, BindHierarchyMode mode) {
    if (bones.empty()) return 0.0f;

    std::vector<DirectX::XMFLOAT4X4> global;
    BuildGlobalBindMatrices(bones, mode, global);

    float error = 0.0f;
    for (size_t i = 0; i < bones.size(); ++i) {
        const DirectX::XMFLOAT4X4 skin = MultiplyMatrices(global[i], bones[i].invBind);
        error += MatrixDistanceToIdentity(skin);
    }
    return error / static_cast<float>(bones.size());
}

void ConvertGlobalBindToLocal(std::vector<RS3Bone>& bones) {
    if (bones.empty()) return;

    std::vector<DirectX::XMFLOAT4X4> global(bones.size());
    for (size_t i = 0; i < bones.size(); ++i) {
        global[i] = bones[i].bind;
    }

    for (size_t i = 0; i < bones.size(); ++i) {
        const int32_t parent = bones[i].parentBone;
        if (parent >= 0 && static_cast<size_t>(parent) < bones.size()) {
            const DirectX::XMFLOAT4X4 parentInv = InverseMatrix(global[static_cast<size_t>(parent)]);
            bones[i].bind = MultiplyMatrices(global[i], parentInv);
        } else {
            bones[i].bind = global[i];
        }
    }
}

bool ResolveModelDir(const std::string& modelId, fs::path& outDir) {
    const fs::path cwd = fs::current_path();
    const fs::path modelRel = fs::path(modelId);

    const std::vector<fs::path> candidates = {
        cwd / "system" / "rs3" / "models" / modelRel,
        cwd / "OpenGunZ-Client" / "system" / "rs3" / "models" / modelRel,
        cwd / ".." / "OpenGunZ-Client" / "system" / "rs3" / "models" / modelRel,
        cwd / ".." / ".." / "OpenGunZ-Client" / "system" / "rs3" / "models" / modelRel,
    };

    for (const auto& c : candidates) {
        std::error_code ec;
        if (!fs::exists(c, ec) || !fs::is_directory(c, ec)) continue;
        const fs::path modelJson = c / "model.json";
        if (fs::exists(modelJson, ec) && fs::is_regular_file(modelJson, ec)) {
            outDir = fs::weakly_canonical(c, ec);
            if (ec) outDir = c;
            return true;
        }
    }

    return false;
}

bool ExtractJsonString(const std::string& text, const std::string& key, std::string& outValue) {
    const std::regex re("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch m;
    if (!std::regex_search(text, m, re)) {
        return false;
    }

    if (m.size() < 2) return false;
    outValue = m[1].str();
    return true;
}

bool LoadMesh(const fs::path& filePath, RS3ModelPackage& outPackage, std::string* outError) {
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(filePath, bytes)) {
        SetError(outError, "Failed to read mesh.bin");
        return false;
    }

    BinReader r(bytes);

    std::array<uint8_t, 8> magic{};
    if (!r.ReadBytes(magic.data(), magic.size())) {
        SetError(outError, "mesh.bin is truncated (magic)");
        return false;
    }

    const std::array<uint8_t, 8> expected = { 0x52, 0x53, 0x33, 0x4D, 0x53, 0x48, 0x31, 0x00 }; // RS3MSH1\0
    if (magic != expected) {
        SetError(outError, "mesh.bin magic mismatch");
        return false;
    }

    uint32_t version = 0;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t submeshCount = 0;
    uint32_t hasSkin = 0;

    if (!r.ReadU32(version) || !r.ReadU32(vertexCount) || !r.ReadU32(indexCount)
        || !r.ReadU32(submeshCount) || !r.ReadU32(hasSkin)) {
        SetError(outError, "mesh.bin is truncated (header)");
        return false;
    }

    if (version != 1 && version != 2) {
        SetError(outError, "mesh.bin version mismatch");
        return false;
    }

    outPackage.vertices.clear();
    outPackage.vertices.resize(vertexCount);

    for (uint32_t i = 0; i < vertexCount; ++i) {
        auto& v = outPackage.vertices[i];

        if (!r.ReadF32(v.pos.x) || !r.ReadF32(v.pos.y) || !r.ReadF32(v.pos.z)
            || !r.ReadF32(v.normal.x) || !r.ReadF32(v.normal.y) || !r.ReadF32(v.normal.z)
            || !r.ReadF32(v.uv.x) || !r.ReadF32(v.uv.y)
            || !r.ReadU16(v.joints[0]) || !r.ReadU16(v.joints[1]) || !r.ReadU16(v.joints[2]) || !r.ReadU16(v.joints[3])
            || !r.ReadF32(v.weights[0]) || !r.ReadF32(v.weights[1]) || !r.ReadF32(v.weights[2]) || !r.ReadF32(v.weights[3])) {
            SetError(outError, "mesh.bin is truncated (vertices)");
            return false;
        }
    }

    outPackage.indices.clear();
    outPackage.indices.resize(indexCount);
    for (uint32_t i = 0; i < indexCount; ++i) {
        if (!r.ReadU32(outPackage.indices[i])) {
            SetError(outError, "mesh.bin is truncated (indices)");
            return false;
        }
        if (outPackage.indices[i] >= outPackage.vertices.size()) {
            SetError(outError, "mesh.bin has out-of-range vertex index");
            return false;
        }
    }

    outPackage.submeshes.clear();
    outPackage.submeshes.resize(submeshCount);
    for (uint32_t i = 0; i < submeshCount; ++i) {
        auto& s = outPackage.submeshes[i];
        if (!r.ReadU32(s.materialIndex) || !r.ReadU32(s.nodeIndex) || !r.ReadU32(s.indexStart) || !r.ReadU32(s.indexCount)) {
            SetError(outError, "mesh.bin is truncated (submeshes)");
            return false;
        }

        if (version >= 2) {
            for (int m = 0; m < 16; ++m) {
                if (!r.ReadF32(reinterpret_cast<float*>(&s.nodeTransform)[m])) {
                    SetError(outError, "mesh.bin is truncated (submesh node transform)");
                    return false;
                }
            }
        } else {
            s.nodeTransform = DirectX::XMFLOAT4X4(
                1, 0, 0, 0,
                0, 1, 0, 0,
                0, 0, 1, 0,
                0, 0, 0, 1);
        }

        const uint64_t end = static_cast<uint64_t>(s.indexStart) + static_cast<uint64_t>(s.indexCount);
        if (end > outPackage.indices.size()) {
            SetError(outError, "mesh.bin submesh range invalid");
            return false;
        }
    }

    (void)hasSkin;
    return true;
}

bool LoadSkeleton(const fs::path& filePath, RS3ModelPackage& outPackage, std::string* outError, size_t* outNormalizedBones = nullptr, bool* outConvertedGlobalToLocal = nullptr) {
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(filePath, bytes)) {
        SetError(outError, "Failed to read skeleton.bin");
        return false;
    }

    BinReader r(bytes);

    std::array<uint8_t, 8> magic{};
    if (!r.ReadBytes(magic.data(), magic.size())) {
        SetError(outError, "skeleton.bin is truncated (magic)");
        return false;
    }

    const std::array<uint8_t, 8> expected = { 0x52, 0x53, 0x33, 0x53, 0x4B, 0x4E, 0x31, 0x00 }; // RS3SKN1\0
    if (magic != expected) {
        SetError(outError, "skeleton.bin magic mismatch");
        return false;
    }

    uint32_t version = 0;
    uint32_t boneCount = 0;

    if (!r.ReadU32(version) || !r.ReadU32(boneCount)) {
        SetError(outError, "skeleton.bin is truncated (header)");
        return false;
    }

    if (version != 1) {
        SetError(outError, "skeleton.bin version mismatch");
        return false;
    }

    outPackage.bones.clear();
    outPackage.bones.resize(boneCount);

    for (uint32_t i = 0; i < boneCount; ++i) {
        auto& b = outPackage.bones[i];

        if (!r.ReadI32(b.parentBone) || !r.ReadString(b.name)) {
            SetError(outError, "skeleton.bin is truncated (bone header)");
            return false;
        }

        for (int m = 0; m < 16; ++m) {
            if (!r.ReadF32(reinterpret_cast<float*>(&b.bind)[m])) {
                SetError(outError, "skeleton.bin is truncated (bind matrix)");
                return false;
            }
        }

        for (int m = 0; m < 16; ++m) {
            if (!r.ReadF32(reinterpret_cast<float*>(&b.invBind)[m])) {
                SetError(outError, "skeleton.bin is truncated (inverse bind matrix)");
                return false;
            }
        }
    }

    size_t normalizedBones = 0;
    for (auto& bone : outPackage.bones) {
        if (LooksLikeColumnMajorMatrix(bone.bind) || LooksLikeColumnMajorMatrix(bone.invBind)) {
            TransposeMatrixInPlace(bone.bind);
            TransposeMatrixInPlace(bone.invBind);
            ++normalizedBones;
        }
    }

    if (outNormalizedBones) {
        *outNormalizedBones = normalizedBones;
    }

    const float errGlobal = ComputeSkinIdentityError(outPackage.bones, BindHierarchyMode::Global);
    const float errLocalFirst = ComputeSkinIdentityError(outPackage.bones, BindHierarchyMode::LocalFirst);
    const float errParentFirst = ComputeSkinIdentityError(outPackage.bones, BindHierarchyMode::ParentFirst);
    const float errHier = std::min(errLocalFirst, errParentFirst);

    bool convertedGlobalToLocal = false;
    if (errGlobal < (errHier * 0.25f)) {
        ConvertGlobalBindToLocal(outPackage.bones);
        convertedGlobalToLocal = true;
    }

    if (outConvertedGlobalToLocal) {
        *outConvertedGlobalToLocal = convertedGlobalToLocal;
    }

    return true;
}

bool LoadAnimation(const fs::path& filePath, RS3ModelPackage& outPackage, std::string* outError) {
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(filePath, bytes)) {
        SetError(outError, "Failed to read anim.bin");
        return false;
    }

    BinReader r(bytes);

    std::array<uint8_t, 8> magic{};
    if (!r.ReadBytes(magic.data(), magic.size())) {
        SetError(outError, "anim.bin is truncated (magic)");
        return false;
    }

    const std::array<uint8_t, 8> expected = { 0x52, 0x53, 0x33, 0x41, 0x4E, 0x49, 0x31, 0x00 }; // RS3ANI1\0
    if (magic != expected) {
        SetError(outError, "anim.bin magic mismatch");
        return false;
    }

    uint32_t version = 0;
    uint32_t clipCount = 0;

    if (!r.ReadU32(version) || !r.ReadU32(clipCount)) {
        SetError(outError, "anim.bin is truncated (header)");
        return false;
    }

    if (version != 1) {
        SetError(outError, "anim.bin version mismatch");
        return false;
    }

    outPackage.clips.clear();
    outPackage.clips.resize(clipCount);

    for (uint32_t c = 0; c < clipCount; ++c) {
        auto& clip = outPackage.clips[c];

        uint32_t channelCount = 0;
        if (!r.ReadString(clip.name) || !r.ReadU32(channelCount)) {
            SetError(outError, "anim.bin is truncated (clip header)");
            return false;
        }

        clip.channels.clear();
        clip.channels.resize(channelCount);

        for (uint32_t ch = 0; ch < channelCount; ++ch) {
            auto& channel = clip.channels[ch];

            uint32_t posCount = 0;
            uint32_t rotCount = 0;

            if (!r.ReadI32(channel.boneIndex) || !r.ReadU32(posCount)) {
                SetError(outError, "anim.bin is truncated (channel header)");
                return false;
            }

            channel.posKeys.clear();
            channel.posKeys.resize(posCount);
            for (uint32_t p = 0; p < posCount; ++p) {
                auto& key = channel.posKeys[p];
                if (!r.ReadF32(key.time) || !r.ReadF32(key.value.x) || !r.ReadF32(key.value.y) || !r.ReadF32(key.value.z)) {
                    SetError(outError, "anim.bin is truncated (position keys)");
                    return false;
                }
            }

            if (!r.ReadU32(rotCount)) {
                SetError(outError, "anim.bin is truncated (rotation count)");
                return false;
            }

            channel.rotKeys.clear();
            channel.rotKeys.resize(rotCount);
            for (uint32_t q = 0; q < rotCount; ++q) {
                auto& key = channel.rotKeys[q];
                if (!r.ReadF32(key.time)
                    || !r.ReadF32(key.value.x) || !r.ReadF32(key.value.y) || !r.ReadF32(key.value.z) || !r.ReadF32(key.value.w)) {
                    SetError(outError, "anim.bin is truncated (rotation keys)");
                    return false;
                }
            }
        }
    }

    return true;
}

bool LoadMaterials(const fs::path& filePath, RS3ModelPackage& outPackage, std::string* outError) {
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(filePath, bytes)) {
        SetError(outError, "Failed to read materials.bin");
        return false;
    }

    BinReader r(bytes);

    std::array<uint8_t, 8> magic{};
    if (!r.ReadBytes(magic.data(), magic.size())) {
        SetError(outError, "materials.bin is truncated (magic)");
        return false;
    }

    const std::array<uint8_t, 8> expected = { 0x52, 0x53, 0x33, 0x4D, 0x41, 0x54, 0x31, 0x00 }; // RS3MAT1\0
    if (magic != expected) {
        SetError(outError, "materials.bin magic mismatch");
        return false;
    }

    uint32_t version = 0;
    uint32_t materialCount = 0;

    if (!r.ReadU32(version) || !r.ReadU32(materialCount)) {
        SetError(outError, "materials.bin is truncated (header)");
        return false;
    }

    if (version != 1) {
        SetError(outError, "materials.bin version mismatch");
        return false;
    }

    outPackage.materials.clear();
    outPackage.materials.resize(materialCount);

    for (uint32_t i = 0; i < materialCount; ++i) {
        auto& m = outPackage.materials[i];
        if (!r.ReadU32(m.legacyFlags) || !r.ReadU32(m.alphaMode)
            || !r.ReadF32(m.metallic) || !r.ReadF32(m.roughness)
            || !r.ReadString(m.baseColorTexture) || !r.ReadString(m.normalTexture)
            || !r.ReadString(m.ormTexture) || !r.ReadString(m.emissiveTexture)
            || !r.ReadString(m.opacityTexture)) {
            SetError(outError, "materials.bin is truncated (materials)");
            return false;
        }
    }

    return true;
}

bool LoadAttachments(const fs::path& filePath, RS3ModelPackage& outPackage, std::string* outError) {
    outPackage.sockets.clear();

    std::error_code ec;
    if (!fs::exists(filePath, ec) || !fs::is_regular_file(filePath, ec)) {
        return true;
    }

    std::string text;
    if (!ReadTextFile(filePath, text)) {
        SetError(outError, "Failed to read attachments.json");
        return false;
    }

    const std::regex socketRe("\\{\\s*\"name\"\\s*:\\s*\"([^\"]+)\"\\s*,\\s*\"nodeIndex\"\\s*:\\s*(-?[0-9]+)");
    auto it = std::sregex_iterator(text.begin(), text.end(), socketRe);
    const auto end = std::sregex_iterator();

    for (; it != end; ++it) {
        RS3AttachmentSocket socket;
        socket.name = (*it)[1].str();
        socket.nodeIndex = std::stoi((*it)[2].str());
        outPackage.sockets.push_back(std::move(socket));
    }

    return true;
}

} // namespace

bool ModelPackageLoader::LoadModelPackage(const std::string& modelId, RS3ModelPackage& outPackage, std::string* outError) {
    outPackage = RS3ModelPackage{};
    outPackage.modelId = modelId;

    fs::path modelDir;
    if (!ResolveModelDir(modelId, modelDir)) {
        SetError(outError, "Model package directory not found for modelId='" + modelId + "'.");
        return false;
    }

    outPackage.baseDir = modelDir;

    const fs::path modelJsonPath = modelDir / "model.json";
    std::string modelJsonText;
    if (ReadTextFile(modelJsonPath, modelJsonText)) {
        std::string sourceGlb;
        if (ExtractJsonString(modelJsonText, "sourceGlb", sourceGlb)) {
            outPackage.sourceGlb = sourceGlb;
        }
    }

    std::string meshFile = "mesh.bin";
    std::string skeletonFile = "skeleton.bin";
    std::string animFile = "anim.bin";
    std::string materialsFile = "materials.bin";
    std::string attachmentsFile = "attachments.json";

    if (!modelJsonText.empty()) {
        std::string value;
        if (ExtractJsonString(modelJsonText, "mesh", value)) meshFile = value;
        if (ExtractJsonString(modelJsonText, "skeleton", value)) skeletonFile = value;
        if (ExtractJsonString(modelJsonText, "animation", value)) animFile = value;
        if (ExtractJsonString(modelJsonText, "materials", value)) materialsFile = value;
        if (ExtractJsonString(modelJsonText, "attachments", value)) attachmentsFile = value;
    }

    const fs::path meshPath = modelDir / fs::path(meshFile);
    const fs::path skeletonPath = modelDir / fs::path(skeletonFile);
    const fs::path animPath = modelDir / fs::path(animFile);
    const fs::path materialsPath = modelDir / fs::path(materialsFile);
    const fs::path attachmentsPath = modelDir / fs::path(attachmentsFile);

    if (!LoadMesh(meshPath, outPackage, outError)) return false;
    size_t normalizedBones = 0;
    bool convertedGlobalToLocal = false;
    if (!LoadSkeleton(skeletonPath, outPackage, outError, &normalizedBones, &convertedGlobalToLocal)) return false;
    if (!LoadAnimation(animPath, outPackage, outError)) return false;
    if (!LoadMaterials(materialsPath, outPackage, outError)) return false;
    if (!LoadAttachments(attachmentsPath, outPackage, outError)) return false;

    if (normalizedBones > 0) {
        AppLogger::Log("[RS3] ModelPackageLoader: normalized column-major skeleton matrices to row-major for modelId='" +
            modelId + "' bones=" + std::to_string(normalizedBones));
    }
    if (convertedGlobalToLocal) {
        AppLogger::Log("[RS3] ModelPackageLoader: converted global bind matrices to local hierarchy for modelId='" +
            modelId + "'.");
    }

    return true;
}

} // namespace RealSpace3
