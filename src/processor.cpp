#include <algorithm>
#include <map>
#include <sstream>
#include <chrono>
#include <future>
#include "processor.h"
#include "logger.h"
#include "rest_api.h"
#include "owt/base/globalconfiguration.h"

//bool Processor::Init(std::shared_ptr<owt::conference::ConferenceClient> room, 
//                     std::shared_ptr<ConferenceInfo> info) {
bool Processor::Init() {
    stringstream ss;
    ss << cfg_.server_url << "/CreateToken";

    fut_ = pro_.get_future();
    room_ = ConferenceClient::Create(owt::conference::ConferenceClientConfiguration());
    std::string errMsg;
	std::string token = getToken(ss.str(), false, cfg_.room_id, "test_zhong", errMsg);
	if (token != "") {
		LOG::info("token: {}", token);
		room_->Join(token, [this](std::shared_ptr<ConferenceInfo> info) mutable {
            LOG::info("Join succeeded! room_id {}, mcu_id {}, user_id {}\n",
            info->Id(), info->Self()->Id(), info->Self()->UserId());
            info_ = info;
            pro_.set_value(true);
		}, [this](unique_ptr<Exception> err) mutable {
		    LOG::info("Join failed {}", err->Message());
            pro_.set_value(true);
		});
	} else {
		LOG::info("Create token error {}", errMsg);
	}
    owt::base::GlobalConfiguration::SetEncodedVideoFrameEnabled(true);
    return true;
}

bool Processor::Run() {

    if(false == fut_.get()) {
        return false;
    }
    dispatcher_->PostTask([this](){
        StartPublish();
        StartSubscribe();
    });

    dispatcher_->PostTask([this](){
        Ticker();
    }, std::chrono::seconds(5));
} 

void Processor::Stop(std::function<void(void)> handler) {
        
    LOG::info("mcu_id:{}, processor stopping...", info_->Self()->Id());
    dispatcher_->PostTask([this, handler] () mutable {
        for(auto it = subscribe_infos_.begin(); it != subscribe_infos_.end();) {
            std::get<0>(it->second)->DetachVideoRenderer();
            std::get<3>(it->second)->RemoveObserver(*std::get<1>(it->second));
            it = subscribe_infos_.erase(it);
            LOG::info("stop subscribe stream: {}", it->first);
        }   
        
        for(auto it = publish_infos_.begin(); it != publish_infos_.end();) {
            std::get<0>(it->second)->RemoveObserver(*std::get<1>(it->second));
            if(!std::get<0>(it->second)->Ended()) {
                std::get<0>(it->second)->Stop();
                LOG::info("stop publish stream: {}", it->first);
            } else {
                LOG::info("stop publish stream: {}, already ended", it->first);
            }   
            it = publish_infos_.erase(it);
        }   
        
        LOG::info("processor stopped");
        handler();
    }); 
}       

bool Processor::StartSubscribe() {

    if(0 == cfg_.sub_num) {
        return true;
    }
    std::vector<std::shared_ptr<RemoteStream>> remote_streams = info_->RemoteStreams();
    //int count = subscribe_infos_.size();
    for (auto& remote_stream : remote_streams) {

        if(sub_count >= cfg_.sub_num) {
            LOG::info("subscribe stream reach max count {} ", cfg_.sub_num);
            break;
        }

        if(subscribe_infos_.find(remote_stream->Id()) != subscribe_infos_.end()) {
            LOG::info("subscribe stream: {}, already exist!", remote_stream->Id());
            continue;
        }

        if(remote_stream->Source().video == VideoSourceInfo::kMixed) {
            continue;
        }
        sub_count++;
        LOG::info("subscribe stream: {} start", remote_stream->Id());
        auto resoltutions = remote_stream->Capabilities().video.resolutions;
        auto bitrates = remote_stream->Capabilities().video.bitrate_multipliers;
        auto framerates = remote_stream->Capabilities().video.frame_rates;
        SubscribeOptions options;
        room_->Subscribe(remote_stream,
            options,
            [this, remote_stream](std::shared_ptr<ConferenceSubscription> subscription) mutable {
                //mix_subscription_ = subscription;
                dispatcher_->PostTask([this, remote_stream, subscription](){
                    LOG::info("subscribe success, stream_id:{}, subscribtion_id:{}", 
                        remote_stream->Id(), subscription->Id());
                    SubscribeInfo info = std::make_tuple(remote_stream, 
                        make_shared<SubObserver>(shared_from_this(), remote_stream->Id()),
                        make_shared<VideoRendererMem>(subscription->Id()), 
                        subscription);
                    subscription->AddObserver(*std::get<1>(info));
                    remote_stream->AttachVideoRenderer(*std::get<2>(info));
                    subscribe_infos_.emplace(remote_stream->Id(), std::move(info));
                });
            },
            [=](std::unique_ptr<Exception>) {
                LOG::error("subscribe error, stream_id:{}", remote_stream->Id());
                //dispatcher_->PostTask([this]{
                //    StartSubscribe();
                //}, std::chrono::seconds(5));
                dispatcher_->PostTask([this](){
                    sub_count--;
                });
        });
        std::this_thread::sleep_for(std::chrono::microseconds(100));
   	}
    return true;
}

bool Processor::StartPublish() {

    if(0 == cfg_.pub_num) {
        LOG::info("disable publish");
        return true;
    }

    //if(publish_infos_.size() > cfg_.pub_num) {
    if(pub_count > cfg_.pub_num) {
        LOG::info("publish reach max: {}", cfg_.pub_num);
        return true;
    }

    //for(int i = publish_infos_.size(); i < cfg_.pub_num; i++) {
    while(pub_count++ < cfg_.pub_num) {
        //std::unique_ptr<FileFrameGenerator> framer(new FileFrameGenerator(960, 540, 30));
#if 0
        std::unique_ptr<FileFrameGenerator> framer(
            new FileFrameGenerator(cfg_.video_cfg.width, cfg_.video_cfg.height, cfg_.video_cfg.fps));
        shared_ptr<LocalCustomizedStreamParameters> lcsp(new LocalCustomizedStreamParameters(true, true));
        auto local_stream = LocalStream::Create(lcsp, std::move(framer));
#endif
        owt::base::VideoEncoderInterface* external_encoder = DirectVideoEncoder::Create(owt::base::VideoCodec::kH264);
        owt::base::Resolution resolution(640, 480);
        shared_ptr<LocalCustomizedStreamParameters> lcsp(new LocalCustomizedStreamParameters(true, true));
        lcsp->Resolution(cfg_.video_cfg.width, cfg_.video_cfg.height);
        lcsp->Fps(cfg_.video_cfg.fps);
        auto local_stream = LocalStream::Create(lcsp, external_encoder);

        local_streams_.push_back(local_stream);

        PublishOptions options;
        VideoCodecParameters codec_params;
        codec_params.name = owt::base::VideoCodec::kH264;
        VideoEncodingParameters encoding_params(codec_params, 4000, true);
        AudioEncodingParameters audio_params;
        audio_params.codec.name = owt::base::AudioCodec::kOpus;
        options.video.push_back(encoding_params);

        LOG::info("publish new stream...");
        room_->Publish(local_stream,
            options,
            [=](std::shared_ptr<ConferencePublication> publication) {

                dispatcher_->PostTask([this, publication](){
                    std::string pub_id = publication->Id();
                    std::string room_id = info_->Id();

                    auto info = std::make_tuple(publication,
                                                make_shared<PubObserver>(shared_from_this(), pub_id));
                    publication->AddObserver(*std::get<1>(info));
                    LOG::info("publish success publication_id: {}", pub_id);
                    publish_infos_.emplace(pub_id, std::move(info));
                });
            },[=](unique_ptr<Exception> err) {
                LOG::error("publish failed");
                dispatcher_->PostTask([this]{
                    pub_count--;
                    //StartPublish();
                });
        });
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return true;
}

void Processor::RemoveNotAliveStream() {
    std::vector<std::shared_ptr<RemoteStream>> remote_streams = info_->RemoteStreams();
    for(auto it = subscribe_infos_.begin(); it != subscribe_infos_.end();) {
        auto &info = *it;
        auto iter = std::find_if(remote_streams.begin(), remote_streams.end(), [&info](shared_ptr<RemoteStream> stream) {
            return stream->Id() == info.first;
        });

        if(iter == remote_streams.end()) {
            LOG::info("remove not alive stream: {}", it->first);
            std::get<0>(info.second)->DetachVideoRenderer();
            std::get<3>(info.second)->RemoveObserver(*std::get<1>(info.second));
            it = subscribe_infos_.erase(it);
        } else {
            LOG::info("stream: {} still alive", (*iter)->Id());
            ++it;
        }
    };
    LOG::info("alive stream count: {}", subscribe_infos_.size());
}

bool Processor::RemoveSubscribe(string stream_id, std::function<void(void)> cb) {
    dispatcher_->PostTask([this, stream_id, cb](){
        if(subscribe_infos_.find(stream_id) != subscribe_infos_.end()) {
            //std::get<0>(subscribe_infos_[stream_id])->DetachVideoRenderer();
            //std::get<3>(subscribe_infos_[stream_id])->RemoveObserver(*std::get<1>(subscribe_infos_[stream_id]));
            subscribe_infos_.erase(stream_id);
            sub_count--;
            LOG::info("remove subscribe stream: {}", stream_id);
            cb();
        }
    });
    return true;
}

bool Processor::RemovePublish(string pub_id, std::function<void(void)> cb) {
    dispatcher_->PostTask([this, pub_id, cb](){
        if(publish_infos_.find(pub_id) != publish_infos_.end()) {
            //std::get<0>(publish_infos_[pub_id])->RemoveObserver(*std::get<1>(publish_infos_[pub_id]));
            //if(!std::get<0>(publish_infos_[pub_id])->Ended()) {
            //    std::get<0>(publish_infos_[pub_id])->Stop();
            //}
            publish_infos_.erase(pub_id);
            pub_count--;
            LOG::info("remove publish id: {}", pub_id);
            cb();
        }
    });
    return true;
}

void Processor::PublishStat() {
    for(auto &info : publish_infos_) {
        string stream_id = info.first;
        std::get<0>(info.second)->GetStats(
            [this, stream_id](std::shared_ptr<ConnectionStats> stat){
                LOG::info("pubscribe stream: {}, bitrate: {}kbps", 
                          stream_id, stat->video_bandwidth_stats.transmit_bitrate/1024);
            },[](std::unique_ptr<Exception>){

        });
    }
}

void Processor::SubscribeStat() {
    for(auto &info : subscribe_infos_) {
        string stream_id = info.first;
        std::get<3>(info.second)->GetStats(
            [this, stream_id](std::shared_ptr<ConnectionStats> stat){
                //LOG::info("subscribe stream: {}, bitrate: {}kbps ", 
                //          stream_id, stat->video_bandwidth_stats.transmit_bitrate/1024);
                for(auto & r : stat->video_receiver_reports) {
                    LOG::info("subscrbe stream_id: {}, bytes_recvd: {}KB", stream_id, r->bytes_rcvd / 1024);
                } 
            },[](std::unique_ptr<Exception>){

        });
    }
}

void Processor::Ticker() {
    //RemoveNotAliveStream();
    PublishStat();
    SubscribeStat();
    StartPublish();
    StartSubscribe();
    dispatcher_->PostTask([this]{
        Ticker();
    }, std::chrono::seconds(5));
}

PubObserver::PubObserver(shared_ptr<Processor> processor, string pub_id):
                         processor_(processor), pub_id_(pub_id){

}

void PubObserver::OnEnded() {
    LOG::info("publish stream: {} end", pub_id_);
    //processor_->RemovePublish(pub_id_);
}

void PubObserver::OnMute(TrackKind track_kind) {

};

void PubObserver::OnUnmute(TrackKind track_kind) {

};

void PubObserver::OnError(std::unique_ptr<Exception> failure) {
    LOG::error("publish stream: {} error: {}", pub_id_, failure->Message());
    processor_->RemovePublish(pub_id_, [this]{
    });
};

SubObserver::SubObserver(shared_ptr<Processor> processor, string stream_id):
                         processor_(processor), stream_id_(stream_id){

}

void SubObserver::OnEnded() {
    LOG::info("subscribe stream: {} end", stream_id_);
    //processor_->RemoveSubscribe(stream_id_);
}

void SubObserver::OnMute(TrackKind track_kind) {

};

void SubObserver::OnUnmute(TrackKind track_kind) {

};

void SubObserver::OnError(std::unique_ptr<Exception> failure) {
    LOG::error("subscribe stream: {} error: {}", stream_id_, failure->Message());
    processor_->RemoveSubscribe(stream_id_, [this]{
    });
};
