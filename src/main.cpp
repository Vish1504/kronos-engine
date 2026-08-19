#include<iostream>
#include <kronos/logger.hpp>
#include <kronos/config.hpp>

int main(){
    // std::cout<<"Kronos Engine";



    kronos::Config configObj("config/kronos.config");
    std::cout<<"Mem table size= "<<configObj.getMemtableSize();
    std::cout << std::boolalpha<< "\nBloom filter status = "<<configObj.getBloomFilterEnabled();
    std::cout<<"\nLog path= "<<configObj.getLogPath();
    
    kronos::Logger loggerObj(configObj.getLogPath());
    loggerObj.info("Testing info here");



    

    return 0;
}