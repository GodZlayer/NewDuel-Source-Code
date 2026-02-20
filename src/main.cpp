#include <windows.h>
#include <shlwapi.h>
#include <tchar.h>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include "NakamaManager.h"
#include "UIManager.h"
#include "InputManager.h"
#include "RenderManager.h"
#include "RealSpace3/Include/RDeviceDX11.h"
#include "RealSpace3/Include/SceneManager.h"
#include "AppLogger.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shlwapi.lib")

std::unique_ptr<RealSpace3::RDeviceDX11> g_pDevice;

namespace {

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool IsTruthyEnv(const char* value) {
    if (!value || !*value) return false;
    const std::string lower = ToLowerAscii(value);
    return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
}

bool IsBrokenLocalProxyValue(const std::string& rawValue) {
    if (rawValue.empty()) return false;

    std::string v = ToLowerAscii(rawValue);
    // Trim spaces.
    while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) v.erase(v.begin());
    while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.pop_back();

    const std::string httpPrefix = "http://";
    const std::string httpsPrefix = "https://";
    if (v.rfind(httpPrefix, 0) == 0) v.erase(0, httpPrefix.size());
    if (v.rfind(httpsPrefix, 0) == 0) v.erase(0, httpsPrefix.size());

    // Remove credentials if present.
    const size_t at = v.rfind('@');
    if (at != std::string::npos) v = v.substr(at + 1);

    // Remove path/query.
    const size_t slash = v.find('/');
    if (slash != std::string::npos) v = v.substr(0, slash);

    return v == "127.0.0.1:9" || v == "localhost:9" || v == "[::1]:9" || v == "0.0.0.0:9";
}

void ClearProcessEnvVar(const char* name) {
    if (!name || !*name) return;
    _putenv_s(name, "");
    SetEnvironmentVariableA(name, nullptr);
}

void SanitizeProxyEnvironment() {
    const bool forceNoProxy = IsTruthyEnv(std::getenv("NDG_FORCE_NO_PROXY"));
    const bool allowProxy = IsTruthyEnv(std::getenv("NDG_ALLOW_PROXY"));
    if (allowProxy && !forceNoProxy) {
        AppLogger::Log("SISTEMA: Proxy do ambiente mantido (NDG_ALLOW_PROXY=1).");
        return;
    }

    const char* proxyVars[] = {
        "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY",
        "http_proxy", "https_proxy", "all_proxy"
    };

    bool changed = false;
    for (const char* name : proxyVars) {
        const char* value = std::getenv(name);
        if (!value || !*value) continue;

        const std::string raw = value;
        if (forceNoProxy || IsBrokenLocalProxyValue(raw)) {
            ClearProcessEnvVar(name);
            changed = true;
            AppLogger::Log("SISTEMA: Limpando proxy de ambiente '" + std::string(name) + "' (valor='" + raw + "').");
        }
    }

    if (changed) {
        // Bypass local loopback explicitly.
        _putenv_s("NO_PROXY", "localhost,127.0.0.1,::1");
        _putenv_s("no_proxy", "localhost,127.0.0.1,::1");
    }
}

} // namespace

LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_DESTROY:
            Nakama::NakamaManager::getInstance().shutdown();
            PostQuitMessage(0);
            return 0;
        case WM_SIZE:
            if (g_pDevice) {
                int w = LOWORD(lp);
                int h = HIWORD(lp);
                if (w > 0 && h > 0) {
                    AppLogger::Log("SISTEMA: Redimensionando para " + std::to_string(w) + "x" + std::to_string(h));
                    g_pDevice->Resize(w, h);
                    RealSpace3::SceneManager::getInstance().setSize(w, h);
                    UIManager::getInstance().resize(w, h);
                }
            }
            return 0;
    }
    if (InputManager::getInstance().handleMessage(hWnd, msg, wp, lp)) return 0;
    return DefWindowProc(hWnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    SetProcessDPIAware();

    // Mudar diretório de trabalho para a pasta do executável
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    PathRemoveFileSpecA(exePath);
    SetCurrentDirectoryA(exePath);
    AppLogger::Log("SISTEMA: Diretorio de trabalho alterado para: " + std::string(exePath));

    AppLogger::Clear();
    AppLogger::Log("--- OPEN GUNZ: SYSTEM REBOOT ---");
    SanitizeProxyEnvironment();

    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_HREDRAW | CS_VREDRAW, WindowProc, 0, 0, hInst, NULL, LoadCursor(NULL, IDC_ARROW), NULL, NULL, _T("GunzNakamaClass"), NULL };
    RegisterClassEx(&wc);
    HWND hWnd = CreateWindowEx(NULL, _T("GunzNakamaClass"), _T("OpenGunZ"), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, NULL, NULL, hInst, NULL);
    if (!hWnd) return 0;
    ShowWindow(hWnd, nShow); UpdateWindow(hWnd);
    RECT r; GetClientRect(hWnd, &r);

    AppLogger::Log("SISTEMA: Inicializando UI...");
    UIManager::getInstance().init(r.right - r.left, r.bottom - r.top);
    
    AppLogger::Log("SISTEMA: Inicializando Nakama...");
    const char* envHost = std::getenv("NDG_NAKAMA_HOST");
    const char* envPort = std::getenv("NDG_NAKAMA_PORT");
    const char* envKey = std::getenv("NDG_NAKAMA_KEY");
    const char* envSSL = std::getenv("NDG_NAKAMA_SSL");

    const std::string nakamaHost = (envHost && *envHost) ? envHost : "server.newduel.pp.ua";
    int nakamaPort = 443;
    if (envPort && *envPort) {
        const int parsed = std::atoi(envPort);
        if (parsed > 0) nakamaPort = parsed;
    }
    const std::string nakamaKey = (envKey && *envKey) ? envKey : "defaultserverkey";
    bool nakamaSSL = true;
    if (envSSL && *envSSL) {
        const std::string sslRaw = envSSL;
        if (sslRaw == "0" || sslRaw == "false" || sslRaw == "FALSE" || sslRaw == "no" || sslRaw == "NO") {
            nakamaSSL = false;
        } else {
            nakamaSSL = true;
        }
    }

    AppLogger::Log("SISTEMA: Nakama host='" + nakamaHost + "' port=" + std::to_string(nakamaPort) +
        " ssl=" + std::string(nakamaSSL ? "true" : "false"));
    Nakama::NakamaManager::getInstance().init(nakamaHost, nakamaPort, nakamaKey, nakamaSSL);
    
    AppLogger::Log("SISTEMA: Inicializando DX11...");
    g_pDevice = std::make_unique<RealSpace3::RDeviceDX11>();
    if (g_pDevice->Initialize(hWnd, r.right - r.left, r.bottom - r.top)) {
        AppLogger::Log("SISTEMA: DX11 OK. Inicializando Managers...");
        RenderManager::getInstance().init(g_pDevice.get());
        RealSpace3::SceneManager::getInstance().init(g_pDevice->GetDevice());
        RealSpace3::SceneManager::getInstance().setSize(r.right - r.left, r.bottom - r.top);
    } else {
        AppLogger::Log("SISTEMA ERRO: Falha ao inicializar DX11.");
        return 0;
    }

    AppLogger::Log("SISTEMA: Entrando no Loop Principal.");
    MSG msg = { 0 };
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) { 
            TranslateMessage(&msg); 
            DispatchMessage(&msg); 
        } else { 
            RenderManager::getInstance().render(); 
        }
    }
    return (int)msg.wParam;
}
