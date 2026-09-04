#pragma once
#include<iostream>
#include <string>
#include <unordered_map>
#include<filesystem>


namespace kronos {
    class Config{
        private:
            std::unordered_map<std::string, std::string> configurations_ ;
        
        public:
            explicit Config(const std::filesystem::path& pathConfig);
            int getMemtableSize();
            bool getBloomFilterEnabled();  
            std::string getLogPath();  //path for kronos.log
    };

};


