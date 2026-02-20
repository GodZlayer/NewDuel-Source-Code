#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <tchar.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "AppLogger.h"
#include "RealSpace3/Include/RDeviceDX11.h"
#include "RealSpace3/Include/SceneManager.h"
#include "RealSpace3/Include/CinematicTimeline.h"
#include "RealSpace3/Include/CinematicPlayer.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")

namespace {

enum : int {
    IDC_VIEWPORT = 1001,
    IDC_SCENE_TREE,
    IDC_INSPECTOR,
    IDC_TIMELINE_TRACK,
    IDC_TRACK_LIST,
    IDC_BTN_PLAY,
    IDC_BTN_PAUSE,
    IDC_BTN_STOP,
    IDC_BTN_ADD_OBJECT,
    IDC_BTN_ADD_KEYFRAME,
    IDC_STATUS_TEXT,

    IDM_FILE_NEW = 2001,
    IDM_FILE_OPEN,
    IDM_FILE_SAVE,
    IDM_FILE_SAVE_AS,
    IDM_FILE_EXIT,
    IDM_IMPORT_MAP,
    IDM_IMPORT_OBJECT,
    IDM_MODE_MAP_ONLY,
    IDM_MODE_SHOWCASE_ONLY,
    IDM_MODE_GAMEPLAY
};

struct CineOptions {
    std::string timelinePath;
    bool preview = false;
    std::string exportMp4Path;
    int width = 1920;
    int height = 1080;
    int fps = 60;
    std::string ffmpegPath = "ffmpeg";
    std::string audioPathOverride;
};

struct StudioUiState {
    HWND viewport = nullptr;
    HWND sceneTree = nullptr;
    HWND inspector = nullptr;
    HWND timelineTrack = nullptr;
    HWND trackList = nullptr;
    HWND btnPlay = nullptr;
    HWND btnPause = nullptr;
    HWND btnStop = nullptr;
    HWND btnAddObject = nullptr;
    HWND btnAddKeyframe = nullptr;
    HWND statusText = nullptr;
    HTREEITEM rootScene = nullptr;
    HTREEITEM rootCamera = nullptr;
    HTREEITEM rootCharacters = nullptr;
    HTREEITEM rootProps = nullptr;
    HTREEITEM rootLights = nullptr;
};

std::unique_ptr<RealSpace3::RDeviceDX11> g_device;
RealSpace3::CinematicPlayer g_player;
RealSpace3::RS3TimelineData g_timelineData;
RealSpace3::RS3TimelinePlaybackOptions g_playbackOptions;
StudioUiState g_ui;
CineOptions g_options;

bool g_running = true;
bool g_playbackPaused = false;
bool g_isExporting = false;
int g_dynamicPropCounter = 0;
HWND g_mainWindow = nullptr;
std::string g_currentTimelinePath;
std::string g_currentShowcaseObjectModel = "props/car_display_platform";
bool g_sceneDirty = false;

constexpr int kSliderMax = 10000;

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return std::string();
    const int len = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), len, nullptr, nullptr);
    return out;
}

bool ParseArgs(CineOptions& out) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;

    auto consumeValue = [&](int& index, std::string& outValue) -> bool {
        if (index + 1 >= argc) return false;
        ++index;
        outValue = WideToUtf8(argv[index]);
        return !outValue.empty();
    };

    for (int i = 1; i < argc; ++i) {
        const std::string arg = WideToUtf8(argv[i]);
        if (arg == "--timeline") {
            if (!consumeValue(i, out.timelinePath)) return false;
        } else if (arg == "--preview") {
            out.preview = true;
        } else if (arg == "--export") {
            if (!consumeValue(i, out.exportMp4Path)) return false;
        } else if (arg == "--width") {
            std::string raw;
            if (!consumeValue(i, raw)) return false;
            out.width = std::max(320, std::atoi(raw.c_str()));
        } else if (arg == "--height") {
            std::string raw;
            if (!consumeValue(i, raw)) return false;
            out.height = std::max(240, std::atoi(raw.c_str()));
        } else if (arg == "--fps") {
            std::string raw;
            if (!consumeValue(i, raw)) return false;
            out.fps = std::max(1, std::atoi(raw.c_str()));
        } else if (arg == "--audio") {
            if (!consumeValue(i, out.audioPathOverride)) return false;
        } else if (arg == "--ffmpeg") {
            if (!consumeValue(i, out.ffmpegPath)) return false;
        }
    }

    LocalFree(argv);

    if (!out.preview && out.exportMp4Path.empty()) {
        out.preview = true;
    }
    return true;
}

std::string JsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
        case '\\\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char tmp[8] = {};
                std::snprintf(tmp, sizeof(tmp), "\\u%04x", static_cast<unsigned int>(c));
                out += tmp;
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return out;
}

RealSpace3::RS3TimelineData MakeDefaultTimeline() {
    RealSpace3::RS3TimelineData tl;
    tl.version = "ndg_cine_v1";
    tl.sceneId = "char_creation_select";
    tl.mode = RealSpace3::RS3RenderMode::MapOnlyCinematic;
    tl.durationSec = 8.0f;
    tl.fps = 60;
    tl.keyframes.clear();
    tl.keyframes.push_back({
        0.0f,
        { -180.0f, -320.0f, 180.0f },
        { 0.0f, 0.0f, 120.0f },
        0.0f,
        58.0f,
        RealSpace3::RS3TimelineEase::EaseInOutCubic
    });
    tl.keyframes.push_back({
        4.0f,
        { 0.0f, -260.0f, 155.0f },
        { 0.0f, 0.0f, 115.0f },
        0.0f,
        55.0f,
        RealSpace3::RS3TimelineEase::EaseInOutCubic
    });
    tl.keyframes.push_back({
        8.0f,
        { 180.0f, -320.0f, 180.0f },
        { 0.0f, 0.0f, 120.0f },
        0.0f,
        58.0f,
        RealSpace3::RS3TimelineEase::EaseInOutCubic
    });
    return tl;
}

std::string TimelineToJson(const RealSpace3::RS3TimelineData& tl) {
    std::ostringstream out;
    out.setf(std::ios::fixed, std::ios::floatfield);
    out.precision(4);
    out << "{\n";
    out << "  \"version\": \"ndg_cine_v1\",\n";
    out << "  \"sceneId\": \"" << JsonEscape(tl.sceneId) << "\",\n";
    out << "  \"mode\": \"" << RealSpace3::ToRenderModeString(tl.mode) << "\",\n";
    out << "  \"durationSec\": " << tl.durationSec << ",\n";
    out << "  \"fps\": " << std::max(1, tl.fps) << ",\n";
    out << "  \"camera\": {\n";
    out << "    \"keyframes\": [\n";
    for (size_t i = 0; i < tl.keyframes.size(); ++i) {
        const auto& kf = tl.keyframes[i];
        const char* ease = (kf.ease == RealSpace3::RS3TimelineEase::EaseInOutCubic) ? "ease-in-out-cubic" : "linear";
        out << "      {\n";
        out << "        \"t\": " << kf.t << ",\n";
        out << "        \"position\": [" << kf.position.x << ", " << kf.position.y << ", " << kf.position.z << "],\n";
        out << "        \"target\": [" << kf.target.x << ", " << kf.target.y << ", " << kf.target.z << "],\n";
        out << "        \"rollDeg\": " << kf.rollDeg << ",\n";
        out << "        \"fovDeg\": " << kf.fovDeg << ",\n";
        out << "        \"ease\": \"" << ease << "\"\n";
        out << "      }";
        if (i + 1 < tl.keyframes.size()) out << ",";
        out << "\n";
    }
    out << "    ]\n";
    out << "  }";
    if (tl.audio.enabled || !tl.audio.file.empty()) {
        out << ",\n";
        out << "  \"audio\": {\n";
        out << "    \"file\": \"" << JsonEscape(tl.audio.file) << "\",\n";
        out << "    \"offsetSec\": " << tl.audio.offsetSec << ",\n";
        out << "    \"gainDb\": " << tl.audio.gainDb << "\n";
        out << "  }\n";
    } else {
        out << "\n";
    }
    out << "}\n";
    return out.str();
}

bool SaveTimelineToFile(const std::string& path, const RealSpace3::RS3TimelineData& tl, std::string* outError = nullptr) {
    if (path.empty()) {
        if (outError) *outError = "Timeline path is empty.";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        if (outError) *outError = "Failed to open file for writing.";
        return false;
    }
    const std::string json = TimelineToJson(tl);
    out.write(json.data(), static_cast<std::streamsize>(json.size()));
    if (!out.good()) {
        if (outError) *outError = "Failed to write timeline file.";
        return false;
    }
    return true;
}

void UpdateWindowTitle() {
    if (!g_mainWindow) return;
    const std::string baseName = g_currentTimelinePath.empty()
        ? std::string("Untitled.ndgcine.json")
        : std::filesystem::path(g_currentTimelinePath).filename().string();
    const std::string title = std::string("RS3CineStudio - NDG Editor v1 - ") + baseName + (g_sceneDirty ? " *" : "");
    SetWindowTextA(g_mainWindow, title.c_str());
}

void MarkSceneDirty(bool dirty) {
    g_sceneDirty = dirty;
    UpdateWindowTitle();
}

bool PickOpenFile(const char* filter, const char* title, std::string& outPath, const char* initialDir = nullptr) {
    char fileBuffer[MAX_PATH] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_mainWindow;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = fileBuffer;
    ofn.nMaxFile = static_cast<DWORD>(std::size(fileBuffer));
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle = title;
    ofn.lpstrInitialDir = initialDir;
    if (!GetOpenFileNameA(&ofn)) {
        return false;
    }
    outPath = fileBuffer;
    return true;
}

bool PickSaveFile(const char* filter, const char* title, std::string& outPath, const char* initialDir = nullptr) {
    char fileBuffer[MAX_PATH] = {};
    if (!outPath.empty()) {
        std::strncpy(fileBuffer, outPath.c_str(), std::size(fileBuffer) - 1);
    }
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_mainWindow;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = fileBuffer;
    ofn.nMaxFile = static_cast<DWORD>(std::size(fileBuffer));
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle = title;
    ofn.lpstrDefExt = "json";
    ofn.lpstrInitialDir = initialDir;
    if (!GetSaveFileNameA(&ofn)) {
        return false;
    }
    outPath = fileBuffer;
    return true;
}

std::string ExtractSceneIdFromSceneJsonPath(const std::string& sceneJsonPath) {
    const std::filesystem::path p(sceneJsonPath);
    if (p.filename() == "scene.json") {
        return p.parent_path().filename().string();
    }
    return p.filename().string();
}

std::string ExtractModelIdFromModelJsonPath(const std::string& modelJsonPath) {
    std::filesystem::path p(modelJsonPath);
    if (p.filename() == "model.json") {
        p = p.parent_path();
    }
    std::string normalized = p.generic_string();
    const std::string marker = "/models/";
    size_t pos = normalized.find(marker);
    if (pos != std::string::npos) {
        normalized = normalized.substr(pos + marker.size());
        return normalized;
    }
    const std::string markerWin = "\\models\\";
    std::string raw = p.string();
    pos = raw.find(markerWin);
    if (pos != std::string::npos) {
        raw = raw.substr(pos + markerWin.size());
        std::replace(raw.begin(), raw.end(), '\\', '/');
        return raw;
    }
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    return normalized;
}

std::string DefaultCinematicsDir() {
    std::filesystem::path cwd = std::filesystem::current_path();
    const std::filesystem::path candidateA = cwd / "system" / "rs3" / "cinematics";
    const std::filesystem::path candidateB = cwd / "OpenGunZ-Client" / "system" / "rs3" / "cinematics";
    if (std::filesystem::exists(candidateA)) return candidateA.string();
    return candidateB.string();
}

std::string DefaultScenesDir() {
    std::filesystem::path cwd = std::filesystem::current_path();
    const std::filesystem::path candidateA = cwd / "system" / "rs3" / "scenes";
    const std::filesystem::path candidateB = cwd / "OpenGunZ-Client" / "system" / "rs3" / "scenes";
    if (std::filesystem::exists(candidateA)) return candidateA.string();
    return candidateB.string();
}

std::string DefaultModelsDir() {
    std::filesystem::path cwd = std::filesystem::current_path();
    const std::filesystem::path candidateA = cwd / "system" / "rs3" / "models";
    const std::filesystem::path candidateB = cwd / "OpenGunZ-Client" / "system" / "rs3" / "models";
    if (std::filesystem::exists(candidateA)) return candidateA.string();
    return candidateB.string();
}

float SliderToTime(int pos) {
    const float t = static_cast<float>(std::clamp(pos, 0, kSliderMax)) / static_cast<float>(kSliderMax);
    return t * std::max(0.001f, g_player.GetDuration());
}

int TimeToSlider(float timeSec) {
    const float dur = std::max(0.001f, g_player.GetDuration());
    const float t = std::clamp(timeSec / dur, 0.0f, 1.0f);
    return static_cast<int>(std::round(t * static_cast<float>(kSliderMax)));
}

void SetInspectorText(const std::string& text) {
    if (!g_ui.inspector) return;
    SetWindowTextA(g_ui.inspector, text.c_str());
}

void UpdateStatusText() {
    if (!g_ui.statusText) return;

    const float cur = g_player.GetCurrentTime();
    const float dur = std::max(0.001f, g_player.GetDuration());
    const char* mode = RealSpace3::ToRenderModeString(g_timelineData.mode);
    const char* state = g_playbackPaused ? "PAUSED" : (g_player.IsPlaying() ? "PLAYING" : "STOPPED");

    char buffer[256] = {};
    std::snprintf(buffer, sizeof(buffer),
        "Scene: %s | Mode: %s | Time: %.2fs / %.2fs | %s",
        g_timelineData.sceneId.c_str(),
        mode ? mode : "unknown",
        cur,
        dur,
        state);
    SetWindowTextA(g_ui.statusText, buffer);
}

void ApplyCurrentCameraPose() {
    RealSpace3::RS3CameraPose pose;
    if (!g_player.EvaluateCameraPose(pose)) {
        return;
    }
    (void)RealSpace3::SceneManager::getInstance().setCameraPose(pose, true);
}

void RefreshTimelineUi() {
    if (g_ui.timelineTrack) {
        SendMessage(g_ui.timelineTrack, TBM_SETPOS, TRUE, TimeToSlider(g_player.GetCurrentTime()));
    }
    UpdateStatusText();
}

void PopulateTrackList() {
    if (!g_ui.trackList) return;

    SendMessage(g_ui.trackList, LB_RESETCONTENT, 0, 0);

    SendMessage(g_ui.trackList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Track 1: Camera"));
    for (size_t i = 0; i < g_timelineData.keyframes.size(); ++i) {
        const auto& kf = g_timelineData.keyframes[i];
        char row[256] = {};
        std::snprintf(row, sizeof(row),
            "KF %02zu | t=%.2fs | pos(%.1f %.1f %.1f) | fov=%.1f",
            i,
            kf.t,
            kf.position.x,
            kf.position.y,
            kf.position.z,
            kf.fovDeg);
        SendMessageA(g_ui.trackList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row));
    }
}

void AddSceneTreeNode(HTREEITEM parent, const char* label, HTREEITEM* outItem = nullptr) {
    if (!g_ui.sceneTree) return;

    TVINSERTSTRUCTA ins = {};
    ins.hParent = parent;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT;
    ins.item.pszText = const_cast<char*>(label);
    HTREEITEM item = TreeView_InsertItem(g_ui.sceneTree, &ins);
    if (outItem) {
        *outItem = item;
    }
}

void PopulateSceneTree() {
    if (!g_ui.sceneTree) return;

    TreeView_DeleteAllItems(g_ui.sceneTree);

    AddSceneTreeNode(TVI_ROOT, "Scene", &g_ui.rootScene);
    AddSceneTreeNode(g_ui.rootScene, g_timelineData.sceneId.c_str());

    AddSceneTreeNode(TVI_ROOT, "Camera", &g_ui.rootCamera);
    AddSceneTreeNode(g_ui.rootCamera, "Camera.Main");

    AddSceneTreeNode(TVI_ROOT, "Characters", &g_ui.rootCharacters);
    AddSceneTreeNode(g_ui.rootCharacters, "Hero.Preview");

    AddSceneTreeNode(TVI_ROOT, "Props", &g_ui.rootProps);
    AddSceneTreeNode(g_ui.rootProps, g_currentShowcaseObjectModel.c_str());

    AddSceneTreeNode(TVI_ROOT, "Lights", &g_ui.rootLights);
    AddSceneTreeNode(g_ui.rootLights, "KeyLight");
    AddSceneTreeNode(g_ui.rootLights, "FillLight");

    TreeView_Expand(g_ui.sceneTree, g_ui.rootScene, TVE_EXPAND);
    TreeView_Expand(g_ui.sceneTree, g_ui.rootCamera, TVE_EXPAND);
    TreeView_Expand(g_ui.sceneTree, g_ui.rootCharacters, TVE_EXPAND);
    TreeView_Expand(g_ui.sceneTree, g_ui.rootProps, TVE_EXPAND);
    TreeView_Expand(g_ui.sceneTree, g_ui.rootLights, TVE_EXPAND);
}

void AddDynamicPropNode() {
    if (!g_ui.sceneTree || !g_ui.rootProps) return;

    ++g_dynamicPropCounter;
    char label[64] = {};
    std::snprintf(label, sizeof(label), "Prop.Dynamic_%03d", g_dynamicPropCounter);
    AddSceneTreeNode(g_ui.rootProps, label);
    TreeView_Expand(g_ui.sceneTree, g_ui.rootProps, TVE_EXPAND);

    SetInspectorText(std::string("New scene node added:\r\n") + label +
        "\r\n\r\nThis is an editor object entry (timeline-ready scaffold).\r\n"
        "Next milestone: bind model ID and transform gizmo.");
    MarkSceneDirty(true);
}

void AddCameraKeyframeAtCurrentTime() {
    RealSpace3::RS3CameraPose pose;
    if (!g_player.EvaluateCameraPose(pose)) {
        return;
    }

    RealSpace3::RS3TimelineKeyframe key;
    key.t = g_player.GetCurrentTime();
    key.position = pose.position;
    key.target = pose.target;
    key.rollDeg = 0.0f;
    key.fovDeg = pose.fovDeg;
    key.ease = RealSpace3::RS3TimelineEase::EaseInOutCubic;

    g_timelineData.keyframes.push_back(key);
    std::sort(g_timelineData.keyframes.begin(), g_timelineData.keyframes.end(), [](const auto& a, const auto& b) {
        return a.t < b.t;
    });

    PopulateTrackList();
    SetInspectorText("Camera keyframe added in memory.\r\nUse this as base for timeline authoring flow.");
    MarkSceneDirty(true);
}

void PausePlayback(bool paused) {
    g_playbackPaused = paused;
    g_player.Pause(paused);
    UpdateStatusText();
}

void StopPlayback() {
    g_player.Seek(g_playbackOptions.startTimeSec);
    PausePlayback(true);
    ApplyCurrentCameraPose();
    RefreshTimelineUi();
}

void LayoutStudioUi(HWND hWnd) {
    RECT rc = {};
    GetClientRect(hWnd, &rc);
    const int w = std::max(1, static_cast<int>(rc.right - rc.left));
    const int h = std::max(1, static_cast<int>(rc.bottom - rc.top));

    const int gap = 8;
    const int leftW = 280;
    const int rightW = 320;
    const int bottomH = 190;
    const int top = gap;
    const int left = gap;
    const int centerX = left + leftW + gap;
    const int centerW = std::max(200, w - leftW - rightW - (gap * 4));
    const int centerH = std::max(220, h - bottomH - (gap * 3));

    if (g_ui.sceneTree) {
        MoveWindow(g_ui.sceneTree, left, top, leftW, centerH, TRUE);
    }

    if (g_ui.inspector) {
        MoveWindow(g_ui.inspector, centerX + centerW + gap, top, rightW, centerH, TRUE);
    }

    if (g_ui.viewport) {
        MoveWindow(g_ui.viewport, centerX, top, centerW, centerH, TRUE);
    }

    const int bottomY = top + centerH + gap;
    const int buttonY = bottomY + gap;

    if (g_ui.btnPlay) MoveWindow(g_ui.btnPlay, left, buttonY, 90, 30, TRUE);
    if (g_ui.btnPause) MoveWindow(g_ui.btnPause, left + 100, buttonY, 90, 30, TRUE);
    if (g_ui.btnStop) MoveWindow(g_ui.btnStop, left + 200, buttonY, 90, 30, TRUE);
    if (g_ui.btnAddObject) MoveWindow(g_ui.btnAddObject, left + 310, buttonY, 130, 30, TRUE);
    if (g_ui.btnAddKeyframe) MoveWindow(g_ui.btnAddKeyframe, left + 450, buttonY, 140, 30, TRUE);

    if (g_ui.timelineTrack) {
        MoveWindow(g_ui.timelineTrack, left, buttonY + 38, w - (gap * 2), 34, TRUE);
    }

    if (g_ui.trackList) {
        MoveWindow(g_ui.trackList, left, buttonY + 78, w - (gap * 2), std::max(50, h - (buttonY + 84) - gap), TRUE);
    }

    if (g_ui.statusText) {
        MoveWindow(g_ui.statusText, left, h - 24 - gap, w - (gap * 2), 24, TRUE);
    }

    if (g_device && g_ui.viewport) {
        RECT vp = {};
        GetClientRect(g_ui.viewport, &vp);
        const int vpW = std::max(1, static_cast<int>(vp.right - vp.left));
        const int vpH = std::max(1, static_cast<int>(vp.bottom - vp.top));
        g_device->Resize(vpW, vpH);
        RealSpace3::SceneManager::getInstance().setSize(vpW, vpH);
    }
}

void CreateStudioUi(HWND hWnd) {
    g_ui.sceneTree = CreateWindowExA(
        WS_EX_CLIENTEDGE,
        WC_TREEVIEWA,
        "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS,
        0, 0, 100, 100,
        hWnd,
        reinterpret_cast<HMENU>(IDC_SCENE_TREE),
        GetModuleHandle(nullptr),
        nullptr);

    g_ui.viewport = CreateWindowExA(
        WS_EX_CLIENTEDGE,
        "STATIC",
        "",
        WS_CHILD | WS_VISIBLE,
        0, 0, 100, 100,
        hWnd,
        reinterpret_cast<HMENU>(IDC_VIEWPORT),
        GetModuleHandle(nullptr),
        nullptr);

    g_ui.inspector = CreateWindowExA(
        WS_EX_CLIENTEDGE,
        "EDIT",
        "Inspector:\r\nSelect a scene node.",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        0, 0, 100, 100,
        hWnd,
        reinterpret_cast<HMENU>(IDC_INSPECTOR),
        GetModuleHandle(nullptr),
        nullptr);

    g_ui.btnPlay = CreateWindowExA(0, "BUTTON", "Play", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 90, 30, hWnd, reinterpret_cast<HMENU>(IDC_BTN_PLAY), GetModuleHandle(nullptr), nullptr);
    g_ui.btnPause = CreateWindowExA(0, "BUTTON", "Pause", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 90, 30, hWnd, reinterpret_cast<HMENU>(IDC_BTN_PAUSE), GetModuleHandle(nullptr), nullptr);
    g_ui.btnStop = CreateWindowExA(0, "BUTTON", "Stop", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 90, 30, hWnd, reinterpret_cast<HMENU>(IDC_BTN_STOP), GetModuleHandle(nullptr), nullptr);
    g_ui.btnAddObject = CreateWindowExA(0, "BUTTON", "Add Object", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 130, 30, hWnd, reinterpret_cast<HMENU>(IDC_BTN_ADD_OBJECT), GetModuleHandle(nullptr), nullptr);
    g_ui.btnAddKeyframe = CreateWindowExA(0, "BUTTON", "Add Keyframe", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 140, 30, hWnd, reinterpret_cast<HMENU>(IDC_BTN_ADD_KEYFRAME), GetModuleHandle(nullptr), nullptr);

    g_ui.timelineTrack = CreateWindowExA(0, TRACKBAR_CLASSA, "",
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
        0, 0, 100, 30,
        hWnd,
        reinterpret_cast<HMENU>(IDC_TIMELINE_TRACK),
        GetModuleHandle(nullptr),
        nullptr);
    SendMessage(g_ui.timelineTrack, TBM_SETRANGE, TRUE, MAKELONG(0, kSliderMax));
    SendMessage(g_ui.timelineTrack, TBM_SETTICFREQ, 250, 0);

    g_ui.trackList = CreateWindowExA(
        WS_EX_CLIENTEDGE,
        "LISTBOX",
        "",
        WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL,
        0, 0, 100, 100,
        hWnd,
        reinterpret_cast<HMENU>(IDC_TRACK_LIST),
        GetModuleHandle(nullptr),
        nullptr);

    g_ui.statusText = CreateWindowExA(
        0,
        "STATIC",
        "",
        WS_CHILD | WS_VISIBLE,
        0, 0, 100, 20,
        hWnd,
        reinterpret_cast<HMENU>(IDC_STATUS_TEXT),
        GetModuleHandle(nullptr),
        nullptr);

    PopulateSceneTree();
    PopulateTrackList();
    SetInspectorText(
        "RS3 Cine Studio v1\r\n"
        "- Scene tree\r\n"
        "- Inspector\r\n"
        "- Timeline + scrub\r\n"
        "- Camera keyframe scaffold");

    LayoutStudioUi(hWnd);
    RefreshTimelineUi();
}

void CreateStudioMenu(HWND hWnd) {
    HMENU menuBar = CreateMenu();
    HMENU fileMenu = CreatePopupMenu();
    HMENU importMenu = CreatePopupMenu();
    HMENU modeMenu = CreatePopupMenu();

    AppendMenuA(fileMenu, MF_STRING, IDM_FILE_NEW, "New Scene\tCtrl+N");
    AppendMenuA(fileMenu, MF_STRING, IDM_FILE_OPEN, "Open Scene...\tCtrl+O");
    AppendMenuA(fileMenu, MF_STRING, IDM_FILE_SAVE, "Save Scene\tCtrl+S");
    AppendMenuA(fileMenu, MF_STRING, IDM_FILE_SAVE_AS, "Save Scene As...");
    AppendMenuA(fileMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(fileMenu, MF_STRING, IDM_FILE_EXIT, "Exit");

    AppendMenuA(importMenu, MF_STRING, IDM_IMPORT_MAP, "Import Map Scene...");
    AppendMenuA(importMenu, MF_STRING, IDM_IMPORT_OBJECT, "Import RS3 Object...");

    AppendMenuA(modeMenu, MF_STRING, IDM_MODE_MAP_ONLY, "Map Only Cinematic");
    AppendMenuA(modeMenu, MF_STRING, IDM_MODE_SHOWCASE_ONLY, "Showcase Only");
    AppendMenuA(modeMenu, MF_STRING, IDM_MODE_GAMEPLAY, "Gameplay");

    AppendMenuA(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(fileMenu), "File");
    AppendMenuA(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(importMenu), "Import");
    AppendMenuA(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(modeMenu), "Mode");
    SetMenu(hWnd, menuBar);
}

void RenderOneFrame(RealSpace3::RDeviceDX11* device) {
    if (!device) return;
    device->Clear(0.02f, 0.02f, 0.05f, 1.0f);
    device->SetStandard3DStates();
    RealSpace3::SceneManager::getInstance().draw(device->GetContext());
    device->SetStandard3DStates();
    RealSpace3::SceneManager::getInstance().drawShowcaseOverlay(device->GetContext());
}

bool RunCommand(const std::string& command) {
    AppLogger::Log("[CINE] Running command: " + command);
    const int rc = std::system(command.c_str());
    AppLogger::Log("[CINE] Command exit code: " + std::to_string(rc));
    return rc == 0;
}

bool WriteBmp32(const std::filesystem::path& filePath, const std::vector<uint8_t>& bgra, uint32_t width, uint32_t height) {
    if (bgra.size() < static_cast<size_t>(width) * static_cast<size_t>(height) * 4u) return false;

    struct BmpFileHeader {
        uint16_t bfType;
        uint32_t bfSize;
        uint16_t bfReserved1;
        uint16_t bfReserved2;
        uint32_t bfOffBits;
    };

    struct BmpInfoHeader {
        uint32_t biSize;
        int32_t biWidth;
        int32_t biHeight;
        uint16_t biPlanes;
        uint16_t biBitCount;
        uint32_t biCompression;
        uint32_t biSizeImage;
        int32_t biXPelsPerMeter;
        int32_t biYPelsPerMeter;
        uint32_t biClrUsed;
        uint32_t biClrImportant;
    };

    const uint32_t rowStride = width * 4u;
    const uint32_t imageSize = rowStride * height;

    BmpFileHeader fileHeader = {};
    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits = sizeof(BmpFileHeader) + sizeof(BmpInfoHeader);
    fileHeader.bfSize = fileHeader.bfOffBits + imageSize;

    BmpInfoHeader infoHeader = {};
    infoHeader.biSize = sizeof(BmpInfoHeader);
    infoHeader.biWidth = static_cast<int32_t>(width);
    infoHeader.biHeight = static_cast<int32_t>(height);
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 32;
    infoHeader.biCompression = 0;
    infoHeader.biSizeImage = imageSize;

    std::ofstream out(filePath, std::ios::binary);
    if (!out.is_open()) return false;

    out.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
    out.write(reinterpret_cast<const char*>(&infoHeader), sizeof(infoHeader));

    for (int y = static_cast<int>(height) - 1; y >= 0; --y) {
        const uint8_t* row = bgra.data() + static_cast<size_t>(y) * rowStride;
        out.write(reinterpret_cast<const char*>(row), rowStride);
    }

    return out.good();
}

bool ConfigureSceneForTimeline(const RealSpace3::RS3TimelineData& timeline) {
    auto& sm = RealSpace3::SceneManager::getInstance();

    if (timeline.mode == RealSpace3::RS3RenderMode::ShowcaseOnly) {
        sm.loadHangar();
        sm.setRenderMode(RealSpace3::RS3RenderMode::ShowcaseOnly);
        return true;
    }

    if (!sm.loadScenePackage(timeline.sceneId)) {
        return false;
    }
    sm.setRenderMode(timeline.mode);
    return true;
}

bool StartTimelinePlayback(bool loop, bool paused, float startTime, float endTime) {
    std::string error;
    g_playbackOptions.loop = loop;
    g_playbackOptions.speed = 1.0f;
    g_playbackOptions.startTimeSec = startTime;
    g_playbackOptions.endTimeSec = endTime;

    if (!g_player.Play(g_timelineData, g_playbackOptions, &error)) {
        AppLogger::Log("[CINE] Failed to start timeline: " + error);
        return false;
    }

    g_player.Seek(startTime);
    g_player.Pause(paused);
    g_playbackPaused = paused;
    ApplyCurrentCameraPose();
    RefreshTimelineUi();
    return true;
}

bool RestartPlaybackForCurrentScene(bool paused) {
    if (!ConfigureSceneForTimeline(g_timelineData)) {
        return false;
    }
    if (g_timelineData.mode == RealSpace3::RS3RenderMode::ShowcaseOnly) {
        auto& sm = RealSpace3::SceneManager::getInstance();
        sm.setCreationPreviewVisible(true);
        (void)sm.setCreationPreview(0, 0, 0, 0);
        (void)sm.setShowcaseObjectModel(g_currentShowcaseObjectModel);
    }
    return StartTimelinePlayback(g_options.preview && g_options.exportMp4Path.empty(), paused, 0.0f, g_timelineData.durationSec);
}

bool ConfirmDiscardIfDirty() {
    if (!g_sceneDirty) {
        return true;
    }
    const int choice = MessageBoxA(
        g_mainWindow,
        "Existem alteracoes nao salvas. Deseja descartar?",
        "RS3CineStudio",
        MB_YESNO | MB_ICONQUESTION);
    return choice == IDYES;
}

bool SaveCurrentSceneToPath(const std::string& path, bool updateCurrentPath) {
    std::string error;
    if (!SaveTimelineToFile(path, g_timelineData, &error)) {
        MessageBoxA(g_mainWindow, ("Falha ao salvar cena: " + error).c_str(), "RS3CineStudio", MB_OK | MB_ICONERROR);
        return false;
    }

    if (updateCurrentPath) {
        g_currentTimelinePath = path;
    }
    MarkSceneDirty(false);
    return true;
}

bool SaveSceneAsInteractive() {
    std::string savePath = g_currentTimelinePath;
    const std::string initialDir = DefaultCinematicsDir();
    if (!PickSaveFile("NDG Cine Timeline (*.ndgcine.json)\0*.ndgcine.json\0JSON (*.json)\0*.json\0\0",
        "Salvar cena RS3 Cine",
        savePath,
        initialDir.c_str())) {
        return false;
    }
    return SaveCurrentSceneToPath(savePath, true);
}

bool SaveSceneInteractive() {
    if (!g_currentTimelinePath.empty()) {
        return SaveCurrentSceneToPath(g_currentTimelinePath, true);
    }
    return SaveSceneAsInteractive();
}

bool LoadSceneFromPath(const std::string& path) {
    RealSpace3::RS3TimelineData loaded;
    std::string error;
    if (!RealSpace3::LoadTimelineFromFile(path, loaded, &error)) {
        MessageBoxA(g_mainWindow, ("Falha ao abrir cena: " + error).c_str(), "RS3CineStudio", MB_OK | MB_ICONERROR);
        return false;
    }

    g_timelineData = std::move(loaded);
    g_currentTimelinePath = path;
    if (!RestartPlaybackForCurrentScene(true)) {
        MessageBoxA(g_mainWindow, "Falha ao aplicar timeline no runtime RS3.", "RS3CineStudio", MB_OK | MB_ICONERROR);
        return false;
    }

    PopulateSceneTree();
    PopulateTrackList();
    MarkSceneDirty(false);
    return true;
}

void CreateNewScene() {
    g_timelineData = MakeDefaultTimeline();
    g_currentTimelinePath.clear();
    g_currentShowcaseObjectModel = "props/car_display_platform";
    (void)RestartPlaybackForCurrentScene(true);
    PopulateSceneTree();
    PopulateTrackList();
    MarkSceneDirty(true);
}

void OpenSceneInteractive() {
    if (!ConfirmDiscardIfDirty()) {
        return;
    }
    std::string path;
    const std::string initialDir = DefaultCinematicsDir();
    if (!PickOpenFile(
        "NDG Cine Timeline (*.ndgcine.json;*.json)\0*.ndgcine.json;*.json\0\0",
        "Abrir cena RS3 Cine",
        path,
        initialDir.c_str())) {
        return;
    }
    (void)LoadSceneFromPath(path);
}

void ImportMapInteractive() {
    std::string path;
    const std::string initialDir = DefaultScenesDir();
    if (!PickOpenFile(
        "RS3 Scene JSON (scene.json)\0scene.json\0JSON (*.json)\0*.json\0\0",
        "Importar mapa/cena RS3",
        path,
        initialDir.c_str())) {
        return;
    }

    const std::string sceneId = ExtractSceneIdFromSceneJsonPath(path);
    if (sceneId.empty()) {
        MessageBoxA(g_mainWindow, "Nao foi possivel resolver sceneId do arquivo selecionado.", "RS3CineStudio", MB_OK | MB_ICONERROR);
        return;
    }

    g_timelineData.sceneId = sceneId;
    g_timelineData.mode = RealSpace3::RS3RenderMode::MapOnlyCinematic;
    if (!RestartPlaybackForCurrentScene(true)) {
        MessageBoxA(g_mainWindow, ("Falha ao carregar mapa RS3: " + sceneId).c_str(), "RS3CineStudio", MB_OK | MB_ICONERROR);
        return;
    }
    SetInspectorText(std::string("Mapa importado com sucesso:\r\n") + sceneId);
    PopulateSceneTree();
    MarkSceneDirty(true);
}

void ImportObjectInteractive() {
    std::string path;
    const std::string initialDir = DefaultModelsDir();
    if (!PickOpenFile(
        "RS3 Model JSON (model.json)\0model.json\0JSON (*.json)\0*.json\0\0",
        "Importar objeto RS3",
        path,
        initialDir.c_str())) {
        return;
    }

    const std::string modelId = ExtractModelIdFromModelJsonPath(path);
    if (modelId.empty()) {
        MessageBoxA(g_mainWindow, "Nao foi possivel resolver modelId do arquivo selecionado.", "RS3CineStudio", MB_OK | MB_ICONERROR);
        return;
    }

    auto& sm = RealSpace3::SceneManager::getInstance();
    sm.loadHangar();
    sm.setRenderMode(RealSpace3::RS3RenderMode::ShowcaseOnly);
    sm.setCreationPreviewVisible(false);
    if (!sm.setShowcaseObjectModel(modelId)) {
        MessageBoxA(g_mainWindow, ("Falha ao importar objeto RS3: " + modelId).c_str(), "RS3CineStudio", MB_OK | MB_ICONERROR);
        return;
    }

    g_currentShowcaseObjectModel = modelId;
    g_timelineData.mode = RealSpace3::RS3RenderMode::ShowcaseOnly;
    SetInspectorText(std::string("Objeto RS3 importado:\r\n") + modelId);
    PopulateSceneTree();
    MarkSceneDirty(true);
}

void ApplyRenderModeCommand(int commandId) {
    switch (commandId) {
    case IDM_MODE_MAP_ONLY:
        g_timelineData.mode = RealSpace3::RS3RenderMode::MapOnlyCinematic;
        break;
    case IDM_MODE_SHOWCASE_ONLY:
        g_timelineData.mode = RealSpace3::RS3RenderMode::ShowcaseOnly;
        break;
    case IDM_MODE_GAMEPLAY:
        g_timelineData.mode = RealSpace3::RS3RenderMode::Gameplay;
        break;
    default:
        return;
    }

    if (!RestartPlaybackForCurrentScene(true)) {
        MessageBoxA(g_mainWindow, "Falha ao alterar modo de renderizacao RS3.", "RS3CineStudio", MB_OK | MB_ICONERROR);
        return;
    }
    MarkSceneDirty(true);
}

bool ExportTimeline(
    RealSpace3::RDeviceDX11* device,
    const CineOptions& options,
    const RealSpace3::RS3TimelineData& timelineData) {
    if (!device) return false;

    namespace fs = std::filesystem;
    const auto nowTicks = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path frameDir = fs::temp_directory_path() / ("ndg_cine_frames_" + std::to_string(nowTicks));
    std::error_code ec;
    fs::create_directories(frameDir, ec);
    if (ec) {
        AppLogger::Log("[CINE] Failed to create frame output dir: " + frameDir.string());
        return false;
    }

    RealSpace3::CinematicPlayer exportPlayer;
    RealSpace3::RS3TimelinePlaybackOptions exportOpts;
    exportOpts.loop = false;
    exportOpts.speed = 1.0f;
    exportOpts.startTimeSec = 0.0f;
    exportOpts.endTimeSec = timelineData.durationSec;

    std::string playError;
    if (!exportPlayer.Play(timelineData, exportOpts, &playError)) {
        AppLogger::Log("[CINE] Failed to start export playback: " + playError);
        return false;
    }

    exportPlayer.Pause(false);

    const int exportFps = std::max(1, options.fps > 0 ? options.fps : timelineData.fps);
    const float dt = 1.0f / static_cast<float>(exportFps);
    const int totalFrames = std::max(1, static_cast<int>(std::ceil(timelineData.durationSec * exportFps)));

    AppLogger::Log("[CINE] Export started: frames=" + std::to_string(totalFrames) + " fps=" + std::to_string(exportFps));

    std::vector<uint8_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;

    for (int frame = 0; frame < totalFrames; ++frame) {
        if (frame > 0) {
            exportPlayer.Update(dt);
        }

        RealSpace3::RS3CameraPose pose;
        if (exportPlayer.EvaluateCameraPose(pose)) {
            (void)RealSpace3::SceneManager::getInstance().setCameraPose(pose, true);
        }

        RealSpace3::SceneManager::getInstance().update(dt);
        RenderOneFrame(device);

        if (!device->ReadBackBufferBGRA(pixels, width, height)) {
            AppLogger::Log("[CINE] Failed to read back frame " + std::to_string(frame));
            return false;
        }

        char fileName[64] = {};
        std::snprintf(fileName, sizeof(fileName), "frame_%06d.bmp", frame);
        if (!WriteBmp32(frameDir / fileName, pixels, width, height)) {
            AppLogger::Log("[CINE] Failed to write frame bitmap: " + std::string(fileName));
            return false;
        }

        device->Present();
    }

    const std::string ffmpeg = options.ffmpegPath.empty() ? "ffmpeg" : options.ffmpegPath;
    std::string command = "\"" + ffmpeg + "\" -y -framerate " + std::to_string(exportFps) +
        " -i \"" + (frameDir / "frame_%06d.bmp").string() + "\"";

    std::string audioPath = options.audioPathOverride;
    if (audioPath.empty() && timelineData.audio.enabled) {
        audioPath = timelineData.audio.file;
    }

    if (!audioPath.empty()) {
        command += " -itsoffset " + std::to_string(timelineData.audio.offsetSec);
        command += " -i \"" + audioPath + "\"";
    }

    command += " -c:v libx264 -pix_fmt yuv420p -preset medium -crf 18";
    if (!audioPath.empty()) {
        command += " -c:a aac -b:a 192k";
        if (std::abs(timelineData.audio.gainDb) > 0.001f) {
            const float gainLinear = std::pow(10.0f, timelineData.audio.gainDb / 20.0f);
            command += " -filter:a \"volume=" + std::to_string(gainLinear) + "\"";
        }
        command += " -shortest";
    }

    command += " \"" + options.exportMp4Path + "\"";

    const bool ok = RunCommand(command);
    if (!ok) {
        AppLogger::Log("[CINE] FFmpeg export failed.");
    } else {
        AppLogger::Log("[CINE] Export finished: " + options.exportMp4Path);
    }
    return ok;
}

LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        LayoutStudioUi(hWnd);
        return 0;

    case WM_COMMAND: {
        const int id = LOWORD(wp);
        switch (id) {
        case IDM_FILE_NEW:
            if (ConfirmDiscardIfDirty()) {
                CreateNewScene();
            }
            return 0;
        case IDM_FILE_OPEN:
            OpenSceneInteractive();
            return 0;
        case IDM_FILE_SAVE:
            (void)SaveSceneInteractive();
            return 0;
        case IDM_FILE_SAVE_AS:
            (void)SaveSceneAsInteractive();
            return 0;
        case IDM_FILE_EXIT:
            SendMessage(hWnd, WM_CLOSE, 0, 0);
            return 0;
        case IDM_IMPORT_MAP:
            ImportMapInteractive();
            return 0;
        case IDM_IMPORT_OBJECT:
            ImportObjectInteractive();
            return 0;
        case IDM_MODE_MAP_ONLY:
        case IDM_MODE_SHOWCASE_ONLY:
        case IDM_MODE_GAMEPLAY:
            ApplyRenderModeCommand(id);
            return 0;
        case IDC_BTN_PLAY:
            PausePlayback(false);
            return 0;
        case IDC_BTN_PAUSE:
            PausePlayback(true);
            return 0;
        case IDC_BTN_STOP:
            StopPlayback();
            return 0;
        case IDC_BTN_ADD_OBJECT:
            AddDynamicPropNode();
            return 0;
        case IDC_BTN_ADD_KEYFRAME:
            AddCameraKeyframeAtCurrentTime();
            return 0;
        default:
            break;
        }
        break;
    }

    case WM_CLOSE:
        if (!ConfirmDiscardIfDirty()) {
            return 0;
        }
        DestroyWindow(hWnd);
        return 0;

    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lp) == g_ui.timelineTrack) {
            const int code = LOWORD(wp);
            if (code == TB_THUMBTRACK || code == TB_THUMBPOSITION || code == TB_LINEUP || code == TB_LINEDOWN ||
                code == TB_PAGEUP || code == TB_PAGEDOWN || code == TB_TOP || code == TB_BOTTOM) {
                const int pos = static_cast<int>(SendMessage(g_ui.timelineTrack, TBM_GETPOS, 0, 0));
                const float t = SliderToTime(pos);
                g_player.Seek(t);
                PausePlayback(true);
                ApplyCurrentCameraPose();
                RefreshTimelineUi();
            }
            return 0;
        }
        break;

    case WM_NOTIFY: {
        const NMHDR* hdr = reinterpret_cast<const NMHDR*>(lp);
        if (hdr && hdr->idFrom == IDC_SCENE_TREE && hdr->code == TVN_SELCHANGEDA) {
            const auto* info = reinterpret_cast<const NMTREEVIEWA*>(lp);
            if (info) {
                char text[256] = {};
                TVITEMA item = {};
                item.mask = TVIF_TEXT;
                item.hItem = info->itemNew.hItem;
                item.pszText = text;
                item.cchTextMax = static_cast<int>(std::size(text));
                if (TreeView_GetItem(g_ui.sceneTree, &item)) {
                    SetInspectorText(std::string("Selected: ") + text +
                        "\r\n\r\nTransform:\r\n  Position: (0,0,0)\r\n  Rotation: (0,0,0)\r\n  Scale: (1,1,1)\r\n\r\n"
                        "Timeline channels:\r\n  - Visibility\r\n  - Transform\r\n  - Material params");
                }
            }
            return 0;
        }
        break;
    }

    case WM_DESTROY:
        g_running = false;
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProc(hWnd, msg, wp, lp);
}

} // namespace

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nShow) {
    SetProcessDPIAware();

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    if (!ParseArgs(g_options)) {
        MessageBoxA(nullptr,
            "Usage: RS3CineStudio [--timeline <file.ndgcine.json>] [--preview] [--export out.mp4] [--width N --height N --fps N] [--audio file] [--ffmpeg path]",
            "RS3CineStudio",
            MB_OK | MB_ICONINFORMATION);
        return 1;
    }

    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::filesystem::path cwd = std::filesystem::path(exePath).parent_path();
    SetCurrentDirectoryA(cwd.string().c_str());

    AppLogger::Clear();
    AppLogger::Log("--- RS3 CINE STUDIO BOOT ---");

    if (!g_options.timelinePath.empty()) {
        std::string timelineError;
        if (!RealSpace3::LoadTimelineFromFile(g_options.timelinePath, g_timelineData, &timelineError)) {
            MessageBoxA(nullptr, ("Timeline load failed: " + timelineError).c_str(), "RS3CineStudio", MB_OK | MB_ICONERROR);
            return 1;
        }
        g_currentTimelinePath = g_options.timelinePath;
    } else {
        g_timelineData = MakeDefaultTimeline();
        g_currentTimelinePath.clear();
    }

    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = _T("RS3CineStudioClass");
    RegisterClassEx(&wc);

    HWND hWnd = CreateWindowEx(
        0,
        _T("RS3CineStudioClass"),
        _T("RS3CineStudio - NDG Editor v1"),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        g_options.width,
        g_options.height,
        nullptr,
        nullptr,
        hInst,
        nullptr);
    if (!hWnd) return 1;
    g_mainWindow = hWnd;

    CreateStudioMenu(hWnd);
    UpdateWindowTitle();

    ShowWindow(hWnd, nShow);
    UpdateWindow(hWnd);

    CreateStudioUi(hWnd);

    RECT vp = {};
    GetClientRect(g_ui.viewport, &vp);
    const int vpW = std::max(1, static_cast<int>(vp.right - vp.left));
    const int vpH = std::max(1, static_cast<int>(vp.bottom - vp.top));

    g_device = std::make_unique<RealSpace3::RDeviceDX11>();
    if (!g_device->Initialize(g_ui.viewport, vpW, vpH)) {
        MessageBoxA(nullptr, "DX11 initialization failed.", "RS3CineStudio", MB_OK | MB_ICONERROR);
        return 1;
    }

    RealSpace3::SceneManager::getInstance().init(g_device->GetDevice());
    RealSpace3::SceneManager::getInstance().setSize(vpW, vpH);

    if (!ConfigureSceneForTimeline(g_timelineData)) {
        MessageBoxA(nullptr, "Failed to initialize scene for timeline mode.", "RS3CineStudio", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (g_timelineData.mode == RealSpace3::RS3RenderMode::ShowcaseOnly) {
        auto& sm = RealSpace3::SceneManager::getInstance();
        sm.setCreationPreviewVisible(true);
        (void)sm.setCreationPreview(0, 0, 0, 0);
        (void)sm.setShowcaseObjectModel(g_currentShowcaseObjectModel);
    }

    if (!StartTimelinePlayback(g_options.preview && g_options.exportMp4Path.empty(), false, 0.0f, g_timelineData.durationSec)) {
        MessageBoxA(nullptr, "Failed to start timeline playback.", "RS3CineStudio", MB_OK | MB_ICONERROR);
        return 1;
    }

    PopulateSceneTree();
    PopulateTrackList();

    MarkSceneDirty(false);

    if (!g_options.exportMp4Path.empty()) {
        g_isExporting = true;
        const bool ok = ExportTimeline(g_device.get(), g_options, g_timelineData);
        return ok ? 0 : 2;
    }

    auto lastTick = std::chrono::steady_clock::now();
    MSG msg = {};
    while (g_running && msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - lastTick).count();
        lastTick = now;
        if (dt <= 0.0f || dt > 1.0f) dt = 1.0f / 60.0f;
        dt = std::clamp(dt, 1.0f / 240.0f, 1.0f / 20.0f);

        if (!g_playbackPaused) {
            g_player.Update(dt);
            if (!g_player.IsPlaying()) {
                g_playbackPaused = true;
            }
        }

        ApplyCurrentCameraPose();
        RealSpace3::SceneManager::getInstance().update(dt);

        RenderOneFrame(g_device.get());
        g_device->Present();

        RefreshTimelineUi();
    }

    return 0;
}
