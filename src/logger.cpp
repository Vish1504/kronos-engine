#include<iostream>
#include<fstream>
#include <kronos/logger.hpp>
#include <stdexcept>
#include <chrono>
#include<iomanip>
#include <ctime>

// Implementation of the Logger constructor.
// 'path' is the location of the log file,
kronos::Logger::Logger(const std::filesystem::path& path){

// Extract the directory portion of the path.
// If the user provides only "kronos.log",
    // parent_path() will be empty, so there is
    // no directory that needs to be created.
    if (!path.parent_path().empty())
    {
            //To create the parent directory if it doesn't exist
        std::filesystem::create_directories(path.parent_path());
    }
// file_ is the std::ofstream MEMBER owned by this Logger.
// file_ must remain alive after the constructor finishes.
    file_.open(path, std::ios::app);

// Opening an ofstream normally does not throw automatically when opening fails. Instead, the stream enters a failed state. So we explicitly check whether the file successfully opened.
    if (!file_.is_open()) {
            throw std::runtime_error("Failed to open log file");
    }
}

void kronos::Logger::info(const std::string& message){
        logFunc("INFO",message);
}

void kronos::Logger::warn(const std::string& message){
        logFunc("WARN",message);
}

void kronos::Logger::error(const std::string& message){
    logFunc("ERROR",message);
}

void kronos::Logger::logFunc(const std::string& level, const std::string& message){

    auto nowObj=std::chrono::system_clock::now(); //nowObj is a complex object representing a point in time
    std::time_t legacy_time = std::chrono::system_clock::to_time_t(nowObj);// Convert to time_t (seconds since epoch)
    std::tm* local_time = std::localtime(&legacy_time);// Convert time_t into local time, accounting for the system's current time zone, and breaks it down into structures like year, month, and day.

    file_<<std::put_time(local_time, "%Y-%m-%d %H:%M:%S") <<" ["<<level<<"] "<<message<<"\n";

}