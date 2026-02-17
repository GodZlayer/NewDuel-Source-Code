#pragma once
#include <string>
#include <memory>
#include <vector>
#include "RScene.h"

namespace RealSpace3 {

class WorldManager {
public:
    static WorldManager& getInstance() {
        static WorldManager instance;
        return instance;
    }

    // Carrega um mundo completo (Cena + Objetos + Texturas)
    bool loadWorld(ID3D11Device* device, const std::string& worldName);
    
    void update(float deltaTime);
    void draw(ID3D11DeviceContext* context, ID3D11Buffer* constantBuffer);

private:
    WorldManager() = default;
    std::unique_ptr<RScene> m_pCurrentScene;
};

}
