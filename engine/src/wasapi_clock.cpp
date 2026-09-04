#include "wasapi_clock.hpp"

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rmx {

std::unique_ptr<WasapiClock> WasapiClock::create(IAudioCallback* cb,
                                                 std::optional<std::uint32_t> rate_hint,
                                                 Failure& failure) {
    auto* clock = new WasapiClock();
    clock->callback_ = cb;

    // 系統預設輸出(eMultimedia;eCommunications 是通話裝置)
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator));
    if (SUCCEEDED(hr))
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
    if (SUCCEEDED(hr) && device != nullptr)
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(&clock->audio_client_));
    if (enumerator != nullptr) enumerator->Release();
    if (device != nullptr) device->Release();
    if (FAILED(hr) || clock->audio_client_ == nullptr) {
        delete clock;
        failure = Failure{Err::kDeviceOpenFailed,
                          "wasapi: default render device unavailable"};
        return nullptr;
    }

    IAudioClient* client = static_cast<IAudioClient*>(clock->audio_client_);
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client->GetMixFormat(&mix)) || mix == nullptr) {
        client->Release();
        delete clock;
        failure = Failure{Err::kDeviceOpenFailed, "wasapi: GetMixFormat failed"};
        return nullptr;
    }
    // shared mode 只支援裝置 mix format;僅收 f32(同 render sink/mic 前例)
    bool is_float = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                    (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                     reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix)->SubFormat ==
                         GUID{0x00000003, 0x0000, 0x0010,
                              {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}});
    if (!is_float || mix->wBitsPerSample != 32 || mix->nChannels < 1) {
        CoTaskMemFree(mix);
        client->Release();
        delete clock;
        failure = Failure{Err::kDeviceOpenFailed, "wasapi: device mix format not f32"};
        return nullptr;
    }
    clock->mix_rate_ = mix->nSamplesPerSec;
    clock->channels_ = mix->nChannels;
    // shared mode 鎖 mix rate:明確 rate 不符要拒(不可默默忽略)
    if (rate_hint && *rate_hint != clock->mix_rate_) {
        char msg[128];
        std::snprintf(msg, sizeof(msg),
                      "WASAPI shared mode runs at the device mix rate (%u Hz)",
                      clock->mix_rate_);
        CoTaskMemFree(mix);
        client->Release();
        delete clock;
        failure = Failure{Err::kDeviceOpenFailed, msg};
        return nullptr;
    }
    HRESULT ihr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                     200000, 0, mix, nullptr);
    CoTaskMemFree(mix);
    if (FAILED(ihr)) {
        client->Release();
        delete clock;
        failure = Failure{Err::kDeviceOpenFailed, "wasapi: Initialize failed"};
        return nullptr;
    }
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (ev == nullptr || FAILED(client->SetEventHandle(ev))) {
        if (ev != nullptr) CloseHandle(ev);
        client->Release();
        delete clock;
        failure = Failure{Err::kDeviceOpenFailed, "wasapi: SetEventHandle failed"};
        return nullptr;
    }
    if (FAILED(client->GetService(__uuidof(IAudioRenderClient),
                                  reinterpret_cast<void**>(&clock->render_client_))) ||
        clock->render_client_ == nullptr) {
        CloseHandle(ev);
        client->Release();
        delete clock;
        failure = Failure{Err::kDeviceOpenFailed, "wasapi: GetService(IAudioRenderClient) failed"};
        return nullptr;
    }
    std::uint32_t buffer_frames = 0;
    if (FAILED(client->GetBufferSize(&buffer_frames)) || buffer_frames == 0) {
        CloseHandle(ev);
        client->Release();
        delete clock;
        failure = Failure{Err::kDeviceOpenFailed, "wasapi: GetBufferSize failed"};
        return nullptr;
    }
    clock->audio_client_ = client;  // 以下無失敗路徑:所有權移交 clock
    clock->event_ = ev;
    clock->buffer_frames_ = buffer_frames;
    REFERENCE_TIME latency_hns = 0;
    if (SUCCEEDED(client->GetStreamLatency(&latency_hns)) && latency_hns > 0)
        clock->output_latency_ = static_cast<std::uint32_t>(
            latency_hns * clock->mix_rate_ / 10'000'000);
    return std::unique_ptr<WasapiClock>(clock);
}

WasapiClock::~WasapiClock() {
    stop();
}

bool WasapiClock::start(std::string& err) {
    (void)err;
    auto* client = static_cast<IAudioClient*>(audio_client_);
    // callbacks_ 單調遞增(engine start 以 delta 驗 liveness,不可歸零)
    if (client == nullptr) return false;
    if (FAILED(client->Start())) return false;
    running_.store(true, std::memory_order_release);
    pump_thread_ = std::thread([c = this] { c->pump(); });
    return true;
}

void WasapiClock::stop() noexcept {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    if (pump_thread_.joinable()) pump_thread_.join();
}

// pump thread:事件驅動,padding 補幀;chunk ≤ kMaxBlockFrames 驅動 engine
// callback(plugin block hint = GetBufferSize,實際 need 可變 — 呼叫端必須分塊,
// 否則 GetBuffer 區域尾段沒人填)。COM 物件 main thread 建、本執行緒用與釋
// (MTA;WASAPI 物件 free-threaded,跨 thread 持有安全 — RenderSink 慣例)。
void WasapiClock::pump() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto* client = static_cast<IAudioClient*>(audio_client_);
    auto* render = static_cast<IAudioRenderClient*>(render_client_);
    auto ev = static_cast<HANDLE>(event_);
    std::vector<float> inter;
    inter.resize(static_cast<std::size_t>(channels_) * kMaxBlockFrames);
    bool device_error = false;

    while (running_.load(std::memory_order_acquire)) {
        if (WaitForSingleObject(ev, 500) != WAIT_OBJECT_0) continue;
        std::uint32_t padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) { device_error = true; break; }
        const std::uint32_t need = buffer_frames_ - padding;
        if (need == 0) continue;
        BYTE* dst = nullptr;
        if (FAILED(render->GetBuffer(need, &dst))) { device_error = true; break; }
        auto* out = reinterpret_cast<float*>(dst);
        std::uint32_t written = 0;
        while (written < need && running_.load(std::memory_order_acquire)) {
            const std::uint32_t slice =
                need - written > kMaxBlockFrames ? kMaxBlockFrames : need - written;
            std::memset(scratch_[0], 0, sizeof(float) * slice);
            std::memset(scratch_[1], 0, sizeof(float) * slice);
            float* chans[2] = {scratch_[0], scratch_[1]};
            const AudioBlock block{slice, {}, std::span<float* const>{chans, 2}};
            callback_->process(block);
            const float* sl = scratch_[0];
            const float* sr = scratch_[1];
            if (channels_ == 1) {
                for (std::uint32_t i = 0; i < slice; ++i)
                    out[written + i] = 0.5F * (sl[i] + sr[i]);
            } else {
                for (std::uint32_t i = 0; i < slice; ++i) {
                    out[static_cast<std::size_t>(written + i) * channels_] = sl[i];
                    out[static_cast<std::size_t>(written + i) * channels_ + 1] = sr[i];
                    for (std::uint32_t c = 2; c < channels_; ++c)
                        out[static_cast<std::size_t>(written + i) * channels_ + c] = 0.0F;
                }
            }
            written += slice;
        }
        callbacks_.fetch_add(1, std::memory_order_relaxed);
        if (FAILED(render->ReleaseBuffer(need, 0))) { device_error = true; break; }
    }

    client->Stop();
    render->Release();
    client->Release();
    CloseHandle(ev);
    audio_client_ = nullptr;
    render_client_ = nullptr;
    event_ = nullptr;
    CoUninitialize();

    if (device_error) xruns_.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace rmx
