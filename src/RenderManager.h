#pragma once
#include <memory>
#include "RealSpace3/Include/RDeviceDX11.h"

class RenderManager {
public:
    static RenderManager& getInstance() {
        static RenderManager instance;
        return instance;
    }

    void init(RealSpace3::RDeviceDX11* device);
    void render();

private:
    RenderManager() = default;
    RealSpace3::RDeviceDX11* m_pDevice = nullptr;
};
