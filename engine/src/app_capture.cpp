#include "app_capture.hpp"

#define PSAPI_VERSION 1  // K32* 進 kernel32,免鏈 psapi.lib
#include <windows.h>

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <psapi.h>

#include <cstdio>
#include <cstring>

namespace rmx {

// ---- 程序列舉(Toolhelp;session 來源存 exe 名,載入時由此重解析 pid)----

bool process_exists(std::uint32_t pid) noexcept {
    if (pid == 0) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr) return false;
    CloseHandle(h);
    return true;
}

std::vector<std::pair<std::uint32_t, std::string>> list_process_basenames() {
    std::vector<std::pair<std::uint32_t, std::string>> out;
    std::vector<DWORD> pids(1024);
    DWORD bytes = 0;
    for (;;) {
        if (!EnumProcesses(pids.data(), static_cast<DWORD>(pids.size() * sizeof(DWORD)), &bytes))
            return out;
        if (bytes <= pids.size() * sizeof(DWORD)) break;
        pids.resize(pids.size() * 2);
    }
    const std::size_t count = bytes / sizeof(DWORD);
    wchar_t path[MAX_PATH] = {};
    for (std::size_t i = 0; i < count; ++i) {
        if (pids[i] == 0) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pids[i]);
        if (h == nullptr) continue;
        DWORD size = MAX_PATH;
        std::wstring name;
        if (QueryFullProcessImageNameW(h, 0, path, &size) && size > 0) {
            name = path;
            const auto slash = name.find_last_of(L"\\/");
            if (slash != std::wstring::npos) name = name.substr(slash + 1);
        }
        CloseHandle(h);
        if (!name.empty()) {
            const int need = WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, nullptr, 0,
                                                 nullptr, nullptr);
            std::string utf8(static_cast<std::size_t>(need > 0 ? need - 1 : 0), '\0');
            if (need > 0)
                WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, utf8.data(),
                                    static_cast<int>(utf8.size() + 1), nullptr, nullptr);
            out.emplace_back(pids[i], std::move(utf8));
        }
    }
    return out;
}

std::uint32_t find_pid_by_name(const std::string& exe_basename) {
    for (const auto& [pid, name] : list_process_basenames())
        if (name == exe_basename) return pid;
    return 0;
}

namespace {



// WASAPI 聲道交錯轉 stereo:取 ch0/ch1(>2ch 時其餘忽略;1ch 複製)
void mix_to_stereo(const BYTE* data, std::uint32_t frames, std::uint32_t channels,
                   int format, float* out) {
    const std::uint32_t ch = channels < 2 ? 1 : 2;
    (void)ch;
    if (format == 0) {  // f32
        const float* src = reinterpret_cast<const float*>(data);
        if (channels == 1) {
            for (std::uint32_t i = 0; i < frames; ++i) {
                out[i * 2] = out[i * 2 + 1] = src[i];
            }
        } else {
            for (std::uint32_t i = 0; i < frames; ++i) {
                out[i * 2] = src[i * channels];
                out[i * 2 + 1] = src[i * channels + 1];
            }
        }
    } else {  // s16
        const std::int16_t* src = reinterpret_cast<const std::int16_t*>(data);
        const float k = 1.0F / 32768.0F;
        if (channels == 1) {
            for (std::uint32_t i = 0; i < frames; ++i) {
                out[i * 2] = out[i * 2 + 1] = src[i] * k;
            }
        } else {
            for (std::uint32_t i = 0; i < frames; ++i) {
                out[i * 2] = src[i * channels] * k;
                out[i * 2 + 1] = src[i * channels + 1] * k;
            }
        }
    }
}

// ActivateAudioInterfaceAsync 的完成回呼(agile:免 marshal,threadpool thread 呼)
class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler,
                                public IAgileObject {
public:
    ActivationHandler() : event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~ActivationHandler() {
        if (event_ != nullptr) CloseHandle(event_);
    }

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override { return refs_.fetch_add(1) + 1; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = refs_.fetch_sub(1) - 1;
        if (n == 0) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (out == nullptr) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        } else if (riid == __uuidof(IAgileObject)) {
            *out = static_cast<IAgileObject*>(this);
        } else {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT hr_activate = E_FAIL;
        IUnknown* unk = nullptr;
        const HRESULT hr = op->GetActivateResult(&hr_activate, &unk);
        if (SUCCEEDED(hr) && SUCCEEDED(hr_activate) && unk != nullptr) {
            hr_ = unk->QueryInterface(__uuidof(IAudioClient),
                                      reinterpret_cast<void**>(&audio_client_));
            if (unk != nullptr) unk->Release();
        } else {
            hr_ = SUCCEEDED(hr) ? hr_activate : hr;
        }
        SetEvent(event_);
        return S_OK;
    }

    // 等完成(呼叫端持 reference,timeout 保護;回 activation HRESULT)
    HRESULT wait(DWORD ms) {
        if (event_ == nullptr) return E_FAIL;
        if (WaitForSingleObject(event_, ms) != WAIT_OBJECT_0) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        return hr_;
    }

    IAudioClient* audio_client_{nullptr};

private:
    std::atomic<ULONG> refs_{1};
    HANDLE event_;
    HRESULT hr_ = E_FAIL;
};

}  // namespace

std::shared_ptr<AppCapture> AppCapture::create(std::uint32_t pid, std::uint32_t dst_rate,
                                               FailCallback on_fail, std::string& err) {
    // activation 參數:目標程序樹 loopback(本 SDK 形態 = DWORD TargetProcessId)
    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode =
        PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT pv{};
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(params);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    auto* handler = new ActivationHandler();
    IActivateAudioInterfaceAsyncOperation* op = nullptr;
    const HRESULT hr = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, handler, &op);
    if (FAILED(hr)) {
        handler->Release();
        if (op != nullptr) op->Release();
        err = "process loopback activation failed: " + std::to_string(static_cast<unsigned long>(hr));
        return nullptr;
    }
    const HRESULT hr_activate = handler->wait(2000);
    IAudioClient* client = handler->audio_client_;  // 成功時 handler 交出引用
    if (op != nullptr) op->Release();
    handler->Release();  // 自己的初引用;client 引用獨立
    if (FAILED(hr_activate) || client == nullptr) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "process loopback not supported or failed (hr=0x%08lX)",
                      static_cast<unsigned long>(hr_activate));
        err = msg;
        return nullptr;
    }

    auto cap = std::shared_ptr<AppCapture>(new AppCapture());
    cap->dst_rate_ = dst_rate;
    cap->target_process_ = OpenProcess(SYNCHRONIZE, FALSE, pid);
    cap->on_fail_ = std::move(on_fail);

    // mix format:process loopback 虛擬裝置不供 GetMixFormat — 用預設 render
    // 裝置的 mix format(MS ApplicationLoopback 樣式)
    WAVEFORMATEX* mix = nullptr;
    {
        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* render = nullptr;
        IAudioClient* render_client = nullptr;
        HRESULT hrf = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       __uuidof(IMMDeviceEnumerator),
                                       reinterpret_cast<void**>(&enumerator));
        if (SUCCEEDED(hrf))
            hrf = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &render);
        if (SUCCEEDED(hrf) && render != nullptr)
            hrf = render->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void**>(&render_client));
        if (SUCCEEDED(hrf) && render_client != nullptr)
            hrf = render_client->GetMixFormat(&mix);
        if (render_client != nullptr) render_client->Release();
        if (render != nullptr) render->Release();
        if (enumerator != nullptr) enumerator->Release();
        if (FAILED(hrf) || mix == nullptr) {
            client->Release();
            err = "process loopback: default render mix format unavailable";
            return nullptr;
        }
    }
    bool is_float = false;
    bool is_s16 = false;
    if (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        is_float = true;
    } else if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix);
        const GUID& sub = ext->SubFormat;
        const GUID float_guid = {0x00000003, 0x0000, 0x0010,
                                 {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
        const GUID pcm_guid = {0x00000001, 0x0000, 0x0010,
                               {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
        if (sub == float_guid) is_float = true;
        else if (sub == pcm_guid && mix->wBitsPerSample == 16) is_s16 = true;
    } else if (mix->wFormatTag == WAVE_FORMAT_PCM && mix->wBitsPerSample == 16) {
        is_s16 = true;
    }
    if (!is_float && !is_s16) {
        CoTaskMemFree(mix);
        client->Release();
        err = "process loopback: unsupported mix format";
        return nullptr;
    }
    cap->src_rate_ = mix->nSamplesPerSec;
    cap->channels_ = mix->nChannels < 1 ? 1 : mix->nChannels;
    cap->sample_format_ = is_float ? 0 : 1;

    // 事件驅動 shared capture(MS ApplicationLoopback 樣式;LOOPBACK flag 不可少)
    HRESULT ihr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                     200000, 0, mix, nullptr);
    if (FAILED(ihr)) {
        // 某些 build 拒 LOOPBACK flag on 虛擬 capture:退純 event capture
        ihr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 200000, 0, mix, nullptr);
    }
    CoTaskMemFree(mix);
    if (FAILED(ihr)) {
        client->Release();
        err = "process loopback: Initialize failed";
        return nullptr;
    }
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (ev == nullptr || FAILED(client->SetEventHandle(ev))) {
        if (ev != nullptr) CloseHandle(ev);
        client->Release();
        err = "process loopback: SetEventHandle failed";
        return nullptr;
    }
    IAudioCaptureClient* capture = nullptr;
    if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture))) ||
        capture == nullptr) {
        CloseHandle(ev);
        client->Release();
        err = "process loopback: GetService(IAudioCaptureClient) failed";
        return nullptr;
    }
    if (FAILED(client->Start())) {
        capture->Release();
        CloseHandle(ev);
        client->Release();
        err = "process loopback: Start failed";
        return nullptr;
    }

    cap->audio_client_ = client;
    cap->capture_client_ = capture;
    cap->event_ = ev;
    cap->running_.store(true, std::memory_order_release);
    cap->pump_thread_ = std::thread([c = cap.get()] { c->pump(); });
    return cap;
}

AppCapture::~AppCapture() {
    stop();
}

void AppCapture::stop() noexcept {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    if (pump_thread_.joinable()) pump_thread_.join();
}

// pump thread:事件驅動 drain capture → FIFO。COM 物件在此建立後首次使用,
// 退出時於本執行緒 Release(MTA;WASAPI 物件 free-threaded,跨 thread 持有安全)
void AppCapture::pump() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto* client = static_cast<IAudioClient*>(audio_client_);
    auto* capture = static_cast<IAudioCaptureClient*>(capture_client_);
    auto ev = static_cast<HANDLE>(event_);

    bool device_error = false;
    std::string device_error_msg;
    std::vector<float> scratch;
    auto target = static_cast<HANDLE>(target_process_);
    while (running_.load(std::memory_order_acquire)) {
        // 程序驗活:loopback 裝置在目標死後常只送靜音 buffer 不報錯 —— 每輪查
        // (signaled handle = 程序已終止;OpenProcess 法會被 activation 持有的
        // handle 欺騙)
        if (target != nullptr && WaitForSingleObject(target, 0) == WAIT_OBJECT_0) {
            device_error = true;
            device_error_msg = "app exited";
            break;
        }
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
                    mix_to_stereo(data, frames, channels_, sample_format_, scratch.data());
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
    if (target != nullptr) CloseHandle(target);
    target_process_ = nullptr;
    CoUninitialize();

    if (device_error && running_.load(std::memory_order_acquire) == false) {
        // stop() 發起的退出:不標失敗
        return;
    }
    if (device_error) {
        pump_error_ = device_error_msg.empty() ? "capture device lost" : device_error_msg;
        failed_.store(true, std::memory_order_release);
        if (on_fail_) on_fail_();
    }
}

}  // namespace rmx
