#pragma once

#include<string>
#include <filesystem>
#include<fstream>
#include<mutex>

namespace kronos {
    class Logger{
        private:
            std::ofstream file_;
            std::mutex mutex_; //To facilitate concurrent writes
            void logFunc(
                const std::string& level, 
                const std::string& message
            );
        public:
            explicit Logger(const std::filesystem::path& path);
            void info(const std::string& message); //logger.info("Kronos started") will give out YYYY-MM-DD HH:MM:SS [INFO] Kronos started
            void warn(const std::string& message);
            void error(const std::string& message);
    };

}