#include <exception>
#include <iostream>
#include <kronos/config.hpp>
#include <kronos/logger.hpp>
#include <kronos/wal.hpp>

int main() {
  // std::cout<<"Kronos Engine";

  try {
    kronos::Config configObj("config/kronos.config");

    std::cout << "\nMem table size= " << configObj.getMemtableSize();

    std::cout << std::boolalpha << "\nBloom filter status = "
              << configObj.getBloomFilterEnabled();

    std::cout << "\nLog path= " << configObj.getLogPath()<<"\n";

    kronos::Logger loggerObj(configObj.getLogPath());
    loggerObj.info("Testing info here");

    // -------------------------
    // WAL test
    // -------------------------

    kronos::Wal wal("test.wal");

    auto recovered = wal.recover();

    std::cout << "\nRecovered " << recovered.size() << " WAL records\n";

    for (const auto &record : recovered) {
      std::cout << "seq=" << record.sequence << " key=" << record.key
                << " value=" << record.value << "\n";
    }

    wal.put("name", "Vishnu");
    wal.put("language", "C++");
    wal.put("project", "Chronos");

    std::cout << "3 new WAL records written successfully\n";
  }

  catch (const std::exception &error) { // Catch exceptions by const reference
                                        // to preserve the original exception.
    std::cerr << "Kronos startup failed - " << error.what() << "\n";
  }

  return 0;
}