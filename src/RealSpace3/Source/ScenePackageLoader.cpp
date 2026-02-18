#include "../Include/ScenePackageLoader.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace RealSpace3 {
namespace {

namespace fs = std::filesystem;

class BinReader {
public:
    explicit BinReader(const std::vector<uint8_t>& data) : m_data(data) {}

    size_t Remaining() const {
        return (m_off <= m_data.size()) ? (m_data.size() - m_off) : 0;
    }

    bool ReadBytes(void* dst, size_t size) {
        if (Remaining() < size) return false;
        std::memcpy(dst, m_data.data() + m_off, size);
        m_off += size;
        return true;
    }

    bool ReadU8(uint8_t& outValue) {
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

        outValue.assign(reinterpret_cast<const char*>(m_data.data() + m_off),
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

bool ReadVec3(BinReader& r, DirectX::XMFLOAT3& out) {
    return r.ReadF32(out.x) && r.ReadF32(out.y) && r.ReadF32(out.z);
}

bool ReadVec4(BinReader& r, DirectX::XMFLOAT4& out) {
    return r.ReadF32(out.x) && r.ReadF32(out.y) && r.ReadF32(out.z) && r.ReadF32(out.w);
}

void SetError(std::string* outError, const std::string& msg) {
    if (outError) *outError = msg;
}

bool ResolveSceneDir(const std::string& sceneId, fs::path& outDir) {
    const fs::path cwd = fs::current_path();

    const std::vector<fs::path> candidates = {
        cwd / "system" / "rs3" / "scenes" / sceneId,
        cwd / "OpenGunZ-Client" / "system" / "rs3" / "scenes" / sceneId,
        cwd / ".." / "OpenGunZ-Client" / "system" / "rs3" / "scenes" / sceneId,
        cwd / ".." / ".." / "OpenGunZ-Client" / "system" / "rs3" / "scenes" / sceneId,
    };

    for (const auto& c : candidates) {
        std::error_code ec;
        if (!fs::exists(c, ec) || !fs::is_directory(c, ec)) continue;
        const fs::path worldBin = c / "world.bin";
        if (fs::exists(worldBin, ec) && fs::is_regular_file(worldBin, ec)) {
            outDir = fs::weakly_canonical(c, ec);
            if (ec) outDir = c;
            return true;
        }
    }

    return false;
}

bool LoadWorld(const fs::path& worldPath, ScenePackageData& outData, std::string* outError) {
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(worldPath, bytes)) {
        SetError(outError, "Failed to read world.bin");
        return false;
    }

    BinReader r(bytes);

    std::array<uint8_t, 8> magic{};
    if (!r.ReadBytes(magic.data(), magic.size())) {
        SetError(outError, "world.bin is truncated (magic)");
        return false;
    }

    const std::array<uint8_t, 8> expected = { 0x52, 0x53, 0x33, 0x53, 0x43, 0x4E, 0x31, 0x00 }; // RS3SCN1\0
    if (magic != expected) {
        SetError(outError, "world.bin magic mismatch");
        return false;
    }

    uint32_t version = 0;
    if (!r.ReadU32(version) || version != 1) {
        SetError(outError, "world.bin version mismatch");
        return false;
    }

    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t materialCount = 0;
    uint32_t sectionCount = 0;
    uint32_t lightCount = 0;

    if (!r.ReadU32(vertexCount) || !r.ReadU32(indexCount) || !r.ReadU32(materialCount)
        || !r.ReadU32(sectionCount) || !r.ReadU32(lightCount)) {
        SetError(outError, "world.bin is truncated (counts)");
        return false;
    }

    if (!ReadVec3(r, outData.cameraPos01) || !ReadVec3(r, outData.cameraDir01)
        || !ReadVec3(r, outData.cameraPos02) || !ReadVec3(r, outData.cameraDir02)
        || !ReadVec3(r, outData.spawnPos) || !ReadVec3(r, outData.spawnDir)) {
        SetError(outError, "world.bin is truncated (camera/spawn)");
        return false;
    }

    if (!r.ReadF32(outData.fogMin) || !r.ReadF32(outData.fogMax) || !ReadVec3(r, outData.fogColor)) {
        SetError(outError, "world.bin is truncated (fog)");
        return false;
    }

    uint32_t fogEnabled = 0;
    if (!r.ReadU32(fogEnabled)) {
        SetError(outError, "world.bin is truncated (fog flag)");
        return false;
    }
    outData.fogEnabled = (fogEnabled != 0);

    if (!ReadVec3(r, outData.boundsMin) || !ReadVec3(r, outData.boundsMax)) {
        SetError(outError, "world.bin is truncated (bounds)");
        return false;
    }

    outData.materials.clear();
    outData.materials.resize(materialCount);
    for (uint32_t i = 0; i < materialCount; ++i) {
        if (!r.ReadU32(outData.materials[i].flags)) {
            SetError(outError, "world.bin is truncated (material flags)");
            return false;
        }
        if (!r.ReadString(outData.materials[i].diffuseMap)) {
            SetError(outError, "world.bin is truncated (material texture)");
            return false;
        }
    }

    outData.lights.clear();
    outData.lights.resize(lightCount);
    for (uint32_t i = 0; i < lightCount; ++i) {
        if (!ReadVec3(r, outData.lights[i].position)
            || !ReadVec3(r, outData.lights[i].color)
            || !r.ReadF32(outData.lights[i].intensity)
            || !r.ReadF32(outData.lights[i].attenuationStart)
            || !r.ReadF32(outData.lights[i].attenuationEnd)) {
            SetError(outError, "world.bin is truncated (lights)");
            return false;
        }
    }

    outData.sections.clear();
    outData.sections.resize(sectionCount);
    for (uint32_t i = 0; i < sectionCount; ++i) {
        if (!r.ReadU32(outData.sections[i].materialIndex)
            || !r.ReadU32(outData.sections[i].indexStart)
            || !r.ReadU32(outData.sections[i].indexCount)) {
            SetError(outError, "world.bin is truncated (sections)");
            return false;
        }
    }

    outData.vertices.clear();
    outData.vertices.resize(vertexCount);
    for (uint32_t i = 0; i < vertexCount; ++i) {
        if (!ReadVec3(r, outData.vertices[i].pos)
            || !ReadVec3(r, outData.vertices[i].normal)
            || !r.ReadF32(outData.vertices[i].uv.x)
            || !r.ReadF32(outData.vertices[i].uv.y)) {
            SetError(outError, "world.bin is truncated (vertices)");
            return false;
        }
    }

    outData.indices.clear();
    outData.indices.resize(indexCount);
    for (uint32_t i = 0; i < indexCount; ++i) {
        if (!r.ReadU32(outData.indices[i])) {
            SetError(outError, "world.bin is truncated (indices)");
            return false;
        }
    }

    for (const auto& sec : outData.sections) {
        if (sec.indexCount == 0) continue;
        const uint64_t end = static_cast<uint64_t>(sec.indexStart) + static_cast<uint64_t>(sec.indexCount);
        if (end > outData.indices.size()) {
            SetError(outError, "world.bin section range is invalid");
            return false;
        }
        if (sec.materialIndex >= outData.materials.size()) {
            SetError(outError, "world.bin section material index is invalid");
            return false;
        }
    }

    for (const auto idx : outData.indices) {
        if (idx >= outData.vertices.size()) {
            SetError(outError, "world.bin contains out-of-range index");
            return false;
        }
    }

    outData.hasCamera01 = true;
    outData.hasCamera02 = true;
    outData.hasSpawn = true;

    return true;
}

bool LoadCollision(const fs::path& collisionPath, ScenePackageData& outData, std::string* outError) {
    std::error_code ec;
    if (!fs::exists(collisionPath, ec) || !fs::is_regular_file(collisionPath, ec)) {
        outData.collision.rootIndex = -1;
        outData.collision.nodes.clear();
        return true;
    }

    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(collisionPath, bytes)) {
        SetError(outError, "Failed to read collision.bin");
        return false;
    }

    BinReader r(bytes);

    std::array<uint8_t, 8> magic{};
    if (!r.ReadBytes(magic.data(), magic.size())) {
        SetError(outError, "collision.bin is truncated (magic)");
        return false;
    }

    const std::array<uint8_t, 8> expected = { 0x52, 0x53, 0x33, 0x43, 0x4F, 0x4C, 0x31, 0x00 }; // RS3COL1\0
    if (magic != expected) {
        SetError(outError, "collision.bin magic mismatch");
        return false;
    }

    uint32_t version = 0;
    if (!r.ReadU32(version) || version != 1) {
        SetError(outError, "collision.bin version mismatch");
        return false;
    }

    uint32_t nodeCount = 0;
    int32_t rootIndex = -1;
    if (!r.ReadU32(nodeCount) || !r.ReadI32(rootIndex)) {
        SetError(outError, "collision.bin is truncated (counts)");
        return false;
    }

    outData.collision.rootIndex = rootIndex;
    outData.collision.nodes.clear();
    outData.collision.nodes.resize(nodeCount);

    for (uint32_t i = 0; i < nodeCount; ++i) {
        auto& n = outData.collision.nodes[i];
        uint8_t solid = 0;
        if (!ReadVec4(r, n.plane) || !r.ReadU8(solid) || !r.ReadI32(n.posChild) || !r.ReadI32(n.negChild)) {
            SetError(outError, "collision.bin is truncated (nodes)");
            return false;
        }
        n.solid = (solid != 0);
    }

    return true;
}

} // namespace

bool ScenePackageLoader::Load(const std::string& sceneId, ScenePackageData& outData, std::string* outError) {
    outData = ScenePackageData{};
    outData.sceneId = sceneId;

    fs::path sceneDir;
    if (!ResolveSceneDir(sceneId, sceneDir)) {
        SetError(outError, "Scene package directory not found");
        return false;
    }

    outData.baseDir = sceneDir.generic_string();

    const fs::path worldPath = sceneDir / "world.bin";
    const fs::path collisionPath = sceneDir / "collision.bin";

    if (!LoadWorld(worldPath, outData, outError)) {
        return false;
    }

    if (!LoadCollision(collisionPath, outData, outError)) {
        return false;
    }

    return true;
}

} // namespace RealSpace3
