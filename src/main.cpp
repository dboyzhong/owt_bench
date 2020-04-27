#include <stdio.h>
#include <string>
#include <memory>
#include <thread>
#include <chrono>
#include <future>
#include "processor.h"
#include "cxxopts.hpp"
#include "logger.h"
#include <boost/asio.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include "dispatcher.h"

#define DEFAULT_SERVER  "http://10.208.135.79:3001/CreateToken"
#define DEFAULT_ROOM_ID "5e78cecf15342f2cbf46f885"

using namespace std;
using namespace owt::conference;

int main(int argc, char **argv) {

    init_log();

    cxxopts::Options options("owt-benchmark", "owt benchmark");
    options.add_options()
        ("f,file", "yuv raw video file name", cxxopts::value<std::string>()->default_value("./source.yuv")) 
        ("W,width", "yuv raw video width", cxxopts::value<int>()->default_value("960"))
        ("H,height", "yuv raw video height", cxxopts::value<int>()->default_value("540"))
        ("F,fps", "yuv raw video fps", cxxopts::value<int>()->default_value("30"))
        ("p,publish_num", "publish number", cxxopts::value<int>()->default_value("0"))
        ("s,subscribe_num", "subscribe number", cxxopts::value<int>()->default_value("0"))
        ("u,url", "server url", cxxopts::value<std::string>()->default_value("http://10.208.135.79:3001"))
        ("r,room_id", "room id", cxxopts::value<std::string>()->default_value("5e78cecf15342f2cbf46f885"))
        ("h,help", "print help");

    auto result = options.parse(argc, argv);
    if(result.count("help")) {
        printf("%s\n", options.help().c_str());
        exit(0);
    }

    ProcessorConfig cfg;
    cfg.file = result["file"].as<string>();
    cfg.server_url = result["url"].as<string>();
    cfg.room_id = result["room_id"].as<string>();
    cfg.pub_num = result["publish_num"].as<int>();
    cfg.sub_num = result["subscribe_num"].as<int>();
    cfg.video_cfg.width = result["width"].as<int>();
    cfg.video_cfg.height = result["height"].as<int>();
    cfg.video_cfg.fps = result["fps"].as<int>();

    auto dispatcher = make_shared<Dispatcher>();
    auto processor = make_shared<Processor>(dispatcher, std::move(cfg));
    dispatcher->AddSignalHandler(SIGINT, [processor, dispatcher](int sig_number){
        if(sig_number == SIGINT) {
            LOG::info("exit...");
            flush_log();
            processor->Stop([dispatcher]{
                dispatcher->SyncStop();
            });
        }
    });
    processor->Init();
    processor->Run();
    dispatcher->Run();
    return 0;
}