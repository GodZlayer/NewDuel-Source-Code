#pragma once
#include <string>
#include <fstream>
#include <iomanip>
#include <ctime>
#include <windows.h>

class AppLogger {
public:
    static void Log(const std::string& message) {
        std::ofstream logFile("client.log", std::ios::app);
        if (logFile.is_open()) {
            auto now = std::time(nullptr);
            auto tm = *std::localtime(&now);
            logFile << "[" << std::put_time(&tm, "%H:%M:%S") << "] " << message << std::endl;
            logFile.close();
        }
        OutputDebugStringA((message + "\n").c_str());
    }

    static void Clear() {
        std::ofstream logFile("client.log", std::ios::trunc);
        logFile.close();
    }
};
