#include<iostream>
#include <kronos/logger.hpp>

int main(){
    // std::cout<<"Kronos Engine";

    kronos::Logger loggerObj("logs/kronos.log");

    loggerObj.info("Starting Kronos 2");
    loggerObj.warn("Test warning 2");
    loggerObj.error("Test error 2");

    

    return 0;
}