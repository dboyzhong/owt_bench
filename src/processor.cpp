#include <algorithm>
#include <map>
#include <sstream>
#include <chrono>
#include <future>
#include <cstdlib>
#include "processor.h"
#include "logger.h"
#include "rest_api.h"
#include "owt/base/globalconfiguration.h"
#include "httpclient/asynchttpclient.h"
#include "audioframegenerator.h"

//bool Processor::Init(std::shared_ptr<owt::conference::ConferenceClient> room, 
//                     std::shared_ptr<ConferenceInfo> info) {
bool Processor::GetToken(std::string room, std::string user_name, std::function<void(bool, string)> cb) {

	std::string content;
	content += "{\"room\":\"";
	content += room;
	content += "\",\"role\":\"presenter\",\"username\":\"";
	content += user_name;
	content += "\"}";

    RequestInfo req;
    stringstream ss;
    ss << cfg_.server_url << "/CreateToken";
    req.set_url(ss.str());
    req.set_method(METHOD_POST);
    req.add_header("Content-Type", "application/json");
    req.add_body(content);
    auto client = make_shared<CAsyncHttpClient>(std::ref(*dispatcher_->IOService()), 5);
    client->make_request(req, [client, cb](const ResponseInfo& r){
        LOG::info("get token: {}", r.content);
        cb(true, r.content);
    });
}

bool Processor::Mix(std::string room, std::string pub_id, std::function<void(bool)> cb) {
	std::string content =
		"[{\"op\":\"add\",\"path\":\"/info/inViews\",\"value\":\"common\"}]";

    RequestInfo req;
    stringstream ss;
    ss << cfg_.server_url << "/rooms/" << room << "/streams/" << pub_id;
    req.set_url(ss.str());
    req.set_method(METHOD_PATCH);
    req.add_header("Content-Type", "application/json");
    req.add_body(content);
    auto client = make_shared<CAsyncHttpClient>(std::ref(*dispatcher_->IOService()), 5);
    client->make_request(req, [client, cb](const ResponseInfo& r){
        LOG::info("mix stream status_code: {}", r.status_code);
        cb(r.status_code == 200);
    });
    return true;
}

bool Processor::Join(std::string room_id, std::string user_name, 
                    std::function<void(bool, std::shared_ptr<ConferenceInfo> info, 
                                    shared_ptr<owt::conference::ConferenceClient> room)> cb) {
    auto room = ConferenceClient::Create(owt::conference::ConferenceClientConfiguration());
    GetToken(cfg_.room_id, "test_zhong", [this, room, cb](bool ret, string token)mutable{
	    if (token != "") {
	    	LOG::info("token: {}", token);
	    	room->Join(token, [this, room, cb](std::shared_ptr<ConferenceInfo> info) mutable {
                LOG::info("Join succeeded! room_id {}, mcu_id {}, user_id {}\n",
                info->Id(), info->Self()->Id(), info->Self()->UserId());
                sub_rooms_.emplace_back(room);
                sub_conference_infos_.emplace_back(info);
                cb(true, info, room);
	    	}, [this, cb](unique_ptr<Exception> err) mutable {
	    	    LOG::info("Join failed {}", err->Message());
                cb(false, nullptr, nullptr);
	    	});
	    } else {
	    	LOG::info("Create token error");
            cb(false, nullptr, nullptr);
	    }
    });
}

bool Processor::Start() {
    srand(time(nullptr));
    stringstream ss;
    ss << cfg_.server_url << "/CreateToken";
    owt::base::GlobalConfiguration::SetEncodedVideoFrameEnabled(true);
    owt::base::GlobalConfiguration::SetCustomizedAudioInputEnabled(true, 
                               make_unique<AudioFrameGenerator>("./source.pcm", 2, 48000));

    room_ = ConferenceClient::Create(owt::conference::ConferenceClientConfiguration());
    GetToken(cfg_.room_id, "test_zhong", [this](bool ret, string token){
	    if (token != "") {
	    	LOG::info("token: {}", token);
	    	room_->Join(token, [this](std::shared_ptr<ConferenceInfo> info) mutable {
               LOG::info("Join succeeded! room_id {}, mcu_id {}, user_id {}\n",
               info->Id(), info->Self()->Id(), info->Self()->UserId());
               info_ = info;
               Run();
	    	}, [this](unique_ptr<Exception> err) mutable {
	    	    LOG::info("Join failed {}", err->Message());
	    	});
	    } else {
	    	LOG::info("Create token error");
	    }

    });
    return true;
}

bool Processor::Run() {

    //if(false == fut_.get()) {
    //    return false;
    //}
    dispatcher_->PostTask([this](){
        if(cfg_.role == "pub") {
            StartPublish();
        } else {
            StartSubscribe();
        }
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

void Processor::SubscribeOne(bool mixed, 
    std::function<void(bool success, shared_ptr<ConferenceSubscription> subscription)> handler) {

    Join(cfg_.room_id, "test_zhong", 
                [this, handler, mixed](bool succ, std::shared_ptr<owt::conference::ConferenceInfo> info,
                            shared_ptr<owt::conference::ConferenceClient> room){

        if(!succ) {
            handler(false, nullptr);
            return;
        }

        std::vector<std::shared_ptr<RemoteStream>> remote_streams = info_->RemoteStreams();
        if(mixed) {
            shared_ptr<RemoteStream> mixed_stream;
            for(auto & stream: remote_streams) {
                if(stream->Source().audio == AudioSourceInfo::kMixed) {
                    LOG::info("subscribe audio found mix auido: {}", stream->Id());
                    mixed_stream = stream;
                    break;
                }
            } 
            if(mixed_stream) {
                SubscribeOptions options;
                LOG::info("start subscribe stream_id:{}", mixed_stream->Id()); 
                room->Subscribe(mixed_stream,
                    options,
                    [this, mixed_stream, handler](std::shared_ptr<ConferenceSubscription> subscription) mutable {
                        //mix_subscription_ = subscription;
                        dispatcher_->PostTask([this, mixed_stream, subscription, handler](){
                            LOG::info("subscribe success, stream_id:{}, subscribtion_id:{}", 
                                mixed_stream->Id(), subscription->Id());
                            SubscribeInfo info = std::make_tuple(mixed_stream, 
                            make_shared<SubObserver>(shared_from_this(), mixed_stream->Id(), subscription->Id()),
                            make_shared<VideoRendererMem>(subscription->Id()), subscription);
                            subscription->AddObserver(*std::get<1>(info));
                            //mixed_stream->AttachVideoRenderer(*std::get<2>(info));
                            //subscribe_infos_.emplace(stream->Id(), std::move(info));
                            subscribe_infos_.emplace(subscription->Id(), std::move(info));
                            handler(true, subscription);
                        });
                    },
                    [=](std::unique_ptr<Exception> e) {
                        LOG::error("subscribe error, stream_id:{}, err:{}", mixed_stream->Id(), e->Message());
                        handler(false, nullptr);
                });
            }
        } else {
            //forward stream
            std::vector<std::shared_ptr<RemoteStream>> remote_streams = info_->RemoteStreams();
            if(remote_streams.size() < 2) {
                LOG::info("no forward stream found");
                handler(false, nullptr);
                return;
            }
            int id = rand() % remote_streams.size();
            while(true) {
                if(remote_streams[id]->Source().video != VideoSourceInfo::kMixed) {
                    LOG::info("subscribe video found forward video: {}", remote_streams[id]->Id());
                    break;
                } else {
                    id = (++id) % remote_streams.size();
                }
            }
            auto stream = remote_streams[id];
            SubscribeOptions options;
            LOG::info("start subscribe stream_id:{}", stream->Id()); 
            room->Subscribe(stream,
                options,
                [this, stream, handler](std::shared_ptr<ConferenceSubscription> subscription) mutable {
                    //mix_subscription_ = subscription;
                    dispatcher_->PostTask([this, stream, subscription, handler](){
                        LOG::info("subscribe success, stream_id:{}, subscribtion_id:{}", 
                            stream->Id(), subscription->Id());
                        SubscribeInfo info = std::make_tuple(stream, 
                        make_shared<SubObserver>(shared_from_this(), stream->Id(), subscription->Id()),
                        make_shared<VideoRendererMem>(subscription->Id()), subscription);
                        subscription->AddObserver(*std::get<1>(info));
                        stream->AttachVideoRenderer(*std::get<2>(info));
                        //subscribe_infos_.emplace(stream->Id(), std::move(info));
                        subscribe_infos_.emplace(subscription->Id(), std::move(info));
                        handler(true, subscription);
                    });
                },
                [=](std::unique_ptr<Exception> e) {
                    LOG::error("subscribe error, stream_id:{}, err:{}", stream->Id(), e->Message());
                    handler(false, nullptr);
            });
        }
    });
}

void Processor::PublishOne(bool is_video, 
            std::function<void(bool, shared_ptr<ConferencePublication> publication)> handler) {
    owt::base::VideoEncoderInterface* external_encoder = DirectVideoEncoder::Create(owt::base::VideoCodec::kH264);
    shared_ptr<LocalCustomizedStreamParameters> lcsp;
    if(is_video) {
        lcsp.reset(new LocalCustomizedStreamParameters(true, true));
    } else {
        lcsp.reset(new LocalCustomizedStreamParameters(true, false));
    } 
    //shared_ptr<LocalCustomizedStreamParameters> lcsp(new LocalCustomizedStreamParameters(true, true));
    lcsp->Resolution(cfg_.video_cfg.width, cfg_.video_cfg.height);
    lcsp->Fps(cfg_.video_cfg.fps);
    auto local_stream = LocalStream::Create(lcsp, external_encoder);
    PublishOptions options;
    VideoCodecParameters codec_params;
    codec_params.name = owt::base::VideoCodec::kH264;
    VideoEncodingParameters encoding_params(codec_params, 4000, true);
    AudioEncodingParameters audio_params;
    audio_params.codec.name = owt::base::AudioCodec::kOpus;
    options.video.push_back(encoding_params);
    options.audio.push_back(audio_params);

    LOG::info("publish new stream video: {}", is_video);
    room_->Publish(local_stream,
        options,
        [=](std::shared_ptr<ConferencePublication> publication) {
            dispatcher_->PostTask([this, publication, is_video, handler](){
                std::string pub_id = publication->Id();
                std::string room_id = info_->Id();
                if(!is_video) {
                    Mix(room_id, pub_id,[pub_id](bool success){
                        LOG::info("mix stream, with pub_id {} {}", pub_id, success);
                    });
                }

                auto info = std::make_tuple(publication,
                                            make_shared<PubObserver>(shared_from_this(), pub_id, is_video));
                publication->AddObserver(*std::get<1>(info));
                LOG::info("publish success publication_id: {}", pub_id);
                publish_infos_.emplace(pub_id, std::move(info));
                handler(true, publication);
            });
        },[=](unique_ptr<Exception> err) {
            //LOG::error("publish failed");
            //dispatcher_->PostTask([this, is_video, handler]{
            //    pub_count--;
            //    //StartPublish();
            //    LOG::info("re publish stream");
            //    dispatcher_->PostTask([this, is_video, handler](){
            //        PublishOne(is_video, handler);
            //    }, std::chrono::seconds(2));
            //});
    });
}

bool Processor::StartSubscribe() {

    if(0 == cfg_.num) {
        return true;
    }
    std::vector<std::shared_ptr<RemoteStream>> remote_streams = info_->RemoteStreams();
    if(cfg_.audio_enable) {
        for(int i = 0; i < cfg_.num; i++) {
            SubscribeOne(true, [](bool success, shared_ptr<ConferenceSubscription> subscription){

            });
        }
    }

    if(cfg_.video_enable) {
        for(int i = 0; i < cfg_.num; i++) {
            SubscribeOne(false, [](bool success, shared_ptr<ConferenceSubscription> subscription){

            });
        }
    }
    return true;
}

bool Processor::StartPublish() {

    if(0 == cfg_.num) {
        return true;
    }

    if(cfg_.audio_enable) {
        int count = 0;
        while(count++ < cfg_.num) {
            PublishOne(false, [](bool success, shared_ptr<owt::conference::ConferencePublication> publication){

            });
        }
    }

    if(cfg_.video_enable) {
        int count = 0;
        while(count++ < cfg_.num) {
            PublishOne(true, [](bool success, shared_ptr<owt::conference::ConferencePublication> publication){

            });
        }
    }
    return true;
}

/*bool Processor::StartSubscribe() {

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
}*/

/*bool Processor::StartPublish() {

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
        shared_ptr<LocalCustomizedStreamParameters> lcsp(new LocalCustomizedStreamParameters(true, false));
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
        options.audio.push_back(audio_params);

        LOG::info("publish new stream...");
        room_->Publish(local_stream,
            options,
            [=](std::shared_ptr<ConferencePublication> publication) {

                dispatcher_->PostTask([this, publication](){
                    std::string pub_id = publication->Id();
                    std::string room_id = info_->Id();
                    Mix(room_id, pub_id,[](bool success){
                        LOG::info("mix stream: {}", success);
                    });

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
}*/

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

bool Processor::RemoveSubscribe(string sub_id, std::function<void(void)> cb) {
    dispatcher_->PostTask([this, sub_id, cb](){
        if(subscribe_infos_.find(sub_id) != subscribe_infos_.end()) {
            //std::get<0>(subscribe_infos_[stream_id])->DetachVideoRenderer();
            //std::get<3>(subscribe_infos_[stream_id])->RemoveObserver(*std::get<1>(subscribe_infos_[stream_id]));
            subscribe_infos_.erase(sub_id);
            sub_count--;
            LOG::info("remove subscribe stream with sub_id {}", sub_id);
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
    //StartPublish();
    //StartSubscribe();
    dispatcher_->PostTask([this]{
        Ticker();
    }, std::chrono::seconds(5));
}

PubObserver::PubObserver(shared_ptr<Processor> processor, string pub_id, bool is_video):
                         processor_(processor), pub_id_(pub_id), is_video_(is_video){

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
    //processor_->RemovePublish(pub_id_, [this]{
    //    processor_->PublishOne(is_video_, 
    //        [](bool success, shared_ptr<owt::conference::ConferencePublication> publication){

    //    });
    //});
};

SubObserver::SubObserver(shared_ptr<Processor> processor, string stream_id, string sub_id):
                         processor_(processor), stream_id_(stream_id), sub_id_(sub_id) {

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
    //processor_->RemoveSubscribe(sub_id_, [this]{
    //    //processor_->SubscribeOne()
    //});
};
