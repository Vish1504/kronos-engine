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
        public:
            explicit Logger(const std::filesystem::path& path);
            void info(const std::string& message);
            void warn(const std::string& message);
            void error(const std::string& message);



    };

}