#pragma once
#include <map>
#include <string>
#include <memory>
#include "RMesh.h"

namespace RealSpace3 {

class ResourceManager {
public:
    static ResourceManager& getInstance() {
        static ResourceManager instance;
        return instance;
    }

    std::shared_ptr<RMesh> getMesh(ID3D11Device* device, const std::string& path);
    void clear();

private:
    ResourceManager() = default;
    std::map<std::string, std::shared_ptr<RMesh>> m_meshCache;
};

}
