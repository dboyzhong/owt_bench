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

#define DEFAULT_SERVER  "http://10.209.22.248:3001/CreateToken"
#define DEFAULT_ROOM_ID "5e796edf6e34e91e95ed31fa"

using namespace std;
using namespace owt::conference;

int main(int argc, char **argv) {

    init_log();

    cxxopts::Options options("owt-benchmark", "owt benchmark");
    options.add_options()
        ("video-file", "video file name", cxxopts::value<std::string>()->default_value("./source.h264")) 
        ("audio-file", "audil file name", cxxopts::value<std::string>()->default_value("./source.pcm")) 
        ("W,width", "video width", cxxopts::value<int>()->default_value("960"))
        ("H,height", "video height", cxxopts::value<int>()->default_value("540"))
        ("F,fps", "video fps", cxxopts::value<int>()->default_value("30"))
        ("u,url", "server url", cxxopts::value<std::string>()->default_value("http://10.208.135.79:3001"))
        ("r,room_id", "room id", cxxopts::value<std::string>()->default_value("5e78cecf15342f2cbf46f885"))
        ("R,role", "role : pub or sub", cxxopts::value<std::string>()->default_value("pub"))
        ("n,num", "concurrency num", cxxopts::value<int>()->default_value("1"))
        ("a,audio_enable", "audio enable", cxxopts::value<int>()->default_value("1"))
        ("v,video_enable", "video enable", cxxopts::value<int>()->default_value("0"))
        ("d,duration", "test duration", cxxopts::value<int>()->default_value("10"))
        ("h,help", "print help");

    auto result = options.parse(argc, argv);
    if(result.count("help")) {
        printf("%s\n", options.help().c_str());
        exit(0);
    }

    ProcessorConfig cfg;
    cfg.video_file = result["video-file"].as<string>();
    cfg.audio_file = result["audio-file"].as<string>();
    cfg.server_url = result["url"].as<string>();
    cfg.room_id = result["room_id"].as<string>();
    cfg.role    = result["role"].as<string>();
    cfg.num = result["num"].as<int>();
    cfg.video_cfg.width = result["width"].as<int>();
    cfg.video_cfg.height = result["height"].as<int>();
    cfg.video_cfg.fps = result["fps"].as<int>();
    cfg.audio_enable = result["audio_enable"].as<int>();
    cfg.video_enable = result["video_enable"].as<int>();
    int test_duration = result["duration"].as<int>();


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
    processor->Start();
    dispatcher->PostTask([processor, dispatcher](){
        processor->Stop([dispatcher]{
            dispatcher->SyncStop();
        });
    }, std::chrono::seconds(test_duration));
    dispatcher->Run();
    LOG::info("exitted...");
    return 0;
}