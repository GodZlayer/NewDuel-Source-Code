#pragma once
#include <string>
#include <fstream>
#include <iomanip>
#include <ctime>
#include <chrono>
#include <mutex>
#include <sstream>
#include <windows.h>

class AppLogger {
public:
    static void Log(const std::string& message) {
        WriteLine("client.log", message);
    }

    static void LogNetwork(const std::string& message) {
        WriteLine("client_network.log", message);
    }

    static void Clear() {
        std::lock_guard<std::mutex> lock(s_logMutex);
        std::ofstream logFile("client.log", std::ios::trunc);
        std::ofstream netFile("client_network.log", std::ios::trunc);
    }

    static void ClearNetwork() {
        std::lock_guard<std::mutex> lock(s_logMutex);
        std::ofstream netFile("client_network.log", std::ios::trunc);
    }

private:
    static std::string BuildTimestamp() {
        const auto now = std::chrono::system_clock::now();
        const auto nowTimeT = std::chrono::system_clock::to_time_t(now);
        std::tm tmNow = {};
        localtime_s(&tmNow, &nowTimeT);

        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        std::ostringstream oss;
        oss << std::put_time(&tmNow, "%H:%M:%S")
            << "."
            << std::setw(3) << std::setfill('0') << ms.count();
        return oss.str();
    }

    static void WriteLine(const char* fileName, const std::string& message) {
        std::lock_guard<std::mutex> lock(s_logMutex);

        std::ofstream logFile(fileName, std::ios::app);
        if (logFile.is_open()) {
            logFile << "[" << BuildTimestamp() << "] " << message << std::endl;
        }

        const std::string debugLine = std::string("[") + fileName + "] " + message + "\n";
        OutputDebugStringA(debugLine.c_str());
    }

    inline static std::mutex s_logMutex;
};
