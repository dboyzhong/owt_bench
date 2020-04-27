#include "logger.h"
#include "spdlog/sinks/basic_file_sink.h"

using namespace std;
static shared_ptr<spdlog::logger> combined_logger;

void init_log() {
    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_sink_st>());
    sinks.push_back(std::make_shared<spdlog::sinks::daily_file_sink_mt>("owt-bench.log", 23, 59));
    sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>("basic_logger","basic.log"));
    combined_logger = std::make_shared<spdlog::logger>("name", begin(sinks), end(sinks));
    //register it if you need to access it globally
    spdlog::register_logger(combined_logger);
    combined_logger->flush_on(spdlog::level::info);
}

void flush_log() {
    combined_logger->flush();
}