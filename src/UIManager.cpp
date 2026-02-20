#include "UIManager.h"
#include "AppLogger.h"
#include "NakamaManager.h"
#include "RealSpace3/Include/SceneManager.h"
#include <direct.h>
#include <algorithm>
#include <cstdio>

#include <Ultralight/Ultralight.h>
#include <Ultralight/JavaScript.h>
#include <AppCore/App.h>

using namespace ultralight;

#define VIEW ((View*)_view)

std::string JSValueToStdString(JSContextRef ctx, JSValueRef val) {
    JSStringRef jsString = JSValueToStringCopy(ctx, val, nullptr);
    if (!jsString) return "";
    size_t n = JSStringGetMaximumUTF8CStringSize(jsString);
    char* buf = new char[n];
    JSStringGetUTF8CString(jsString, buf, n);
    std::string result(buf);
    delete[] buf;
    JSStringRelease(jsString);
    return result;
}

std::string JsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (unsigned char c : value) {
        switch (c) {
        case '\"': out += "\\\""; break;
        case '\'': out += "\\u0027"; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[7];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
            break;
        }
    }
    return out;
}

std::string BuildFileUrl(const std::string& relativePath) {
    char buffer[MAX_PATH];
    _getcwd(buffer, MAX_PATH);
    std::string path = "file:///";
    path += buffer;
    std::replace(path.begin(), path.end(), '\\', '/');
    return path + relativePath;
}

void SendToUI(const std::string& functionName, const std::string& jsonPayload) {
    if (!UIManager::getInstance().getView()) return;
    std::string js = "if(window." + functionName + ") " + functionName + "(" + jsonPayload + ")";
    ((View*)UIManager::getInstance().getView())->EvaluateScript(js.c_str());
}

class UIListener : public LoadListener {
public:
    virtual void OnDOMReady(View* caller, uint64_t frame_id, bool is_main_frame, const String& url) override {
        const std::string pageUrl = std::string(url.utf8().data());
        AppLogger::Log("INTERFACE: Pagina ativa -> " + pageUrl);

        // Disable RS3 showcase overlay outside char select to avoid unnecessary GPU work on login/loading.
        if (pageUrl.find("character_selection.html") == std::string::npos) {
            RealSpace3::SceneManager::getInstance().setShowcaseViewport(0, 0, 0, 0);
            RealSpace3::SceneManager::getInstance().setCreationPreviewVisible(false);
            RealSpace3::SceneManager::getInstance().setRenderMode(RealSpace3::RS3RenderMode::MapOnlyCinematic);

            const bool wantsCineBackground =
                (pageUrl.find("login.html") != std::string::npos) ||
                (pageUrl.find("loading.html") != std::string::npos) ||
                (pageUrl.find("lobby.html") != std::string::npos);
            if (wantsCineBackground) {
                RealSpace3::RS3TimelinePlaybackOptions opts;
                opts.loop = true;
                opts.speed = 1.0f;
                opts.startTimeSec = 0.0f;
                opts.endTimeSec = -1.0f;
                (void)RealSpace3::SceneManager::getInstance().playTimeline("char_select_intro.ndgcine.json", opts);
            } else {
                RealSpace3::SceneManager::getInstance().stopTimeline();
            }
        } else {
            RealSpace3::SceneManager::getInstance().stopTimeline();
            RealSpace3::SceneManager::getInstance().loadHangar();
            RealSpace3::SceneManager::getInstance().setRenderMode(RealSpace3::RS3RenderMode::ShowcaseOnly);
        }

        JSContextRef ctx = caller->LockJSContext()->ctx();
        JSObjectRef global = JSContextGetGlobalObject(ctx);
        auto bind = [&](const char* name, JSObjectCallAsFunctionCallback cb) {
            JSStringRef s = JSStringCreateWithUTF8CString(name);
            JSObjectSetProperty(ctx, global, s, JSObjectMakeFunctionWithCallback(ctx, s, cb), kJSPropertyAttributeNone, nullptr);
            JSStringRelease(s);
        };

        bind("login", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            if (argc >= 2) {
                std::string u = JSValueToStdString(ctx, argv[0]);
                std::string p = JSValueToStdString(ctx, argv[1]);
                AppLogger::Log("LOGIN: tentativa de autenticacao para '" + u + "'.");
                AppLogger::LogNetwork("[UI] login() called email='" + u + "' password_len=" + std::to_string(p.size()));
                Nakama::NakamaManager::getInstance().authenticateEmail(u, p, [](bool s, const std::string& e) {
                    if (s) {
                        AppLogger::Log("LOGIN: autenticacao OK, abrindo character_selection.");
                        AppLogger::LogNetwork("[UI] login result=ok -> loading character_selection.html");
                        UIManager::getInstance().loadURL(BuildFileUrl("/ui/character_selection.html"));
                    } else {
                        AppLogger::Log("LOGIN: falha de autenticacao -> " + e);
                        AppLogger::LogNetwork("[UI] login result=error message='" + e + "'");
                        const std::string safe = JsonEscape(e.empty() ? "falha desconhecida" : e);
                        std::string js = "if(window.setAuthStatus) setAuthStatus('ERRO: " + safe + "', false)";
                        ((View*)UIManager::getInstance().getView())->EvaluateScript(js.c_str());
                    }
                });
            }
            return JSValueMakeUndefined(ctx);
        });

        bind("list_characters", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            Nakama::NakamaManager::getInstance().listCharacters([](bool s, const std::string& p) {
                if (s) {
                    SendToUI("onCharacterList", p);
                } else {
                    SendToUI("onCharacterListError", "{\"message\":\"" + JsonEscape(p) + "\"}");
                }
            });
            return JSValueMakeUndefined(ctx);
        });

        bind("create_character", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            if (argc < 5) return JSValueMakeUndefined(ctx);

            const std::string name = JSValueToStdString(ctx, argv[0]);
            const int sex = static_cast<int>(JSValueToNumber(ctx, argv[1], nullptr));
            const int face = static_cast<int>(JSValueToNumber(ctx, argv[2], nullptr));
            const int preset = static_cast<int>(JSValueToNumber(ctx, argv[3], nullptr));
            const int hair = static_cast<int>(JSValueToNumber(ctx, argv[4], nullptr));

            Nakama::NakamaManager::getInstance().createCharacter(name, sex, face, hair, preset,
                [](bool s, const std::string& payload) {
                    if (s) {
                        SendToUI("onCreateCharacterResult", "{\"success\":true}");
                        Nakama::NakamaManager::getInstance().listCharacters([](bool ok, const std::string& listPayload) {
                            if (ok) SendToUI("onCharacterList", listPayload);
                        });
                    } else {
                        std::string safe = JsonEscape(payload);
                        SendToUI("onCreateCharacterResult", "{\"success\":false,\"message\":\"" + safe + "\"}");
                    }
                });
            return JSValueMakeUndefined(ctx);
        });

        bind("delete_character", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            if (argc < 1) return JSValueMakeUndefined(ctx);
            const std::string charId = JSValueToStdString(ctx, argv[0]);
            if (charId.empty()) return JSValueMakeUndefined(ctx);

            Nakama::NakamaManager::getInstance().deleteCharacter(charId,
                [](bool s, const std::string& payload) {
                    if (s) {
                        SendToUI("onDeleteCharacterResult", "{\"success\":true}");
                        Nakama::NakamaManager::getInstance().listCharacters([](bool ok, const std::string& listPayload) {
                            if (ok) SendToUI("onCharacterList", listPayload);
                        });
                    } else {
                        std::string safe = JsonEscape(payload);
                        SendToUI("onDeleteCharacterResult", "{\"success\":false,\"message\":\"" + safe + "\"}");
                    }
                });
            return JSValueMakeUndefined(ctx);
        });

        bind("select_character", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            if (argc < 1) return JSValueMakeUndefined(ctx);
            const std::string charId = JSValueToStdString(ctx, argv[0]);
            if (charId.empty()) return JSValueMakeUndefined(ctx);

            Nakama::NakamaManager::getInstance().selectCharacter(charId,
                [](bool s, const std::string& payload) {
                    if (s) {
                        SendToUI("onSelectCharacterResult", "{\"success\":true}");
                    } else {
                        std::string safe = JsonEscape(payload);
                        SendToUI("onSelectCharacterResult", "{\"success\":false,\"message\":\"" + safe + "\"}");
                    }
                });
            return JSValueMakeUndefined(ctx);
        });

        bind("enter_lobby", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            RealSpace3::SceneManager::getInstance().loadLobbyBasic();
            UIManager::getInstance().loadURL(BuildFileUrl("/ui/lobby.html"));
            return JSValueMakeUndefined(ctx);
        });

        bind("go_character_select", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            UIManager::getInstance().loadURL(BuildFileUrl("/ui/character_selection.html"));
            return JSValueMakeUndefined(ctx);
        });

        bind("go_lobby", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            UIManager::getInstance().loadURL(BuildFileUrl("/ui/lobby.html"));
            return JSValueMakeUndefined(ctx);
        });

        bind("go_shop", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            UIManager::getInstance().loadURL(BuildFileUrl("/ui/shop.html"));
            return JSValueMakeUndefined(ctx);
        });

        bind("go_equip", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            UIManager::getInstance().loadURL(BuildFileUrl("/ui/equip.html"));
            return JSValueMakeUndefined(ctx);
        });

        bind("go_options", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            UIManager::getInstance().loadURL(BuildFileUrl("/ui/options.html"));
            return JSValueMakeUndefined(ctx);
        });

        bind("set_character_preview", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            if (argc < 4) return JSValueMakeBoolean(ctx, false);
            const int sex = static_cast<int>(JSValueToNumber(ctx, argv[0], nullptr));
            const int face = static_cast<int>(JSValueToNumber(ctx, argv[1], nullptr));
            const int preset = static_cast<int>(JSValueToNumber(ctx, argv[2], nullptr));
            const int hair = static_cast<int>(JSValueToNumber(ctx, argv[3], nullptr));
            const bool ok = RealSpace3::SceneManager::getInstance().setCreationPreview(sex, face, preset, hair);
            return JSValueMakeBoolean(ctx, ok);
        });

        bind("set_preview_visible", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            if (argc < 1) return JSValueMakeUndefined(ctx);
            const bool visible = JSValueToBoolean(ctx, argv[0]);
            RealSpace3::SceneManager::getInstance().setCreationPreviewVisible(visible);
            return JSValueMakeUndefined(ctx);
        });

        bind("set_preview_rect", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            if (argc < 4) return JSValueMakeBoolean(ctx, false);
            const int x = static_cast<int>(JSValueToNumber(ctx, argv[0], nullptr));
            const int y = static_cast<int>(JSValueToNumber(ctx, argv[1], nullptr));
            const int w = static_cast<int>(JSValueToNumber(ctx, argv[2], nullptr));
            const int h = static_cast<int>(JSValueToNumber(ctx, argv[3], nullptr));
            RealSpace3::SceneManager::getInstance().setShowcaseViewport(x, y, w, h);
            return JSValueMakeBoolean(ctx, true);
        });

        bind("set_rs3_render_mode", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            if (argc < 1) return JSValueMakeBoolean(ctx, false);
            const std::string mode = JSValueToStdString(ctx, argv[0]);
            RealSpace3::RS3RenderMode renderMode;
            if (!RealSpace3::ParseRenderModeString(mode, renderMode)) {
                return JSValueMakeBoolean(ctx, false);
            }
            const bool ok = RealSpace3::SceneManager::getInstance().setRenderMode(renderMode);
            return JSValueMakeBoolean(ctx, ok);
        });

        bind("load_rs3_scene", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            if (argc < 1) return JSValueMakeBoolean(ctx, false);
            const std::string sceneId = JSValueToStdString(ctx, argv[0]);
            if (sceneId.empty()) return JSValueMakeBoolean(ctx, false);
            const bool ok = RealSpace3::SceneManager::getInstance().loadScenePackage(sceneId);
            return JSValueMakeBoolean(ctx, ok);
        });

        bind("play_rs3_timeline", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            if (argc < 1) return JSValueMakeBoolean(ctx, false);
            const std::string timelinePath = JSValueToStdString(ctx, argv[0]);
            const bool loop = (argc >= 2) ? JSValueToBoolean(ctx, argv[1]) : false;
            RealSpace3::RS3TimelinePlaybackOptions opts;
            opts.loop = loop;
            const bool ok = RealSpace3::SceneManager::getInstance().playTimeline(timelinePath, opts);
            return JSValueMakeBoolean(ctx, ok);
        });

        bind("stop_rs3_timeline", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            RealSpace3::SceneManager::getInstance().stopTimeline();
            return JSValueMakeUndefined(ctx);
        });

        bind("set_rs3_camera_pose", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            if (argc < 12) return JSValueMakeBoolean(ctx, false);
            RealSpace3::RS3CameraPose pose;
            pose.position = {
                static_cast<float>(JSValueToNumber(ctx, argv[0], nullptr)),
                static_cast<float>(JSValueToNumber(ctx, argv[1], nullptr)),
                static_cast<float>(JSValueToNumber(ctx, argv[2], nullptr))
            };
            pose.target = {
                static_cast<float>(JSValueToNumber(ctx, argv[3], nullptr)),
                static_cast<float>(JSValueToNumber(ctx, argv[4], nullptr)),
                static_cast<float>(JSValueToNumber(ctx, argv[5], nullptr))
            };
            pose.up = {
                static_cast<float>(JSValueToNumber(ctx, argv[6], nullptr)),
                static_cast<float>(JSValueToNumber(ctx, argv[7], nullptr)),
                static_cast<float>(JSValueToNumber(ctx, argv[8], nullptr))
            };
            pose.fovDeg = static_cast<float>(JSValueToNumber(ctx, argv[9], nullptr));
            pose.nearZ = static_cast<float>(JSValueToNumber(ctx, argv[10], nullptr));
            pose.farZ = static_cast<float>(JSValueToNumber(ctx, argv[11], nullptr));
            const bool immediate = (argc >= 13) ? JSValueToBoolean(ctx, argv[12]) : true;
            const bool ok = RealSpace3::SceneManager::getInstance().setCameraPose(pose, immediate);
            return JSValueMakeBoolean(ctx, ok);
        });

        bind("adjust_creation_camera", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            if (argc < 3) return JSValueMakeBoolean(ctx, false);
            const float yawDeltaDeg = static_cast<float>(JSValueToNumber(ctx, argv[0], nullptr));
            const float pitchDeltaDeg = static_cast<float>(JSValueToNumber(ctx, argv[1], nullptr));
            const float zoomDelta = static_cast<float>(JSValueToNumber(ctx, argv[2], nullptr));
            const bool ok = RealSpace3::SceneManager::getInstance().adjustCreationCamera(yawDeltaDeg, pitchDeltaDeg, zoomDelta);
            return JSValueMakeBoolean(ctx, ok);
        });

        bind("adjust_creation_character_yaw", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            if (argc < 1) return JSValueMakeBoolean(ctx, false);
            const float yawDeltaDeg = static_cast<float>(JSValueToNumber(ctx, argv[0], nullptr));
            const bool ok = RealSpace3::SceneManager::getInstance().adjustCreationCharacterYaw(yawDeltaDeg);
            return JSValueMakeBoolean(ctx, ok);
        });

        bind("set_creation_camera_pose", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            if (argc < 5) return JSValueMakeBoolean(ctx, false);
            const float yawDeg = static_cast<float>(JSValueToNumber(ctx, argv[0], nullptr));
            const float pitchDeg = static_cast<float>(JSValueToNumber(ctx, argv[1], nullptr));
            const float distance = static_cast<float>(JSValueToNumber(ctx, argv[2], nullptr));
            const float focusHeight = static_cast<float>(JSValueToNumber(ctx, argv[3], nullptr));
            const bool autoOrbit = JSValueToBoolean(ctx, argv[4]);
            const bool ok = RealSpace3::SceneManager::getInstance().setCreationCameraPose(yawDeg, pitchDeg, distance, focusHeight, autoOrbit);
            return JSValueMakeBoolean(ctx, ok);
        });

        bind("set_creation_camera_auto_orbit", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            if (argc < 1) return JSValueMakeUndefined(ctx);
            const bool enabled = JSValueToBoolean(ctx, argv[0]);
            RealSpace3::SceneManager::getInstance().setCreationCameraAutoOrbit(enabled);
            return JSValueMakeUndefined(ctx);
        });

        bind("reset_creation_camera", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            RealSpace3::SceneManager::getInstance().resetCreationCamera();
            return JSValueMakeUndefined(ctx);
        });

        bind("list_stages", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            std::string payload = "{}";
            if (argc >= 1) payload = JSValueToStdString(ctx, argv[0]);
            if (payload.empty()) payload = "{}";

            Nakama::NakamaManager::getInstance().listStages(payload, [](bool s, const std::string& response) {
                if (s) {
                    SendToUI("onStageList", response);
                } else {
                    SendToUI("onLobbyError", "{\"scope\":\"list_stages\",\"message\":\"" + JsonEscape(response) + "\"}");
                }
            });
            return JSValueMakeUndefined(ctx);
        });

        bind("create_stage", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            std::string payload = "{}";
            if (argc >= 1) payload = JSValueToStdString(ctx, argv[0]);
            if (payload.empty()) payload = "{}";

            Nakama::NakamaManager::getInstance().createStage(payload, [](bool s, const std::string& response) {
                if (s) {
                    SendToUI("onCreateStageResult", "{\"success\":true,\"data\":" + response + "}");
                } else {
                    SendToUI("onCreateStageResult", "{\"success\":false,\"message\":\"" + JsonEscape(response) + "\"}");
                }
            });
            return JSValueMakeUndefined(ctx);
        });

        bind("fetch_bootstrap_v2", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            Nakama::NakamaManager::getInstance().getBootstrapV2([](bool s, const std::string& response) {
                if (s) {
                    SendToUI("onBootstrapV2", response);
                } else {
                    const std::string msg = JsonEscape(response);
                    SendToUI("onBootstrapV2", "{\"ok\":false,\"reason\":\"rpc_error\",\"message\":\"" + msg + "\"}");
                    SendToUI("onRtProtocolError", "{\"code\":\"BOOTSTRAP_ERROR\",\"detail\":\"" + msg + "\"}");
                }
            });
            return JSValueMakeUndefined(ctx);
        });

        bind("fetch_game_data", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            const std::string key = (argc >= 1) ? JSValueToStdString(ctx, argv[0]) : "";
            Nakama::NakamaManager::getInstance().getGameData(key, [key](bool s, const std::string& response) {
                const std::string safeKey = JsonEscape(key);
                if (s) {
                    SendToUI("onGameDataResult", "{\"key\":\"" + safeKey + "\",\"data\":" + response + "}");
                } else {
                    const std::string msg = JsonEscape(response);
                    SendToUI("onGameDataResult", "{\"key\":\"" + safeKey + "\",\"data\":null,\"error\":\"" + msg + "\"}");
                    SendToUI("onRtProtocolError", "{\"code\":\"GAME_DATA_ERROR\",\"detail\":\"" + msg + "\"}");
                }
            });
            return JSValueMakeUndefined(ctx);
        });

        bind("list_inventory", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            Nakama::NakamaManager::getInstance().listInventory([](bool s, const std::string& response) {
                if (s) {
                    SendToUI("onInventoryResult", "{\"success\":true,\"data\":" + response + "}");
                } else {
                    SendToUI("onInventoryResult", "{\"success\":false,\"message\":\"" + JsonEscape(response) + "\"}");
                }
            });
            return JSValueMakeUndefined(ctx);
        });

        bind("list_char_inventory", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            const std::string charId = (argc >= 1) ? JSValueToStdString(ctx, argv[0]) : "";
            Nakama::NakamaManager::getInstance().listCharInventory(charId, [](bool s, const std::string& response) {
                if (s) {
                    SendToUI("onCharInventoryResult", "{\"success\":true,\"data\":" + response + "}");
                } else {
                    SendToUI("onCharInventoryResult", "{\"success\":false,\"message\":\"" + JsonEscape(response) + "\"}");
                }
            });
            return JSValueMakeUndefined(ctx);
        });

        bind("bring_account_item", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            const std::string charId = (argc >= 1) ? JSValueToStdString(ctx, argv[0]) : "";
            const std::string instanceId = (argc >= 2) ? JSValueToStdString(ctx, argv[1]) : "";
            const int count = (argc >= 3) ? static_cast<int>(JSValueToNumber(ctx, argv[2], nullptr)) : 1;
            Nakama::NakamaManager::getInstance().bringAccountItem(charId, instanceId, count, [](bool s, const std::string& response) {
                if (s) {
                    SendToUI("onBringAccountItemResult", "{\"success\":true,\"data\":" + response + "}");
                } else {
                    SendToUI("onBringAccountItemResult", "{\"success\":false,\"message\":\"" + JsonEscape(response) + "\"}");
                }
            });
            return JSValueMakeUndefined(ctx);
        });

        bind("bring_back_account_item", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            const std::string charId = (argc >= 1) ? JSValueToStdString(ctx, argv[0]) : "";
            const std::string instanceId = (argc >= 2) ? JSValueToStdString(ctx, argv[1]) : "";
            const int count = (argc >= 3) ? static_cast<int>(JSValueToNumber(ctx, argv[2], nullptr)) : 1;
            Nakama::NakamaManager::getInstance().bringBackAccountItem(charId, instanceId, count, [](bool s, const std::string& response) {
                if (s) {
                    SendToUI("onBringBackAccountItemResult", "{\"success\":true,\"data\":" + response + "}");
                } else {
                    SendToUI("onBringBackAccountItemResult", "{\"success\":false,\"message\":\"" + JsonEscape(response) + "\"}");
                }
            });
            return JSValueMakeUndefined(ctx);
        });

        bind("equip_item", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            const std::string charId = (argc >= 1) ? JSValueToStdString(ctx, argv[0]) : "";
            const std::string instanceId = (argc >= 2) ? JSValueToStdString(ctx, argv[1]) : "";
            const std::string slot = (argc >= 3) ? JSValueToStdString(ctx, argv[2]) : "";
            Nakama::NakamaManager::getInstance().equipItem(charId, instanceId, slot, [](bool s, const std::string& response) {
                if (s) {
                    SendToUI("onEquipItemResult", "{\"success\":true,\"data\":" + response + "}");
                } else {
                    SendToUI("onEquipItemResult", "{\"success\":false,\"message\":\"" + JsonEscape(response) + "\"}");
                }
            });
            return JSValueMakeUndefined(ctx);
        });

        bind("takeoff_item", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            const std::string charId = (argc >= 1) ? JSValueToStdString(ctx, argv[0]) : "";
            const std::string slot = (argc >= 2) ? JSValueToStdString(ctx, argv[1]) : "";
            Nakama::NakamaManager::getInstance().takeoffItem(charId, slot, [](bool s, const std::string& response) {
                if (s) {
                    SendToUI("onTakeoffItemResult", "{\"success\":true,\"data\":" + response + "}");
                } else {
                    SendToUI("onTakeoffItemResult", "{\"success\":false,\"message\":\"" + JsonEscape(response) + "\"}");
                }
            });
            return JSValueMakeUndefined(ctx);
        });

        bind("list_shop", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            const std::string payload = (argc >= 1) ? JSValueToStdString(ctx, argv[0]) : "{}";
            Nakama::NakamaManager::getInstance().listShop(payload, [](bool s, const std::string& response) {
                if (s) {
                    SendToUI("onShopListResult", "{\"success\":true,\"data\":" + response + "}");
                } else {
                    SendToUI("onShopListResult", "{\"success\":false,\"message\":\"" + JsonEscape(response) + "\"}");
                }
            });
            return JSValueMakeUndefined(ctx);
        });

        bind("buy_item", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            const int itemId = (argc >= 1) ? static_cast<int>(JSValueToNumber(ctx, argv[0], nullptr)) : 0;
            const int count = (argc >= 2) ? static_cast<int>(JSValueToNumber(ctx, argv[1], nullptr)) : 1;
            Nakama::NakamaManager::getInstance().buyItem(itemId, count, [](bool s, const std::string& response) {
                if (s) {
                    SendToUI("onBuyItemResult", "{\"success\":true,\"data\":" + response + "}");
                } else {
                    SendToUI("onBuyItemResult", "{\"success\":false,\"message\":\"" + JsonEscape(response) + "\"}");
                }
            });
            return JSValueMakeUndefined(ctx);
        });

        bind("sell_item", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            const std::string instanceId = (argc >= 1) ? JSValueToStdString(ctx, argv[0]) : "";
            const int count = (argc >= 2) ? static_cast<int>(JSValueToNumber(ctx, argv[1], nullptr)) : 1;
            Nakama::NakamaManager::getInstance().sellItem(instanceId, count, [](bool s, const std::string& response) {
                if (s) {
                    SendToUI("onSellItemResult", "{\"success\":true,\"data\":" + response + "}");
                } else {
                    SendToUI("onSellItemResult", "{\"success\":false,\"message\":\"" + JsonEscape(response) + "\"}");
                }
            });
            return JSValueMakeUndefined(ctx);
        });

        bind("send_client_ready", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            const std::string recipeHash = (argc >= 1) ? JSValueToStdString(ctx, argv[0]) : "";
            const std::string contentHash = (argc >= 2) ? JSValueToStdString(ctx, argv[1]) : "";
            Nakama::NakamaManager::getInstance().sendClientReady(recipeHash, contentHash);
            return JSValueMakeUndefined(ctx);
        });

        bind("join_stage", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            if (argc < 1) return JSValueMakeUndefined(ctx);
            const std::string matchId = JSValueToStdString(ctx, argv[0]);
            const std::string password = (argc >= 2) ? JSValueToStdString(ctx, argv[1]) : "";

            Nakama::NakamaManager::getInstance().joinStage(matchId, password, [](bool s, const std::string& response) {
                if (s) {
                    SendToUI("onJoinStageResult", "{\"success\":true,\"data\":" + response + "}");
                } else {
                    SendToUI("onJoinStageResult", "{\"success\":false,\"message\":\"" + JsonEscape(response) + "\"}");
                }
            });
            return JSValueMakeUndefined(ctx);
        });

        bind("leave_stage", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            Nakama::NakamaManager::getInstance().leaveStage([](bool s, const std::string& response) {
                if (s) {
                    SendToUI("onLeaveStageResult", "{\"success\":true}");
                } else {
                    SendToUI("onLeaveStageResult", "{\"success\":false,\"message\":\"" + JsonEscape(response) + "\"}");
                }
            });
            return JSValueMakeUndefined(ctx);
        });

        bind("request_stage_state", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            std::string matchId = (argc >= 1) ? JSValueToStdString(ctx, argv[0]) : "";
            Nakama::NakamaManager::getInstance().requestStageState(matchId, [](bool s, const std::string& response) {
                if (s) {
                    SendToUI("onStageState", response);
                } else {
                    SendToUI("onLobbyError", "{\"scope\":\"request_stage_state\",\"message\":\"" + JsonEscape(response) + "\"}");
                }
            });
            return JSValueMakeUndefined(ctx);
        });

        bind("set_stage_ready", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            const std::string matchId = (argc >= 1) ? JSValueToStdString(ctx, argv[0]) : "";
            const bool ready = (argc >= 2) ? JSValueToBoolean(ctx, argv[1]) : false;
            Nakama::NakamaManager::getInstance().setStageReady(matchId, ready, [](bool s, const std::string& response) {
                if (s) {
                    SendToUI("onStageReadyResult", "{\"success\":true,\"data\":" + response + "}");
                } else {
                    SendToUI("onStageReadyResult", "{\"success\":false,\"message\":\"" + JsonEscape(response) + "\"}");
                }
            });
            return JSValueMakeUndefined(ctx);
        });

        bind("set_stage_team", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            const std::string matchId = (argc >= 1) ? JSValueToStdString(ctx, argv[0]) : "";
            const int team = (argc >= 2) ? static_cast<int>(JSValueToNumber(ctx, argv[1], nullptr)) : 0;
            Nakama::NakamaManager::getInstance().setStageTeam(matchId, team, [](bool s, const std::string& response) {
                if (s) {
                    SendToUI("onStageTeamResult", "{\"success\":true,\"data\":" + response + "}");
                } else {
                    SendToUI("onStageTeamResult", "{\"success\":false,\"message\":\"" + JsonEscape(response) + "\"}");
                }
            });
            return JSValueMakeUndefined(ctx);
        });

        bind("stage_chat", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            if (argc < 2) return JSValueMakeUndefined(ctx);
            const std::string matchId = JSValueToStdString(ctx, argv[0]);
            const std::string message = JSValueToStdString(ctx, argv[1]);
            Nakama::NakamaManager::getInstance().stageChat(matchId, message, [](bool s, const std::string& response) {
                if (!s) {
                    SendToUI("onLobbyError", "{\"scope\":\"stage_chat\",\"message\":\"" + JsonEscape(response) + "\"}");
                }
            });
            return JSValueMakeUndefined(ctx);
        });

        bind("stage_start", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            const std::string matchId = (argc >= 1) ? JSValueToStdString(ctx, argv[0]) : "";
            Nakama::NakamaManager::getInstance().startStage(matchId, [](bool s, const std::string& response) {
                if (s) {
                    SendToUI("onStageStartResult", "{\"success\":true,\"data\":" + response + "}");
                } else {
                    SendToUI("onStageStartResult", "{\"success\":false,\"message\":\"" + JsonEscape(response) + "\"}");
                }
            });
            return JSValueMakeUndefined(ctx);
        });

        bind("stage_end", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            const std::string matchId = (argc >= 1) ? JSValueToStdString(ctx, argv[0]) : "";
            Nakama::NakamaManager::getInstance().endStage(matchId, [](bool s, const std::string& response) {
                if (s) {
                    SendToUI("onStageEndResult", "{\"success\":true,\"data\":" + response + "}");
                } else {
                    SendToUI("onStageEndResult", "{\"success\":false,\"message\":\"" + JsonEscape(response) + "\"}");
                }
            });
            return JSValueMakeUndefined(ctx);
        });

        Nakama::NakamaManager::getInstance().setRtMatchDataCallback([](int64_t opCode, const std::string& data) {
            SendToUI("onStageRtMessage",
                "{\"opCode\":" + std::to_string(opCode) + ",\"data\":\"" + JsonEscape(data) + "\"}");
        });

        const std::string userId = Nakama::NakamaManager::getInstance().getSessionUserId();
        if (!userId.empty()) {
            const std::string username = Nakama::NakamaManager::getInstance().getSessionUsername();
            SendToUI("onSessionInfo",
                "{\"userId\":\"" + JsonEscape(userId) + "\",\"username\":\"" + JsonEscape(username) + "\"}");
        }
    }
};

static UIListener g_uiListener;

void UIManager::init(int width, int height) {
    if (width <= 0 || height <= 0) return;
    m_width = width; m_height = height;
    char buffer[MAX_PATH]; _getcwd(buffer, MAX_PATH);
    std::string rootPath = buffer; std::replace(rootPath.begin(), rootPath.end(), '\\', '/');
    Settings settings; settings.app_name = "OpenGunZ"; settings.file_system_path = rootPath.c_str(); 
    Config config; config.resource_path_prefix = "resources/";
    RefPtr<App> app = App::Create(settings, config);
    if (!app) return;
    app->AddRef(); _app = (void*)app.get();
    RefPtr<Renderer> renderer = app->renderer();
    renderer->AddRef(); _renderer = (void*)renderer.get();
    ViewConfig viewConfig; viewConfig.is_accelerated = false; viewConfig.is_transparent = true; 
    RefPtr<View> view = renderer->CreateView(width, height, viewConfig, nullptr);
    if (view) {
        view->AddRef(); _view = (void*)view.get();
        view->set_load_listener(&g_uiListener);
        view->LoadURL(("file:///" + rootPath + "/ui/loading.html").c_str());
    }
}

void UIManager::resize(int width, int height) {
    if (_view && width > 0 && height > 0) {
        m_width = width; m_height = height;
        VIEW->Resize(width, height);
        m_forceRepaintFrames = 0;
    }
}

void UIManager::update() { 
    std::string urlToLoad;
    { std::lock_guard<std::mutex> lock(m_urlMutex); if (!m_pendingURL.empty()) { urlToLoad = m_pendingURL; m_pendingURL.clear(); } }
    if (!urlToLoad.empty() && _view) {
        VIEW->LoadURL(urlToLoad.c_str());
        // Force a short repaint burst after navigation. This avoids stale loading frames
        // that only refresh after an external resize event.
        if (m_width > 0 && m_height > 0) {
            VIEW->Resize(m_width, m_height);
        }
        m_forceRepaintFrames = 6;
    }

    if (_renderer) ((Renderer*)_renderer)->Update();

    if (_view && m_forceRepaintFrames > 0) {
        if (m_width > 0 && m_height > 0) {
            VIEW->Resize(m_width, m_height);
        }
        --m_forceRepaintFrames;
    }
}

void UIManager::render() { if (_renderer) ((Renderer*)_renderer)->Render(); }
void UIManager::onMouseMove(int x, int y) { if (_view) { MouseEvent evt; evt.type = MouseEvent::kType_MouseMoved; evt.x = x; evt.y = y; evt.button = MouseEvent::kButton_None; VIEW->FireMouseEvent(evt); } }
void UIManager::onMouseDown(int x, int y) { if (_view) { MouseEvent evt; evt.type = MouseEvent::kType_MouseDown; evt.x = x; evt.y = y; evt.button = MouseEvent::kButton_Left; VIEW->FireMouseEvent(evt); } }
void UIManager::onMouseUp(int x, int y) { if (_view) { MouseEvent evt; evt.type = MouseEvent::kType_MouseUp; evt.x = x; evt.y = y; evt.button = MouseEvent::kButton_Left; VIEW->FireMouseEvent(evt); } }
void UIManager::onKey(UINT msg, WPARAM wp, LPARAM lp) { if (_view) { KeyEvent::Type t; switch(msg){case WM_KEYDOWN:t=KeyEvent::kType_RawKeyDown;break;case WM_KEYUP:t=KeyEvent::kType_KeyUp;break;case WM_CHAR:t=KeyEvent::kType_Char;break;default:return;} KeyEvent e(t,(uintptr_t)wp,(intptr_t)lp,false); VIEW->FireKeyEvent(e); } }
unsigned char* UIManager::getLockPixels(uint32_t& rb, uint32_t& w, uint32_t& h) { if (!_view) return nullptr; Surface* s = VIEW->surface(); if (!s) return nullptr; rb = s->row_bytes(); w = s->width(); h = s->height(); return (unsigned char*)s->LockPixels(); }
void UIManager::unlockPixels() { if (_view && VIEW->surface()) VIEW->surface()->UnlockPixels(); }
void UIManager::loadURL(const std::string& url) { std::lock_guard<std::mutex> lock(m_urlMutex); m_pendingURL = url; }
void UIManager::setProgress(float p) { if (_view) VIEW->EvaluateScript(("if(window.setProgress) setProgress(" + std::to_string(p) + ")").c_str()); }
void UIManager::setStatus(const std::string& s) { if (_view) VIEW->EvaluateScript(("if(window.setStatus) setStatus('" + s + "')").c_str()); }
void* UIManager::getView() { return _view; }
