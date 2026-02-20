#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <mutex>

class UIManager {
public:
    static UIManager& getInstance() {
        static UIManager instance;
        return instance;
    }

    void init(int width, int height);
    void resize(int width, int height); // Novo
    void update();
    void render();

    unsigned char* getLockPixels(uint32_t& rowBytes, uint32_t& width, uint32_t& height);
    void unlockPixels();

    void setProgress(float percent);
    void setStatus(const std::string& status);
    void loadURL(const std::string& url);

    void onMouseMove(int x, int y);
    void onMouseDown(int x, int y);
    void onMouseUp(int x, int y);
    void onKey(UINT msg, WPARAM wp, LPARAM lp);

    void* getView();

private:
    UIManager() = default;

    void* _app = nullptr;
    void* _renderer = nullptr;
    void* _view = nullptr;

    std::string m_pendingURL;
    std::mutex m_urlMutex;
    int m_width = 0;
    int m_height = 0;
    int m_forceRepaintFrames = 0;
};
