#include <exception>
#include<iostream>
#include <kronos/logger.hpp>
#include <kronos/config.hpp>

int main(){
    // std::cout<<"Kronos Engine";


try
    {
    kronos::Config configObj("config/kronos.config");
    
    std::cout<<"\nMem table size= "<<configObj.getMemtableSize();

    std::cout << std::boolalpha<< "\nBloom filter status = "<<configObj.getBloomFilterEnabled();
    
    std::cout<<"\nLog path= "<<configObj.getLogPath();
    
    kronos::Logger loggerObj(configObj.getLogPath());
    loggerObj.info("Testing info here");

    }
    catch(const std::exception& error){ //Catch exceptions by const reference to preserve the original exception.
        std::cerr<<"Kronos startup failed "<<error.what()<<"\n";
    }



    

    return 0;
}