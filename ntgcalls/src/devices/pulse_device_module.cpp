//
// Created by Laky64 on 18/09/24.
//

#include "ntgcalls/devices/pulse_device_module.hpp"

#ifdef IS_LINUX

#include <ntgcalls/exceptions.hpp>
#include <modules/audio_device/linux/audio_device_pulse_linux.h>

#define LATE(sym) \
LATESYM_GET(webrtc::adm_linux_pulse::PulseAudioSymbolTable, GetPulseSymbolTable(), sym)

namespace ntgcalls {
    PulseDeviceModule::PulseDeviceModule(const AudioDescription* desc, const bool isCapture): BaseDeviceModule(desc) {
        paMainloop = LATE(pa_threaded_mainloop_new)();
        if (!paMainloop) {
            throw MediaDeviceError("Cannot create mainloop");
        }
        if (const auto err = LATE(pa_threaded_mainloop_start)(paMainloop); err != PA_OK) {
            throw MediaDeviceError("Cannot start mainloop, error=" + std::to_string(err));
        }
        paLock();
        paMainloopApi = LATE(pa_threaded_mainloop_get_api)(paMainloop);
        if (!paMainloopApi) {
            paUnLock();
            throw MediaDeviceError("Cannot get mainloop api");
        }
        paContext = LATE(pa_context_new)(paMainloopApi, "NTgCalls VoiceEngine");
        if (!paContext) {
            paUnLock();
            throw MediaDeviceError("Cannot create context");
        }
        LATE(pa_context_set_state_callback)(paContext, paContextStateCallback, this);
        paStateChanged = false;

        if (const auto err = LATE(pa_context_connect)(paContext, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr); err != PA_OK) {
            paUnLock();
            throw MediaDeviceError("Cannot connect to pulseaudio, error=" + std::to_string(err));
        }

        while (!paStateChanged) {
            LATE(pa_threaded_mainloop_wait)(paMainloop);
        }

        if (const auto state = LATE(pa_context_get_state)(paContext); state != PA_CONTEXT_READY) {
            std::string error;
            if (state == PA_CONTEXT_FAILED) {
                error = "Failed to connect to PulseAudio sound server";
            } else if (state == PA_CONTEXT_TERMINATED) {
                error = "PulseAudio connection terminated early";
            } else {
                error = "Unknown problem connecting to PulseAudio";
            }
            paUnLock();
            throw MediaDeviceError(error);
        }
        paUnLock();

        checkPulseAudioVersion();

        pa_sample_spec sampleSpec;
        sampleSpec.channels = channels;
        sampleSpec.format = PA_SAMPLE_S16LE;
        sampleSpec.rate = rate;
        stream = LATE(pa_stream_new)(paContext, isCapture ? "recStream":"playStream", &sampleSpec, nullptr);
        if (!stream) {
            throw MediaDeviceError("Cannot create stream, err=" + std::to_string(LATE(pa_context_errno)(paContext)));
        }
        if (isCapture) {
            LATE(pa_stream_set_state_callback)(stream, paStreamStateCallback, this);
        }
    }

    void PulseDeviceModule::paLock() const {
        LATE(pa_threaded_mainloop_lock)(paMainloop);
    }

    void PulseDeviceModule::paUnLock() const {
        LATE(pa_threaded_mainloop_unlock)(paMainloop);
    }

    void PulseDeviceModule::checkPulseAudioVersion() {
        paLock();
        pa_operation* paOperation = nullptr;
        paOperation = LATE(pa_context_get_server_info)(paContext, paServerInfoCallback, this);
        waitForOperationCompletion(paOperation);
        paUnLock();
        RTC_LOG(LS_VERBOSE) << "PulseAudio version: " << paServerVersion;
    }

    void PulseDeviceModule::enableReadCallback() {
        LATE(pa_stream_set_read_callback)(stream, &paStreamReadCallback, this);
    }

    void PulseDeviceModule::disableReadCallback() const {
        LATE(pa_stream_set_read_callback)(stream, nullptr, nullptr);
    }

    void PulseDeviceModule::waitForOperationCompletion(pa_operation* paOperation) const {
        if (!paOperation) {
            RTC_LOG(LS_ERROR) << "PaOperation NULL in WaitForOperationCompletion";
            return;
        }
        while (LATE(pa_operation_get_state)(paOperation) == PA_OPERATION_RUNNING) {
            LATE(pa_threaded_mainloop_wait)(paMainloop);
        }
        LATE(pa_operation_unref)(paOperation);
    }

    // ReSharper disable once CppParameterMayBeConstPtrOrRef
    void PulseDeviceModule::paContextStateCallback(pa_context* c, void* pThis) {
        const auto thiz = static_cast<PulseDeviceModule*>(pThis);
        switch (LATE(pa_context_get_state)(c)) {
        case PA_CONTEXT_UNCONNECTED:
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
            break;
        case PA_CONTEXT_FAILED:
        case PA_CONTEXT_TERMINATED:
        case PA_CONTEXT_READY:
            thiz->paStateChanged = true;
            LATE(pa_threaded_mainloop_signal)(thiz->paMainloop, 0);
            break;
        }
    }

    void PulseDeviceModule::paServerInfoCallback(pa_context*, const pa_server_info* i, void* pThis) {
        const auto thiz = static_cast<PulseDeviceModule*>(pThis);
        strncpy(thiz->paServerVersion, i->server_version, 31);
        thiz->paServerVersion[31] = '\0';
        LATE(pa_threaded_mainloop_signal)(thiz->paMainloop, 0);
    }

    void PulseDeviceModule::paStreamStateCallback(pa_stream*, void* pThis) {
        LATE(pa_threaded_mainloop_signal)(static_cast<PulseDeviceModule*>(pThis)->paMainloop, 0);
    }

    void PulseDeviceModule::paStreamReadCallback(pa_stream*, const size_t size, void* pThis) {
        const auto thiz = static_cast<PulseDeviceModule*>(pThis);
        size_t nBytes = size;
        while(nBytes > 0) {
            size_t count = nBytes;
            const void *audio_data;
            const int result = LATE(pa_stream_peek)(thiz->stream, &audio_data, &count);
            if(count == 0) {
                return;
            }
            if(audio_data == nullptr) {
                LATE(pa_stream_drop)(thiz->stream);
                return;
            }
            thiz -> recBuffer = bytes::make_unique_binary(nBytes);
            memcpy(thiz->recBuffer.get(), audio_data, count);
            if(result != 0) {
                return;
            }
            LATE(pa_stream_drop)(thiz->stream);
            nBytes -= count;
        }
    }

    bytes::unique_binary PulseDeviceModule::read(const int64_t size) {
        if (!recording) {
            paLock();
            pa_buffer_attr buffer_attr;
            buffer_attr.maxlength = -1;
            buffer_attr.tlength = -1;
            buffer_attr.prebuf = -1;
            buffer_attr.minreq = -1;
            buffer_attr.fragsize = size;
            if (LATE(pa_stream_connect_record)(stream, deviceId.c_str(), &buffer_attr, PA_STREAM_NOFLAGS) != PA_OK) {
                throw MediaDeviceError("cannot connect to stream");
            }
            RTC_LOG(LS_VERBOSE) << "Connecting stream";
            while (LATE(pa_stream_get_state)(stream) != PA_STREAM_READY) {
                LATE(pa_threaded_mainloop_wait)(paMainloop);
            }
            RTC_LOG(LS_VERBOSE) << "Connected stream";
            enableReadCallback();
            paUnLock();
            recording = true;
            return {};
        }
        return std::move(recBuffer);
    }

    bool PulseDeviceModule::isSupported() {
        return GetPulseSymbolTable()->Load();
    }

    void PulseDeviceModule::close() {
        paLock();
        if (recording) {
            disableReadCallback();
            recording = false;
        }
        if (LATE(pa_stream_get_state)(stream) != PA_STREAM_UNCONNECTED) {
            if (LATE(pa_stream_disconnect)(stream) != PA_OK) {
                paUnLock();
                throw MediaDeviceError("Failed to disconnect stream, err=" + std::to_string(LATE(pa_context_errno)(paContext)));
            }
            RTC_LOG(LS_VERBOSE) << "Disconnected recording";
        }
        LATE(pa_stream_unref)(stream);
        LATE(pa_context_disconnect)(paContext);
        LATE(pa_context_unref)(paContext);
        paUnLock();
        LATE(pa_threaded_mainloop_stop)(paMainloop);
        LATE(pa_threaded_mainloop_free)(paMainloop);
    }
} // pulse

#endif