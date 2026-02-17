#include "InputManager.h"
#include "UIManager.h"
#include <windowsx.h>

bool InputManager::handleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_MOUSEMOVE:
            UIManager::getInstance().onMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return true;
        case WM_LBUTTONDOWN:
            UIManager::getInstance().onMouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return true;
        case WM_LBUTTONUP:
            UIManager::getInstance().onMouseUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return true;
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_CHAR:
            UIManager::getInstance().onKey(msg, wParam, lParam);
            return true;
    }
    return false;
}
