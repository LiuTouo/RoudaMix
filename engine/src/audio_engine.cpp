#include "audio_engine.hpp"

#include <windows.h>

#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <propkeydef.h>  // DEFINE_PROPERTYKEY(functiondiscoverykeys 前置)
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "app_capture.hpp"
#include "render_sink.hpp"

namespace rmx {

namespace {
inline std::uint32_t f32_bits(float v) noexcept {
    std::uint32_t b;
    std::memcpy(&b, &v, sizeof(b));
    return b;
}
inline float bits_f32(std::uint32_t b) noexcept {
    float v;
    std::memcpy(&v, &b, sizeof(v));
    return v;
}
// meter 掃描(RT 安全:純讀 + accumulate atomic)
inline void meter_band(MeterAccumulator& m, std::size_t strip, const float* l,
                       const float* r, std::uint32_t n) noexcept {
    float pl = 0.0F, pr = 0.0F, sl = 0.0F, sr = 0.0F;
    for (std::uint32_t i = 0; i < n; ++i) {
        const float a = std::abs(l[i]), b = r != nullptr ? std::abs(r[i]) : a;
        if (a > pl) pl = a;
        if (b > pr) pr = b;
        sl += l[i] * l[i];
        sr += (r != nullptr ? r[i] : l[i]) * (r != nullptr ? r[i] : l[i]);
    }
    m.accumulate(strip, pl, pr, sl, sr, n);
}
// dest summing:dst += src(RT 內聯熱迴圈)
inline void bus_add(float* dst, const float* src, std::uint32_t n) noexcept {
    for (std::uint32_t i = 0; i < n; ++i) dst[i] += src[i];
}
}  // namespace

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine() {
    exiting_.store(true, std::memory_order_release);
    if (publish_thread_.joinable()) publish_thread_.join();
    device_.close();
    delete rt_graph_.load(std::memory_order_relaxed);
    for (auto& r : retired_) delete r.graph;
    retired_.clear();
    if (shm_ != nullptr) UnmapViewOfFile(shm_);
    if (shm_mapping_ != nullptr) CloseHandle(shm_mapping_);
}

std::vector<AudioEngine::DeviceSummary> AudioEngine::list_devices() {
    std::vector<DeviceSummary> result;
    for (const auto& entry : enumerate_drivers()) {
        DeviceSummary sum;
        sum.key = entry.clsid;
        sum.name = entry.name;
        // 輕量 probe:能開就填能力,開不了只回名字(UI 仍可顯示)
        AsioDevice probe_device;
        std::string err;
        if (probe_device.probe(entry.clsid, err)) {
            const auto& cap = probe_device.capability();
            sum.max_in = cap.max_in;
            sum.max_out = cap.max_out;
            sum.min_buffer = cap.min_buffer;
            sum.max_buffer = cap.max_buffer;
            sum.preferred_buffer = cap.preferred_buffer;
            sum.sample_rates = cap.sample_rates;
            sum.buffer_sizes = cap.buffer_options();
            sum.current_sample_rate = cap.current_sample_rate;
            sum.input_names = cap.input_names;
            sum.output_names = cap.output_names;
        }
        probe_device.close();
        result.push_back(std::move(sum));
    }
    return result;
}

// M5b:預設 render 裝置的 active audio sessions = 正在出聲的 app。
// 呼叫端 = main thread(STA);列舉失敗(無裝置等)= 空清單不報錯
std::vector<AudioEngine::AudioAppInfo> AudioEngine::list_audio_apps() {
    std::vector<AudioAppInfo> apps;
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioSessionManager2* manager = nullptr;
    IAudioSessionEnumerator* sessions = nullptr;
    auto release_all = [&]() {
        if (sessions != nullptr) sessions->Release();
        if (manager != nullptr) manager->Release();
        if (device != nullptr) device->Release();
        if (enumerator != nullptr) enumerator->Release();
    };
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) return apps;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device)) ||
        device == nullptr) {
        release_all();
        return apps;
    }
    if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&manager))) ||
        manager == nullptr) {
        release_all();
        return apps;
    }
    if (FAILED(manager->GetSessionEnumerator(&sessions)) || sessions == nullptr) {
        release_all();
        return apps;
    }
    int count = 0;
    if (FAILED(sessions->GetCount(&count))) {
        release_all();
        return apps;
    }
    const std::uint32_t self_pid = GetCurrentProcessId();
    for (int i = 0; i < count; ++i) {
        IAudioSessionControl* control = nullptr;
        if (FAILED(sessions->GetSession(i, &control)) || control == nullptr) continue;
        IAudioSessionControl2* control2 = nullptr;
        if (SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2),
                                             reinterpret_cast<void**>(&control2))) &&
            control2 != nullptr) {
            AudioSessionState state = AudioSessionStateInactive;
            DWORD pid = 0;
            if (control2->GetState(&state) == S_OK && state == AudioSessionStateActive &&
                control2->GetProcessId(&pid) == S_OK && pid != 0 && pid != self_pid) {
                bool known = false;
                for (const auto& a : apps) known = known || a.pid == pid;
                if (!known) {
                    AudioAppInfo info;
                    info.pid = pid;
                    // session display name 常空:直接用 exe basename
                    for (const auto& [p, n] : list_process_basenames()) {
                        if (p == pid) {
                            info.name = n;
                            break;
                        }
                    }
                    if (info.name.empty()) info.name = "pid " + std::to_string(pid);
                    apps.push_back(std::move(info));
                }
            }
            control2->Release();
        }
        control->Release();
    }
    release_all();
    return apps;
}

// M5c:render endpoints 列舉(main thread STA;default 記號 + mix 率)
std::vector<AudioEngine::RenderDeviceInfo> AudioEngine::list_render_devices() {
    std::vector<RenderDeviceInfo> out;
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDeviceCollection* collection = nullptr;
    IMMDevice* def_device = nullptr;
    LPWSTR def_id = nullptr;
    auto release_all = [&]() {
        if (def_id != nullptr) CoTaskMemFree(def_id);
        if (def_device != nullptr) def_device->Release();
        if (collection != nullptr) collection->Release();
        if (enumerator != nullptr) enumerator->Release();
    };
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator))))
        return out;
    (void)enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &def_device);
    if (def_device != nullptr) (void)def_device->GetId(&def_id);
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection)) ||
        collection == nullptr) {
        release_all();
        return out;
    }
    UINT count = 0;
    if (SUCCEEDED(collection->GetCount(&count))) {
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* device = nullptr;
            if (FAILED(collection->Item(i, &device)) || device == nullptr) continue;
            LPWSTR id = nullptr;
            IPropertyStore* props = nullptr;
            if (SUCCEEDED(device->GetId(&id)) && id != nullptr &&
                SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && props != nullptr) {
                RenderDeviceInfo info;
                const int need =
                    WideCharToMultiByte(CP_UTF8, 0, id, -1, nullptr, 0, nullptr, nullptr);
                if (need > 0) {
                    info.id.resize(static_cast<std::size_t>(need - 1));
                    WideCharToMultiByte(CP_UTF8, 0, id, -1, info.id.data(), need, nullptr,
                                        nullptr);
                }
                PROPVARIANT name{};
                PropVariantInit(&name);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) &&
                    name.vt == VT_LPWSTR && name.pwszVal != nullptr) {
                    const int nlen = WideCharToMultiByte(CP_UTF8, 0, name.pwszVal, -1, nullptr,
                                                        0, nullptr, nullptr);
                    if (nlen > 0) {
                        info.name.resize(static_cast<std::size_t>(nlen - 1));
                        WideCharToMultiByte(CP_UTF8, 0, name.pwszVal, -1, info.name.data(),
                                            nlen, nullptr, nullptr);
                    }
                }
                PropVariantClear(&name);
                if (def_id != nullptr && wcscmp(id, def_id) == 0) info.is_default = true;
                // mix 率(開 client 問;開不了就 0)
                IAudioClient* probe_client = nullptr;
                if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                               reinterpret_cast<void**>(&probe_client))) &&
                    probe_client != nullptr) {
                    WAVEFORMATEX* mix = nullptr;
                    if (SUCCEEDED(probe_client->GetMixFormat(&mix)) && mix != nullptr) {
                        info.sample_rate = mix->nSamplesPerSec;
                        CoTaskMemFree(mix);
                    }
                    probe_client->Release();
                }
                if (!info.id.empty()) out.push_back(std::move(info));
                props->Release();
            }
            if (id != nullptr) CoTaskMemFree(id);
            device->Release();
        }
    }
    release_all();
    return out;
}

// M5b/M5c:pump 失敗(main thread 經 callback 轉入;持 g_engine_mutex)
void AudioEngine::handle_track_failed(std::uint32_t track_id) {
    TrackNode* t = find_track_mut(track_id);
    if (t == nullptr) return;
    if (t->capture != nullptr) {
        t->track_error =
            t->capture->pump_error().empty() ? "capture failed" : t->capture->pump_error();
        t->capture.reset();
        return;
    }
    if (t->render != nullptr) {
        t->track_error =
            t->render->pump_error().empty() ? "render failed" : t->render->pump_error();
        t->render.reset();
    }
}

bool AudioEngine::start(const std::string& device_key,
                        std::optional<std::uint32_t> sample_rate,
                        std::optional<std::uint32_t> buffer_size, std::string& err) {
    if (device_.running()) {
        err = "already running";
        return false;
    }
    // 面板開著時 driver 不能重開(controlPanel 多為 modal,detach thread 還在裡面)
    if (panel_open_.load(std::memory_order_acquire) > 0) {
        err = "hardware panel is open; close it first";
        return false;
    }
    // 換裝置或換取樣率:整個 driver 重開。部分 driver(SSL 實測)在已 init 的
    // instance 上 setSampleRate 回 OK 但 callback 從此不來 —— 重 init 才是真換率
    if (!device_.clsid().empty() &&
        (device_.clsid() != device_key ||
         (sample_rate.has_value() &&
          *sample_rate != device_.capability().current_sample_rate))) {
        device_.close();
    }
    if (!device_.probe(device_key, err)) return false;
    const auto& cap = device_.capability();
    const std::uint32_t rate = sample_rate.value_or(cap.current_sample_rate);
    const std::uint32_t buffer = buffer_size.value_or(0);  // 0 = driver preferred

    // 從所有軌的 source/output 收集 ASIO channel 聯集(M5:軌道自選 pair)
    std::vector<std::uint32_t> in_chans, out_chans;
    for (const auto& t : tracks_) {
        if (t.source.type == TrackSource::kAsioIn) {
            in_chans.push_back(t.source.asio_in_ch);
            in_chans.push_back(t.source.asio_in_ch + 1);
        }
        if (t.output.type == TrackOutput::kAsioOut) {
            out_chans.push_back(t.output.asio_out_ch);
            out_chans.push_back(t.output.asio_out_ch + 1);
        }
    }
    std::sort(in_chans.begin(), in_chans.end());
    in_chans.erase(std::unique(in_chans.begin(), in_chans.end()), in_chans.end());
    std::sort(out_chans.begin(), out_chans.end());
    out_chans.erase(std::unique(out_chans.begin(), out_chans.end()), out_chans.end());
    if (out_chans.empty()) out_chans = {0, 1};  // createBuffers 至少要一組 out

    if (!device_.prepare(rate, in_chans, out_chans, buffer, err)) {
        device_.close();
        return false;
    }
    device_.set_callback(this);

    // M5b/M5c:app capture + wasapi render 啟動(失敗 = 該軌 track_error,
    // 不擋 start;其他軌照跑)
    for (auto& t : tracks_) {
        if (t.source.type == TrackSource::kApp) {
            stop_capture(t);
            std::string cap_err;
            (void)ensure_capture(t, rate, cap_err);
        }
        if (t.output.type == TrackOutput::kWasapiRender) {
            stop_render(t);
            std::string ren_err;
            (void)ensure_render(t, rate, ren_err);
        }
    }

    // 全部軌的 plugin 先 initialize + 進 RT graph,再開 device —— callback 一啟動
    // 就拿到已就緒的 plugin(順序反了 RT 會拿到未 initialize 的鏈,process 全 fail)。
    // terminate 先跑:前次 start 失敗殘留的 initialized 狀態會讓 initialize 拒絕
    for (auto& t : tracks_) {
        for (auto& slot : t.chain) {
            slot.plugin->terminate();
            if (!slot.plugin->initialize(static_cast<double>(rate), device_.block_size())) {
                device_.close();
                err = "plugin '" + slot.name + "' init failed: " + slot.plugin->last_error();
                return false;
            }
        }
    }
    swap_graph();

    const std::uint64_t callbacks_before = device_.callbacks();
    if (!device_.start(err)) {
        device_.close();
        err = std::string("ASIO start failed after rack ready: ") + err;
        return false;
    }
    // SSL 這類 driver:start() 回 OK 但硬體時脈沒換時 callback 從不來(死流)。
    // 短等驗證沒 callback 就明確失敗,引導用硬體面板改率(600ms:Start 鍵可感知延遲)
    Sleep(600);
    if (device_.callbacks() == callbacks_before) {
        device_.close();
        err = "driver did not deliver audio callbacks at " + std::to_string(rate) +
              " Hz; open hardware panel, set rate there, then Start again";
        return false;
    }

    rt_sample_rate_.store(rate, std::memory_order_relaxed);
    last_device_key_ = device_key;  // session 用:stop 後存檔仍記得裝置
    last_sample_rate_ = rate;
    last_buffer_size_ = device_.block_size();
    meters_.set_runtime(static_cast<float>(rate), device_.block_size(),
                        cap.input_latency, cap.output_latency);

    if (shm_ == nullptr && shm_mapping_ == nullptr) {
        shm_mapping_ = telemetry_create(&shm_);
        if (shm_mapping_ == nullptr) {
            // SHM 開不了:音訊照跑、錶不可用(不 fail start)
            std::fprintf(stderr, "[engine] telemetry SHM create failed: %lu\n", GetLastError());
        }
    }
    if (!publish_thread_.joinable()) {
        exiting_.store(false, std::memory_order_release);
        publish_thread_ = std::thread([this] {
            using clock = std::chrono::steady_clock;
            auto next = clock::now();
            while (!exiting_.load(std::memory_order_acquire)) {
                next += std::chrono::milliseconds(33);  // ~30Hz
                std::this_thread::sleep_until(next);
                if (shm_ == nullptr) continue;
                // strip 位置 = 陣列索引:0 = engine 輸出、其後依 snapshot 的
                // track_strip / chain_strips 放軌(kind 1)與 plugin(kind 0)
                std::uint32_t ids[kTelemetryStrips] = {0xFFFFFFFFu};
                std::uint8_t kinds[kTelemetryStrips] = {2};
                std::size_t count = 1;
                const TrackGraph* g = rt_graph_.load(std::memory_order_acquire);
                if (g != nullptr) {
                    for (const auto& t : g->nodes) {
                        if (t.track_strip != kNoStrip && t.track_strip < kTelemetryStrips) {
                            ids[t.track_strip] = t.track_id;
                            kinds[t.track_strip] = 1;
                            count = (std::max)(count, static_cast<std::size_t>(t.track_strip) + 1);
                        }
                        for (std::size_t i = 0; i < t.chain.size() && i < t.chain_strips.size();
                             ++i) {
                            const auto s = t.chain_strips[i];
                            if (s != kNoStrip && s < kTelemetryStrips) {
                                ids[s] = t.chain[i].instance_id;
                                kinds[s] = 0;
                                count = (std::max)(count, static_cast<std::size_t>(s) + 1);
                            }
                        }
                    }
                }
                meters_.publish(*shm_, device_.xruns(), ids, kinds, count, device_.running());
            }
        });
    }
    return true;
}

void AudioEngine::stop() noexcept {
    device_.stop();
    // RT 停 callback 後退 graph、卸 plugin(下次 start 依新 rate 重建)
    retire_graph();
    for (auto& t : tracks_)
        for (auto& slot : t.chain) slot.plugin->terminate();
    stop_captures();
    stop_renders();
    clear_expired_retired(/*force=*/true);
}

// ---- track graph swap(同舊 rack 的 grace 模式)----

void AudioEngine::swap_graph() noexcept {
    // 深拷貝結構殼:chain 內 plugin/ring shared_ptr、RT buffer shared_ptr 共用
    auto* fresh = new TrackGraph{tracks_, {}, {}};
    fresh->order = graph_topo_order(tracks_);
    if (fresh->order.empty()) {
        fresh->order.clear();
        for (std::uint32_t i = 0; i < tracks_.size(); ++i) fresh->order.push_back(i);
    }
    // id → node index(RT dest sum 查表)
    if (!tracks_.empty()) {
        std::uint32_t max_id = 0;
        for (const auto& t : tracks_) max_id = (std::max)(max_id, t.track_id);
        fresh->id_index.assign(static_cast<std::size_t>(max_id) + 1, kNoStrip);
        for (std::uint32_t i = 0; i < tracks_.size(); ++i)
            fresh->id_index[tracks_[i].track_id] = i;
    }
    // src/out 解析:軌選的 pair 基底 → ASIO scratch 位置(沒建 = -1 靜音)
    const auto& imap = device_.input_map();
    const auto& omap = device_.output_map();
    auto resolve = [](const std::vector<std::uint32_t>& map, std::uint32_t ch) -> std::int32_t {
        for (std::size_t i = 0; i < map.size(); ++i)
            if (map[i] == ch) return static_cast<std::int32_t>(i);
        return -1;
    };
    // telemetry strip 預算:0 = engine 輸出,之後 master 序逐軌(軌錶優先,
    // plugin 錶超 64 根就省略 — 錶少幾根好過動 SHM 大小)
    std::size_t next_strip = 1;
    for (auto& t : fresh->nodes) {
        t.src_l = t.src_r = t.out_l = t.out_r = -1;
        if (t.source.type == TrackSource::kAsioIn) {
            t.src_l = resolve(imap, t.source.asio_in_ch);
            t.src_r = resolve(imap, t.source.asio_in_ch + 1);
        }
        if (t.output.type == TrackOutput::kAsioOut) {
            t.out_l = resolve(omap, t.output.asio_out_ch);
            t.out_r = resolve(omap, t.output.asio_out_ch + 1);
        }
        t.track_strip = kNoStrip;
        t.chain_strips.assign(t.chain.size(), kNoStrip);
        if (next_strip < kTelemetryStrips) {
            t.track_strip = static_cast<std::uint32_t>(next_strip++);
            for (auto& s : t.chain_strips) {
                if (next_strip >= kTelemetryStrips) break;
                s = static_cast<std::uint32_t>(next_strip++);
            }
        }
    }
    TrackGraph* old = rt_graph_.exchange(fresh, std::memory_order_acq_rel);
    if (old != nullptr) retired_.push_back({old, GetTickCount64()});
    clear_expired_retired(false);
}

void AudioEngine::retire_graph() noexcept {
    TrackGraph* old = rt_graph_.exchange(nullptr, std::memory_order_acq_rel);
    if (old != nullptr) retired_.push_back({old, GetTickCount64()});
}

bool AudioEngine::rebuild_asio_channels(std::string& err) {
    if (!device_.running()) return true;
    // 與 start() 同款:從所有軌收 ASIO channel 聯集
    std::vector<std::uint32_t> in_chans, out_chans;
    for (const auto& t : tracks_) {
        if (t.source.type == TrackSource::kAsioIn) {
            in_chans.push_back(t.source.asio_in_ch);
            in_chans.push_back(t.source.asio_in_ch + 1);
        }
        if (t.output.type == TrackOutput::kAsioOut) {
            out_chans.push_back(t.output.asio_out_ch);
            out_chans.push_back(t.output.asio_out_ch + 1);
        }
    }
    std::sort(in_chans.begin(), in_chans.end());
    in_chans.erase(std::unique(in_chans.begin(), in_chans.end()), in_chans.end());
    std::sort(out_chans.begin(), out_chans.end());
    out_chans.erase(std::unique(out_chans.begin(), out_chans.end()), out_chans.end());
    if (out_chans.empty()) out_chans = {0, 1};
    // 現行 buffer map 已涵蓋 = 不動(省一次 stop/start;縮減聯集也不回收,無害)
    const auto& imap = device_.input_map();
    const auto& omap = device_.output_map();
    const auto covered = [](const std::vector<std::uint32_t>& map,
                            const std::vector<std::uint32_t>& chans) {
        for (const auto ch : chans)
            if (std::find(map.begin(), map.end(), ch) == map.end()) return false;
        return true;
    };
    if (covered(imap, in_chans) && covered(omap, out_chans)) return true;
    retire_graph();  // RT 停後退 graph;新 map 位置由呼叫端 swap_graph 重解析
    const std::uint32_t rate = rt_sample_rate_.load(std::memory_order_relaxed);
    if (!device_.prepare(rate, in_chans, out_chans, device_.block_size(), err)) {
        device_.close();
        err = "rebuild ASIO buffers failed: " + err;
        return false;
    }
    if (!device_.start(err)) {
        device_.close();
        err = "restart ASIO failed: " + err;
        return false;
    }
    return true;
}

void AudioEngine::clear_expired_retired(bool force) noexcept {
    const std::uint64_t now = GetTickCount64();
    std::vector<Retired> still;
    still.reserve(retired_.size());
    for (auto& r : retired_) {
        if (force || (now - r.tick) > 500)
            delete r.graph;
        else
            still.push_back(std::move(r));
    }
    retired_ = std::move(still);
}

// ---- tracks(控制面)----

TrackNode* AudioEngine::find_track_mut(std::uint32_t track_id) noexcept {
    for (auto& t : tracks_)
        if (t.track_id == track_id) return &t;
    return nullptr;
}

// ponytail:mono 來源也佔整組 pair(不做 ch 級拆用:兩軌共用 pair 的 L/R 屬日後需求)
bool AudioEngine::asio_in_pair_busy(std::uint32_t ch, std::uint32_t except_track) const noexcept {
    for (const auto& t : tracks_)
        if (t.track_id != except_track && t.source.type == TrackSource::kAsioIn &&
            t.source.asio_in_ch == ch)
            return true;
    return false;
}

bool AudioEngine::asio_out_pair_busy(std::uint32_t ch, std::uint32_t except_track) const noexcept {
    for (const auto& t : tracks_)
        if (t.track_id != except_track && t.output.type == TrackOutput::kAsioOut &&
            t.output.asio_out_ch == ch)
            return true;
    return false;
}

bool AudioEngine::track_add(TrackKind kind, const std::string& name, std::uint32_t color,
                            std::uint32_t& track_id, std::string& err) {
    TrackNode t;
    t.kind = kind;
    t.track_id = next_track_id_++;
    t.name = name.empty() ? (std::string(track_kind_str(kind)) + " " +
                             std::to_string(next_track_id_ - 1))
                          : name;
    if (color == 0) {
        // 調色盤輪替:8 色循環,不用使用者挑
        static constexpr std::uint32_t kPalette[] = {0x4da3ff, 0x3ddc84, 0xffb454, 0xff5c5c,
                                                     0xb48cff, 0x4dd0e1, 0xf06292, 0xaed581};
        t.color = kPalette[(t.track_id - 1) % 8];
    } else {
        t.color = color & 0xFFFFFF;
    }
    t.buf = std::make_shared<TrackRt>();
    track_id = t.track_id;
    tracks_.push_back(std::move(t));
    swap_graph();
    return true;
}

bool AudioEngine::track_remove(std::uint32_t track_id, std::string& err) {
    auto it = std::find_if(tracks_.begin(), tracks_.end(),
                           [&](const TrackNode& t) { return t.track_id == track_id; });
    if (it == tracks_.end()) {
        err = "unknown trackId " + std::to_string(track_id);
        return false;
    }
    // 該軌的 plugin editor 先收(editor 與 dispatch 同在 main thread,無並發)
    for (auto& slot : it->chain) {
        if (slot.plugin->editor_open()) slot.plugin->close_editor();
    }
    stop_capture(*it);  // M5b:capture pump 先收(join)再毀節點
    stop_render(*it);   // M5c:render pump 同
    tracks_.erase(it);
    // 其他軌 dests 的懸空引用一併清
    for (auto& t : tracks_) {
        t.dests.erase(std::remove(t.dests.begin(), t.dests.end(), track_id), t.dests.end());
    }
    swap_graph();
    return true;
}

bool AudioEngine::track_set(std::uint32_t track_id, std::optional<std::string> name,
                            std::optional<std::uint32_t> color, std::optional<float> gain,
                            std::optional<bool> mute, std::string& err) {
    TrackNode* t = find_track_mut(track_id);
    if (t == nullptr) {
        err = "unknown trackId " + std::to_string(track_id);
        return false;
    }
    if (name && !name->empty()) t->name = *name;
    if (color) t->color = *color & 0xFFFFFF;
    if (gain) {
        if (!std::isfinite(*gain) || *gain < 0.0F || *gain > 4.0F) {
            err = "gain must be in [0, 4]";
            return false;
        }
        t->gain = *gain;
    }
    if (mute) t->mute = *mute;
    swap_graph();
    return true;
}

bool AudioEngine::track_set_source(std::uint32_t track_id, const TrackSource& source,
                                   std::string& err, std::string& code) {
    TrackNode* t = find_track_mut(track_id);
    if (t == nullptr) {
        err = "unknown trackId " + std::to_string(track_id);
        code = "track_not_found";
        return false;
    }
    if (source.type == TrackSource::kApp) {
        if (t->kind != TrackKind::kApp) {
            err = "only app tracks take an app source";
            code = "bad_command";
            return false;
        }
        if (source.pid == 0 && source.app_name.empty()) {
            err = "app source needs pid or name";
            code = "bad_command";
            return false;
        }
        if (source.pid != 0 && !process_exists(source.pid)) {
            err = "process " + std::to_string(source.pid) + " not found";
            code = "app_not_found";
            return false;
        }
        t->source = source;
        stop_capture(*t);
        t->track_error.clear();
        // running 中即時啟動;失敗 = 命令失敗 + 回滾(未啟動 = start 時再試,軟失敗)
        if (device_.running()) {
            const auto rate = rt_sample_rate_.load(std::memory_order_relaxed);
            std::string cap_err;
            if (!ensure_capture(*t, rate, cap_err)) {
                t->source = TrackSource{};
                err = cap_err;
                code = cap_err.find("process loopback") != std::string::npos
                           ? "unsupported_windows"
                           : (cap_err.find("process not found") != std::string::npos
                                  ? "app_not_found"
                                  : "bad_command");
                return false;
            }
        }
        swap_graph();
        return true;
    }
    if (t->kind == TrackKind::kApp && source.type != TrackSource::kNone) {
        err = "app track takes an app source";
        code = "bad_command";
        return false;
    }
    // fx/output 軌沒有來源選擇(insert 型:上游 dest 指進來)
    if ((t->kind == TrackKind::kFx || t->kind == TrackKind::kOutput) &&
        source.type != TrackSource::kNone) {
        err = track_kind_str(t->kind) + std::string(" track has no source");
        code = "bad_command";
        return false;
    }
    if (source.type == TrackSource::kSine &&
        (source.sine_freq < 20.0F || source.sine_freq > 20000.0F)) {
        err = "sine freq out of range [20,20000]";
        code = "bad_command";
        return false;
    }
    if (source.type == TrackSource::kAsioIn) {
        if (asio_in_pair_busy(source.asio_in_ch, track_id)) {
            err = "asio input pair already used by another track";
            code = "device_busy";
            return false;
        }
        if (!device_.capability().input_types.empty() &&
            source.asio_in_ch + 1 >= device_.capability().input_types.size()) {
            err = "asio input channel out of range";
            code = "bad_command";
            return false;
        }
    }
    const TrackSource prev = t->source;
    t->source = source;
    t->track_error.clear();
    if (source.type != TrackSource::kApp) stop_capture(*t);
    // 跑著時新選的 ASIO pair 要重建裝置 buffer,否則 resolve 不到 = 靜音 + 死錶
    if (source.type == TrackSource::kAsioIn) {
        std::string rerr;
        if (!rebuild_asio_channels(rerr)) {
            t->source = prev;  // 回滾;失敗時串流已停,UI 顯示錯誤、Start 恢復
            swap_graph();
            err = rerr;
            code = "device_busy";
            return false;
        }
    }
    swap_graph();
    return true;
}

// M5b:capture 生命週期(控制面;pid=0 時依 app_name 重解析 — session 載入路徑)
bool AudioEngine::ensure_capture(TrackNode& t, std::uint32_t dst_rate, std::string& err) {
    std::uint32_t pid = t.source.pid;
    if (pid == 0 && !t.source.app_name.empty()) pid = find_pid_by_name(t.source.app_name);
    if (pid == 0 || !process_exists(pid)) {
        t.track_error = "app not running" +
                        (t.source.app_name.empty() ? "" : ": " + t.source.app_name);
        err = t.track_error;
        return false;
    }
    auto cap = AppCapture::create(pid, dst_rate, [this, tid = t.track_id] {
        if (capture_failed_cb_) capture_failed_cb_(tid);
    }, err);
    if (cap == nullptr) {
        t.track_error = err;
        return false;
    }
    t.track_error.clear();
    t.capture = std::move(cap);
    return true;
}

void AudioEngine::stop_capture(TrackNode& t) noexcept {
    if (t.capture != nullptr) t.capture->stop();
    t.capture.reset();
}

void AudioEngine::stop_captures() noexcept {
    for (auto& t : tracks_) stop_capture(t);
}

// M5c:wasapi render sink 生命週期(同 capture 語意)
bool AudioEngine::ensure_render(TrackNode& t, std::uint32_t src_rate, std::string& err) {
    auto sink = RenderSink::create(t.output.wasapi_id, src_rate,
                                   [this, tid = t.track_id] {
                                       if (capture_failed_cb_) capture_failed_cb_(tid);
                                   },
                                   err);
    if (sink == nullptr) {
        t.track_error = err;
        return false;
    }
    t.track_error.clear();
    t.render = std::move(sink);
    return true;
}

void AudioEngine::stop_render(TrackNode& t) noexcept {
    if (t.render != nullptr) t.render->stop();
    t.render.reset();
}

void AudioEngine::stop_renders() noexcept {
    for (auto& t : tracks_) stop_render(t);
}

bool AudioEngine::track_set_dests(std::uint32_t track_id, std::vector<std::uint32_t> dests,
                                  std::string& err, std::string& code) {
    TrackNode* t = find_track_mut(track_id);
    if (t == nullptr) {
        err = "unknown trackId " + std::to_string(track_id);
        code = "track_not_found";
        return false;
    }
    for (const auto d : dests) {
        if (d == track_id) {
            err = "track cannot route to itself";
            code = "bad_command";
            return false;
        }
        if (find_track_mut(d) == nullptr) {
            err = "unknown dest trackId " + std::to_string(d);
            code = "track_not_found";
            return false;
        }
    }
    std::sort(dests.begin(), dests.end());
    dests.erase(std::unique(dests.begin(), dests.end()), dests.end());
    // 有環不動 master(冪等重試安全);RT 端契約 = control 面保證無環
    const auto old = t->dests;
    t->dests = std::move(dests);
    if (graph_has_cycle(tracks_)) {
        t->dests = std::move(old);
        err = "routing would create a cycle";
        code = "cycle_detected";
        return false;
    }
    swap_graph();
    return true;
}

bool AudioEngine::track_set_output(std::uint32_t track_id, const TrackOutput& output,
                                   std::string& err, std::string& code) {
    TrackNode* t = find_track_mut(track_id);
    if (t == nullptr) {
        err = "unknown trackId " + std::to_string(track_id);
        code = "track_not_found";
        return false;
    }
    if (t->kind != TrackKind::kOutput && output.type != TrackOutput::kNone) {
        err = "only output tracks take a sink";
        code = "bad_command";
        return false;
    }
    if (output.type == TrackOutput::kWasapiRender) {
        if (output.wasapi_id.empty()) {
            err = "wasapi deviceId empty";
            code = "bad_command";
            return false;
        }
        // 裝置存在性:endpoint id 對得起來(不開 stream;真正開在 start)
        bool known = false;
        for (const auto& d : list_render_devices()) known = known || d.id == output.wasapi_id;
        if (!known) {
            err = "wasapi device not found: " + output.wasapi_id;
            code = "device_busy";
            return false;
        }
        t->output = output;
        t->track_error.clear();
        stop_render(*t);
        if (device_.running()) {
            const auto rate = rt_sample_rate_.load(std::memory_order_relaxed);
            std::string ren_err;
            if (!ensure_render(*t, rate, ren_err)) {
                t->output = TrackOutput{};
                err = ren_err;
                code = "device_busy";
                return false;
            }
        }
        swap_graph();
        return true;
    }
    if (output.type == TrackOutput::kAsioOut) {
        if (asio_out_pair_busy(output.asio_out_ch, track_id)) {
            err = "asio output pair already used by another track";
            code = "device_busy";
            return false;
        }
        if (!device_.capability().output_types.empty() &&
            output.asio_out_ch + 1 >= device_.capability().output_types.size()) {
            err = "asio output channel out of range";
            code = "bad_command";
            return false;
        }
    }
    const TrackOutput prev = t->output;
    t->output = output;
    t->track_error.clear();
    if (output.type != TrackOutput::kWasapiRender) stop_render(*t);
    // 同 track_set_source:跑著時新 ASIO out pair 要重建裝置 buffer
    if (output.type == TrackOutput::kAsioOut) {
        std::string rerr;
        if (!rebuild_asio_channels(rerr)) {
            t->output = prev;
            swap_graph();
            err = rerr;
            code = "device_busy";
            return false;
        }
    }
    swap_graph();
    return true;
}

bool AudioEngine::track_move(std::uint32_t track_id, std::size_t new_index, std::string& err) {
    // master 陣列絕對索引重排(UI 輸入/輸出帶內拖放;帶是 kind 過濾,各成連續相對序)
    const auto cur = static_cast<std::size_t>(
        std::find_if(tracks_.begin(), tracks_.end(),
                     [&](const TrackNode& t) { return t.track_id == track_id; }) -
        tracks_.begin());
    if (cur >= tracks_.size()) {
        err = "unknown trackId " + std::to_string(track_id);
        return false;
    }
    if (cur == new_index) return true;
    TrackNode moved = std::move(tracks_[cur]);  // move 保住 shared_ptr buf(gain_state 延續)
    tracks_.erase(tracks_.begin() + static_cast<std::ptrdiff_t>(cur));
    const auto pos = (std::min)(new_index, tracks_.size());  // erase 後插入位 [0, N-1];超尾 = 移到尾端
    tracks_.insert(tracks_.begin() + static_cast<std::ptrdiff_t>(pos), std::move(moved));
    swap_graph();
    return true;
}

RackSlot* AudioEngine::find_slot_mut(std::uint32_t instance_id) noexcept {
    for (auto& t : tracks_)
        for (auto& s : t.chain)
            if (s.instance_id == instance_id) return &s;
    return nullptr;
}

const RackSlot* AudioEngine::find_slot(std::uint32_t instance_id) const noexcept {
    for (const auto& t : tracks_)
        for (const auto& s : t.chain)
            if (s.instance_id == instance_id) return &s;
    return nullptr;
}

std::vector<AudioEngine::PluginTabInfo> AudioEngine::plugin_tabs() const {
    std::vector<PluginTabInfo> tabs;
    for (const auto& t : tracks_) {
        for (const auto& s : t.chain) {
            tabs.push_back({s.instance_id, t.name + " · " + s.name, s.plugin->editor_capable(),
                            s.bypass});
        }
    }
    return tabs;
}

bool AudioEngine::add_plugin(std::uint32_t track_id, const std::string& module_path,
                             const std::string& class_id, std::uint32_t& instance_id,
                             std::string& err) {
    TrackNode* t = find_track_mut(track_id);
    if (t == nullptr) {
        err = "unknown trackId " + std::to_string(track_id);
        return false;
    }
    RackSlot slot;
    slot.plugin = std::make_shared<Vst3Plugin>(module_path, class_id);
    if (!slot.plugin->loaded()) {
        err = slot.plugin->last_error();
        return false;
    }
    if (device_.running() &&
        !slot.plugin->initialize(static_cast<double>(rt_sample_rate_.load(std::memory_order_relaxed)),
                                 device_.block_size())) {
        err = slot.plugin->last_error();
        return false;
    }
    slot.instance_id = next_instance_id_++;
    slot.module_path = module_path;
    slot.class_id = slot.plugin->class_uid();
    slot.name = slot.plugin->name();
    for (const auto& p : slot.plugin->params())
        slot.param_values.push_back({p.id, p.default_normalized});
    instance_id = slot.instance_id;
    t->chain.push_back(std::move(slot));
    swap_graph();
    return true;
}

bool AudioEngine::remove_plugin(std::uint32_t instance_id, std::string& err) {
    for (auto& t : tracks_) {
        for (auto it = t.chain.begin(); it != t.chain.end(); ++it) {
            if (it->instance_id == instance_id) {
                // editor 視窗先收(同步 DestroyWindow;editor 與 dispatch 同在 main
                // thread,無並發——performEdit 回呼不會同時跑)
                if (it->plugin->editor_open()) it->plugin->close_editor();
                t.chain.erase(it);
                swap_graph();
                return true;
            }
        }
    }
    err = "unknown instanceId " + std::to_string(instance_id);
    return false;
}

bool AudioEngine::move_plugin(std::uint32_t instance_id, std::size_t to_index,
                              std::string& err) {
    for (auto& t : tracks_) {
        auto& chain = t.chain;
        if (to_index >= chain.size() &&
            std::none_of(chain.begin(), chain.end(),
                         [&](const RackSlot& s) { return s.instance_id == instance_id; }))
            continue;
        const auto from = std::find_if(chain.begin(), chain.end(),
                                       [&](const RackSlot& s) { return s.instance_id == instance_id; });
        if (from == chain.end()) continue;
        if (to_index >= chain.size()) {
            err = "toIndex out of range";
            return false;
        }
        RackSlot moved = std::move(*from);
        chain.erase(from);
        chain.insert(chain.begin() + static_cast<std::ptrdiff_t>(to_index), std::move(moved));
        swap_graph();
        return true;
    }
    err = "unknown instanceId " + std::to_string(instance_id);
    return false;
}

bool AudioEngine::set_bypass(std::uint32_t instance_id, bool bypass, std::string& err) {
    RackSlot* s = find_slot_mut(instance_id);
    if (s == nullptr) {
        err = "unknown instanceId " + std::to_string(instance_id);
        return false;
    }
    s->bypass = bypass;
    swap_graph();
    return true;
}

bool AudioEngine::set_param(std::uint32_t instance_id, std::uint32_t param_id, double value,
                            std::string& err) {
    RackSlot* s = find_slot_mut(instance_id);
    if (s == nullptr) {
        err = "unknown instanceId " + std::to_string(instance_id);
        return false;
    }
    if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
        err = "param value must be normalized [0,1]";
        return false;
    }
    bool found = false;
    for (auto& [id, v] : s->param_values) {
        if (id == param_id) {
            v = value;
            found = true;
            break;
        }
    }
    if (!found) {
        err = "unknown paramId " + std::to_string(param_id);
        return false;
    }
    s->ring->push({param_id, value});  // 滿 = drop;權威值已更新,UI 重送冪等
    return true;
}

bool AudioEngine::save_preset(std::uint32_t instance_id, const std::filesystem::path& file,
                              std::string& err) {
    const RackSlot* s = find_slot(instance_id);
    if (s == nullptr) {
        err = "unknown instanceId " + std::to_string(instance_id);
        return false;
    }
    // getState 與 RT process 不得併發(VST3 契約):掛 bypass 讓 RT 放掉 plugin,
    // 等在飛的舊 graph block 跑完再 IO,做完還原
    RackSlot* mut = find_slot_mut(instance_id);
    const bool orig_bypass = mut->bypass;
    mut->bypass = true;
    swap_graph();
    Sleep(60);  // > 2 個最大 ASIO block:RT 不再持舊鏈
    const bool ok = mut->plugin->save_preset(file, mut->param_values, err);
    mut->bypass = orig_bypass;
    swap_graph();
    return ok;
}

bool AudioEngine::load_preset(std::uint32_t instance_id, const std::filesystem::path& file,
                              std::string& err) {
    RackSlot* s = find_slot_mut(instance_id);
    if (s == nullptr) {
        err = "unknown instanceId " + std::to_string(instance_id);
        return false;
    }
    // setState 與 RT process 不得併發:同 save_preset,先掛 bypass
    const bool orig_bypass = s->bypass;
    s->bypass = true;
    swap_graph();
    Sleep(60);
    bool host_values_from_file = false;
    const bool ok = s->plugin->load_preset(file, s->param_values, err, host_values_from_file);
    s->bypass = orig_bypass;
    swap_graph();
    if (!ok) return false;
    // 檔案無 RmxP(外部 host 存的 preset)且 controller 同步成功:拿 controller
    // 值重同步 host 權威表。兩者皆無 = 保持現值(component 已套用,UI 值不明)
    if (!host_values_from_file) {
        for (auto& [id, v] : s->param_values) {
            const double fresh = s->plugin->param_value(id);
            if (std::isfinite(fresh)) v = fresh;
        }
    }
    // 三路同步:load_preset 只更新了 host 權威表/component state —— RT(ring)與
    // controller(editor GUI 顯示)都沒吃到,kHs Gain 實測數值不回來。這裡補推
    for (auto& [id, v] : s->param_values) {
        s->ring->push({id, v});                    // RT 下一個 block 套用
        s->plugin->set_param_normalized(id, v);    // controller → editor GUI
    }
    return true;
}

void AudioEngine::sync_controller_params(std::uint32_t instance_id) {
    // session 載入:set_param 只餵 RT ring,controller(editor GUI)不知道 ——
    // 開 GUI 會看到舊值/預設值。這裡把 host 權威值推給 controller 同步顯示
    RackSlot* s = find_slot_mut(instance_id);
    if (s == nullptr) return;
    for (const auto& [id, v] : s->param_values) s->plugin->set_param_normalized(id, v);
}

// controlPanel() 多數 driver 是 modal(關面板才返回)— 呼叫端(detach thread)
// 會在裡面待到面板關閉;panel_open_ 期間 start() 拒絕(driver 銷毀 race)
bool AudioEngine::open_control_panel(std::string& err) {
    if (!device_.running()) {
        err = "not running";
        return false;
    }
    panel_open_.fetch_add(1, std::memory_order_acq_rel);
    const bool okp = device_.open_control_panel(err);
    panel_open_.fetch_sub(1, std::memory_order_acq_rel);
    return okp;
}

EngineStatusInfo AudioEngine::status() const {
    EngineStatusInfo s;
    s.running = device_.running();
    s.device_key = device_.running() ? device_.clsid() : "";
    s.sample_rate =
        device_.running() ? static_cast<float>(rt_sample_rate_.load(std::memory_order_relaxed))
                          : 0.0F;
    s.buffer_size = device_.running() ? device_.block_size() : 0;
    if (device_.running() && device_.capability().latency_valid) {
        s.input_latency = device_.capability().input_latency;
        s.output_latency = device_.capability().output_latency;
    }
    s.xruns = device_.xruns();
    s.track_count = static_cast<std::uint32_t>(tracks_.size());
    s.plugin_fails = rt_plugin_fails_.load(std::memory_order_relaxed);
    return s;
}

// ---- RT:audio callback(禁配置/鎖/系統呼叫)----
void AudioEngine::process(const AudioBlock& block) noexcept {
    const std::uint32_t frames =
        block.frames > kMaxBlockFrames ? kMaxBlockFrames : block.frames;
    TrackGraph* g = rt_graph_.load(std::memory_order_acquire);
    if (g == nullptr || frames == 0) return;  // 輸出已由 asio_device 清零 = 靜音

    // 1) 清所有軌的 summing bus(16 軌 @512f = 8K floats,可忽略)
    for (const auto& t : g->nodes) {
        if (t.buf != nullptr) {
            std::memset(t.buf->in[0], 0, frames * sizeof(float));
            std::memset(t.buf->in[1], 0, frames * sizeof(float));
        }
    }

    const float rate = static_cast<float>(rt_sample_rate_.load(std::memory_order_relaxed));
    const float* engine_l = nullptr;
    const float* engine_r = nullptr;
    Vst3ParamEdit edits[kMaxParamEditsPerBlock];

    // 2) 依拓撲序逐軌
    for (const auto idx : g->order) {
        const TrackNode& n = g->nodes[idx];
        if (n.buf == nullptr) continue;
        float* cur_l = n.buf->in[0];
        float* cur_r = n.buf->in[1];
        float* alt_l = n.buf->alt[0];
        float* alt_r = n.buf->alt[1];

        // 來源(kNone = FX/output 軌:bus 已含上游 sum)
        switch (n.source.type) {
            case TrackSource::kSine: {
                // 相位表 sine:2π = 2^32;相位存 TrackRt(每軌獨立,跨 snapshot 存續)
                const float freq = n.source.sine_freq;
                const std::uint64_t step =
                    rate > 0.0F
                        ? static_cast<std::uint64_t>(4294967296.0 * static_cast<double>(freq) / rate)
                        : 0;
                std::uint64_t p = n.buf->sine_phase;
                for (std::uint32_t i = 0; i < frames; ++i) {
                    const double a = static_cast<double>(p >> 8) *
                                     (2.0 * 3.14159265358979323846 / 16777216.0);
                    const float s = 0.25F * static_cast<float>(std::sin(a));  // -12 dBFS 防爆
                    cur_l[i] = s;
                    cur_r[i] = s;
                    p += step;
                }
                n.buf->sine_phase = p;
                break;
            }
            case TrackSource::kAsioIn: {
                const float* il = (n.src_l >= 0 &&
                                   static_cast<std::size_t>(n.src_l) < block.inputs.size())
                                      ? block.inputs[static_cast<std::size_t>(n.src_l)]
                                      : nullptr;
                if (n.source.mono) {
                    // 單聲道來源:單 ch 複製到 L/R(監聽兩耳都有;R 錶同步)
                    for (std::uint32_t i = 0; i < frames; ++i) {
                        const float s = il != nullptr ? il[i] : 0.0F;
                        cur_l[i] = s;
                        cur_r[i] = s;
                    }
                    break;
                }
                const float* ir = (n.src_r >= 0 &&
                                   static_cast<std::size_t>(n.src_r) < block.inputs.size())
                                      ? block.inputs[static_cast<std::size_t>(n.src_r)]
                                      : nullptr;
                for (std::uint32_t i = 0; i < frames; ++i) {
                    cur_l[i] = il != nullptr ? il[i] : 0.0F;
                    cur_r[i] = ir != nullptr ? ir[i] : 0.0F;
                }
                break;
            }
            case TrackSource::kApp:
                // M5b:process loopback FIFO 讀(漂移校正內建;無 capture = 靜音)
                if (n.capture != nullptr)
                    n.capture->read(cur_l, cur_r, frames, static_cast<std::uint32_t>(rate));
                break;
            case TrackSource::kNone:
                break;
        }

        // VST 鏈 ping-pong(每 slot 就地)
        for (std::size_t si = 0; si < n.chain.size(); ++si) {
            const RackSlot& slot = n.chain[si];
            Vst3Plugin* plugin = slot.plugin.get();
            if (!slot.bypass && plugin != nullptr) {
                const std::size_t cnt = slot.ring->pop_all(edits, kMaxParamEditsPerBlock);
                if (plugin->process(cur_l, cur_r, alt_l, alt_r,
                                    static_cast<std::int32_t>(frames), edits, cnt)) {
                    std::swap(cur_l, alt_l);
                    std::swap(cur_r, alt_r);
                } else {
                    // plugin 拒絕本 block:維持原樣(bypass 效果)、計失敗
                    rt_plugin_fails_.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (si < n.chain_strips.size() && n.chain_strips[si] != kNoStrip)
                meter_band(meters_, n.chain_strips[si], cur_l, cur_r, frames);
        }

        // gain/mute:post-fader,每 sample 套 target;值變時 block 內線性斜坡防爆音
        // (收斂時 inc = 0 = 常數乘;不能只斜坡一個 block — 穩態也要真的乘上 gain)
        const float target = n.mute ? 0.0F : n.gain;
        const float g0 = n.buf->gain_state;
        const float inc = (target - g0) / static_cast<float>(frames);
        float gv = g0;
        for (std::uint32_t i = 0; i < frames; ++i) {
            gv += inc;
            cur_l[i] *= gv;
            cur_r[i] *= gv;
        }
        n.buf->gain_state = target;

        if (n.track_strip != kNoStrip) meter_band(meters_, n.track_strip, cur_l, cur_r, frames);

        // 目的地多選 = 加總
        for (const auto d : n.dests) {
            if (d >= g->id_index.size()) continue;
            const auto di = g->id_index[d];
            if (di == kNoStrip || di >= g->nodes.size()) continue;
            const auto& dst = g->nodes[di];
            if (dst.buf == nullptr) continue;
            bus_add(dst.buf->in[0], cur_l, frames);
            bus_add(dst.buf->in[1], cur_r, frames);
        }

        // Sink(ASIO out scratch 已清零,直接 +=)
        if (n.output.type == TrackOutput::kAsioOut) {
            if (n.out_l >= 0 && static_cast<std::size_t>(n.out_l) < block.outputs.size()) {
                bus_add(block.outputs[static_cast<std::size_t>(n.out_l)], cur_l, frames);
                if (n.out_r >= 0 && static_cast<std::size_t>(n.out_r) < block.outputs.size())
                    bus_add(block.outputs[static_cast<std::size_t>(n.out_r)], cur_r, frames);
            }
            // strip 0(engine 輸出/頻譜)= 拓撲序最後一條有 ASIO out 的軌
            engine_l = cur_l;
            engine_r = cur_r;
        }
        // M5c:串流軌寫 render FIFO(滿 = 少寫;pump 端 drift 把 fill 拉回)
        if (n.output.type == TrackOutput::kWasapiRender && n.render != nullptr)
            n.render->write(cur_l, cur_r, frames);
    }

    if (engine_l != nullptr) {
        meter_band(meters_, 0, engine_l, engine_r, frames);
        meters_.append_spectrum(engine_l, engine_r, frames);  // 最終輸出進頻譜 ring
    }
}

}  // namespace rmx
