//
// Created by Laky64 on 20/09/2024.
//

#include "ntgcalls/devices/win_core_device_module.hpp"

#ifdef IS_WINDOWS

#include <ntgcalls/exceptions.hpp>
#include <cmath>

namespace ntgcalls {

    WinCoreDeviceModule::WinCoreDeviceModule(const AudioDescription* desc, const bool isCapture):
        BaseDeviceModule(desc), comInitializer(webrtc::ScopedCOMInitializer::kMTA), mmcssRegistration(L"Pro Audio"), isCapture(isCapture)
    {
        RTC_DCHECK(comInitializer.Succeeded());
        RTC_DCHECK(mmcssRegistration.Succeeded());

        audioSamplesEvent.Set(CreateEvent(nullptr, false, false, nullptr));
        RTC_DCHECK(audioSamplesEvent.IsValid());
        restartEvent.Set(CreateEvent(nullptr, false, false, nullptr));
        RTC_DCHECK(restartEvent.IsValid());
        stopEvent.Set(CreateEvent(nullptr, false, false, nullptr));
        RTC_DCHECK(stopEvent.IsValid());

        const auto metadata = extractMetadata();
        if (metadata.empty() || metadata.size() < 3) {
            throw MediaDeviceError("Invalid device metadata");
        }
        deviceUID = metadata[0];
        deviceIndex = std::stoi(metadata[1]);
        automaticRestart = metadata[2] == "true";
        if (deviceIndex < 0) {
            throw MediaDeviceError("Invalid device index");
        }
    }

    bytes::unique_binary WinCoreDeviceModule::read(int64_t size) {
        if (!firstRead) {
            init();
            thread = std::thread(&WinCoreDeviceModule::runDataListener, this);
            firstRead = true;
            return {};
        }
        if (!buffer) {
            return {};
        }
        return std::move(buffer);
    }

    bool WinCoreDeviceModule::isSupported() {
        return core_audio_utility::IsMMCSSSupported();
    }

    void WinCoreDeviceModule::close() {
        SetEvent(stopEvent.Get());
        if (thread.joinable()) {
            thread.join();
        }
        ResetEvent(stopEvent.Get());
        ResetEvent(restartEvent.Get());
        ResetEvent(audioSamplesEvent.Get());
        stop();
    }

    void WinCoreDeviceModule::init() {
        if (isInitialized) return;
        isInitialized = true;
        const auto dataFlow = isCapture ? eCapture:eRender;
        std::string deviceId = webrtc::AudioDeviceName::kDefaultDeviceId;
        auto role = ERole();
        switch (deviceIndex) {
        case 0:
            role = eConsole;
            break;
        case 1:
            role = eCommunications;
            break;
        default:
            deviceId = deviceUID;
            break;
        }

        const auto audioClientVersion = core_audio_utility::GetAudioClientVersion();
        switch (audioClientVersion) {
        case 3:
            RTC_LOG(LS_INFO) << "Using CoreAudioV3";
            audioClient = core_audio_utility::CreateClient3(deviceId, dataFlow, role);
            break;
        case 2:
            RTC_LOG(LS_INFO) << "Using CoreAudioV2";
            audioClient = core_audio_utility::CreateClient2(deviceId, dataFlow, role);
            break;
        default:
            RTC_LOG(LS_ERROR) << "Using CoreAudioV1";
            audioClient = core_audio_utility::CreateClient(deviceId, dataFlow, role);
        }
        if (!audioClient) {
            throw MediaDeviceError("Failed to create audio client");
        }
        if (audioClientVersion >= 2) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
            if (FAILED(core_audio_utility::SetClientProperties(static_cast<IAudioClient2*>(audioClient.Get())))) {
                throw MediaDeviceError("Failed to set client properties");
            }
        }

        webrtc::AudioParameters params;
        if (FAILED(core_audio_utility::GetPreferredAudioParameters(audioClient.Get(), &params, rate))) {
            throw MediaDeviceError("Failed to get preferred audio parameters");
        }

        WAVEFORMATEX* f = &format.Format;
        f->wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        f->nChannels = rtc::dchecked_cast<WORD>(params.channels());
        f->nSamplesPerSec = rtc::dchecked_cast<DWORD>(params.sample_rate());
        f->wBitsPerSample = rtc::dchecked_cast<WORD>(params.bits_per_sample());
        f->nBlockAlign = f->wBitsPerSample / 8 * f->nChannels;
        f->nAvgBytesPerSec = f->nSamplesPerSec * f->nBlockAlign;
        f->cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        format.Samples.wValidBitsPerSample = rtc::dchecked_cast<WORD>(params.bits_per_sample());
        format.dwChannelMask = f->nChannels == 1 ? KSAUDIO_SPEAKER_MONO : KSAUDIO_SPEAKER_STEREO;
        format.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

        // TODO: Low latency mode is not supported yet
        if (FAILED(core_audio_utility::SharedModeInitialize(audioClient.Get(), &format, audioSamplesEvent, 0, true, &endpointBufferSizeFrames))) {
            throw MediaDeviceError("Failed to initialize shared mode");
        }

        REFERENCE_TIME device_period;
        if (FAILED(core_audio_utility::GetDevicePeriod(audioClient.Get(), AUDCLNT_SHAREMODE_SHARED, &device_period))) {
            throw MediaDeviceError("Failed to get device period");
        }

        const double device_period_in_seconds = static_cast<double>(core_audio_utility::ReferenceTimeToTimeDelta(device_period).ms()) / 1000.0;
        if (const int preferred_frames_per_buffer = static_cast<int>(lround(params.sample_rate() * device_period_in_seconds)); preferred_frames_per_buffer % params.frames_per_buffer()) {
            RTC_LOG(LS_WARNING) << "Preferred frames per buffer is not a multiple of frames per buffer";
        }

        audioSessionControl = core_audio_utility::CreateAudioSessionControl(audioClient.Get());
        if (!audioSessionControl.Get()) {
            throw MediaDeviceError("Failed to create audio session control");
        }

        AudioSessionState state;
        if (FAILED(audioSessionControl->GetState(&state))) {
            throw MediaDeviceError("Failed to get audio session state");
        }

        if (FAILED(audioSessionControl->RegisterAudioSessionNotification(this))) {
            throw MediaDeviceError("Failed to register audio session notification");
        }

        if (isCapture) {
            audioCaptureClient = core_audio_utility::CreateCaptureClient(audioClient.Get());
        } else {
            core_audio_utility::FillRenderEndpointBufferWithSilence(audioClient.Get(), audioRenderClient.Get());
        }
        if (FAILED(static_cast<_com_error>(audioClient->Start()).Error())) {
            throw MediaDeviceError("Failed to start audio client");
        }
    }

    void WinCoreDeviceModule::releaseCOMObjects() {
        if (audioRenderClient.Get()) {
            audioRenderClient.Reset();
        }
        if (audioCaptureClient.Get()) {
            audioCaptureClient.Reset();
        }
        if (audioClient) {
            audioClient.Reset();
        }
        if (audioSessionControl.Get()) {
            audioSessionControl.Reset();
        }
    }

    HRESULT WinCoreDeviceModule::QueryInterface(const IID& riid, void** ppvObject) {
        if (ppvObject == nullptr) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == __uuidof(IAudioSessionEvents)) {
            *ppvObject = static_cast<IAudioSessionEvents*>(this);
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG WinCoreDeviceModule::AddRef() {
        return InterlockedIncrement(&refCount);
    }

    ULONG WinCoreDeviceModule::Release() {
        return InterlockedDecrement(&refCount);
    }

    HRESULT WinCoreDeviceModule::OnDisplayNameChanged(LPCWSTR NewDisplayName, LPCGUID EventContext) {
        return S_OK;
    }

    HRESULT WinCoreDeviceModule::OnIconPathChanged(LPCWSTR NewIconPath, LPCGUID EventContext) {
        return S_OK;
    }

    HRESULT WinCoreDeviceModule::OnSimpleVolumeChanged(float NewVolume, BOOL NewMute, LPCGUID EventContext) {
        return S_OK;
    }

    HRESULT WinCoreDeviceModule::OnChannelVolumeChanged(DWORD ChannelCount, float NewChannelVolumeArray[], DWORD ChangedChannel, LPCGUID EventContext) {
        return S_OK;
    }

    HRESULT WinCoreDeviceModule::OnGroupingParamChanged(LPCGUID NewGroupingParam, LPCGUID EventContext) {
        return S_OK;
    }

    HRESULT WinCoreDeviceModule::OnStateChanged(AudioSessionState NewState) {
        return S_OK;
    }

    HRESULT WinCoreDeviceModule::OnSessionDisconnected(const AudioSessionDisconnectReason DisconnectReason) {
        if (!automaticRestart) {
            return S_OK;
        }
        if (isRestarting) {
            return S_OK;
        }
        if (DisconnectReason == DisconnectReasonDeviceRemoval || DisconnectReason == DisconnectReasonFormatChanged) {
            isRestarting = true;
            SetEvent(restartEvent.Get());
        }
        return S_OK;
    }

    // ReSharper disable once CppDFAUnreachableFunctionCall
    bool WinCoreDeviceModule::handleRestartEvent() {
        bool restartOk = true;
        try {
            stop();
            switchDevice();
            init();
        } catch (...) {
            restartOk = false;
        }
        isRestarting = false;
        return restartOk;
    }

    void WinCoreDeviceModule::runDataListener() {
        bool streaming = true;
        bool error = false;
        HANDLE wait_array[] = {stopEvent.Get(), restartEvent.Get(), audioSamplesEvent.Get()};
        while (streaming && !error) {
            switch (WaitForMultipleObjects(arraysize(wait_array), wait_array, false, INFINITE)) {
            case WAIT_OBJECT_0 + 0:
                streaming = false;
                break;
            case WAIT_OBJECT_0 + 1:
                error = !handleRestartEvent();
                break;
            case WAIT_OBJECT_0 + 2:
                error = !handleDataEvent();
                break;
            default:
                error = true;
                break;
            }
        }
        if (streaming && error) {
            if (const _com_error result = audioClient->Stop(); FAILED(result.Error())) {
                RTC_LOG(LS_ERROR) << "IAudioClient::Stop failed: " << core_audio_utility::ErrorToString(result);
            }
        }
    }

    // ReSharper disable once CppDFAUnreachableFunctionCall
    bool WinCoreDeviceModule::handleDataEvent() {
        if (!isInitialized) {
            return false;
        }
        UINT32 numFramesInNextPacket = 0;
        _com_error error = audioCaptureClient->GetNextPacketSize(&numFramesInNextPacket);
        if (error.Error() == AUDCLNT_E_DEVICE_INVALIDATED) {
            return true;
        }
        if (FAILED(error.Error())) {
            return false;
        }
        while (numFramesInNextPacket > 0) {
            uint8_t* audioData;
            UINT32 numFramesToRead = 0;
            DWORD flags = 0;
            UINT64 devicePositionFrames = 0;
            UINT64 captureTime100ns = 0;
            error = audioCaptureClient->GetBuffer(&audioData, &numFramesToRead, &flags, &devicePositionFrames, &captureTime100ns);
            if (error.Error() == AUDCLNT_S_BUFFER_EMPTY) {
                return true;
            }
            if (FAILED(error.Error())) {
                return false;
            }
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                rtc::ExplicitZeroMemory(audioData, format.Format.nBlockAlign * numFramesToRead);
                RTC_DLOG(LS_WARNING) << "Captured audio is replaced by silence";
            } else {
                buffer = bytes::make_unique_binary(format.Format.nBlockAlign * numFramesToRead);
                memcpy(buffer.get(), audioData, format.Format.nBlockAlign * numFramesToRead);
            }
            error = audioCaptureClient->ReleaseBuffer(numFramesToRead);
            if (FAILED(error.Error())) {
                return false;
            }
            error = audioCaptureClient->GetNextPacketSize(&numFramesInNextPacket);
            if (FAILED(error.Error())) {
                return false;
            }
        }
        return true;
    }

    void WinCoreDeviceModule::stop() {
        if (!isInitialized) return;
        isInitialized = false;
        if (FAILED(static_cast<_com_error>(audioClient->Stop()).Error())) {
            throw MediaDeviceError("Failed to stop audio client");
        }
        if (FAILED(static_cast<_com_error>(audioClient->Reset()).Error())) {
            throw MediaDeviceError("Failed to reset audio client");
        }
        if (!isCapture) {
            UINT32 num_queued_frames = 0;
            if (FAILED(audioClient->GetCurrentPadding(&num_queued_frames))) {
                throw MediaDeviceError("Failed to get current padding");
            }
            RTC_DCHECK_EQ(0u, num_queued_frames);
        }
        if (FAILED(static_cast<_com_error>(audioSessionControl->UnregisterAudioSessionNotification(this)).Error())) {
            throw MediaDeviceError("Failed to unregister audio session notification");
        }
        releaseCOMObjects();
    }

    // ReSharper disable once CppDFAUnreachableFunctionCall
    void WinCoreDeviceModule::switchDevice() {
        if (core_audio_utility::NumberOfActiveDevices(isCapture ? eCapture:eRender) < 1) {
            throw MediaDeviceError("No active devices");
        }
        std::string newDeviceUID;
        switch (deviceIndex) {
        case 0:
            newDeviceUID = isCapture ? core_audio_utility::GetDefaultInputDeviceID() : core_audio_utility::GetDefaultOutputDeviceID();
            break;
        case 1:
            newDeviceUID = isCapture ? core_audio_utility::GetCommunicationsInputDeviceID() : core_audio_utility::GetCommunicationsOutputDeviceID();
            break;
        default:
            webrtc::AudioDeviceNames device_names;
            if (isCapture ? core_audio_utility::GetInputDeviceNames(&device_names) : core_audio_utility::GetOutputDeviceNames(&device_names)) {
                if (deviceIndex < device_names.size()) {
                    newDeviceUID = device_names[deviceIndex].unique_id;
                }
            }
        }
        if (newDeviceUID != deviceUID) {
            deviceUID = newDeviceUID;
            deviceIndex = 0;
        }
    }
} // ntgcalls

#endif