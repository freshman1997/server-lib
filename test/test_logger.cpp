#include "log.h"
#include "logger_factory.h"
#include <iostream>

int main()
{
    std::cout << "\033[32mHello \033[0mWorld!!\n";
    using namespace yuan::log;


    auto logger = LoggerFactory::get_instance()->get_logger(yuan::log::LoggerType::console_);
    logger->log_fmt(Logger::Level::fatal, "{}:{}({}) test...", __FILE__, __LINE__, __func__);

    return 0;
}