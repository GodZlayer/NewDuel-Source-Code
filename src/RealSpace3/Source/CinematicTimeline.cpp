#include "../Include/CinematicTimeline.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace RealSpace3 {
namespace {

namespace fs = std::filesystem;

void SetError(std::string* outError, const std::string& message) {
    if (outError) {
        *outError = message;
    }
}

struct JsonValue {
    enum class Type {
        Null,
        Bool,
        Number,
        String,
        Object,
        Array
    };

    Type type = Type::Null;
    bool boolValue = false;
    double numberValue = 0.0;
    std::string stringValue;
    std::map<std::string, JsonValue> objectValue;
    std::vector<JsonValue> arrayValue;
};

class JsonParser {
public:
    explicit JsonParser(const std::string& source)
        : m_source(source) {}

    bool Parse(JsonValue& outValue, std::string* outError) {
        SkipWs();
        if (!ParseValue(outValue, outError)) {
            return false;
        }
        SkipWs();
        if (m_offset != m_source.size()) {
            SetError(outError, "Unexpected trailing JSON content.");
            return false;
        }
        return true;
    }

private:
    bool ParseValue(JsonValue& outValue, std::string* outError) {
        SkipWs();
        if (m_offset >= m_source.size()) {
            SetError(outError, "Unexpected end of JSON input.");
            return false;
        }

        const char c = m_source[m_offset];
        if (c == '{') return ParseObject(outValue, outError);
        if (c == '[') return ParseArray(outValue, outError);
        if (c == '"') return ParseStringValue(outValue, outError);
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return ParseNumber(outValue, outError);
        if (MatchLiteral("true")) {
            outValue.type = JsonValue::Type::Bool;
            outValue.boolValue = true;
            return true;
        }
        if (MatchLiteral("false")) {
            outValue.type = JsonValue::Type::Bool;
            outValue.boolValue = false;
            return true;
        }
        if (MatchLiteral("null")) {
            outValue.type = JsonValue::Type::Null;
            return true;
        }

        SetError(outError, std::string("Unexpected token near offset ") + std::to_string(m_offset));
        return false;
    }

    bool ParseObject(JsonValue& outValue, std::string* outError) {
        if (!Consume('{')) {
            SetError(outError, "Expected '{'.");
            return false;
        }
        outValue = JsonValue{};
        outValue.type = JsonValue::Type::Object;

        SkipWs();
        if (Consume('}')) {
            return true;
        }

        while (m_offset < m_source.size()) {
            std::string key;
            if (!ParseStringLiteral(key, outError)) {
                return false;
            }
            SkipWs();
            if (!Consume(':')) {
                SetError(outError, "Expected ':' after object key.");
                return false;
            }

            JsonValue item;
            if (!ParseValue(item, outError)) {
                return false;
            }
            outValue.objectValue.emplace(std::move(key), std::move(item));

            SkipWs();
            if (Consume('}')) {
                return true;
            }
            if (!Consume(',')) {
                SetError(outError, "Expected ',' or '}' in object.");
                return false;
            }
            SkipWs();
        }

        SetError(outError, "Unterminated object.");
        return false;
    }

    bool ParseArray(JsonValue& outValue, std::string* outError) {
        if (!Consume('[')) {
            SetError(outError, "Expected '['.");
            return false;
        }
        outValue = JsonValue{};
        outValue.type = JsonValue::Type::Array;

        SkipWs();
        if (Consume(']')) {
            return true;
        }

        while (m_offset < m_source.size()) {
            JsonValue item;
            if (!ParseValue(item, outError)) {
                return false;
            }
            outValue.arrayValue.push_back(std::move(item));

            SkipWs();
            if (Consume(']')) {
                return true;
            }
            if (!Consume(',')) {
                SetError(outError, "Expected ',' or ']' in array.");
                return false;
            }
            SkipWs();
        }

        SetError(outError, "Unterminated array.");
        return false;
    }

    bool ParseStringValue(JsonValue& outValue, std::string* outError) {
        std::string value;
        if (!ParseStringLiteral(value, outError)) {
            return false;
        }
        outValue = JsonValue{};
        outValue.type = JsonValue::Type::String;
        outValue.stringValue = std::move(value);
        return true;
    }

    bool ParseStringLiteral(std::string& outString, std::string* outError) {
        if (!Consume('"')) {
            SetError(outError, "Expected string literal.");
            return false;
        }

        outString.clear();
        while (m_offset < m_source.size()) {
            const char c = m_source[m_offset++];
            if (c == '"') {
                return true;
            }
            if (c == '\\') {
                if (m_offset >= m_source.size()) {
                    SetError(outError, "Unterminated escape sequence.");
                    return false;
                }
                const char esc = m_source[m_offset++];
                switch (esc) {
                case '"': outString.push_back('"'); break;
                case '\\': outString.push_back('\\'); break;
                case '/': outString.push_back('/'); break;
                case 'b': outString.push_back('\b'); break;
                case 'f': outString.push_back('\f'); break;
                case 'n': outString.push_back('\n'); break;
                case 'r': outString.push_back('\r'); break;
                case 't': outString.push_back('\t'); break;
                case 'u':
                    // Keep parser simple for timeline files: store unicode escape verbatim.
                    if ((m_offset + 3) >= m_source.size()) {
                        SetError(outError, "Invalid unicode escape.");
                        return false;
                    }
                    outString += "\\u";
                    outString.append(m_source.substr(m_offset, 4));
                    m_offset += 4;
                    break;
                default:
                    SetError(outError, "Unknown escape sequence.");
                    return false;
                }
                continue;
            }
            outString.push_back(c);
        }

        SetError(outError, "Unterminated string literal.");
        return false;
    }

    bool ParseNumber(JsonValue& outValue, std::string* outError) {
        const size_t start = m_offset;

        if (Peek() == '-') {
            ++m_offset;
        }

        if (!std::isdigit(static_cast<unsigned char>(Peek()))) {
            SetError(outError, "Invalid number token.");
            return false;
        }

        if (Peek() == '0') {
            ++m_offset;
        } else {
            while (std::isdigit(static_cast<unsigned char>(Peek()))) {
                ++m_offset;
            }
        }

        if (Peek() == '.') {
            ++m_offset;
            if (!std::isdigit(static_cast<unsigned char>(Peek()))) {
                SetError(outError, "Invalid number fraction.");
                return false;
            }
            while (std::isdigit(static_cast<unsigned char>(Peek()))) {
                ++m_offset;
            }
        }

        if (Peek() == 'e' || Peek() == 'E') {
            ++m_offset;
            if (Peek() == '+' || Peek() == '-') {
                ++m_offset;
            }
            if (!std::isdigit(static_cast<unsigned char>(Peek()))) {
                SetError(outError, "Invalid number exponent.");
                return false;
            }
            while (std::isdigit(static_cast<unsigned char>(Peek()))) {
                ++m_offset;
            }
        }

        const std::string token = m_source.substr(start, m_offset - start);
        char* endPtr = nullptr;
        const double number = std::strtod(token.c_str(), &endPtr);
        if (!endPtr || *endPtr != '\0') {
            SetError(outError, "Failed to parse number token.");
            return false;
        }

        outValue = JsonValue{};
        outValue.type = JsonValue::Type::Number;
        outValue.numberValue = number;
        return true;
    }

    char Peek() const {
        if (m_offset >= m_source.size()) return '\0';
        return m_source[m_offset];
    }

    bool MatchLiteral(const char* literal) {
        const size_t len = std::strlen(literal);
        if (m_offset + len > m_source.size()) return false;
        if (m_source.compare(m_offset, len, literal) != 0) return false;
        m_offset += len;
        return true;
    }

    bool Consume(char c) {
        if (m_offset >= m_source.size() || m_source[m_offset] != c) return false;
        ++m_offset;
        return true;
    }

    void SkipWs() {
        while (m_offset < m_source.size() && std::isspace(static_cast<unsigned char>(m_source[m_offset]))) {
            ++m_offset;
        }
    }

private:
    const std::string& m_source;
    size_t m_offset = 0;
};

const JsonValue* FindObjectField(const JsonValue& objectValue, const char* key) {
    if (objectValue.type != JsonValue::Type::Object) return nullptr;
    const auto it = objectValue.objectValue.find(key);
    if (it == objectValue.objectValue.end()) return nullptr;
    return &it->second;
}

std::optional<std::string> TryReadString(const JsonValue& objectValue, const char* key) {
    const JsonValue* field = FindObjectField(objectValue, key);
    if (!field || field->type != JsonValue::Type::String) return std::nullopt;
    return field->stringValue;
}

std::optional<double> TryReadNumber(const JsonValue& objectValue, const char* key) {
    const JsonValue* field = FindObjectField(objectValue, key);
    if (!field || field->type != JsonValue::Type::Number) return std::nullopt;
    return field->numberValue;
}

bool ReadVec3(const JsonValue& objectValue, const char* key, DirectX::XMFLOAT3& outValue) {
    const JsonValue* field = FindObjectField(objectValue, key);
    if (!field || field->type != JsonValue::Type::Array || field->arrayValue.size() != 3) return false;
    if (field->arrayValue[0].type != JsonValue::Type::Number ||
        field->arrayValue[1].type != JsonValue::Type::Number ||
        field->arrayValue[2].type != JsonValue::Type::Number) {
        return false;
    }
    outValue = {
        static_cast<float>(field->arrayValue[0].numberValue),
        static_cast<float>(field->arrayValue[1].numberValue),
        static_cast<float>(field->arrayValue[2].numberValue)
    };
    return true;
}

bool ResolveTimelinePath(const std::string& rawPath, fs::path& outPath) {
    if (rawPath.empty()) return false;

    const fs::path provided(rawPath);
    const fs::path cwd = fs::current_path();

    std::vector<fs::path> candidates;
    candidates.push_back(provided);
    candidates.push_back(cwd / provided);
    candidates.push_back(cwd / "OpenGunZ-Client" / "system" / "rs3" / "cinematics" / provided);
    candidates.push_back(cwd / "system" / "rs3" / "cinematics" / provided);

    if (!provided.has_extension()) {
        const fs::path withExt = provided.string() + ".ndgcine.json";
        candidates.push_back(withExt);
        candidates.push_back(cwd / withExt);
        candidates.push_back(cwd / "OpenGunZ-Client" / "system" / "rs3" / "cinematics" / withExt);
        candidates.push_back(cwd / "system" / "rs3" / "cinematics" / withExt);
    }

    for (const auto& c : candidates) {
        std::error_code ec;
        if (!fs::exists(c, ec) || !fs::is_regular_file(c, ec)) continue;
        outPath = fs::weakly_canonical(c, ec);
        if (ec) outPath = c;
        return true;
    }

    return false;
}

RS3TimelineEase ParseEase(const std::string& value) {
    if (value == "ease-in-out-cubic" || value == "easeInOutCubic") {
        return RS3TimelineEase::EaseInOutCubic;
    }
    return RS3TimelineEase::Linear;
}

} // namespace

bool LoadTimelineFromFile(const std::string& timelinePath, RS3TimelineData& outTimeline, std::string* outError) {
    fs::path resolvedPath;
    if (!ResolveTimelinePath(timelinePath, resolvedPath)) {
        SetError(outError, "Timeline file not found: '" + timelinePath + "'.");
        return false;
    }

    std::ifstream input(resolvedPath, std::ios::binary);
    if (!input.is_open()) {
        SetError(outError, "Failed to open timeline file.");
        return false;
    }

    std::stringstream buffer;
    buffer << input.rdbuf();
    const std::string json = buffer.str();
    if (json.empty()) {
        SetError(outError, "Timeline file is empty.");
        return false;
    }

    JsonParser parser(json);
    JsonValue root;
    std::string parseError;
    if (!parser.Parse(root, &parseError)) {
        SetError(outError, "Timeline JSON parse failed: " + parseError);
        return false;
    }
    if (root.type != JsonValue::Type::Object) {
        SetError(outError, "Timeline root must be an object.");
        return false;
    }

    RS3TimelineData parsed;

    const auto version = TryReadString(root, "version");
    if (!version || *version != "ndg_cine_v1") {
        SetError(outError, "Timeline version must be 'ndg_cine_v1'.");
        return false;
    }
    parsed.version = *version;

    const auto sceneId = TryReadString(root, "sceneId");
    if (!sceneId || sceneId->empty()) {
        SetError(outError, "Timeline sceneId is required.");
        return false;
    }
    parsed.sceneId = *sceneId;

    const auto modeText = TryReadString(root, "mode");
    if (!modeText || !ParseRenderModeString(*modeText, parsed.mode)) {
        SetError(outError, "Timeline mode is invalid.");
        return false;
    }

    const auto durationSec = TryReadNumber(root, "durationSec");
    if (!durationSec || *durationSec <= 0.0) {
        SetError(outError, "Timeline durationSec must be > 0.");
        return false;
    }
    parsed.durationSec = static_cast<float>(*durationSec);

    if (const auto fps = TryReadNumber(root, "fps")) {
        parsed.fps = std::max(1, static_cast<int>(*fps));
    }

    const JsonValue* cameraObject = FindObjectField(root, "camera");
    if (!cameraObject || cameraObject->type != JsonValue::Type::Object) {
        SetError(outError, "Timeline camera object is required.");
        return false;
    }

    const JsonValue* keyframesArray = FindObjectField(*cameraObject, "keyframes");
    if (!keyframesArray || keyframesArray->type != JsonValue::Type::Array) {
        SetError(outError, "Timeline camera.keyframes array is required.");
        return false;
    }

    parsed.keyframes.clear();
    parsed.keyframes.reserve(keyframesArray->arrayValue.size());
    for (const auto& item : keyframesArray->arrayValue) {
        if (item.type != JsonValue::Type::Object) {
            SetError(outError, "Each keyframe must be an object.");
            return false;
        }

        RS3TimelineKeyframe kf;
        const auto t = TryReadNumber(item, "t");
        if (!t) {
            SetError(outError, "Keyframe field 't' is required.");
            return false;
        }
        kf.t = static_cast<float>(*t);

        if (!ReadVec3(item, "position", kf.position)) {
            SetError(outError, "Keyframe field 'position' must be vec3.");
            return false;
        }
        if (!ReadVec3(item, "target", kf.target)) {
            SetError(outError, "Keyframe field 'target' must be vec3.");
            return false;
        }

        if (const auto rollDeg = TryReadNumber(item, "rollDeg")) {
            kf.rollDeg = static_cast<float>(*rollDeg);
        }
        if (const auto fovDeg = TryReadNumber(item, "fovDeg")) {
            kf.fovDeg = static_cast<float>(*fovDeg);
        }
        if (const auto ease = TryReadString(item, "ease")) {
            kf.ease = ParseEase(*ease);
        }

        parsed.keyframes.push_back(kf);
    }

    if (parsed.keyframes.empty()) {
        SetError(outError, "Timeline keyframes array is empty.");
        return false;
    }

    std::sort(parsed.keyframes.begin(), parsed.keyframes.end(), [](const RS3TimelineKeyframe& a, const RS3TimelineKeyframe& b) {
        return a.t < b.t;
    });

    if (parsed.keyframes.front().t > 0.0f) {
        parsed.keyframes.front().t = 0.0f;
    }
    parsed.durationSec = std::max(parsed.durationSec, parsed.keyframes.back().t);

    if (const JsonValue* audioObject = FindObjectField(root, "audio")) {
        if (audioObject->type == JsonValue::Type::Object) {
            if (const auto file = TryReadString(*audioObject, "file")) {
                parsed.audio.file = *file;
                parsed.audio.enabled = !parsed.audio.file.empty();
            }
            if (const auto offsetSecValue = TryReadNumber(*audioObject, "offsetSec")) {
                parsed.audio.offsetSec = static_cast<float>(*offsetSecValue);
            }
            if (const auto gainDbValue = TryReadNumber(*audioObject, "gainDb")) {
                parsed.audio.gainDb = static_cast<float>(*gainDbValue);
            }
        }
    }

    outTimeline = std::move(parsed);
    return true;
}

} // namespace RealSpace3
