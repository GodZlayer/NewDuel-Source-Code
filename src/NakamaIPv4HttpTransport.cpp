#include "NakamaIPv4HttpTransport.h"
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include "AppLogger.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY WINHTTP_ACCESS_TYPE_DEFAULT_PROXY
#endif

namespace Nakama {

namespace {

std::string UrlEncode(const std::string& raw) {
    std::ostringstream out;
    out.fill('0');
    out << std::hex << std::uppercase;

    for (unsigned char c : raw) {
        const bool safe =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) {
            out << static_cast<char>(c);
        } else {
            out << '%' << std::setw(2) << static_cast<int>(c);
        }
    }
    return out.str();
}

std::string Win32ErrorToString(DWORD errorCode) {
    LPWSTR wideMsg = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD len = FormatMessageW(flags, nullptr, errorCode, 0, reinterpret_cast<LPWSTR>(&wideMsg), 0, nullptr);
    if (len == 0 || !wideMsg) {
        return "win32_error=" + std::to_string(static_cast<unsigned long>(errorCode));
    }

    std::wstring msg(wideMsg, len);
    LocalFree(wideMsg);
    while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n' || msg.back() == L' ' || msg.back() == L'\t')) {
        msg.pop_back();
    }

    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, msg.c_str(), static_cast<int>(msg.size()), nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0) {
        return "win32_error=" + std::to_string(static_cast<unsigned long>(errorCode));
    }
    std::string utf8(utf8Len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, msg.c_str(), static_cast<int>(msg.size()), utf8.data(), utf8Len, nullptr, nullptr);
    return utf8;
}

std::string TrimAscii(std::string value) {
    auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!value.empty() && isSpace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && isSpace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool HasQueryParam(const std::string& path, const std::string& key) {
    const size_t q = path.find('?');
    if (q == std::string::npos) return false;
    const std::string needle = key + "=";
    size_t pos = q + 1;
    while (pos < path.size()) {
        const size_t amp = path.find('&', pos);
        const size_t end = amp == std::string::npos ? path.size() : amp;
        if (end > pos && path.compare(pos, needle.size(), needle) == 0) return true;
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return false;
}

std::string ExtractJsonStringField(const std::string& json, const std::string& fieldName) {
    if (json.empty() || fieldName.empty()) return "";
    const std::string quotedKey = "\"" + fieldName + "\"";
    const size_t keyPos = json.find(quotedKey);
    if (keyPos == std::string::npos) return "";
    const size_t colonPos = json.find(':', keyPos + quotedKey.size());
    if (colonPos == std::string::npos) return "";
    size_t valueStart = colonPos + 1;
    while (valueStart < json.size() && std::isspace(static_cast<unsigned char>(json[valueStart]))) ++valueStart;
    if (valueStart >= json.size() || json[valueStart] != '"') return "";
    ++valueStart;

    std::string out;
    out.reserve(32);
    bool escaped = false;
    for (size_t i = valueStart; i < json.size(); ++i) {
        const char c = json[i];
        if (escaped) {
            out.push_back(c);
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') break;
        out.push_back(c);
    }
    return out;
}

bool BodyIndicatesCreateTrue(const std::string& body) {
    if (body.empty()) return false;
    const std::string lower = ToLowerAscii(body);
    if (lower.find("\"create\":true") != std::string::npos ||
        lower.find("\"create\" : true") != std::string::npos ||
        lower.find("\"create\": true") != std::string::npos) {
        return true;
    }
    const std::string username = ExtractJsonStringField(body, "username");
    return !username.empty();
}

} // namespace

NakamaIPv4HttpTransport::NakamaIPv4HttpTransport(bool forceIPv4)
    : _forceIPv4(forceIPv4), _timeout(std::chrono::seconds(30)) {
}

void NakamaIPv4HttpTransport::setBaseUri(const std::string& uri) {
    std::lock_guard<std::mutex> lock(_stateMutex);
    _baseUri = uri;
}

void NakamaIPv4HttpTransport::setTimeout(std::chrono::milliseconds time) {
    std::lock_guard<std::mutex> lock(_stateMutex);
    _timeout = time;
}

void NakamaIPv4HttpTransport::tick() {
    std::vector<PendingCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(_pendingMutex);
        callbacks.swap(_pendingCallbacks);
    }
    for (auto& entry : callbacks) {
        if (entry.callback) {
            entry.callback(entry.response);
        }
    }
}

void NakamaIPv4HttpTransport::request(const NHttpRequest& req, const NHttpResponseCallback& callback) {
    std::shared_ptr<NakamaIPv4HttpTransport> self;
    try {
        self = shared_from_this();
    } catch (const std::bad_weak_ptr&) {
        if (callback) {
            auto response = std::make_shared<NHttpResponse>();
            response->statusCode = InternalStatusCodes::NOT_INITIALIZED_ERROR;
            response->errorMessage = "transport ownership error";
            callback(response);
        }
        return;
    }

    std::string baseUri;
    std::chrono::milliseconds timeout;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        baseUri = _baseUri;
        timeout = _timeout;
    }

    const uint64_t requestId = ++_requestCounter;
    const uint64_t cancelGeneration = _cancelGeneration.load();
    const std::string method = httpMethodToString(req.method);
    AppLogger::LogNetwork("[HTTP#" + std::to_string(requestId) + "] -> " + method +
        " path='" + req.path + "' body_bytes=" + std::to_string(req.body.size()) +
        " force_ipv4=" + std::string(_forceIPv4 ? "true" : "false"));

    std::thread([self, requestId, req, callback, baseUri, timeout, cancelGeneration]() {
        const NHttpResponsePtr response = self->performRequest(requestId, req, baseUri, timeout, cancelGeneration);
        if (!callback) return;
        if (self->_cancelGeneration.load() != cancelGeneration) return;

        std::lock_guard<std::mutex> lock(self->_pendingMutex);
        if (self->_cancelGeneration.load() != cancelGeneration) return;
        self->_pendingCallbacks.push_back(PendingCallback{callback, response});
    }).detach();
}

void NakamaIPv4HttpTransport::cancelAllRequests() {
    ++_cancelGeneration;
    {
        std::lock_guard<std::mutex> lock(_pendingMutex);
        _pendingCallbacks.clear();
    }
    AppLogger::LogNetwork("[HTTP] cancelAllRequests() invoked.");
}

NHttpResponsePtr NakamaIPv4HttpTransport::performRequest(
    uint64_t requestId,
    const NHttpRequest& req,
    const std::string& baseUri,
    std::chrono::milliseconds timeout,
    uint64_t cancelGeneration) const {

    auto response = std::make_shared<NHttpResponse>();
    const auto startedAt = std::chrono::steady_clock::now();
    const auto finishWithLog = [&](const std::string& result, int statusCode) {
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startedAt).count();
        AppLogger::LogNetwork("[HTTP#" + std::to_string(requestId) + "] <- " + result +
            " status=" + std::to_string(statusCode) +
            " elapsed_ms=" + std::to_string(elapsedMs) +
            " resp_bytes=" + std::to_string(response->body.size()) +
            (response->errorMessage.empty() ? "" : " error='" + response->errorMessage + "'"));
    };

    if (_cancelGeneration.load() != cancelGeneration) {
        response->statusCode = InternalStatusCodes::CANCELLED_BY_USER;
        response->errorMessage = "cancelled";
        finishWithLog("cancelled", response->statusCode);
        return response;
    }

    const ParsedBaseUri base = parseBaseUri(baseUri);
    if (!base.valid) {
        response->statusCode = InternalStatusCodes::INTERNAL_TRANSPORT_ERROR;
        response->errorMessage = "invalid base URI: " + baseUri;
        finishWithLog("error", response->statusCode);
        return response;
    }

    const std::string objectPath = buildObjectPath(base, req);
    AppLogger::LogNetwork("[HTTP#" + std::to_string(requestId) + "] request_path='" + objectPath + "'");
    const std::wstring hostWide = toWide(base.host);
    const std::wstring objectWide = toWide(objectPath);
    const std::wstring methodWide = toWide(httpMethodToString(req.method));

    HINTERNET hSession = WinHttpOpen(
        L"NakamaIPv4HttpTransport/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);

    if (!hSession) {
        const DWORD err = GetLastError();
        response->statusCode = InternalStatusCodes::CONNECTION_ERROR;
        response->errorMessage = "WinHttpOpen failed: " + Win32ErrorToString(err);
        finishWithLog("error", response->statusCode);
        return response;
    }

    const int timeoutMs = timeout.count() > 0 ? static_cast<int>(timeout.count()) : 30000;
    WinHttpSetTimeouts(hSession, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    DWORD retries = 2;
    WinHttpSetOption(hSession, WINHTTP_OPTION_CONNECT_RETRIES, &retries, sizeof(retries));

    HINTERNET hConnect = WinHttpConnect(hSession, hostWide.c_str(), static_cast<INTERNET_PORT>(base.port), 0);
    if (!hConnect) {
        const DWORD err = GetLastError();
        response->statusCode = InternalStatusCodes::CONNECTION_ERROR;
        response->errorMessage = "WinHttpConnect failed: " + Win32ErrorToString(err);
        WinHttpCloseHandle(hSession);
        finishWithLog("error", response->statusCode);
        return response;
    }

    DWORD openFlags = base.secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect,
        methodWide.c_str(),
        objectWide.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        openFlags);

    if (!hRequest) {
        const DWORD err = GetLastError();
        response->statusCode = InternalStatusCodes::CONNECTION_ERROR;
        response->errorMessage = "WinHttpOpenRequest failed: " + Win32ErrorToString(err);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        finishWithLog("error", response->statusCode);
        return response;
    }

#ifndef WINHTTP_OPTION_RESOLUTION_HOSTNAME
#define WINHTTP_OPTION_RESOLUTION_HOSTNAME 203
#endif

    std::string resolvedIPv4;
    if (shouldForceIPv4Host(base.host, _forceIPv4)) {
        resolvedIPv4 = resolveIPv4Literal(base.host);
        if (!resolvedIPv4.empty()) {
            const std::wstring resolvedWide = toWide(resolvedIPv4);
            DWORD bytes = static_cast<DWORD>((resolvedWide.size() + 1) * sizeof(wchar_t));
            if (!WinHttpSetOption(hRequest, WINHTTP_OPTION_RESOLUTION_HOSTNAME,
                reinterpret_cast<LPVOID>(const_cast<wchar_t*>(resolvedWide.c_str())), bytes)) {
                AppLogger::LogNetwork("[HTTP#" + std::to_string(requestId) + "] ipv4_resolution_host set failed: " +
                    Win32ErrorToString(GetLastError()));
            } else {
                AppLogger::LogNetwork("[HTTP#" + std::to_string(requestId) + "] ipv4_resolution_host='" + resolvedIPv4 + "'");
            }
        } else {
            AppLogger::LogNetwork("[HTTP#" + std::to_string(requestId) + "] ipv4_resolution_host unavailable for '" + base.host + "'");
        }
    }

    for (const auto& header : req.headers) {
        std::wstring line = toWide(header.first + ": " + header.second);
        WinHttpAddRequestHeaders(hRequest, line.c_str(), static_cast<DWORD>(line.size()),
            WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    LPVOID bodyPtr = WINHTTP_NO_REQUEST_DATA;
    DWORD bodyBytes = 0;
    if (!req.body.empty()) {
        bodyBytes = static_cast<DWORD>(req.body.size());
        bodyPtr = const_cast<char*>(req.body.data());
    }

    auto setNoClientCert = [&]() {
        if (!WinHttpSetOption(hRequest,
            WINHTTP_OPTION_CLIENT_CERT_CONTEXT,
            WINHTTP_NO_CLIENT_CERT_CONTEXT,
            0)) {
            const DWORD setErr = GetLastError();
            AppLogger::LogNetwork("[HTTP#" + std::to_string(requestId) +
                "] no-client-cert set failed code=" + std::to_string(static_cast<unsigned long>(setErr)) +
                " msg='" + Win32ErrorToString(setErr) + "'");
        }
    };

    setNoClientCert();

    bool sendOk = WinHttpSendRequest(hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        bodyPtr, bodyBytes, bodyBytes, 0) == TRUE;
    DWORD sendErr = sendOk ? ERROR_SUCCESS : GetLastError();

    if (!sendOk && sendErr == ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED) {
        AppLogger::LogNetwork("[HTTP#" + std::to_string(requestId) +
            "] winhttp 12044 (client cert requested), retrying with WINHTTP_NO_CLIENT_CERT_CONTEXT.");
        setNoClientCert();
        sendOk = WinHttpSendRequest(hRequest,
            WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            bodyPtr, bodyBytes, bodyBytes, 0) == TRUE;
        sendErr = sendOk ? ERROR_SUCCESS : GetLastError();
    }

    if (!sendOk) {
        response->statusCode = InternalStatusCodes::CONNECTION_ERROR;
        response->errorMessage = "WinHttpSendRequest failed (code=" +
            std::to_string(static_cast<unsigned long>(sendErr)) + "): " + Win32ErrorToString(sendErr);
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        finishWithLog("error", response->statusCode);
        return response;
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        const DWORD err = GetLastError();
        response->statusCode = InternalStatusCodes::CONNECTION_ERROR;
        response->errorMessage = "WinHttpReceiveResponse failed: " + Win32ErrorToString(err);
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        finishWithLog("error", response->statusCode);
        return response;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        statusCode = 0;
    }
    response->statusCode = static_cast<int>(statusCode);

    std::string body;
    for (;;) {
        if (_cancelGeneration.load() != cancelGeneration) {
            response->statusCode = InternalStatusCodes::CANCELLED_BY_USER;
            response->errorMessage = "cancelled";
            break;
        }

        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &available)) {
            response->errorMessage = "WinHttpQueryDataAvailable failed: " + Win32ErrorToString(GetLastError());
            break;
        }
        if (available == 0) break;

        std::vector<char> chunk(available);
        DWORD bytesRead = 0;
        if (!WinHttpReadData(hRequest, chunk.data(), available, &bytesRead)) {
            response->errorMessage = "WinHttpReadData failed: " + Win32ErrorToString(GetLastError());
            break;
        }
        if (bytesRead == 0) break;
        body.append(chunk.data(), bytesRead);
    }

    response->body = body;
    if (!response->errorMessage.empty() && response->statusCode == 0) {
        response->statusCode = InternalStatusCodes::CONNECTION_ERROR;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    finishWithLog(response->errorMessage.empty() ? "ok" : "error", response->statusCode);
    return response;
}

NakamaIPv4HttpTransport::ParsedBaseUri NakamaIPv4HttpTransport::parseBaseUri(const std::string& uri) const {
    ParsedBaseUri out;
    std::string source = TrimAscii(uri);
    if (source.empty()) return out;

    const size_t schemePos = source.find("://");
    if (schemePos == std::string::npos) return out;
    const std::string scheme = source.substr(0, schemePos);
    out.secure = (scheme == "https");

    std::string rest = source.substr(schemePos + 3);
    size_t slashPos = rest.find('/');
    std::string hostPort = slashPos == std::string::npos ? rest : rest.substr(0, slashPos);
    out.basePath = slashPos == std::string::npos ? "" : rest.substr(slashPos);

    if (hostPort.empty()) return out;

    if (!hostPort.empty() && hostPort.front() == '[') {
        size_t close = hostPort.find(']');
        if (close == std::string::npos) return out;
        out.host = hostPort.substr(1, close - 1);
        if (close + 1 < hostPort.size() && hostPort[close + 1] == ':') {
            try {
                out.port = static_cast<uint16_t>(std::stoi(hostPort.substr(close + 2)));
            } catch (...) {
                return ParsedBaseUri{};
            }
        }
    } else {
        size_t colon = hostPort.rfind(':');
        if (colon != std::string::npos && hostPort.find(':') == colon) {
            out.host = hostPort.substr(0, colon);
            try {
                out.port = static_cast<uint16_t>(std::stoi(hostPort.substr(colon + 1)));
            } catch (...) {
                return ParsedBaseUri{};
            }
        } else {
            out.host = hostPort;
        }
    }

    if (out.host.empty()) return out;
    if (out.port == 0) out.port = out.secure ? 443 : 80;
    out.valid = true;
    return out;
}

std::string NakamaIPv4HttpTransport::buildObjectPath(const ParsedBaseUri& base, const NHttpRequest& req) const {
    std::string path = req.path.empty() ? "/" : req.path;
    if (path.front() != '/') path.insert(path.begin(), '/');

    if (!base.basePath.empty() && base.basePath != "/" && path.rfind(base.basePath, 0) != 0) {
        std::string prefix = base.basePath;
        if (prefix.back() == '/') prefix.pop_back();
        path = prefix + path;
    }

    bool hasQuery = path.find('?') != std::string::npos;
    if (!req.queryArgs.empty()) {
        path.push_back(hasQuery ? '&' : '?');
        bool first = true;
        for (const auto& arg : req.queryArgs) {
            if (!first) path.push_back('&');
            first = false;
            path += UrlEncode(arg.first);
            path.push_back('=');
            path += UrlEncode(arg.second);
        }
        hasQuery = true;
    }

    const bool isEmailAuthPath =
        path.find("/v2/account/authenticate/email") != std::string::npos;
    if (isEmailAuthPath && !HasQueryParam(path, "create")) {
        const bool createValue = BodyIndicatesCreateTrue(req.body);
        path += (hasQuery ? "&" : "?");
        path += "create=";
        path += createValue ? "true" : "false";
        hasQuery = true;

        if (createValue && !HasQueryParam(path, "username")) {
            const std::string username = ExtractJsonStringField(req.body, "username");
            if (!username.empty()) {
                path += "&username=" + UrlEncode(username);
            }
        }
    }
    return path;
}

std::wstring NakamaIPv4HttpTransport::toWide(const std::string& utf8) const {
    if (utf8.empty()) return std::wstring();
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(), needed);
    return out;
}

std::string NakamaIPv4HttpTransport::httpMethodToString(NHttpReqMethod method) const {
    switch (method) {
    case NHttpReqMethod::GET: return "GET";
    case NHttpReqMethod::POST: return "POST";
    case NHttpReqMethod::PUT: return "PUT";
    case NHttpReqMethod::DEL: return "DELETE";
    default: return "POST";
    }
}

std::string NakamaIPv4HttpTransport::resolveIPv4Literal(const std::string& host) const {
    static std::once_flag s_wsaInit;
    std::call_once(s_wsaInit, []() {
        WSADATA wsaData{};
        WSAStartup(MAKEWORD(2, 2), &wsaData);
    });

    {
        std::lock_guard<std::mutex> lock(_resolverMutex);
        auto it = _ipv4Cache.find(host);
        if (it != _ipv4Cache.end()) return it->second;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* results = nullptr;
    const int rc = getaddrinfo(host.c_str(), nullptr, &hints, &results);
    if (rc != 0 || !results) {
        return "";
    }

    std::string ipv4;
    for (addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
        if (!ai->ai_addr || ai->ai_family != AF_INET) continue;
        char buffer[INET_ADDRSTRLEN] = {};
        sockaddr_in* sa = reinterpret_cast<sockaddr_in*>(ai->ai_addr);
        if (inet_ntop(AF_INET, &sa->sin_addr, buffer, sizeof(buffer))) {
            ipv4 = buffer;
            break;
        }
    }
    freeaddrinfo(results);

    if (!ipv4.empty()) {
        std::lock_guard<std::mutex> lock(_resolverMutex);
        _ipv4Cache[host] = ipv4;
    }
    return ipv4;
}

bool NakamaIPv4HttpTransport::isLikelyIPv4Literal(const std::string& host) {
    if (host.empty()) return false;
    for (unsigned char c : host) {
        if (!(std::isdigit(c) || c == '.')) return false;
    }
    return host.find('.') != std::string::npos;
}

bool NakamaIPv4HttpTransport::shouldForceIPv4Host(const std::string& host, bool forceIPv4) {
    if (!forceIPv4) return false;
    if (host.empty()) return false;
    if (isLikelyIPv4Literal(host)) return false;
    return true;
}

} // namespace Nakama
