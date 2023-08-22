//
// Created by Laky64 on 12/08/2023.
//

#pragma once


#include "media/audio_streamer.hpp"
#include "media/video_streamer.hpp"
#include "utils/dispatch_queue.hpp"
#include "io/base_reader.hpp"
#include "configs.hpp"

namespace ntgcalls {
    class Stream {
    public:
        enum Type {
            Audio,
            Video,
        };

        Stream();

        ~Stream();

        void setAVStream(StreamConfig streamConfig);

        void start();

        void pause();

        void resume();

        void mute();

        void unmute();

        void stop();

        void addTracks(const std::shared_ptr<wrtc::PeerConnection> &pc);

        void onStreamEnd(std::function<void(Stream::Type)> &callback);

    private:
        std::shared_ptr<AudioStreamer> audio;
        std::shared_ptr<VideoStreamer> video;
        wrtc::MediaStreamTrack *audioTrack, *videoTrack;
        std::shared_ptr<BaseReader> rAudio, rVideo;
        bool running = false, idling = false, lipSync = false;

        wrtc::synchronized_callback<Type> onEOF;

        DispatchQueue dispatchQueue = DispatchQueue("StreamQueue");

        void sendSample();

        void checkStream();

        std::pair<std::shared_ptr<BaseStreamer>, std::shared_ptr<BaseReader>> unsafePrepareForSample();
    };
}