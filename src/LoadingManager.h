#pragma once
#include <string>

class LoadingManager {
public:
    static LoadingManager& getInstance() {
        static LoadingManager instance;
        return instance;
    }

    void update(float deltaTime);
    void reset();
    bool isFinished() const { return m_switched; }

private:
    LoadingManager() = default;
    float m_progress = 0.0f;
    bool m_switched = false;
    int m_milestone = 0;
    int m_holdFrames = 0;
};
