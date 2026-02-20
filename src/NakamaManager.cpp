#include "NakamaManager.h"
#include "NakamaIPv4HttpTransport.h"
#include "AppLogger.h"
#include <iostream>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <chrono>
#include <thread>
#include <cstdlib>

namespace Nakama {

namespace {

std::atomic<uint64_t> gNetEventId{0};

uint64_t NextNetEventId() {
    return ++gNetEventId;
}

std::string BoolText(bool value) {
    return value ? "true" : "false";
}

std::string SummarizePayload(const std::string& payload, size_t limit = 220) {
    if (payload.empty()) return "<empty>";
    std::string flat = payload;
    std::replace(flat.begin(), flat.end(), '\n', ' ');
    std::replace(flat.begin(), flat.end(), '\r', ' ');
    if (flat.size() <= limit) return flat;
    return flat.substr(0, limit) + "...";
}

std::string MaskEmail(const std::string& email) {
    const size_t at = email.find('@');
    if (at == std::string::npos || at == 0) return "***";
    if (at == 1) return std::string(email.substr(0, 1)) + "***";
    return email.substr(0, 2) + "***" + email.substr(at);
}

std::string BuildAuthUsernameFromEmail(const std::string& email) {
    const size_t at = email.find('@');
    const std::string local = (at == std::string::npos) ? email : email.substr(0, at);
    std::string out;
    out.reserve(local.size());
    for (unsigned char c : local) {
        if (std::isalnum(c) || c == '_' || c == '-' || c == '.') {
            out.push_back(static_cast<char>(std::tolower(c)));
        }
    }
    if (out.empty()) {
        out = "ndg_user";
    }
    if (out.size() > 24) {
        out.resize(24);
    }
    return out;
}

std::string TrimCopy(std::string value) {
    auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char c) { return !isSpace(static_cast<unsigned char>(c)); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](char c) { return !isSpace(static_cast<unsigned char>(c)); }).base(), value.end());
    return value;
}

std::string NormalizeAuthError(const std::string& raw) {
    std::string message = TrimCopy(raw);
    std::string lower = message;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    const bool genericOnly = lower.empty() || lower == "message:" || lower == "message" || lower == "error";
    const bool timeoutHint = lower.find("timeout") != std::string::npos ||
        lower.find("timed out") != std::string::npos ||
        lower.find("deadline") != std::string::npos;

    if (genericOnly || timeoutHint) {
        return "Falha de conexao com o servidor (timeout). Verifique host/porta e status do Nakama.";
    }

    return message;
}

bool IsTimeoutLikeAuthError(const std::string& normalizedMessage) {
    std::string lower = normalizedMessage;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lower.find("timeout") != std::string::npos ||
        lower.find("falha de conexao") != std::string::npos ||
        lower.find("nao foi possivel conectar") != std::string::npos;
}

bool IsAccountMissingAuthError(const std::string& raw, const std::string& normalizedMessage) {
    std::string lower = TrimCopy(raw.empty() ? normalizedMessage : raw);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (lower.empty()) return false;

    return
        lower.find("user not found") != std::string::npos ||
        lower.find("account not found") != std::string::npos ||
        lower.find("not found") != std::string::npos ||
        lower.find("no account") != std::string::npos ||
        lower.find("does not exist") != std::string::npos;
}

std::string NormalizeRpcError(const std::string& rpcId, const std::string& raw) {
    const std::string message = TrimCopy(raw);
    std::string lower = message;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    const bool genericOnly =
        lower.empty() ||
        lower == "message:" ||
        lower == "message" ||
        lower == "error" ||
        lower == "rpc error";

    if (genericOnly) {
        return "RPC '" + rpcId + "' falhou sem detalhe do servidor.";
    }

    return message;
}

bool IsTruthyEnv(const char* value) {
    if (!value || !*value) return false;
    std::string lower = TrimCopy(value);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
}

} // namespace

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

void NakamaManager::shutdown() {
    const uint64_t generation = ++_clientGeneration;
    NClientPtr activeClient = _client;
    NSessionPtr activeSession = _session;

    if (_rtClient) {
        try {
            _rtClient->disconnect();
            AppLogger::LogNetwork("[RT] disconnect() requested during shutdown.");
        } catch (const std::exception& e) {
            AppLogger::LogNetwork(std::string("[RT] disconnect() exception during shutdown: ") + e.what());
        } catch (...) {
            AppLogger::LogNetwork("[RT] disconnect() unknown exception during shutdown.");
        }
    }

    if (activeClient && activeSession) {
        std::atomic<bool> logoutDone{false};
        std::atomic<bool> logoutSuccess{false};
        std::mutex logoutStateMutex;
        std::string logoutError;
        try {
            AppLogger::LogNetwork("[AUTH] sessionLogout() requested during shutdown.");
            activeClient->sessionLogout(
                activeSession,
                [&logoutDone, &logoutSuccess]() {
                    logoutSuccess.store(true);
                    logoutDone.store(true);
                },
                [&logoutDone, &logoutStateMutex, &logoutError](const NError& e) {
                    std::lock_guard<std::mutex> lock(logoutStateMutex);
                    logoutError = e.message;
                    logoutDone.store(true);
                });

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
            while (!logoutDone.load() && std::chrono::steady_clock::now() < deadline) {
                activeClient->tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!logoutDone.load()) {
                AppLogger::LogNetwork("[AUTH] sessionLogout() pending/timeout during shutdown (proceeding).");
            } else if (logoutSuccess.load()) {
                AppLogger::LogNetwork("[AUTH] sessionLogout() completed successfully during shutdown.");
            } else {
                std::lock_guard<std::mutex> lock(logoutStateMutex);
                AppLogger::LogNetwork("[AUTH] sessionLogout() failed during shutdown: '" + logoutError + "'");
            }
        } catch (const std::exception& e) {
            AppLogger::LogNetwork(std::string("[AUTH] sessionLogout() exception during shutdown: ") + e.what());
        } catch (...) {
            AppLogger::LogNetwork("[AUTH] sessionLogout() unknown exception during shutdown.");
        }
    }

    {
        std::lock_guard<std::mutex> lock(_rtMutex);
        _rtConnectWaiters.clear();
        _rtConnecting = false;
        _rtMatchDataCallback = nullptr;
    }

    _currentStageMatchId.clear();
    _rtListener.reset();
    _rtClient.reset();
    _session.reset();
    _client.reset();
    _httpTransport.reset();

    AppLogger::LogNetwork("[NET] shutdown/reset generation=" + std::to_string(generation));
}

void NakamaManager::resolveRtConnectWaiters(bool success, const std::string& message) {
    std::vector<std::function<void(bool, const std::string&)>> waiters;
    {
        std::lock_guard<std::mutex> lock(_rtMutex);
        _rtConnecting = false;
        waiters.swap(_rtConnectWaiters);
    }
    AppLogger::LogNetwork("[RT] resolve waiters: success=" + BoolText(success) +
        " count=" + std::to_string(waiters.size()) + " message='" + message + "'");
    for (auto& waiter : waiters) {
        if (waiter) waiter(success, message);
    }
}

void NakamaManager::ensureRtClient() {
    if (_rtClient || !_client) return;

    AppLogger::LogNetwork("[RT] creating realtime client...");
    _rtClient = _client->createRtClient();
    if (!_rtClient) {
        AppLogger::Log("Nakama RT: falha ao criar cliente RT.");
        AppLogger::LogNetwork("[RT] createRtClient returned null.");
        return;
    }
    AppLogger::LogNetwork("[RT] realtime client created.");

    _rtListener = std::make_shared<NRtDefaultClientListener>();
    _rtListener->setConnectCallback([this]() {
        AppLogger::Log("Nakama RT: conectado.");
        AppLogger::LogNetwork("[RT] connected callback.");
        resolveRtConnectWaiters(true, "");
    });
    _rtListener->setDisconnectCallback([this](const NRtClientDisconnectInfo& info) {
        _currentStageMatchId.clear();
        AppLogger::Log("Nakama RT: desconectado (" + std::to_string(info.code) + ") " + info.reason);
        AppLogger::LogNetwork("[RT] disconnected code=" + std::to_string(info.code) + " reason='" + info.reason + "'");
    });
    _rtListener->setErrorCallback([this](const NRtError& error) {
        AppLogger::Log("Nakama RT erro: " + error.message);
        AppLogger::LogNetwork("[RT] error callback: '" + error.message + "'");
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
        AppLogger::LogNetwork("[RT] match data opCode=" + std::to_string(matchData.opCode) +
            " bytes=" + std::to_string(matchData.data.size()));
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
        AppLogger::LogNetwork("[RT] ensureRtConnected aborted: no session.");
        callback(false, "No session");
        return;
    }

    ensureRtClient();
    if (!_rtClient) {
        AppLogger::LogNetwork("[RT] ensureRtConnected aborted: RT client unavailable.");
        callback(false, "RT client unavailable");
        return;
    }

    if (_rtClient->isConnected()) {
        AppLogger::LogNetwork("[RT] ensureRtConnected: already connected.");
        callback(true, "");
        return;
    }

    bool shouldConnect = false;
    size_t waiterCount = 0;
    {
        std::lock_guard<std::mutex> lock(_rtMutex);
        _rtConnectWaiters.push_back(callback);
        waiterCount = _rtConnectWaiters.size();
        if (!_rtConnecting) {
            _rtConnecting = true;
            shouldConnect = true;
        }
    }
    AppLogger::LogNetwork("[RT] ensureRtConnected queued waiter. count=" + std::to_string(waiterCount) +
        " shouldConnect=" + BoolText(shouldConnect));

    if (!shouldConnect) return;

    try {
        AppLogger::LogNetwork("[RT] connecting realtime socket...");
        _rtClient->connect(_session, true);
    } catch (const std::exception& e) {
        AppLogger::LogNetwork(std::string("[RT] connect exception: ") + e.what());
        resolveRtConnectWaiters(false, e.what());
    } catch (...) {
        AppLogger::LogNetwork("[RT] connect unknown exception.");
        resolveRtConnectWaiters(false, "RT connect unknown error");
    }
}

void NakamaManager::rpcCall(const std::string& rpcId, const std::string& payload, std::function<void(bool, const std::string&)> callback) {
    if (!_session || !_client) {
        AppLogger::LogNetwork("[RPC] aborted id='" + rpcId + "' reason='No session/client'.");
        callback(false, "No session");
        return;
    }

    const uint64_t reqId = NextNetEventId();
    const uint64_t expectedGeneration = _clientGeneration.load();
    const auto startedAt = std::chrono::steady_clock::now();
    AppLogger::LogNetwork("[RPC#" + std::to_string(reqId) + "] -> id='" + rpcId + "' payload_bytes=" +
        std::to_string(payload.size()) + " payload='" + SummarizePayload(payload) + "'");

    _client->rpc(
        _session, rpcId, payload,
        [this, callback, reqId, rpcId, startedAt, expectedGeneration](const NRpc& rpc) {
            if (_clientGeneration.load() != expectedGeneration) {
                AppLogger::LogNetwork("[RPC#" + std::to_string(reqId) + "] drop stale success due to generation mismatch.");
                return;
            }
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startedAt).count();
            AppLogger::LogNetwork("[RPC#" + std::to_string(reqId) + "] <- ok id='" + rpcId +
                "' elapsed_ms=" + std::to_string(elapsedMs) +
                " response_bytes=" + std::to_string(rpc.payload.size()) +
                " response='" + SummarizePayload(rpc.payload) + "'");
            callback(true, rpc.payload);
        },
        [this, callback, reqId, rpcId, startedAt, expectedGeneration](const NError& err) {
            if (_clientGeneration.load() != expectedGeneration) {
                AppLogger::LogNetwork("[RPC#" + std::to_string(reqId) + "] drop stale error due to generation mismatch.");
                return;
            }
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startedAt).count();
            const std::string normalized = NormalizeRpcError(rpcId, err.message);
            AppLogger::LogNetwork("[RPC#" + std::to_string(reqId) + "] <- err id='" + rpcId +
                "' elapsed_ms=" + std::to_string(elapsedMs) +
                " raw='" + err.message + "' normalized='" + normalized + "'");
            callback(false, normalized);
        });
}

void NakamaManager::init(const std::string& host, int port, const std::string& serverKey, bool useSSL) {
    shutdown();
    const uint64_t reqId = NextNetEventId();
    const auto startedAt = std::chrono::steady_clock::now();
    const bool forceIPv4 = !IsTruthyEnv(std::getenv("NDG_NAKAMA_DISABLE_FORCE_IPV4"));
    try {
        AppLogger::Log("Nakama: Preparando conexao com " + host);
        AppLogger::LogNetwork("[INIT#" + std::to_string(reqId) + "] -> host='" + host +
            "' port=" + std::to_string(port) +
            " ssl=" + BoolText(useSSL) +
            " timeout_ms=30000" +
            " force_ipv4=" + BoolText(forceIPv4) +
            " key_len=" + std::to_string(serverKey.size()));
        _params.host = host;
        _params.port = port;
        _params.serverKey = serverKey;
        _params.ssl = useSSL;
        _params.timeout = std::chrono::seconds(30);
        
        AppLogger::Log("Nakama: Chamando createDefaultClient...");
        AppLogger::Log(" - Host: " + _params.host);
        AppLogger::Log(" - Port: " + std::to_string(_params.port));
        AppLogger::Log(" - Key: " + _params.serverKey);
        AppLogger::Log(" - SSL: " + std::string(_params.ssl ? "true" : "false"));
        AppLogger::Log(" - Timeout(ms): " + std::to_string(_params.timeout.count()));
        AppLogger::Log(" - ForceIPv4: " + std::string(forceIPv4 ? "true" : "false"));
        AppLogger::LogNetwork("[INIT#" + std::to_string(reqId) + "] createRestClient(custom_http_transport)...");
        
        try {
            _httpTransport = std::make_shared<NakamaIPv4HttpTransport>(forceIPv4);
            _client = createRestClient(_params, _httpTransport);
        } catch (const std::bad_alloc& e) {
             AppLogger::Log("Nakama CRITICAL (Std::bad_alloc): " + std::string(e.what()));
             const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - startedAt).count();
             AppLogger::LogNetwork("[INIT#" + std::to_string(reqId) + "] <- err bad_alloc elapsed_ms=" +
                 std::to_string(elapsedMs) + " message='" + e.what() + "'");
             return;
        } catch (const std::exception& e) {
             AppLogger::Log("Nakama CRITICAL (Std::exception): " + std::string(e.what()));
             const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - startedAt).count();
             AppLogger::LogNetwork("[INIT#" + std::to_string(reqId) + "] <- err exception elapsed_ms=" +
                 std::to_string(elapsedMs) + " message='" + e.what() + "'");
             return;
        }

        
        if (_client) {
            AppLogger::Log("Nakama: Cliente criado com sucesso.");
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startedAt).count();
            AppLogger::LogNetwork("[INIT#" + std::to_string(reqId) + "] <- ok elapsed_ms=" +
                std::to_string(elapsedMs));
        } else {
            AppLogger::Log("Nakama: FALHA ao criar cliente (retornou nulo).");
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startedAt).count();
            AppLogger::LogNetwork("[INIT#" + std::to_string(reqId) + "] <- err elapsed_ms=" +
                std::to_string(elapsedMs) + " message='createDefaultClient returned null'");
        }
    } catch (const std::exception& e) {
        AppLogger::Log("Nakama CRITICAL EXCEPTION: " + std::string(e.what()));
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startedAt).count();
        AppLogger::LogNetwork("[INIT#" + std::to_string(reqId) + "] <- err outer_exception elapsed_ms=" +
            std::to_string(elapsedMs) + " message='" + e.what() + "'");
    } catch (...) {
        AppLogger::Log("Nakama CRITICAL ERROR: Excecao desconhecida.");
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startedAt).count();
        AppLogger::LogNetwork("[INIT#" + std::to_string(reqId) + "] <- err unknown elapsed_ms=" +
            std::to_string(elapsedMs));
    }
}

void NakamaManager::tick() {
    if (_client) _client->tick();
    if (_rtClient) _rtClient->tick();
}

void NakamaManager::authenticateEmail(const std::string& email, const std::string& password, std::function<void(bool, const std::string&)> callback) {
    const uint64_t reqId = NextNetEventId();
    const uint64_t expectedGeneration = _clientGeneration.load();
    const auto startedAt = std::chrono::steady_clock::now();
    const std::string maskedEmail = MaskEmail(email);
    const std::string authUsername = BuildAuthUsernameFromEmail(email);
    if (!_client) {
        AppLogger::Log("Nakama AUTH erro: cliente nao inicializado.");
        AppLogger::LogNetwork("[AUTH#" + std::to_string(reqId) + "] <- err elapsed_ms=0 reason='client_not_initialized'");
        callback(false, "cliente nao inicializado");
        return;
    }
    AppLogger::Log("Nakama AUTH: iniciando login para '" + email + "'.");
    AppLogger::LogNetwork("[AUTH#" + std::to_string(reqId) + "] -> method=email email='" + maskedEmail +
        "' username='" + authUsername + "' password_len=" + std::to_string(password.size()) + " create=false");

    auto attempt = std::make_shared<int>(1);
    auto runAttempt = std::make_shared<std::function<void(bool)>>();
    *runAttempt = [this, email, password, authUsername, callback, reqId, startedAt, attempt, runAttempt, expectedGeneration](bool createOnMissing) {
        if (_clientGeneration.load() != expectedGeneration) {
            AppLogger::LogNetwork("[AUTH#" + std::to_string(reqId) + "] drop attempt due to generation mismatch.");
            callback(false, "cliente reiniciado durante autenticacao");
            return;
        }
        NClientPtr activeClient = _client;
        if (!activeClient) {
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startedAt).count();
            AppLogger::LogNetwork("[AUTH#" + std::to_string(reqId) + "] <- err elapsed_ms=" +
                std::to_string(elapsedMs) + " reason='client_not_initialized_retry'");
            callback(false, "cliente nao inicializado");
            return;
        }

        const std::string usernameForCall = createOnMissing ? authUsername : "";
        AppLogger::LogNetwork("[AUTH#" + std::to_string(reqId) + "] attempt=" + std::to_string(*attempt) +
            " create=" + BoolText(createOnMissing) + " username='" + usernameForCall + "'");

        activeClient->authenticateEmail(email, password, usernameForCall, createOnMissing, {},
            [this, callback, email, reqId, startedAt, attempt, createOnMissing, expectedGeneration](NSessionPtr s) {
                if (_clientGeneration.load() != expectedGeneration) {
                    AppLogger::LogNetwork("[AUTH#" + std::to_string(reqId) + "] drop stale success due to generation mismatch.");
                    return;
                }
                _session = s;
                AppLogger::Log("Nakama AUTH: login OK para '" + email + "'.");
                const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - startedAt).count();
                const std::string userId = s ? s->getUserId() : "";
                const std::string username = s ? s->getUsername() : "";
                AppLogger::LogNetwork("[AUTH#" + std::to_string(reqId) + "] <- ok elapsed_ms=" +
                    std::to_string(elapsedMs) +
                    " attempt=" + std::to_string(*attempt) +
                    " create=" + BoolText(createOnMissing) +
                    " user_id='" + userId + "' username='" + username + "'");
                callback(true, "");
            },
            [this, callback, email, reqId, startedAt, attempt, runAttempt, createOnMissing, expectedGeneration](const NError& e) {
                if (_clientGeneration.load() != expectedGeneration) {
                    AppLogger::LogNetwork("[AUTH#" + std::to_string(reqId) + "] drop stale error due to generation mismatch.");
                    return;
                }
                const std::string normalized = NormalizeAuthError(e.message);
                const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - startedAt).count();

                if (!createOnMissing && IsAccountMissingAuthError(e.message, normalized)) {
                    AppLogger::LogNetwork("[AUTH#" + std::to_string(reqId) + "] account_missing_on_login -> retry create=true");
                    ++(*attempt);
                    (*runAttempt)(true);
                    return;
                }

                AppLogger::Log("Nakama AUTH: login falhou para '" + email + "' -> raw='" + e.message + "' normalized='" + normalized + "'");
                AppLogger::LogNetwork("[AUTH#" + std::to_string(reqId) + "] <- err elapsed_ms=" +
                    std::to_string(elapsedMs) +
                    " attempt=" + std::to_string(*attempt) +
                    " create=" + BoolText(createOnMissing) +
                    " raw='" + e.message + "' normalized='" + normalized + "' retry=disabled");
                callback(false, normalized);
            });
    };

    (*runAttempt)(false);
}

void NakamaManager::listCharacters(std::function<void(bool, const std::string&)> callback) {
    rpcCall("list_characters", "{}", callback);
}

void NakamaManager::createCharacter(const std::string& name, int sex, int face, int hair, int costume, std::function<void(bool, const std::string&)> callback) {
    std::string payload = "{\"name\":\"" + escapeJson(name) + "\", \"sex\":" + std::to_string(sex) +
        ", \"face\":" + std::to_string(face) + ", \"hair\":" + std::to_string(hair) +
        ", \"costume\":" + std::to_string(costume) +
        ", \"preset\":" + std::to_string(costume) + "}";
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

void NakamaManager::getBootstrapV2(std::function<void(bool, const std::string&)> callback) {
    // HTTP-compatible RPC payload format expected by server: body is a JSON string.
    rpcCall("get_bootstrap_v2", "\"{\\\"clientVersion\\\":\\\"ndg-local\\\",\\\"rtVersion\\\":1}\"", callback);
}

void NakamaManager::getGameData(const std::string& key, std::function<void(bool, const std::string&)> callback) {
    if (key.empty()) {
        rpcCall("get_game_data", "{}", callback);
        return;
    }
    rpcCall("get_game_data", "{\"key\":\"" + escapeJson(key) + "\"}", callback);
}

void NakamaManager::listInventory(std::function<void(bool, const std::string&)> callback) {
    rpcCall("list_inventory", "{}", callback);
}

void NakamaManager::listCharInventory(const std::string& charId, std::function<void(bool, const std::string&)> callback) {
    if (charId.empty()) {
        callback(false, "charId vazio");
        return;
    }
    rpcCall("list_char_inventory", "{\"charId\":\"" + escapeJson(charId) + "\"}", callback);
}

void NakamaManager::bringAccountItem(const std::string& charId, const std::string& instanceId, int count, std::function<void(bool, const std::string&)> callback) {
    if (charId.empty()) {
        callback(false, "charId vazio");
        return;
    }
    if (instanceId.empty()) {
        callback(false, "instanceId vazio");
        return;
    }
    const int safeCount = std::max(1, count);
    rpcCall("bring_account_item",
        "{\"charId\":\"" + escapeJson(charId) + "\",\"instanceId\":\"" + escapeJson(instanceId) + "\",\"count\":" + std::to_string(safeCount) + "}",
        callback);
}

void NakamaManager::bringBackAccountItem(const std::string& charId, const std::string& instanceId, int count, std::function<void(bool, const std::string&)> callback) {
    if (charId.empty()) {
        callback(false, "charId vazio");
        return;
    }
    if (instanceId.empty()) {
        callback(false, "instanceId vazio");
        return;
    }
    const int safeCount = std::max(1, count);
    rpcCall("bring_back_account_item",
        "{\"charId\":\"" + escapeJson(charId) + "\",\"instanceId\":\"" + escapeJson(instanceId) + "\",\"count\":" + std::to_string(safeCount) + "}",
        callback);
}

void NakamaManager::equipItem(const std::string& charId, const std::string& instanceId, const std::string& slot, std::function<void(bool, const std::string&)> callback) {
    if (charId.empty()) {
        callback(false, "charId vazio");
        return;
    }
    if (instanceId.empty()) {
        callback(false, "instanceId vazio");
        return;
    }
    if (slot.empty()) {
        callback(false, "slot vazio");
        return;
    }
    rpcCall("equip_item",
        "{\"charId\":\"" + escapeJson(charId) + "\",\"instanceId\":\"" + escapeJson(instanceId) + "\",\"slot\":\"" + escapeJson(slot) + "\"}",
        callback);
}

void NakamaManager::takeoffItem(const std::string& charId, const std::string& slot, std::function<void(bool, const std::string&)> callback) {
    if (charId.empty()) {
        callback(false, "charId vazio");
        return;
    }
    if (slot.empty()) {
        callback(false, "slot vazio");
        return;
    }
    rpcCall("takeoff_item",
        "{\"charId\":\"" + escapeJson(charId) + "\",\"slot\":\"" + escapeJson(slot) + "\"}",
        callback);
}

void NakamaManager::listShop(const std::string& filterJson, std::function<void(bool, const std::string&)> callback) {
    const std::string payload = filterJson.empty() ? "{}" : filterJson;
    rpcCall("list_shop", payload, callback);
}

void NakamaManager::buyItem(int itemId, int count, std::function<void(bool, const std::string&)> callback) {
    const int safeCount = std::max(1, count);
    rpcCall("buy_item",
        "{\"itemId\":" + std::to_string(itemId) + ",\"count\":" + std::to_string(safeCount) + "}",
        callback);
}

void NakamaManager::sellItem(const std::string& instanceId, int count, std::function<void(bool, const std::string&)> callback) {
    if (instanceId.empty()) {
        callback(false, "instanceId vazio");
        return;
    }
    const int safeCount = std::max(1, count);
    rpcCall("sell_item",
        "{\"instanceId\":\"" + escapeJson(instanceId) + "\",\"count\":" + std::to_string(safeCount) + "}",
        callback);
}

void NakamaManager::joinStage(const std::string& matchId, const std::string& password, std::function<void(bool, const std::string&)> callback) {
    const uint64_t reqId = NextNetEventId();
    const auto startedAt = std::chrono::steady_clock::now();
    AppLogger::LogNetwork("[STAGE-JOIN#" + std::to_string(reqId) + "] -> matchId='" + matchId +
        "' password=" + std::string(password.empty() ? "<empty>" : "<provided>"));

    if (matchId.empty()) {
        AppLogger::LogNetwork("[STAGE-JOIN#" + std::to_string(reqId) + "] <- err elapsed_ms=0 reason='matchId_empty'");
        callback(false, "matchId vazio");
        return;
    }

    rpcCall("join_stage", "{\"matchId\":\"" + escapeJson(matchId) + "\"}",
        [this, matchId, password, callback, reqId, startedAt](bool ok, const std::string& err) {
            if (!ok) {
                const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - startedAt).count();
                AppLogger::LogNetwork("[STAGE-JOIN#" + std::to_string(reqId) + "] <- err elapsed_ms=" +
                    std::to_string(elapsedMs) + " rpc_error='" + err + "'");
                callback(false, err);
                return;
            }

            AppLogger::LogNetwork("[STAGE-JOIN#" + std::to_string(reqId) + "] rpc ok; ensuring realtime connection.");
            ensureRtConnected([this, matchId, password, callback, reqId, startedAt](bool connected, const std::string& connectErr) {
                if (!connected) {
                    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - startedAt).count();
                    AppLogger::LogNetwork("[STAGE-JOIN#" + std::to_string(reqId) + "] <- err elapsed_ms=" +
                        std::to_string(elapsedMs) + " rt_connect_error='" + connectErr + "'");
                    callback(false, connectErr);
                    return;
                }

                auto doJoin = [this, matchId, password, callback, reqId, startedAt]() {
                    NStringMap metadata;
                    if (!password.empty()) metadata["password"] = password;
                    AppLogger::LogNetwork("[STAGE-JOIN#" + std::to_string(reqId) + "] rt joinMatch -> matchId='" + matchId +
                        "' metadata_password=" + BoolText(!password.empty()));
                    _rtClient->joinMatch(
                        matchId,
                        metadata,
                        [this, callback, reqId, startedAt](const NMatch& match) {
                            _currentStageMatchId = match.matchId;
                            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - startedAt).count();
                            AppLogger::LogNetwork("[STAGE-JOIN#" + std::to_string(reqId) + "] <- ok elapsed_ms=" +
                                std::to_string(elapsedMs) + " matchId='" + match.matchId +
                                "' size=" + std::to_string(match.size));
                            callback(true, "{\"matchId\":\"" + escapeJson(match.matchId) + "\",\"size\":" + std::to_string(match.size) + "}");
                        },
                        [callback, reqId, startedAt](const NRtError& rtErr) {
                            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - startedAt).count();
                            AppLogger::LogNetwork("[STAGE-JOIN#" + std::to_string(reqId) + "] <- err elapsed_ms=" +
                                std::to_string(elapsedMs) + " rt_error='" + rtErr.message + "'");
                            callback(false, rtErr.message);
                        });
                };

                if (!_currentStageMatchId.empty() && _currentStageMatchId != matchId) {
                    const std::string previous = _currentStageMatchId;
                    AppLogger::LogNetwork("[STAGE-JOIN#" + std::to_string(reqId) + "] leaving previous_match='" + previous +
                        "' before joining new match.");
                    _rtClient->leaveMatch(
                        previous,
                        [this, doJoin, reqId]() {
                            _currentStageMatchId.clear();
                            AppLogger::LogNetwork("[STAGE-JOIN#" + std::to_string(reqId) + "] previous leave ok; continuing join.");
                            doJoin();
                        },
                        [this, doJoin, reqId](const NRtError& leaveErr) {
                            _currentStageMatchId.clear();
                            AppLogger::LogNetwork("[STAGE-JOIN#" + std::to_string(reqId) + "] previous leave err='" +
                                leaveErr.message + "'; continuing join.");
                            doJoin();
                        });
                    return;
                }

                doJoin();
            });
        });
}

void NakamaManager::leaveStage(std::function<void(bool, const std::string&)> callback) {
    const uint64_t reqId = NextNetEventId();
    const auto startedAt = std::chrono::steady_clock::now();
    AppLogger::LogNetwork("[STAGE-LEAVE#" + std::to_string(reqId) + "] -> current_match='" + _currentStageMatchId + "'");

    if (!_rtClient || _currentStageMatchId.empty()) {
        AppLogger::LogNetwork("[STAGE-LEAVE#" + std::to_string(reqId) + "] <- ok elapsed_ms=0 reason='no_active_stage'");
        callback(true, "{}");
        return;
    }

    const std::string stageId = _currentStageMatchId;
    _rtClient->leaveMatch(
        stageId,
        [this, callback, reqId, startedAt]() {
            _currentStageMatchId.clear();
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startedAt).count();
            AppLogger::LogNetwork("[STAGE-LEAVE#" + std::to_string(reqId) + "] <- ok elapsed_ms=" +
                std::to_string(elapsedMs));
            callback(true, "{}");
        },
        [callback, reqId, startedAt](const NRtError& err) {
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startedAt).count();
            AppLogger::LogNetwork("[STAGE-LEAVE#" + std::to_string(reqId) + "] <- err elapsed_ms=" +
                std::to_string(elapsedMs) + " message='" + err.message + "'");
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
    const uint64_t reqId = NextNetEventId();
    const auto startedAt = std::chrono::steady_clock::now();
    AppLogger::LogNetwork("[MATCH-CREATE#" + std::to_string(reqId) + "] -> request");

    if (!_session) {
        AppLogger::LogNetwork("[MATCH-CREATE#" + std::to_string(reqId) + "] <- err elapsed_ms=0 reason='no_session'");
        callback(false, "No session");
        return;
    }
    ensureRtConnected([this, callback, reqId, startedAt](bool ok, const std::string& err) {
        if (!ok) {
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startedAt).count();
            AppLogger::LogNetwork("[MATCH-CREATE#" + std::to_string(reqId) + "] <- err elapsed_ms=" +
                std::to_string(elapsedMs) + " rt_connect_error='" + err + "'");
            callback(false, err);
            return;
        }
        AppLogger::LogNetwork("[MATCH-CREATE#" + std::to_string(reqId) + "] realtime connected; creating match.");
        _rtClient->createMatch(
            [this, callback, reqId, startedAt](const NMatch& m) {
                _currentStageMatchId = m.matchId;
                const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - startedAt).count();
                AppLogger::LogNetwork("[MATCH-CREATE#" + std::to_string(reqId) + "] <- ok elapsed_ms=" +
                    std::to_string(elapsedMs) + " matchId='" + m.matchId + "'");
                callback(true, m.matchId);
            },
            [callback, reqId, startedAt](const NRtError& e) {
                const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - startedAt).count();
                AppLogger::LogNetwork("[MATCH-CREATE#" + std::to_string(reqId) + "] <- err elapsed_ms=" +
                    std::to_string(elapsedMs) + " message='" + e.message + "'");
                callback(false, e.message);
            });
    });
}

void NakamaManager::sendMatchData(int64_t opCode, const std::string& data) {
    if (!_rtClient || !_session || _currentStageMatchId.empty()) {
        AppLogger::LogNetwork("[RT-SEND] dropped opCode=" + std::to_string(opCode) +
            " reason='" + std::string(!_session ? "no_session" : (!_rtClient ? "no_rt_client" : "no_match")) + "'");
        return;
    }
    const uint64_t reqId = NextNetEventId();
    AppLogger::LogNetwork("[RT-SEND#" + std::to_string(reqId) + "] -> matchId='" + _currentStageMatchId +
        "' opCode=" + std::to_string(opCode) +
        " bytes=" + std::to_string(data.size()) +
        " payload='" + SummarizePayload(data, 180) + "'");
    _rtClient->sendMatchData(_currentStageMatchId, opCode, data);
}

void NakamaManager::sendClientReady(const std::string& recipeHash, const std::string& contentHash) {
    if (!_rtClient || !_session || _currentStageMatchId.empty()) {
        AppLogger::LogNetwork("[CLIENT-READY] skipped reason='" +
            std::string(!_session ? "no_session" : (!_rtClient ? "no_rt_client" : "no_match")) + "'");
        return;
    }
    const uint64_t reqId = NextNetEventId();
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string payload =
        "{\"v\":1,\"t\":" + std::to_string(nowMs) +
        ",\"payload\":{\"recipeHash\":\"" + escapeJson(recipeHash) +
        "\",\"contentHash\":\"" + escapeJson(contentHash) + "\"}}";
    AppLogger::LogNetwork("[CLIENT-READY#" + std::to_string(reqId) + "] -> matchId='" + _currentStageMatchId +
        "' recipeHash='" + recipeHash + "' contentHash='" + contentHash +
        "' bytes=" + std::to_string(payload.size()));
    _rtClient->sendMatchData(_currentStageMatchId, 4108, payload);
}

} // namespace Nakama
