#include <stdio.h>
#include <string>
#include <memory>
#include <thread>
#include <chrono>
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
#include "rest_api.h"

#define DEFAULT_SERVER  "http://10.208.135.79:3001/CreateToken"
#define DEFAULT_ROOM_ID "5e78cecf15342f2cbf46f885"

using namespace std;
using namespace owt::conference;

class VideoRendererMem : public owt::base::VideoRendererInterface {

public:
	void RenderFrame(std::unique_ptr<VideoBuffer> buffer) override {
		printf("render frame...\n");
	}
    virtual ~VideoRendererMem() override {}
    /// Render type that indicates the VideoBufferType the renderer would receive.
    VideoRendererType Type() override {
		return VideoRendererType::kARGB;
	};
};

int main() {

	//std::shared_ptr<owt::conference::RemoteMixedStream> remote_mixed_stream;
	std::shared_ptr<owt::conference::RemoteStream> remote_mixed_stream;
	VideoRendererMem *render_window = new VideoRendererMem;
  std::shared_ptr<ConferenceSubscription> subscription_;
	auto room = ConferenceClient::Create(owt::conference::ConferenceClientConfiguration());
	std::string errMsg;
	std::string token = getToken(DEFAULT_SERVER, false, DEFAULT_ROOM_ID, "test_zhong", errMsg);
	if (token != "") {
		printf("token %s\n", token.c_str());
		room->Join(token,
			[=](std::shared_ptr<ConferenceInfo> info) mutable {
		if (room) {
			printf("Join succeeded! room_id %s, mcu_id %s, user_id %s\n",
				info->Id().c_str(), info->Self()->Id().c_str(), info->Self()->UserId().c_str());

			for (auto &per : info->Participants())
			{
				printf("Participant: McuUserId %s, UserId %s, role %s\n", per->Id().c_str(), per->UserId().c_str(), per->Role().c_str());
			}
            std::vector<std::shared_ptr<RemoteStream>> remote_streams = info->RemoteStreams();
            for (auto& remote_stream : remote_streams) {
              // We only subscribe the first mixed stream. If you would like to subscribe other 
              // mixed stream or forward stream that exists already when joining the room, follow
              // the same approach to subscribe and attach renderer to it.
              if (remote_stream->Source().video == VideoSourceInfo::kMixed) {
                //remote_mixed_stream = std::static_pointer_cast<owt::conference::RemoteMixedStream>(remote_stream);
                remote_mixed_stream = remote_stream;
                break;
              }
            }
            if (remote_mixed_stream != nullptr) {
              auto resoltutions = remote_mixed_stream->Capabilities().video.resolutions;
              auto bitrates = remote_mixed_stream->Capabilities().video.bitrate_multipliers;
              auto framerates = remote_mixed_stream->Capabilities().video.frame_rates;
              SubscribeOptions options;
              for (auto it = resoltutions.begin(); it != resoltutions.end(); it++) {
                options.video.resolution.width = (*it).width;
                options.video.resolution.height = (*it).height;
                break;
              }

              VideoCodecParameters codec_params1;
              codec_params1.name = owt::base::VideoCodec::kH264;
              VideoEncodingParameters encoding_params1(codec_params1, 0, false);
              options.video.codecs.push_back(codec_params1);

              room->Subscribe(remote_mixed_stream,
                options,
                [=](std::shared_ptr<ConferenceSubscription> subscription) mutable {
                //mix_subscription_ = subscription;
                subscription_ = subscription;
                remote_mixed_stream->AttachVideoRenderer(*render_window);
                //SetDlgItemText(IDC_EDIT1, L"Subscribe succeed");
                },
                [=](std::unique_ptr<Exception>) {
    		    });
   		    }
			}
		    },
		    	[=](unique_ptr<Exception> err) {
		    	printf("Join failed %s\n", err->Message().c_str());
		    }
	    );
	} else {
		printf("Create token error %s\n", errMsg.c_str());
	}
    std::this_thread::sleep_for(chrono::seconds(500));
    return 0;
}