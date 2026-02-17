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
        AppLogger::Log("INTERFACE: Pagina ativa -> " + std::string(url.utf8().data()));
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
                Nakama::NakamaManager::getInstance().authenticateEmail(u, p, [](bool s, const std::string& e) {
                    if (s) {
                        UIManager::getInstance().loadURL(BuildFileUrl("/ui/character_selection.html"));
                    } else {
                        std::string js = "if(window.setAuthStatus) setAuthStatus('ERRO: CREDENCIAIS INVALIDAS', false)";
                        ((View*)UIManager::getInstance().getView())->EvaluateScript(js.c_str());
                    }
                });
            }
            return JSValueMakeUndefined(ctx);
        });

        bind("list_characters", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            Nakama::NakamaManager::getInstance().listCharacters([](bool s, const std::string& p) {
                if (s) SendToUI("onCharacterList", p);
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

        bind("set_character_preview", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            if (argc < 4) return JSValueMakeUndefined(ctx);
            const int sex = static_cast<int>(JSValueToNumber(ctx, argv[0], nullptr));
            const int face = static_cast<int>(JSValueToNumber(ctx, argv[1], nullptr));
            const int preset = static_cast<int>(JSValueToNumber(ctx, argv[2], nullptr));
            const int hair = static_cast<int>(JSValueToNumber(ctx, argv[3], nullptr));
            RealSpace3::SceneManager::getInstance().setCreationPreview(sex, face, preset, hair);
            return JSValueMakeUndefined(ctx);
        });

        bind("set_preview_visible", [](JSContextRef ctx, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef* ex) -> JSValueRef {
            if (argc < 1) return JSValueMakeUndefined(ctx);
            const bool visible = JSValueToBoolean(ctx, argv[0]);
            RealSpace3::SceneManager::getInstance().setCreationPreviewVisible(visible);
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
    }
}

void UIManager::update() { 
    if (_renderer) ((Renderer*)_renderer)->Update(); 
    std::string urlToLoad;
    { std::lock_guard<std::mutex> lock(m_urlMutex); if (!m_pendingURL.empty()) { urlToLoad = m_pendingURL; m_pendingURL.clear(); } }
    if (!urlToLoad.empty() && _view) VIEW->LoadURL(urlToLoad.c_str());
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
