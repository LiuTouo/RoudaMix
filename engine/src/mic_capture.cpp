#include "mic_capture.hpp"

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <cstring>
#include <vector>

#include "pcm_convert.hpp"

namespace rmx {

std::shared_ptr<MicCapture> MicCapture::create(const std::string& device_id,
                                               std::uint32_t dst_rate, FailCallback on_fail,
                                               Failure& failure) {
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator));
    if (SUCCEEDED(hr)) {
        if (device_id.empty()) {
            // 空 = 系統預設麥克風(eMultimedia;eCommunications 是通話裝置)
            hr = enumerator->GetDefaultAudioEndpoint(eCapture, eMultimedia, &device);
        } else {
            // endpoint id string → UTF-16
            const int wlen = MultiByteToWideChar(CP_UTF8, 0, device_id.c_str(), -1, nullptr, 0);
            std::wstring wid(static_cast<std::size_t>(wlen > 0 ? wlen : 1), L'\0');
            if (wlen > 0)
                MultiByteToWideChar(CP_UTF8, 0, device_id.c_str(), -1, wid.data(), wlen);
            hr = enumerator->GetDevice(wid.c_str(), &device);
        }
    }
    if (SUCCEEDED(hr) && device != nullptr)
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(&client));
    if (enumerator != nullptr) enumerator->Release();
    if (device != nullptr) device->Release();
    if (FAILED(hr) || client == nullptr) {
        if (client != nullptr) client->Release();
        failure = Failure{Err::kDeviceBusy,
                          device_id.empty() ? "default capture device not found"
                                            : "capture device not found: " + device_id};
        return nullptr;
    }

    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client->GetMixFormat(&mix)) || mix == nullptr) {
        client->Release();
        failure = Failure{Err::kDeviceBusy, "mic: GetMixFormat failed"};
        return nullptr;
    }
    // shared mode 只支援裝置 mix format;僅收 f32(同 render sink 前例)
    bool is_float = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                    (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                     reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix)->SubFormat ==
                         GUID{0x00000003, 0x0000, 0x0010,
                              {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}});
    if (!is_float || mix->wBitsPerSample != 32 || mix->nChannels < 1) {
        CoTaskMemFree(mix);
        client->Release();
        failure = Failure{Err::kDeviceBusy, "mic: device mix format not f32"};
        return nullptr;
    }
    const std::uint32_t src_rate = mix->nSamplesPerSec;
    const std::uint32_t channels = mix->nChannels;

    // shared + event 驅動一般 capture(無 LOOPBACK flag — 那是 loopback 專屬)
    HRESULT ihr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                     200000, 0, mix, nullptr);
    CoTaskMemFree(mix);
    if (FAILED(ihr)) {
        client->Release();
        failure = Failure{Err::kDeviceBusy, "mic: Initialize failed"};
        return nullptr;
    }
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (ev == nullptr || FAILED(client->SetEventHandle(ev))) {
        if (ev != nullptr) CloseHandle(ev);
        client->Release();
        failure = Failure{Err::kDeviceBusy, "mic: SetEventHandle failed"};
        return nullptr;
    }
    IAudioCaptureClient* capture = nullptr;
    if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture))) ||
        capture == nullptr) {
        CloseHandle(ev);
        client->Release();
        failure = Failure{Err::kDeviceBusy, "mic: GetService(IAudioCaptureClient) failed"};
        return nullptr;
    }
    if (FAILED(client->Start())) {
        capture->Release();
        CloseHandle(ev);
        client->Release();
        failure = Failure{Err::kDeviceBusy, "mic: Start failed"};
        return nullptr;
    }

    auto cap = std::shared_ptr<MicCapture>(new MicCapture());
    cap->src_rate_ = src_rate;
    cap->dst_rate_ = dst_rate;
    cap->channels_ = channels;
    cap->audio_client_ = client;
    cap->capture_client_ = capture;
    cap->event_ = ev;
    cap->on_fail_ = std::move(on_fail);
    cap->running_.store(true, std::memory_order_release);
    cap->pump_thread_ = std::thread([c = cap.get()] { c->pump(); });
    return cap;
}

MicCapture::~MicCapture() {
    stop();
}

void MicCapture::stop() noexcept {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    if (pump_thread_.joinable()) pump_thread_.join();
}

// pump thread:事件驅動 drain capture → FIFO。COM 物件在此建立後首次使用,
// 退出時於本執行緒 Release(MTA;WASAPI 物件 free-threaded,跨 thread 持有安全)
void MicCapture::pump() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto* client = static_cast<IAudioClient*>(audio_client_);
    auto* capture = static_cast<IAudioCaptureClient*>(capture_client_);
    auto ev = static_cast<HANDLE>(event_);

    bool device_error = false;
    std::vector<float> scratch;
    while (running_.load(std::memory_order_acquire)) {
        if (WaitForSingleObject(ev, 500) != WAIT_OBJECT_0) continue;
        for (;;) {
            std::uint32_t packet = 0;
            if (FAILED(capture->GetNextPacketSize(&packet))) { device_error = true; break; }
            if (packet == 0) break;
            BYTE* data = nullptr;
            std::uint32_t frames = 0;
            DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
                device_error = true;
                break;
            }
            if (frames > 0) {
                const std::size_t need = static_cast<std::size_t>(frames) * 2;
                if (scratch.size() < need) scratch.resize(need);
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::memset(scratch.data(), 0, need * sizeof(float));
                } else {
                    wasapi_mix_to_stereo(reinterpret_cast<const std::byte*>(data), frames,
                                         channels_, 0, scratch.data());
                }
                // 滿 = 丟新(舊資料保住延遲上界;drift 回饋會把 fill 拉回)
                (void)fifo_.write(scratch.data(), frames);
            }
            capture->ReleaseBuffer(frames);
        }
        if (device_error) break;
    }

    client->Stop();
    capture->Release();
    client->Release();
    CloseHandle(ev);
    CoUninitialize();

    // stop() 發起的退出 = running_ 已翻 false,不標失敗
    if (device_error && running_.load(std::memory_order_acquire) == false) return;
    if (device_error) {
        pump_error_ = "capture device lost";
        failed_.store(true, std::memory_order_release);
        if (on_fail_) on_fail_();
    }
}

}  // namespace rmx
