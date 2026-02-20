#pragma once

#include "nakama-cpp/NHttpTransportInterface.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Nakama {

class NakamaIPv4HttpTransport final : public NHttpTransportInterface, public std::enable_shared_from_this<NakamaIPv4HttpTransport> {
public:
    explicit NakamaIPv4HttpTransport(bool forceIPv4);
    ~NakamaIPv4HttpTransport() override = default;

    void setBaseUri(const std::string& uri) override;
    void setTimeout(std::chrono::milliseconds time) override;
    void tick() override;
    void request(const NHttpRequest& req, const NHttpResponseCallback& callback = nullptr) override;
    void cancelAllRequests() override;

private:
    struct PendingCallback {
        NHttpResponseCallback callback;
        NHttpResponsePtr response;
    };

    struct ParsedBaseUri {
        bool valid = false;
        bool secure = false;
        std::string host;
        uint16_t port = 0;
        std::string basePath;
    };

    NHttpResponsePtr performRequest(
        uint64_t requestId,
        const NHttpRequest& req,
        const std::string& baseUri,
        std::chrono::milliseconds timeout,
        uint64_t cancelGeneration) const;

    ParsedBaseUri parseBaseUri(const std::string& uri) const;
    std::string buildObjectPath(const ParsedBaseUri& base, const NHttpRequest& req) const;
    std::wstring toWide(const std::string& utf8) const;
    std::string httpMethodToString(NHttpReqMethod method) const;
    std::string resolveIPv4Literal(const std::string& host) const;
    static bool isLikelyIPv4Literal(const std::string& host);
    static bool shouldForceIPv4Host(const std::string& host, bool forceIPv4);

private:
    bool _forceIPv4;
    mutable std::mutex _stateMutex;
    std::string _baseUri;
    std::chrono::milliseconds _timeout;
    std::atomic<uint64_t> _cancelGeneration{0};
    std::atomic<uint64_t> _requestCounter{0};

    mutable std::mutex _resolverMutex;
    mutable std::unordered_map<std::string, std::string> _ipv4Cache;

    std::mutex _pendingMutex;
    std::vector<PendingCallback> _pendingCallbacks;
};

} // namespace Nakama
