#include "render_sink.hpp"

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <cstdio>
#include <cstring>

namespace rmx {

std::shared_ptr<RenderSink> RenderSink::create(const std::string& device_id,
                                               std::uint32_t src_rate, FailCallback on_fail,
                                               Failure& failure) {
    if (device_id.empty()) {
        failure = Failure{Err::kDeviceBusy, "wasapi deviceId empty"};
        return nullptr;
    }
    // endpoint id string → UTF-16
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, device_id.c_str(), -1, nullptr, 0);
    std::wstring wid(static_cast<std::size_t>(wlen > 0 ? wlen : 1), L'\0');
    if (wlen > 0)
        MultiByteToWideChar(CP_UTF8, 0, device_id.c_str(), -1, wid.data(), wlen);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator));
    if (SUCCEEDED(hr)) hr = enumerator->GetDevice(wid.c_str(), &device);
    if (SUCCEEDED(hr) && device != nullptr)
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(&client));
    if (enumerator != nullptr) enumerator->Release();
    if (device != nullptr) device->Release();
    if (FAILED(hr) || client == nullptr) {
        if (client != nullptr) client->Release();
        failure = Failure{Err::kDeviceBusy, "wasapi device not found: " + device_id};
        return nullptr;
    }

    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client->GetMixFormat(&mix)) || mix == nullptr) {
        client->Release();
        failure = Failure{Err::kDeviceBusy, "wasapi: GetMixFormat failed"};
        return nullptr;
    }
    // shared mode 只支援裝置 mix format;僅收 f32(絕大多數裝置 mix = float)
    bool is_float = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                    (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                     reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix)->SubFormat ==
                         GUID{0x00000003, 0x0000, 0x0010,
                              {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}});
    if (!is_float || mix->wBitsPerSample != 32 || mix->nChannels < 1) {
        CoTaskMemFree(mix);
        client->Release();
        failure = Failure{Err::kDeviceBusy, "wasapi: device mix format not f32"};
        return nullptr;
    }
    const std::uint32_t dst_rate = mix->nSamplesPerSec;
    const std::uint32_t channels = mix->nChannels;

    // shared + event 驅動;200ms 緩衝上限(實際由 engine period 決定)
    HRESULT ihr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                     200000, 0, mix, nullptr);
    CoTaskMemFree(mix);
    if (FAILED(ihr)) {
        client->Release();
        failure = Failure{Err::kDeviceBusy, "wasapi: Initialize failed"};
        return nullptr;
    }
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (ev == nullptr || FAILED(client->SetEventHandle(ev))) {
        if (ev != nullptr) CloseHandle(ev);
        client->Release();
        failure = Failure{Err::kDeviceBusy, "wasapi: SetEventHandle failed"};
        return nullptr;
    }
    IAudioRenderClient* render = nullptr;
    if (FAILED(client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render))) ||
        render == nullptr) {
        CloseHandle(ev);
        client->Release();
        failure = Failure{Err::kDeviceBusy, "wasapi: GetService(IAudioRenderClient) failed"};
        return nullptr;
    }
    std::uint32_t buffer_frames = 0;
    if (FAILED(client->GetBufferSize(&buffer_frames)) || buffer_frames == 0) {
        render->Release();
        CloseHandle(ev);
        client->Release();
        failure = Failure{Err::kDeviceBusy, "wasapi: GetBufferSize failed"};
        return nullptr;
    }
    if (FAILED(client->Start())) {
        render->Release();
        CloseHandle(ev);
        client->Release();
        failure = Failure{Err::kDeviceBusy, "wasapi: Start failed"};
        return nullptr;
    }

    auto sink = std::shared_ptr<RenderSink>(new RenderSink());
    sink->src_rate_ = src_rate;
    sink->dst_rate_ = dst_rate;
    sink->buffer_frames_ = buffer_frames;
    sink->channels_ = channels;
    sink->audio_client_ = client;
    sink->render_client_ = render;
    sink->event_ = ev;
    sink->on_fail_ = std::move(on_fail);
    sink->running_.store(true, std::memory_order_release);
    sink->pump_thread_ = std::thread([s = sink.get()] { s->pump(); });
    return sink;
}

RenderSink::~RenderSink() {
    stop();
}

void RenderSink::stop() noexcept {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    if (pump_thread_.joinable()) pump_thread_.join();
}

// pump thread:事件驅動,padding 補幀;DriftReader 做反向漂移(ASIO→裝置 clock)
void RenderSink::pump() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto* client = static_cast<IAudioClient*>(audio_client_);
    auto* render = static_cast<IAudioRenderClient*>(render_client_);
    auto ev = static_cast<HANDLE>(event_);
    std::vector<float> frame;      // 交錯緩衝(1 dst frame * channels)
    frame.resize(static_cast<std::size_t>(channels_) * 2);
    std::vector<float> deint;      // DriftReader 輸出(L/R)
    deint.resize(2 * 8192);
    bool device_error = false;

    while (running_.load(std::memory_order_acquire)) {
        if (WaitForSingleObject(ev, 500) != WAIT_OBJECT_0) continue;
        std::uint32_t padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) { device_error = true; break; }
        const std::uint32_t need = buffer_frames_ - padding;
        if (need == 0) continue;
        BYTE* dst = nullptr;
        if (FAILED(render->GetBuffer(need, &dst))) { device_error = true; break; }
        // 漂移讀:src_rate(ASIO)→ dst_rate(裝置),fill-level 回饋在此。
        // DriftReader 輸出 = 分離 L/R buffer(l[0..n)、r[0..n))
        float* dl = deint.data();
        float* dr = dl + 8192;
        reader_.read(fifo_, dl, dr, need, src_rate_, dst_rate_);
        auto* out = reinterpret_cast<float*>(dst);
        const std::uint32_t ch = channels_;
        for (std::uint32_t i = 0; i < need; ++i) {
            const float l = dl[i];
            const float r = dr[i];
            if (ch == 1) {
                out[i] = 0.5F * (l + r);
            } else {
                out[i * ch] = l;
                out[i * ch + 1] = r;
                for (std::uint32_t c = 2; c < ch; ++c) out[i * ch + c] = 0.0F;
            }
        }
        if (FAILED(render->ReleaseBuffer(need, 0))) { device_error = true; break; }
    }

    client->Stop();
    render->Release();
    client->Release();
    CloseHandle(ev);
    CoUninitialize();

    // stop() 發起的退出 = running_ 已翻 false,不標失敗
    if (device_error && running_.load(std::memory_order_acquire) == false) return;
    if (device_error) {
        pump_error_ = "render device lost";
        failed_.store(true, std::memory_order_release);
        if (on_fail_) on_fail_();
    }
}

}  // namespace rmx
