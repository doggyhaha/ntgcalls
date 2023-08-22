//
// Created by Laky64 on 12/08/2023.
//

#include "video_streamer.hpp"

namespace ntgcalls {
    ntgcalls::VideoStreamer::VideoStreamer() {
        video = std::make_shared<wrtc::RTCVideoSource>();
    }

    ntgcalls::VideoStreamer::~VideoStreamer() {
        fps = 0;
        w = 0;
        h = 0;
        video = nullptr;
    }

    uint64_t ntgcalls::VideoStreamer::frameTime() {
        return 1000 / fps;
    }

    wrtc::MediaStreamTrack *VideoStreamer::createTrack() {
        return video->createTrack();
    }

    void VideoStreamer::sendData(wrtc::binary sample) {
        BaseStreamer::sendData(sample);
        video->OnFrame(
            wrtc::i420ImageData(
                w,
                h,
                sample
            )
        );
    }

    uint64_t VideoStreamer::frameSize() {
        return w * h * 1.5f;
    }

    void VideoStreamer::setConfig(uint16_t width, uint16_t height, uint8_t framesPerSecond) {
        clear();
        w = width;
        h = height;
        fps = framesPerSecond;
    }
}
