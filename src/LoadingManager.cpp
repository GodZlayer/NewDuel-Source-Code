#include "LoadingManager.h"
#include "UIManager.h"
#include "AppLogger.h"
#include "RealSpace3/Include/SceneManager.h"
#include <windows.h>
#include <direct.h>
#include <algorithm>

void LoadingManager::update(float deltaTime) {
    if (m_switched) return;

    if (m_progress < 100.0f) {
        m_progress += 0.35f; 
        UIManager::getInstance().setProgress(m_progress);

        if (m_milestone == 0 && m_progress >= 5)  { UIManager::getInstance().setStatus("Iniciando Bridge Nakama..."); AppLogger::Log("LOADING: Inicializando Nakama SDK"); m_milestone++; }
        if (m_milestone == 1 && m_progress >= 25) { 
            UIManager::getInstance().setStatus("Carregando Dados do Mundo..."); 
            AppLogger::Log("LOADING: Carregando cena RS3 para cinematic background");
            (void)RealSpace3::SceneManager::getInstance().loadScenePackage("char_creation_select");
            (void)RealSpace3::SceneManager::getInstance().setRenderMode(RealSpace3::RS3RenderMode::MapOnlyCinematic);
            m_milestone++; 
        }
        if (m_milestone == 2 && m_progress >= 50) { UIManager::getInstance().setStatus("Alocando Geometrias DX11..."); AppLogger::Log("LOADING: Alocando Memoria GPU"); m_milestone++; }
        if (m_milestone == 3 && m_progress >= 75) { UIManager::getInstance().setStatus("Sincronizando RealSpace3..."); m_milestone++; }
        if (m_milestone == 4 && m_progress >= 95) { UIManager::getInstance().setStatus("Sistemas Operacionais."); AppLogger::Log("LOADING: Pronto."); m_milestone++; }
    } else {
        if (++m_holdFrames > 60) {
            AppLogger::Log("BOOT: Transicao para LOGIN.");
            char buffer[MAX_PATH]; _getcwd(buffer, MAX_PATH);
            std::string path = "file:///"; path += buffer;
            std::replace(path.begin(), path.end(), '\\', '/');
            UIManager::getInstance().loadURL(path + "/ui/login.html");
            m_switched = true;
        }
    }
}

void LoadingManager::reset() { m_progress = 0.0f; m_switched = false; m_milestone = 0; m_holdFrames = 0; }
