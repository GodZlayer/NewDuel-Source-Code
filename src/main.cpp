#include <windows.h>
#include <shlwapi.h>
#include <tchar.h>
#include <memory>
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

LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_DESTROY: PostQuitMessage(0); return 0;
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

    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_HREDRAW | CS_VREDRAW, WindowProc, 0, 0, hInst, NULL, LoadCursor(NULL, IDC_ARROW), NULL, NULL, _T("GunzNakamaClass"), NULL };
    RegisterClassEx(&wc);
    HWND hWnd = CreateWindowEx(NULL, _T("GunzNakamaClass"), _T("OpenGunZ"), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, NULL, NULL, hInst, NULL);
    if (!hWnd) return 0;
    ShowWindow(hWnd, nShow); UpdateWindow(hWnd);
    RECT r; GetClientRect(hWnd, &r);

    AppLogger::Log("SISTEMA: Inicializando UI...");
    UIManager::getInstance().init(r.right - r.left, r.bottom - r.top);
    
    AppLogger::Log("SISTEMA: Inicializando Nakama...");
    Nakama::NakamaManager::getInstance().init("168.232.199.161", 7350, "defaultserverkey");
    
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
