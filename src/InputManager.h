#pragma once
#include <windows.h>

class InputManager {
public:
    static InputManager& getInstance() {
        static InputManager instance;
        return instance;
    }

    bool handleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    InputManager() = default;
};
