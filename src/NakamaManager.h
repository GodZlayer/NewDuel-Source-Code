#pragma once
#include "nakama-cpp/Nakama.h"
#include <memory>
#include <string>
#include <functional>
#include <mutex>
#include <vector>

namespace Nakama {
    class NakamaManager {
    public:
        static NakamaManager& getInstance() {
            static NakamaManager instance;
            return instance;
        }

        void init(const std::string& host, int port, const std::string& serverKey);
        void tick();

        void authenticateDevice(const std::string& deviceId, std::function<void(bool)> callback);
        void authenticateEmail(const std::string& email, const std::string& password, std::function<void(bool, const std::string&)> callback);
        
        void listCharacters(std::function<void(bool, const std::string&)> callback);
        // Atualizado para incluir sex
        void createCharacter(const std::string& name, int sex, int face, int hair, int costume, std::function<void(bool, const std::string&)> callback);
        void deleteCharacter(const std::string& charId, std::function<void(bool, const std::string&)> callback);
        void selectCharacter(const std::string& charId, std::function<void(bool, const std::string&)> callback);

        void listStages(const std::string& filterJson, std::function<void(bool, const std::string&)> callback);
        void createStage(const std::string& createJson, std::function<void(bool, const std::string&)> callback);
        void getBootstrapV2(std::function<void(bool, const std::string&)> callback);
        void getGameData(const std::string& key, std::function<void(bool, const std::string&)> callback);
        void joinStage(const std::string& matchId, const std::string& password, std::function<void(bool, const std::string&)> callback);
        void leaveStage(std::function<void(bool, const std::string&)> callback);
        void requestStageState(const std::string& matchId, std::function<void(bool, const std::string&)> callback);
        void setStageReady(const std::string& matchId, bool ready, std::function<void(bool, const std::string&)> callback);
        void setStageTeam(const std::string& matchId, int team, std::function<void(bool, const std::string&)> callback);
        void stageChat(const std::string& matchId, const std::string& message, std::function<void(bool, const std::string&)> callback);
        void startStage(const std::string& matchId, std::function<void(bool, const std::string&)> callback);
        void endStage(const std::string& matchId, std::function<void(bool, const std::string&)> callback);

        void setRtMatchDataCallback(std::function<void(int64_t, const std::string&)> callback);
        std::string getCurrentStageMatchId() const { return _currentStageMatchId; }
        std::string getSessionUserId() const { return _session ? _session->getUserId() : ""; }
        std::string getSessionUsername() const { return _session ? _session->getUsername() : ""; }
        
        void joinMatch(std::function<void(bool, const std::string&)> callback);
        void sendMatchData(int64_t opCode, const std::string& data);
        void sendClientReady(const std::string& recipeHash, const std::string& contentHash);

        NRtClientPtr getRtClient() { return _rtClient; }
        NClientPtr getClient() { return _client; }
        NSessionPtr getSession() { return _session; }

    private:
        NakamaManager() = default;
        
        NClientPtr _client;
        NSessionPtr _session;
        NRtClientPtr _rtClient;
        std::shared_ptr<NRtDefaultClientListener> _rtListener;
        NClientParameters _params;
        std::string _currentStageMatchId;

        std::function<void(int64_t, const std::string&)> _rtMatchDataCallback;
        bool _rtConnecting = false;
        std::vector<std::function<void(bool, const std::string&)>> _rtConnectWaiters;
        std::mutex _rtMutex;

        static std::string escapeJson(const std::string& value);
        void rpcCall(const std::string& rpcId, const std::string& payload, std::function<void(bool, const std::string&)> callback);
        void ensureRtClient();
        void ensureRtConnected(std::function<void(bool, const std::string&)> callback);
        void resolveRtConnectWaiters(bool success, const std::string& message);
    };
}
