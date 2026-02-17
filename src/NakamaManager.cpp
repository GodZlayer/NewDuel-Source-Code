#include "NakamaManager.h"
#include "AppLogger.h"
#include <iostream>
#include <algorithm>
#include <cstdio>

namespace Nakama {

std::string NakamaManager::escapeJson(const std::string& value) {
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

void NakamaManager::resolveRtConnectWaiters(bool success, const std::string& message) {
    std::vector<std::function<void(bool, const std::string&)>> waiters;
    {
        std::lock_guard<std::mutex> lock(_rtMutex);
        _rtConnecting = false;
        waiters.swap(_rtConnectWaiters);
    }
    for (auto& waiter : waiters) {
        if (waiter) waiter(success, message);
    }
}

void NakamaManager::ensureRtClient() {
    if (_rtClient || !_client) return;

    _rtClient = _client->createRtClient();
    if (!_rtClient) {
        AppLogger::Log("Nakama RT: falha ao criar cliente RT.");
        return;
    }

    _rtListener = std::make_shared<NRtDefaultClientListener>();
    _rtListener->setConnectCallback([this]() {
        AppLogger::Log("Nakama RT: conectado.");
        resolveRtConnectWaiters(true, "");
    });
    _rtListener->setDisconnectCallback([this](const NRtClientDisconnectInfo& info) {
        _currentStageMatchId.clear();
        AppLogger::Log("Nakama RT: desconectado (" + std::to_string(info.code) + ") " + info.reason);
    });
    _rtListener->setErrorCallback([this](const NRtError& error) {
        AppLogger::Log("Nakama RT erro: " + error.message);
        bool connecting = false;
        {
            std::lock_guard<std::mutex> lock(_rtMutex);
            connecting = _rtConnecting;
        }
        if (connecting) {
            resolveRtConnectWaiters(false, error.message);
        }
    });
    _rtListener->setMatchDataCallback([this](const NMatchData& matchData) {
        std::function<void(int64_t, const std::string&)> callback;
        {
            std::lock_guard<std::mutex> lock(_rtMutex);
            callback = _rtMatchDataCallback;
        }
        if (callback) callback(matchData.opCode, matchData.data);
    });

    _rtClient->setListener(_rtListener.get());
}

void NakamaManager::ensureRtConnected(std::function<void(bool, const std::string&)> callback) {
    if (!_session) {
        callback(false, "No session");
        return;
    }

    ensureRtClient();
    if (!_rtClient) {
        callback(false, "RT client unavailable");
        return;
    }

    if (_rtClient->isConnected()) {
        callback(true, "");
        return;
    }

    bool shouldConnect = false;
    {
        std::lock_guard<std::mutex> lock(_rtMutex);
        _rtConnectWaiters.push_back(callback);
        if (!_rtConnecting) {
            _rtConnecting = true;
            shouldConnect = true;
        }
    }

    if (!shouldConnect) return;

    try {
        _rtClient->connect(_session, true);
    } catch (const std::exception& e) {
        resolveRtConnectWaiters(false, e.what());
    } catch (...) {
        resolveRtConnectWaiters(false, "RT connect unknown error");
    }
}

void NakamaManager::rpcCall(const std::string& rpcId, const std::string& payload, std::function<void(bool, const std::string&)> callback) {
    if (!_session || !_client) {
        callback(false, "No session");
        return;
    }
    _client->rpc(
        _session, rpcId, payload,
        [callback](const NRpc& rpc) { callback(true, rpc.payload); },
        [callback](const NError& err) { callback(false, err.message); });
}

void NakamaManager::init(const std::string& host, int port, const std::string& serverKey) {
    try {
        AppLogger::Log("Nakama: Preparando conexao com " + host);
        _params.host = host;
        _params.port = port;
        _params.serverKey = serverKey;
        _params.ssl = false;
        
        AppLogger::Log("Nakama: Chamando createDefaultClient...");
        AppLogger::Log(" - Host: " + _params.host);
        AppLogger::Log(" - Port: " + std::to_string(_params.port));
        AppLogger::Log(" - Key: " + _params.serverKey);
        AppLogger::Log(" - SSL: " + std::string(_params.ssl ? "true" : "false"));
        
        try {
            _client = createDefaultClient(_params);
        } catch (const std::bad_alloc& e) {
             AppLogger::Log("Nakama CRITICAL (Std::bad_alloc): " + std::string(e.what()));
             return;
        } catch (const std::exception& e) {
             AppLogger::Log("Nakama CRITICAL (Std::exception): " + std::string(e.what()));
             return;
        }

        
        if (_client) {
            AppLogger::Log("Nakama: Cliente criado com sucesso.");
        } else {
            AppLogger::Log("Nakama: FALHA ao criar cliente (retornou nulo).");
        }
    } catch (const std::exception& e) {
        AppLogger::Log("Nakama CRITICAL EXCEPTION: " + std::string(e.what()));
    } catch (...) {
        AppLogger::Log("Nakama CRITICAL ERROR: Excecao desconhecida.");
    }
}

void NakamaManager::tick() {
    if (_client) _client->tick();
    if (_rtClient) _rtClient->tick();
}

void NakamaManager::authenticateEmail(const std::string& email, const std::string& password, std::function<void(bool, const std::string&)> callback) {
    if (!_client) return;
    _client->authenticateEmail(email, password, "", true, {}, 
        [this, callback](NSessionPtr s) { _session = s; callback(true, ""); },
        [callback](const NError& e) { callback(false, e.message); });
}

void NakamaManager::listCharacters(std::function<void(bool, const std::string&)> callback) {
    rpcCall("list_characters", "{}", callback);
}

void NakamaManager::createCharacter(const std::string& name, int sex, int face, int hair, int costume, std::function<void(bool, const std::string&)> callback) {
    std::string payload = "{\"name\":\"" + escapeJson(name) + "\", \"sex\":" + std::to_string(sex) +
        ", \"face\":" + std::to_string(face) + ", \"hair\":" + std::to_string(hair) +
        ", \"costume\":" + std::to_string(costume) + "}";
    rpcCall("create_character", payload, callback);
}

void NakamaManager::deleteCharacter(const std::string& charId, std::function<void(bool, const std::string&)> callback) {
    rpcCall("delete_character", "{\"charId\":\"" + escapeJson(charId) + "\"}", callback);
}

void NakamaManager::selectCharacter(const std::string& charId, std::function<void(bool, const std::string&)> callback) {
    rpcCall("select_character", "{\"charId\":\"" + escapeJson(charId) + "\"}",
        [callback](bool success, const std::string& payload) {
            if (success) callback(true, "");
            else callback(false, payload);
        });
}

void NakamaManager::listStages(const std::string& filterJson, std::function<void(bool, const std::string&)> callback) {
    const std::string payload = filterJson.empty() ? "{}" : filterJson;
    rpcCall("list_stages", payload, callback);
}

void NakamaManager::createStage(const std::string& createJson, std::function<void(bool, const std::string&)> callback) {
    const std::string payload = createJson.empty() ? "{}" : createJson;
    rpcCall("create_stage", payload, callback);
}

void NakamaManager::joinStage(const std::string& matchId, const std::string& password, std::function<void(bool, const std::string&)> callback) {
    if (matchId.empty()) {
        callback(false, "matchId vazio");
        return;
    }

    rpcCall("join_stage", "{\"matchId\":\"" + escapeJson(matchId) + "\"}",
        [this, matchId, password, callback](bool ok, const std::string& err) {
            if (!ok) {
                callback(false, err);
                return;
            }

            ensureRtConnected([this, matchId, password, callback](bool connected, const std::string& connectErr) {
                if (!connected) {
                    callback(false, connectErr);
                    return;
                }

                auto doJoin = [this, matchId, password, callback]() {
                    NStringMap metadata;
                    if (!password.empty()) metadata["password"] = password;
                    _rtClient->joinMatch(
                        matchId,
                        metadata,
                        [this, callback](const NMatch& match) {
                            _currentStageMatchId = match.matchId;
                            callback(true, "{\"matchId\":\"" + escapeJson(match.matchId) + "\",\"size\":" + std::to_string(match.size) + "}");
                        },
                        [callback](const NRtError& rtErr) {
                            callback(false, rtErr.message);
                        });
                };

                if (!_currentStageMatchId.empty() && _currentStageMatchId != matchId) {
                    const std::string previous = _currentStageMatchId;
                    _rtClient->leaveMatch(
                        previous,
                        [this, doJoin]() {
                            _currentStageMatchId.clear();
                            doJoin();
                        },
                        [this, doJoin](const NRtError&) {
                            _currentStageMatchId.clear();
                            doJoin();
                        });
                    return;
                }

                doJoin();
            });
        });
}

void NakamaManager::leaveStage(std::function<void(bool, const std::string&)> callback) {
    if (!_rtClient || _currentStageMatchId.empty()) {
        callback(true, "{}");
        return;
    }

    const std::string stageId = _currentStageMatchId;
    _rtClient->leaveMatch(
        stageId,
        [this, callback]() {
            _currentStageMatchId.clear();
            callback(true, "{}");
        },
        [callback](const NRtError& err) {
            callback(false, err.message);
        });
}

void NakamaManager::requestStageState(const std::string& matchId, std::function<void(bool, const std::string&)> callback) {
    const std::string target = !matchId.empty() ? matchId : _currentStageMatchId;
    if (target.empty()) {
        callback(false, "matchId vazio");
        return;
    }
    rpcCall("request_stage_state", "{\"matchId\":\"" + escapeJson(target) + "\"}", callback);
}

void NakamaManager::setStageReady(const std::string& matchId, bool ready, std::function<void(bool, const std::string&)> callback) {
    const std::string target = !matchId.empty() ? matchId : _currentStageMatchId;
    if (target.empty()) {
        callback(false, "matchId vazio");
        return;
    }
    rpcCall("set_ready",
        "{\"matchId\":\"" + escapeJson(target) + "\",\"ready\":" + std::string(ready ? "true" : "false") + "}",
        callback);
}

void NakamaManager::setStageTeam(const std::string& matchId, int team, std::function<void(bool, const std::string&)> callback) {
    const std::string target = !matchId.empty() ? matchId : _currentStageMatchId;
    if (target.empty()) {
        callback(false, "matchId vazio");
        return;
    }
    rpcCall("set_team",
        "{\"matchId\":\"" + escapeJson(target) + "\",\"team\":" + std::to_string(team) + "}",
        callback);
}

void NakamaManager::stageChat(const std::string& matchId, const std::string& message, std::function<void(bool, const std::string&)> callback) {
    const std::string target = !matchId.empty() ? matchId : _currentStageMatchId;
    if (target.empty()) {
        callback(false, "matchId vazio");
        return;
    }
    rpcCall("stage_chat",
        "{\"matchId\":\"" + escapeJson(target) + "\",\"message\":\"" + escapeJson(message) + "\"}",
        callback);
}

void NakamaManager::startStage(const std::string& matchId, std::function<void(bool, const std::string&)> callback) {
    const std::string target = !matchId.empty() ? matchId : _currentStageMatchId;
    if (target.empty()) {
        callback(false, "matchId vazio");
        return;
    }
    rpcCall("stage_start", "{\"matchId\":\"" + escapeJson(target) + "\"}", callback);
}

void NakamaManager::endStage(const std::string& matchId, std::function<void(bool, const std::string&)> callback) {
    const std::string target = !matchId.empty() ? matchId : _currentStageMatchId;
    if (target.empty()) {
        callback(false, "matchId vazio");
        return;
    }
    rpcCall("stage_end", "{\"matchId\":\"" + escapeJson(target) + "\"}", callback);
}

void NakamaManager::setRtMatchDataCallback(std::function<void(int64_t, const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(_rtMutex);
    _rtMatchDataCallback = std::move(callback);
}

void NakamaManager::joinMatch(std::function<void(bool, const std::string&)> callback) {
    if (!_session) { callback(false, "No session"); return; }
    ensureRtConnected([this, callback](bool ok, const std::string& err) {
        if (!ok) {
            callback(false, err);
            return;
        }
        _rtClient->createMatch(
            [this, callback](const NMatch& m) {
                _currentStageMatchId = m.matchId;
                callback(true, m.matchId);
            },
            [callback](const NRtError& e) { callback(false, e.message); });
    });
}

void NakamaManager::sendMatchData(int64_t opCode, const std::string& data) {
    if (!_rtClient || !_session || _currentStageMatchId.empty()) return;
    _rtClient->sendMatchData(_currentStageMatchId, opCode, data);
}

} // namespace Nakama
