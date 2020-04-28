#pragma once

#include <stdio.h>
#include <string>
#include <memory>
#include <thread>
#include <vector>
#include <map>
#include <chrono>
#include <tuple>
#include <sstream>
#include "owt/conference/conferenceclient.h"
#include "owt/base/localcamerastreamparameters.h"
#include "owt/base/videorendererinterface.h"
#include "owt/base/stream.h"
#include "owt/conference/remotemixedstream.h"
#include "owt/base/deviceutils.h"
#include "owt/conference/conferenceclient.h"
#include "owt/conference/conferencepublication.h"
#include "owt/conference/conferencesubscription.h"
#include "owt/base/publication.h"
#include "EncodedVideoInput.h"
#include "YuvVideoInput.h"
//#include "cxxopts.hpp"
#include "dispatcher.h"
#include "logger.h"

using namespace std;
using namespace owt::conference;

class VideoRendererMem : public owt::base::VideoRendererInterface {

public:
    VideoRendererMem(string subscribe_id):fp_(nullptr),subscribe_id_(subscribe_id) {
        //stringstream ss;
        //ss << "render_" << subscribe_id << ".yuv"; 
        //fp_ = fopen(ss.str().c_str(), "wb");
    }
	void RenderFrame(std::unique_ptr<VideoBuffer> buffer) override {
        uint64_t buf_size = buffer->resolution.width * buffer->resolution.height;
        uint64_t qsize = buf_size / 4;
        uint64_t frame_data_size = buf_size + 2 * qsize;
        //fwrite(buffer->buffer, frame_data_size, 1, fp_);
	}
    virtual ~VideoRendererMem() override {
        //fclose(fp_);
        //fp_ = nullptr;
    }
    /// Render type that indicates the VideoBufferType the renderer would receive.
    VideoRendererType Type() override {
		return VideoRendererType::kI420;
	};
private:
    FILE *fp_;
    string subscribe_id_;
};

class Processor;
class PubObserver : public PublicationObserver {
public:
    PubObserver(shared_ptr<Processor> processor, string pub_id, bool is_video);
    void OnEnded() override; 
    void OnMute(TrackKind track_kind) override;
    void OnUnmute(TrackKind track_kind) override;
    void OnError(std::unique_ptr<Exception> failure);
protected:
    shared_ptr<Processor> processor_;
    string pub_id_;
    bool is_video_;
};

class SubObserver : public SubscriptionObserver {
public:
    SubObserver(shared_ptr<Processor> processor, string stream_id, string sub_id_);
    void OnEnded() override; 
    void OnMute(TrackKind track_kind) override;
    void OnUnmute(TrackKind track_kind) override;
    void OnError(std::unique_ptr<Exception> failure);
protected:
    shared_ptr<Processor> processor_;
    string stream_id_;
    string sub_id_;
};

struct VideoConfig {
    int width;
    int height;
    int fps;
    int bitRate;
    VideoConfig():width(0), height(0), fps(0), bitRate(0){}
};

struct ProcessorConfig {
    string file;
    string server_url;
    string room_id;
    string role;
    int    pub_num = 0;
    int    sub_num = 0;
    int    num     = 0;
    bool   audio_enable = true;
    bool   video_enable = false;
    VideoConfig video_cfg;
};

class Processor : public enable_shared_from_this<Processor> {
typedef std::tuple<shared_ptr<RemoteStream>,
                   shared_ptr<SubObserver>, 
                   shared_ptr<VideoRendererMem>, 
                   shared_ptr<ConferenceSubscription>> SubscribeInfo;

typedef std::tuple<shared_ptr<ConferencePublication>, shared_ptr<PubObserver>> PublishInfo;
public:
    friend class SubObserver;
    friend class PubObserver;

    Processor(shared_ptr<Dispatcher> dispatcher, ProcessorConfig cfg):
        dispatcher_(dispatcher), cfg_(std::move(cfg)) {};
    //bool Init(std::shared_ptr<owt::conference::ConferenceClient> room,
    //          std::shared_ptr<ConferenceInfo> info);
    bool Start();
    bool Run();
    void Stop(std::function<void(void)> handler);

protected:
    bool GetToken(std::string room, std::string user_name, std::function<void(bool, string)> cb);
    bool Mix(std::string room, std::string pub_id, std::function<void(bool)> cb);
    bool Join(std::string room, std::string user_name, 
                std::function<void(bool,std::shared_ptr<ConferenceInfo> info, 
                                    shared_ptr<owt::conference::ConferenceClient> room)> cb);
    bool StartSubscribe();
    bool StartPublish();
    bool RemoveSubscribe(string stream_id, std::function<void(void)> cb);
    bool RemovePublish(string pub_id, std::function<void(void)> cb);
    void RemoveNotAliveStream();
    void Ticker();
    void SubscribeStat();
    void PublishStat();
    void SubscribeOne(bool mixed, std::function<void(bool, shared_ptr<ConferenceSubscription>)> handler);
    void PublishOne(bool is_video, 
                      std::function<void(bool, shared_ptr<ConferencePublication>)> handler);

protected:

    int pub_count = 0;
    int sub_count = 0;

    ProcessorConfig cfg_;
    shared_ptr<owt::conference::ConferenceClient> room_;
    shared_ptr<ConferenceInfo> info_;
    map<string, SubscribeInfo>   subscribe_infos_;

    vector<shared_ptr<LocalStream>> local_streams_;
    vector<shared_ptr<ConferencePublication>> publications_;
    map<string, PublishInfo> publish_infos_;

    shared_ptr<Dispatcher> dispatcher_;
    std::promise<bool> pro_;
    std::future<bool>  fut_;
    vector<shared_ptr<owt::conference::ConferenceClient>> sub_rooms_;
    vector<shared_ptr<ConferenceInfo>> sub_conference_infos_;
};