//
// Created by Laky64 on 07/10/24.
//

#include <ntgcalls/devices/media_device.hpp>
#include <ntgcalls/io/file_reader.hpp>
#include <ntgcalls/io/audio_file_writer.hpp>
#include <ntgcalls/io/shell_reader.hpp>
#include <ntgcalls/media/media_source_factory.hpp>
#include <rtc_base/logging.h>

namespace ntgcalls {

    std::unique_ptr<BaseReader> MediaSourceFactory::fromInput(const BaseMediaDescription& desc, BaseSink *sink) {
        // SUPPORTED INPUT MODES
        switch (desc.mediaSource) {
        case BaseMediaDescription::MediaSource::File:
            RTC_LOG(LS_INFO) << "Using file reader for " << desc.input;
            return std::make_unique<FileReader>(desc.input, sink);
        case BaseMediaDescription::MediaSource::Shell:
#ifdef BOOST_ENABLED
            RTC_LOG(LS_INFO) << "Using shell reader for " << desc.input;
            return std::make_unique<ShellReader>(desc.input, sink);
#else
            RTC_LOG(LS_ERROR) << "Shell execution is not yet supported on your OS/Architecture";
            throw ShellError("Shell execution is not yet supported on your OS/Architecture");
#endif
        case BaseMediaDescription::MediaSource::Device:
            return MediaDevice::CreateInput(desc, sink);
        case BaseMediaDescription::MediaSource::FFmpeg:
            RTC_LOG(LS_ERROR) << "FFmpeg encoder is not yet supported";
            throw FFmpegError("FFmpeg encoder is not yet supported");
        default:
            RTC_LOG(LS_ERROR) << "Invalid input mode";
            throw InvalidParams("Invalid input mode");
        }
    }

    std::unique_ptr<AudioWriter> MediaSourceFactory::fromAudioOutput(const BaseMediaDescription& desc, BaseSink *sink) {
        // SUPPORTED OUTPUT AUDIO MODES
        switch (desc.mediaSource) {
        case BaseMediaDescription::MediaSource::File:
            RTC_LOG(LS_INFO) << "Using file writer for " << desc.input;
            return std::make_unique<AudioFileWriter>(desc.input, sink);
        default:
            RTC_LOG(LS_ERROR) << "Invalid input mode";
            throw InvalidParams("Invalid input mode");
        }
    }

} // ntgcalls