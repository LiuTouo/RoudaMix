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
#include <intrin.h>  // __rdtsc(RT load 量測;兩次 = ~數十 cycle,無鎖/配置/系統呼叫)
#include <limits>
#include <unordered_set>

#include "app_capture.hpp"
#include "render_sink.hpp"

namespace rmx {

namespace {

inline std::uint64_t plugin_tsc_begin() noexcept {
    _mm_lfence();
    return __rdtsc();
}
inline std::uint64_t plugin_tsc_end() noexcept {
    unsigned aux = 0;
    const auto value = __rdtscp(&aux);
    _mm_lfence();
    return value;
}
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

RoutePlan route_plan_for_tracks(const std::vector<TrackNode>& tracks,
                                std::uint64_t sample_rate) {
    std::vector<RouteTrackSpec> specs;
    specs.reserve(tracks.size());
    for (const auto& track : tracks) {
        RouteTrackSpec spec;
        spec.track_id = track.track_id;
        spec.muted = track.mute;
        spec.is_output = track.kind == TrackKind::kOutput;
        spec.latency_policy = track.latency_policy;
        spec.dests = track.dests;
        spec.uses_input_bus = track.source.type == TrackSource::kNone;
        spec.slots.reserve(track.chain.size());
        for (const auto& slot : track.chain) {
            spec.slots.push_back({
                slot.instance_id,
                slot.bypass,
                slot.plugin != nullptr,
                slot.latency_known,
                slot.latency_samples,
                slot.primary_state,
                slot.monitor_bypass,
                slot.monitor_shadow != nullptr,
                slot.monitor_latency_known,
                slot.monitor_latency_samples,
                slot.monitor_state,
            });
        }
        specs.push_back(std::move(spec));
    }
    return plan_routes(
        specs,
        {sample_rate * 2u, 256u * 1024u * 1024u, 2u, sizeof(float), kMaxBlockFrames});
}

bool monitor_route_changed(const RouteTrackPlan& previous,
                           const RouteTrackPlan& next,
                           const TrackNode& previous_node,
                           const TrackNode& next_node) {
    if (previous.monitor_required != next.monitor_required ||
        previous.monitor_diverged != next.monitor_diverged ||
        previous.slots.size() != next.slots.size() ||
        previous_node.chain.size() != next_node.chain.size())
        return true;
    for (std::size_t i = 0; i < next.slots.size(); ++i) {
        if (previous.slots[i].instance_id != next.slots[i].instance_id ||
            previous.slots[i].monitor != next.slots[i].monitor ||
            previous_node.chain[i].monitor_shadow != next_node.chain[i].monitor_shadow)
            return true;
    }
    return false;
}

bool pre_roll_shadow(Vst3Plugin& shadow,
                     const std::vector<std::pair<std::uint32_t, double>>& params,
                     std::uint32_t sample_rate, std::uint32_t block_frames,
                     std::string& err) {
    if (sample_rate == 0 || block_frames == 0 || block_frames > kMaxBlockFrames) {
        err = "invalid monitor shadow pre-roll format";
        return false;
    }
    std::vector<float> silence_l(block_frames, 0.0F), silence_r(block_frames, 0.0F);
    std::vector<float> out_l(block_frames, 0.0F), out_r(block_frames, 0.0F);
    std::vector<Vst3ParamEdit> edits;
    edits.reserve(params.size());
    for (const auto& [id, value] : params) edits.push_back({id, value});
    const std::uint64_t total = static_cast<std::uint64_t>(sample_rate) / 2u;
    std::uint64_t rendered = 0;
    std::size_t edit_offset = 0;
    while (rendered < total) {
        const auto frames = static_cast<std::uint32_t>(
            (std::min)(static_cast<std::uint64_t>(block_frames), total - rendered));
        const auto count = (std::min)(kMaxParamEditsPerBlock, edits.size() - edit_offset);
        std::fill_n(out_l.data(), frames, 0.0F);
        std::fill_n(out_r.data(), frames, 0.0F);
        if (!shadow.process(silence_l.data(), silence_r.data(), out_l.data(), out_r.data(),
                            static_cast<std::int32_t>(frames),
                            count > 0 ? edits.data() + edit_offset : nullptr, count)) {
            err = "monitor shadow rejected 500 ms pre-roll";
            return false;
        }
        edit_offset += count;
        rendered += frames;
    }
    return true;
}
}  // namespace

AudioEngine::AudioEngine() {
    plugin_timing_overhead_ = (std::numeric_limits<std::uint64_t>::max)();
    for (int i = 0; i < 64; ++i) {
        const auto begin = plugin_tsc_begin();
        const auto elapsed = plugin_tsc_end() - begin;
        plugin_timing_overhead_ = (std::min)(plugin_timing_overhead_, elapsed);
    }
}

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
    // exe 路徑表(一次列舉,basename 給 name、完整路徑給 path = 同名程序辨識)
    const auto proc_paths = list_process_full_paths();
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
                    for (const auto& [p, full] : proc_paths) {
                        if (p != pid) continue;
                        info.path = full;
                        const auto slash = full.find_last_of("\\/");
                        info.name = slash != std::string::npos ? full.substr(slash + 1) : full;
                        break;
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

// M5b/M5c:pump 失敗(main thread 經 callback 轉入 Router 臨界區)
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
    asio_channel_union(tracks_, in_chans, out_chans);

    if (!device_.prepare(rate, in_chans, out_chans, buffer, err)) {
        device_.close();
        return false;
    }
    device_.set_callback(this);

    // 失敗回滾 guard:capture/render/plugin 已啟動後任何一步失敗,全部收乾淨
    // (不留背景 pump、不留 initialized 殘態、不留半開的 device)
    struct StartRollback {
        AudioEngine* e;
        bool armed{true};
        ~StartRollback() {
            if (!armed) return;
            e->stop_captures();
            e->stop_renders();
            for (auto& t : e->tracks_)
                for (auto& s : t.chain)
                    if (s.plugin) {
                        s.plugin->terminate();
                        if (s.monitor_shadow) s.monitor_shadow->terminate();
                    }
            e->device_.close();
        }
    } rollback{this};

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
    // terminate 先跑:前次 start 失敗殘留的 initialized 狀態會讓 initialize 拒絕。
    // placeholder(plugin == null)跳過:不參與 DSP
    for (auto& t : tracks_) {
        for (auto& slot : t.chain) {
            if (!slot.plugin) continue;
            slot.plugin->terminate();
            if (!slot.plugin->initialize(static_cast<double>(rate), device_.block_size())) {
                err = "plugin '" + slot.name + "' init failed: " + slot.plugin->last_error();
                return false;  // rollback guard 收 capture/render/plugin/device
            }
            refresh_latency(slot);
            if (slot.monitor_shadow) {
                slot.monitor_shadow->terminate();
                if (!slot.monitor_shadow->initialize(static_cast<double>(rate),
                                                     device_.block_size())) {
                    err = "monitor shadow '" + slot.name + "' init failed: " +
                          slot.monitor_shadow->last_error();
                    return false;
                }
                if (!pre_roll_shadow(*slot.monitor_shadow, slot.param_values, rate,
                                     device_.block_size(), err))
                    return false;
                slot.monitor_latency_samples = slot.monitor_shadow->latency_samples();
                slot.monitor_latency_known = true;
            }
        }
    }
    rt_sample_rate_.store(rate, std::memory_order_relaxed);
    if (!swap_graph()) {
        err = "PDC plan exceeds latency or memory safety limits";
        return false;
    }

    const std::uint64_t callbacks_before = device_.callbacks();
    if (!device_.start(err)) {
        err = std::string("ASIO start failed after rack ready: ") + err;
        return false;
    }
    // SSL 這類 driver:start() 回 OK 但硬體時脈沒換時 callback 從不來(死流)。
    // 短等驗證沒 callback 就明確失敗,引導用硬體面板改率(600ms:Start 鍵可感知延遲)
    Sleep(600);
    if (device_.callbacks() == callbacks_before) {
        err = "driver did not deliver audio callbacks at " + std::to_string(rate) +
              " Hz; open hardware panel, set rate there, then Start again";
        return false;
    }
    rollback.armed = false;

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
                const TrackGraph* g = rt_graph_.load(std::memory_order_acquire);
                if (g != nullptr) {
                    for (const auto& track : g->nodes) {
                        for (const auto& slot : track.chain) {
                            const auto& mailbox = slot.latency_change_mailbox;
                            if (mailbox == nullptr) continue;
                            if (mailbox->primary.exchange(false, std::memory_order_acq_rel) &&
                                latency_changed_cb_)
                                latency_changed_cb_(slot.instance_id, false);
                            if (mailbox->monitor.exchange(false, std::memory_order_acq_rel) &&
                                latency_changed_cb_)
                                latency_changed_cb_(slot.instance_id, true);
                        }
                    }
                }
                if (shm_ == nullptr) continue;
                std::uint32_t plugin_ids[kPluginLoadEntries]{};
                std::uint32_t plugin_variants[kPluginLoadEntries]{};
                std::size_t plugin_count = 0;
                if (g != nullptr) {
                    for (const auto& t : g->nodes) {
                        for (std::size_t i = 0; i < t.chain.size() && i < t.chain_strips.size();
                             ++i) {
                            const auto& slot = t.chain[i];
                            if (slot.primary_cpu_index < kPluginLoadEntries) {
                                plugin_ids[slot.primary_cpu_index] = slot.instance_id;
                                plugin_variants[slot.primary_cpu_index] = 0;
                                plugin_count = (std::max)(plugin_count,
                                    static_cast<std::size_t>(slot.primary_cpu_index) + 1);
                            }
                            if (slot.shadow_cpu_index < kPluginLoadEntries) {
                                plugin_ids[slot.shadow_cpu_index] = slot.instance_id;
                                plugin_variants[slot.shadow_cpu_index] = 1;
                                plugin_count = (std::max)(plugin_count,
                                    static_cast<std::size_t>(slot.shadow_cpu_index) + 1);
                            }
                        }
                    }
                }
                const auto* strip_table =
                    g != nullptr && !g->strip_table.empty() ? g->strip_table.data() : nullptr;
                const auto strip_count = g != nullptr ? g->strip_table.size() : 0;
                meters_.publish(*shm_, device_.xruns(), strip_table, strip_count,
                                plugin_ids, plugin_variants, plugin_count,
                                device_.running());
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
        for (auto& slot : t.chain)
            if (slot.plugin) {
                slot.plugin->terminate();
                if (slot.monitor_shadow) slot.monitor_shadow->terminate();
            }
    stop_captures();
    stop_renders();
    clear_expired_retired(/*force=*/true);
}

// ---- track graph swap(同舊 rack 的 grace 模式)----

bool AudioEngine::swap_graph() noexcept {
    // 深拷貝結構殼:chain 內 plugin/ring shared_ptr、RT buffer shared_ptr 共用
    auto* fresh = new TrackGraph{};
    fresh->nodes = tracks_;
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
    // telemetry strip 預算:兩輪、可預測(track 全拿完才輪 plugin;純函式與
    // status_json 的 metered 共用 — 見 track_graph.cpp plan_telemetry_strips)
    auto strips = plan_telemetry_strips(fresh->nodes, kTelemetryStrips);
    fresh->engine_strip = strips.engine_strip;
    fresh->strip_table = std::move(strips.table);
    for (std::size_t ti = 0; ti < fresh->nodes.size(); ++ti) {
        auto& t = fresh->nodes[ti];
        t.src_l = t.src_r = t.out_l = t.out_r = -1;
        if (t.source.type == TrackSource::kAsioIn) {
            t.src_l = resolve(imap, t.source.asio_in_ch);
            t.src_r = resolve(imap, t.source.asio_in_ch + 1);
        }
        if (t.output.type == TrackOutput::kAsioOut) {
            t.out_l = resolve(omap, t.output.asio_out_ch);
            t.out_r = resolve(omap, t.output.asio_out_ch + 1);
        }
        t.track_strip = strips.tracks[ti].track_strip;
        t.chain_strips = std::move(strips.tracks[ti].chain_strips);
        for (auto& slot : t.chain) {
            slot.primary_cpu_index = 0xFFFFFFFFu;
            slot.shadow_cpu_index = 0xFFFFFFFFu;
        }
    }
    const auto running_rate = rt_sample_rate_.load(std::memory_order_relaxed);
    const std::uint64_t plan_rate =
        running_rate > 0 ? running_rate : (last_sample_rate_ > 0 ? last_sample_rate_ : 48000u);
    fresh->route_plan = route_plan_for_tracks(fresh->nodes, plan_rate);
    if (!fresh->route_plan.ok()) {
        // Candidate plan 不合法即不 commit；caller 負責回滾剛才的 control
        // mutation。既有 RT graph 與 last-known-good plan 完全不動。
        delete fresh;
        return false;
    }

    std::uint32_t next_cpu = 0;
    for (std::size_t ti = 0; ti < fresh->nodes.size(); ++ti) {
        auto& track = fresh->nodes[ti];
        const auto& track_plan = fresh->route_plan.tracks[ti];
        for (std::size_t si = 0; si < track.chain.size(); ++si) {
            auto& slot = track.chain[si];
            const auto& slot_plan = track_plan.slots[si];
            if (slot_plan.primary.action == RouteSlotAction::kProcess &&
                next_cpu < kPluginLoadEntries)
                slot.primary_cpu_index = next_cpu++;
            if (slot_plan.monitor.action == RouteSlotAction::kProcess &&
                next_cpu < kPluginLoadEntries)
                slot.shadow_cpu_index = next_cpu++;
        }
    }

    const TrackGraph* active_graph = rt_graph_.load(std::memory_order_acquire);
    for (std::size_t ti = 0; ti < fresh->nodes.size(); ++ti) {
        auto& track = fresh->nodes[ti];
        const auto& track_plan = fresh->route_plan.tracks[ti];
        if (!track_plan.monitor_diverged || track.buf == nullptr) continue;
        bool monitor_path_changed = active_graph == nullptr;
        if (active_graph != nullptr && track.track_id < active_graph->id_index.size()) {
            const auto old_index = active_graph->id_index[track.track_id];
            if (old_index == kNoStrip || old_index >= active_graph->route_plan.tracks.size()) {
                monitor_path_changed = true;
            } else {
                monitor_path_changed = monitor_route_changed(
                    active_graph->route_plan.tracks[old_index], track_plan,
                    active_graph->nodes[old_index], track);
            }
        }
        if (monitor_path_changed && running_rate > 0)
            track.buf->monitor_crossfade.request(static_cast<std::uint32_t>(plan_rate / 50u));
    }
    std::unordered_set<std::uint64_t> active_dry_keys;
    struct PendingDryDelay {
        std::uint64_t key{};
        std::shared_ptr<PdcDelayLine> line;
        std::uint64_t target{};
        bool reused{};
    };
    std::vector<PendingDryDelay> pending_dry_delays;
    bool dry_resources_ready = true;
    auto dry_delay = [&](std::uint32_t instance_id, std::uint32_t variant,
                         std::uint64_t samples) -> std::shared_ptr<PdcDelayLine> {
        if (samples == 0) return nullptr;
        const std::uint64_t key = (static_cast<std::uint64_t>(instance_id) << 1u) | variant;
        active_dry_keys.insert(key);
        const auto found = dry_delay_states_.find(key);
        if (found != dry_delay_states_.end() &&
            found->second->max_delay_samples() >= samples) {
            pending_dry_delays.push_back({key, found->second, samples, true});
            return found->second;
        }
        auto line = std::make_shared<PdcDelayLine>();
        if (!line->prepare(samples, kMaxBlockFrames) || !line->set_delay(samples, 0)) {
            dry_resources_ready = false;
            return nullptr;
        }
        pending_dry_delays.push_back({key, line, samples, false});
        return line;
    };
    for (std::size_t ti = 0; ti < fresh->nodes.size(); ++ti) {
        auto& track = fresh->nodes[ti];
        const auto& track_plan = fresh->route_plan.tracks[ti];
        for (std::size_t si = 0; si < track.chain.size(); ++si) {
            auto& slot = track.chain[si];
            const auto& slot_plan = track_plan.slots[si];
            slot.primary_dry_delay = dry_delay(
                slot.instance_id, 0, slot_plan.primary.dry_delay_samples);
            slot.shadow_dry_delay = dry_delay(
                slot.instance_id, 1, slot_plan.monitor.dry_delay_samples);
        }
    }
    if (!dry_resources_ready) {
        delete fresh;
        return false;
    }
    for (auto& t : fresh->nodes) t.dest_pdc.assign(t.dests.size(), nullptr);
    struct PendingPdcLine {
        std::uint64_t key{};
        std::uint64_t target{};
        std::shared_ptr<PdcDelayLine> line;
        bool reused{};
    };
    std::vector<PendingPdcLine> pending_lines;
    bool resources_ready = true;
    for (const auto& edge : fresh->route_plan.latency.edge_delays) {
        if (edge.from_track_id >= fresh->id_index.size()) continue;
        const auto from_index = fresh->id_index[edge.from_track_id];
        if (from_index == kNoStrip || from_index >= fresh->nodes.size()) continue;
        auto& from = fresh->nodes[from_index];
        const auto dest = std::find(from.dests.begin(), from.dests.end(), edge.to_track_id);
        if (dest == from.dests.end()) continue;
        const auto route = static_cast<std::size_t>(dest - from.dests.begin());
        const std::uint64_t key = (static_cast<std::uint64_t>(edge.from_track_id) << 32u) |
                                  edge.to_track_id;
        auto found = pdc_delay_states_.find(key);
        std::shared_ptr<PdcDelayLine> delay;
        bool reused = false;
        if (found != pdc_delay_states_.end() &&
            found->second->max_delay_samples() >= plan_rate * 2u) {
            delay = found->second;
            reused = true;
        } else {
            delay = std::make_shared<PdcDelayLine>();
            if (!delay->prepare(plan_rate * 2u, kMaxBlockFrames) ||
                !delay->set_delay(edge.delay_samples, 0u)) {
                resources_ready = false;
                break;
            }
        }
        from.dest_pdc[route] = delay;
        pending_lines.push_back({key, edge.delay_samples, std::move(delay), reused});
    }
    if (!resources_ready) {
        delete fresh;
        return false;
    }
    for (const auto& pending : pending_dry_delays)
        dry_delay_states_[pending.key] = pending.line;
    for (auto it = dry_delay_states_.begin(); it != dry_delay_states_.end();) {
        if (!active_dry_keys.contains(it->first))
            it = dry_delay_states_.erase(it);
        else
            ++it;
    }
    std::unordered_set<std::uint64_t> active_pdc_keys;
    for (auto& pending : pending_lines) {
        active_pdc_keys.insert(pending.key);
        pdc_delay_states_[pending.key] = pending.line;
    }
    for (auto it = pdc_delay_states_.begin(); it != pdc_delay_states_.end();) {
        if (!active_pdc_keys.contains(it->first))
            it = pdc_delay_states_.erase(it);
        else
            ++it;
    }
    last_route_plan_ = fresh->route_plan;
    latency_generation_.fetch_add(1, std::memory_order_relaxed);
    TrackGraph* old = rt_graph_.exchange(fresh, std::memory_order_acq_rel);
    // Commit 後才發布共享 delay 的 target，candidate 失敗時現行 graph 不會
    // 提前看到新 latency。Audio thread 仍只在下一個 block boundary 套用。
    const auto transition_samples =
        running_rate > 0 ? static_cast<std::uint32_t>(plan_rate / 50u) : 0u;
    for (auto& pending : pending_lines)
        if (pending.reused)
            (void)pending.line->set_delay(pending.target, transition_samples);
    for (auto& pending : pending_dry_delays)
        if (pending.reused)
            (void)pending.line->set_delay(pending.target, transition_samples);
    if (old != nullptr) retired_.push_back({old, GetTickCount64()});
    clear_expired_retired(false);
    return true;
}

bool AudioEngine::swap_safety_graph() noexcept {
    const TrackGraph* active = rt_graph_.load(std::memory_order_acquire);
    if (active == nullptr) return true;
    auto* safe = new TrackGraph(*active);
    for (auto& track : safe->nodes) {
        for (auto& slot : track.chain) {
            slot.bypass = true;
            slot.primary_cpu_index = 0xFFFFFFFFu;
            slot.shadow_cpu_index = 0xFFFFFFFFu;
        }
    }
    safe->route_plan = plan_route_suspension(active->route_plan);
    TrackGraph* old = rt_graph_.exchange(safe, std::memory_order_acq_rel);
    if (old != nullptr) retired_.push_back({old, GetTickCount64()});
    clear_expired_retired(false);
    return true;
}

void AudioEngine::refresh_latency(RackSlot& slot) noexcept {
    if (slot.plugin == nullptr) {
        slot.latency_samples = 0;
        slot.latency_known = false;
        return;
    }
    slot.latency_samples = slot.plugin->latency_samples();
    slot.latency_known = true;
}

void AudioEngine::bind_latency_callback(RackSlot& slot) {
    if (slot.plugin == nullptr) return;
    const auto mailbox = slot.latency_change_mailbox;
    slot.plugin->set_latency_changed_callback([mailbox] {
        // Plugin 可在 audio callback 內同步通知；RT 只置位，不碰系統 API。
        if (mailbox != nullptr) mailbox->primary.store(true, std::memory_order_release);
    });
}

void AudioEngine::handle_latency_changed(std::uint32_t instance_id, bool monitor_shadow) {
    RackSlot* slot = find_slot_mut(instance_id);
    if (slot == nullptr) return;
    if (monitor_shadow) {
        if (slot->monitor_shadow == nullptr) return;
        const auto previous_latency = slot->monitor_latency_samples;
        const auto previous_known = slot->monitor_latency_known;
        const auto previous_state = slot->monitor_state;
        slot->monitor_latency_samples = slot->monitor_shadow->latency_samples();
        slot->monitor_latency_known = true;
        slot->monitor_state = RackSlot::RuntimeState::kActive;
        std::string prepare_error;
        if (!prepare_monitor_variants(prepare_error))
            slot->monitor_state = RackSlot::RuntimeState::kDegraded;
        if (!swap_graph()) {
            // Shadow-only 超限只讓 low-latency variant 走 dry；primary Stream 保留。
            slot->monitor_state = RackSlot::RuntimeState::kDegraded;
            if (!swap_graph()) {
                slot->monitor_latency_samples = previous_latency;
                slot->monitor_latency_known = previous_known;
                slot->monitor_state = previous_state;
                (void)swap_graph();
            }
        }
        return;
    }
    if (slot->plugin == nullptr) return;
    const auto previous_latency = slot->latency_samples;
    const auto previous_known = slot->latency_known;
    const auto previous_state = slot->primary_state;
    slot->latency_samples = slot->plugin->latency_samples();
    slot->latency_known = true;
    slot->primary_state = RackSlot::RuntimeState::kActive;
    std::string prepare_error;
    if (!prepare_monitor_variants(prepare_error))
        slot->primary_state = RackSlot::RuntimeState::kSuspended;
    if (!swap_graph()) {
        // Runtime primary 超限不是使用者 transaction：保留觀察值但 suspend
        // 該 instance，讓所有輸出走 dry，其他 plugin/graph 繼續工作。
        slot->primary_state = RackSlot::RuntimeState::kSuspended;
        if (!swap_graph()) {
            slot->latency_samples = previous_latency;
            slot->latency_known = previous_known;
            slot->primary_state = previous_state;
            (void)swap_graph();
        }
    }
}

bool AudioEngine::ensure_monitor_shadows(std::string& err) {
    const auto rate = rt_sample_rate_.load(std::memory_order_relaxed);
    const std::uint64_t plan_rate =
        rate > 0 ? rate : (last_sample_rate_ > 0 ? last_sample_rate_ : 48000u);
    const auto plan = route_plan_for_tracks(tracks_, plan_rate);
    if (!plan.ok()) {
        err = "PDC plan exceeds latency or memory safety limits";
        return false;
    }
    struct PendingShadow {
        RackSlot* slot{};
        std::shared_ptr<Vst3Plugin> plugin;
        std::uint64_t latency{};
    };
    std::vector<PendingShadow> pending;
    std::vector<RackSlot*> releases;
    for (std::size_t ti = 0; ti < tracks_.size(); ++ti) {
        auto& track = tracks_[ti];
        const auto& track_plan = plan.tracks[ti];
        for (std::size_t si = 0; si < track.chain.size(); ++si) {
            auto& slot = track.chain[si];
            const auto disposition = track_plan.slots[si].shadow;
            if (disposition == ShadowDisposition::kRelease) {
                releases.push_back(&slot);
                continue;
            }
            if (disposition != ShadowDisposition::kCreate || slot.plugin == nullptr)
                continue;
            auto shadow = std::make_shared<Vst3Plugin>(slot.module_path, slot.class_id);
            if (!shadow->loaded()) {
                err = "monitor shadow load failed for '" + slot.name + "': " +
                      shadow->last_error();
                return false;
            }
            Vst3RuntimeState state;
            std::string state_err;
            if (!slot.plugin->capture_runtime_state(state, state_err)) {
                err = "monitor shadow state capture failed for '" + slot.name + "': " + state_err;
                return false;
            }
            if (!shadow->restore_runtime_state(state, state_err)) {
                err = "monitor shadow state restore failed for '" + slot.name + "': " + state_err;
                return false;
            }
            if (device_.running() &&
                !shadow->initialize(
                    static_cast<double>(rt_sample_rate_.load(std::memory_order_relaxed)),
                    device_.block_size())) {
                err = "monitor shadow init failed for '" + slot.name + "': " +
                      shadow->last_error();
                return false;
            }
            for (const auto& [id, value] : slot.param_values) {
                shadow->set_param_normalized(id, value);
            }
            if (device_.running() &&
                !pre_roll_shadow(*shadow, slot.param_values,
                                 rt_sample_rate_.load(std::memory_order_relaxed),
                                 device_.block_size(), state_err)) {
                err = "monitor shadow pre-roll failed for '" + slot.name + "': " + state_err;
                return false;
            }
            const auto mailbox = slot.latency_change_mailbox;
            shadow->set_latency_changed_callback([mailbox] {
                if (mailbox != nullptr)
                    mailbox->monitor.store(true, std::memory_order_release);
            });
            const auto shadow_latency = shadow->latency_samples();
            pending.push_back({&slot, std::move(shadow), shadow_latency});
        }
    }
    // 所有 shadow 都成功後才一次 commit；中途任何失敗不修改 master graph。
    for (auto* slot : releases) {
        slot->monitor_shadow.reset();
        slot->monitor_latency_samples = 0;
        slot->monitor_latency_known = false;
        slot->monitor_state = RackSlot::RuntimeState::kActive;
    }
    for (auto& item : pending) {
        item.slot->monitor_shadow = std::move(item.plugin);
        item.slot->monitor_latency_samples = item.latency;
        item.slot->monitor_latency_known = true;
        item.slot->monitor_state = RackSlot::RuntimeState::kActive;
        for (const auto& [id, value] : item.slot->param_values)
            item.slot->monitor_ring->push({id, value});
    }
    return true;
}

bool AudioEngine::prepare_monitor_variants(std::string& err) {
    const auto rate = rt_sample_rate_.load(std::memory_order_relaxed);
    const std::uint64_t plan_rate =
        rate > 0 ? rate : (last_sample_rate_ > 0 ? last_sample_rate_ : 48000u);
    const auto plan = route_plan_for_tracks(tracks_, plan_rate);
    const bool missing = plan.ok() && std::any_of(
        plan.tracks.begin(), plan.tracks.end(), [](const RouteTrackPlan& track) {
            return std::any_of(track.slots.begin(), track.slots.end(),
                               [](const RouteSlotPlan& slot) {
                                   return slot.shadow == ShadowDisposition::kCreate;
                               });
        });
    if (device_.running() && missing) {
        // 只在 RT snapshot 暫時 bypass；master/user intent 不變，失敗時不用
        // 回填整份 bypass vector，也不會污染 Session dirty 狀態。
        if (!swap_safety_graph()) {
            err = "cannot prepare monitor shadow safety graph";
            return false;
        }
        Sleep(60);
    }
    if (!ensure_monitor_shadows(err)) {
        if (device_.running()) (void)swap_graph();
        return false;
    }
    return true;
}

void AudioEngine::retire_graph() noexcept {
    TrackGraph* old = rt_graph_.exchange(nullptr, std::memory_order_acq_rel);
    if (old != nullptr) retired_.push_back({old, GetTickCount64()});
}

bool AudioEngine::rebuild_asio_channels(std::string& err) {
    if (!device_.running()) return true;
    // 與 start() 同款:從所有軌收 ASIO channel 聯集
    std::vector<std::uint32_t> in_chans, out_chans;
    asio_channel_union(tracks_, in_chans, out_chans);
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
    // 交易式:記下現行可工作設定,新 map 啟動失敗 = 自動恢復舊設定(不做 stop 後
    // 殘局;真恢復不了才停在 stopped,dispatch 會廣播權威 status 給 UI)
    const std::string clsid = device_.clsid();
    const std::uint32_t rate = rt_sample_rate_.load(std::memory_order_relaxed);
    const std::uint32_t block = device_.block_size();
    const std::vector<std::uint32_t> prev_in = imap;
    const std::vector<std::uint32_t> prev_out = omap;
    retire_graph();  // RT 停後退 graph;新 map 位置由呼叫端 swap_graph 重解析
    if (!device_.prepare(rate, in_chans, out_chans, block, err) ||
        !device_.start(err)) {
        // 回滾:舊 channel map 重開;成功 = 串流續跑(指令仍回報失敗,設定沒套上)
        device_.close();
        std::string rb_err;
        if (device_.probe(clsid, rb_err) &&
            device_.prepare(rate, prev_in, prev_out, block, rb_err) &&
            device_.start(rb_err)) {
            err = "rebuild ASIO buffers failed (" + err + "); previous channels restored";
        } else {
            device_.close();
            err = "rebuild ASIO buffers failed (" + err +
                  ") and rollback also failed, engine stopped (" + rb_err + ")";
        }
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
    // 系統輸出(monitor/stream)不可刪:每個 session 必須恰好各一條(engine 端
    // 權威驗證,不靠 UI);UI 也隱藏移除鈕
    if (it->system_role != SystemRole::kNone) {
        err = std::string("system ") + system_role_str(it->system_role) +
              " output cannot be removed (rename or re-route it instead)";
        return false;
    }
    // 該軌的 plugin editor 先收(editor 與 dispatch 同在 main thread,無並發)
    for (auto& slot : it->chain) {
        if (slot.plugin && slot.plugin->editor_open()) slot.plugin->close_editor();
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

bool AudioEngine::track_set_latency_policy(std::uint32_t track_id,
                                           OutputLatencyPolicy policy,
                                           std::string& err) {
    TrackNode* t = find_track_mut(track_id);
    if (t == nullptr) {
        err = "unknown trackId " + std::to_string(track_id);
        return false;
    }
    if (t->kind != TrackKind::kOutput) {
        err = "latency policy applies only to output tracks";
        return false;
    }
    if (t->latency_policy == policy) return true;
    const auto previous = t->latency_policy;
    t->latency_policy = policy;
    if (!prepare_monitor_variants(err)) {
        t->latency_policy = previous;
        std::string cleanup_error;
        (void)ensure_monitor_shadows(cleanup_error);
        (void)swap_graph();
        return false;
    }
    if (!swap_graph()) {
        t->latency_policy = previous;
        std::string cleanup_error;
        (void)ensure_monitor_shadows(cleanup_error);
        (void)swap_graph();
        err = "PDC plan exceeds latency or memory safety limits";
        return false;
    }
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
    if (!swap_graph()) {
        t->source = prev;
        err = "PDC plan exceeds latency or memory safety limits";
        code = "bad_command";
        return false;
    }
    return true;
}

// M5b:capture 生命週期(控制面)。P1-C:pid==0(session 載入只帶名)= needsRebind
// —— engine 不依 exe 名猜 PID(同名多程序會綁錯);UI 用程序選擇器讓使用者選。
bool AudioEngine::ensure_capture(TrackNode& t, std::uint32_t dst_rate, std::string& err) {
    std::uint32_t pid = t.source.pid;
    if (pid == 0) {
        t.track_error = "app source not bound: pick a process for this track" +
                        (t.source.app_name.empty() ? ""
                                                   : " (saved source: " + t.source.app_name + ")");
        err = t.track_error;
        return false;
    }
    if (!process_exists(pid)) {
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
    if (!prepare_monitor_variants(err)) {
        t->dests = old;
        std::string cleanup_error;
        (void)ensure_monitor_shadows(cleanup_error);
        (void)swap_graph();
        code = "plugin_state_failed";
        return false;
    }
    if (!swap_graph()) {
        t->dests = old;
        std::string cleanup_error;
        (void)ensure_monitor_shadows(cleanup_error);
        err = "PDC plan exceeds latency or memory safety limits";
        code = "bad_command";
        return false;
    }
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

bool AudioEngine::primary_route_processes(std::uint32_t instance_id) const noexcept {
    for (const auto& track : last_route_plan_.tracks)
        for (const auto& slot : track.slots)
            if (slot.instance_id == instance_id)
                return slot.primary.action == RouteSlotAction::kProcess;
    return false;
}

std::vector<AudioEngine::PluginTabInfo> AudioEngine::plugin_tabs() const {
    std::vector<PluginTabInfo> tabs;
    for (const auto& t : tracks_) {
        for (const auto& s : t.chain) {
            tabs.push_back({s.instance_id, s.name, t.name,
                            s.plugin != nullptr && s.plugin->editor_capable(), s.bypass});
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
    refresh_latency(slot);
    bind_latency_callback(slot);
    for (const auto& p : slot.plugin->params())
        slot.param_values.push_back({p.id, p.default_normalized});
    instance_id = slot.instance_id;
    t->chain.push_back(std::move(slot));
    if (!prepare_monitor_variants(err)) {
        t->chain.pop_back();
        (void)swap_graph();
        return false;
    }
    if (!swap_graph()) {
        t->chain.pop_back();
        std::string cleanup_error;
        (void)ensure_monitor_shadows(cleanup_error);
        err = "PDC plan exceeds latency or memory safety limits";
        return false;
    }
    return true;
}

bool AudioEngine::add_placeholder_plugin(std::uint32_t track_id, const std::string& module_path,
                                         const std::string& class_id, const std::string& name,
                                         bool bypassed, RackSlot::Availability why,
                                         const std::string& load_error,
                                         const std::vector<std::pair<std::uint32_t, double>>& params,
                                         std::uint32_t& instance_id, std::string& err) {
    TrackNode* t = find_track_mut(track_id);
    if (t == nullptr) {
        err = "unknown trackId " + std::to_string(track_id);
        return false;
    }
    RackSlot slot;
    slot.instance_id = next_instance_id_++;
    slot.module_path = module_path;
    slot.class_id = class_id;
    slot.name = name.empty() ? std::filesystem::path(module_path).filename().string() : name;
    slot.bypass = bypassed;  // placeholder 不參與 DSP;bypass 值照存(load 後還原)
    slot.availability = why;
    slot.load_error = load_error;
    slot.param_values = params;  // session 帶回的權威值(載回時重放)
    instance_id = slot.instance_id;
    t->chain.push_back(std::move(slot));
    swap_graph();
    return true;
}

bool AudioEngine::load_placeholder(std::uint32_t instance_id, const std::string& module_path,
                                   const std::string& class_id, std::string& err) {
    RackSlot* s = find_slot_mut(instance_id);
    if (s == nullptr) {
        err = "unknown instanceId " + std::to_string(instance_id);
        return false;
    }
    if (!s->is_placeholder()) {
        err = "instance is not a placeholder";
        return false;
    }
    const RackSlot previous = *s;
    // 原位置/instanceId/params/bypass 全保留:只把 plugin 補上、狀態轉 ok
    auto plugin = std::make_shared<Vst3Plugin>(module_path, class_id);
    if (!plugin->loaded()) {
        err = plugin->last_error();
        return false;
    }
    if (device_.running() &&
        !plugin->initialize(static_cast<double>(rt_sample_rate_.load(std::memory_order_relaxed)),
                            device_.block_size())) {
        err = plugin->last_error();
        return false;
    }
    s->plugin = std::move(plugin);
    s->module_path = module_path;
    s->class_id = class_id;
    s->name = s->plugin->name();
    s->availability = RackSlot::Availability::kOk;
    s->load_error.clear();
    refresh_latency(*s);
    bind_latency_callback(*s);
    if (!prepare_monitor_variants(err)) {
        *s = previous;
        (void)swap_graph();
        return false;
    }
    // session 帶回來的 host 權威值推 RT + controller(同 load_preset 三路同步)
    for (const auto& [id, v] : s->param_values) {
        s->ring->push({id, v});
        s->plugin->set_param_normalized(id, v);
    }
    if (!swap_graph()) {
        *s = previous;
        std::string cleanup_error;
        (void)ensure_monitor_shadows(cleanup_error);
        err = "PDC plan exceeds latency or memory safety limits";
        return false;
    }
    return true;
}

bool AudioEngine::remove_plugin(std::uint32_t instance_id, std::string& err,
                                PluginMutationFailure* failure) {
    if (failure != nullptr) *failure = PluginMutationFailure::kNone;
    for (auto& t : tracks_) {
        for (auto it = t.chain.begin(); it != t.chain.end(); ++it) {
            if (it->instance_id == instance_id) {
                // editor 視窗先收(同步 DestroyWindow;editor 與 dispatch 同在 main
                // thread,無並發——performEdit 回呼不會同時跑)
                if (it->plugin && it->plugin->editor_open()) it->plugin->close_editor();
                t.chain.erase(it);
                swap_graph();
                return true;
            }
        }
    }
    if (failure != nullptr) *failure = PluginMutationFailure::kNotFound;
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

bool AudioEngine::set_bypass(std::uint32_t instance_id, bool bypass, std::string& err,
                             PluginMutationFailure* failure) {
    if (failure != nullptr) *failure = PluginMutationFailure::kNone;
    RackSlot* s = find_slot_mut(instance_id);
    if (s == nullptr) {
        if (failure != nullptr) *failure = PluginMutationFailure::kNotFound;
        err = "unknown instanceId " + std::to_string(instance_id);
        return false;
    }
    if (s->bypass == bypass) return true;
    const bool previous = s->bypass;
    s->bypass = bypass;
    if (!prepare_monitor_variants(err)) {
        if (failure != nullptr) *failure = PluginMutationFailure::kStateFailed;
        s->bypass = previous;
        std::string cleanup_error;
        (void)ensure_monitor_shadows(cleanup_error);
        (void)swap_graph();
        return false;
    }
    if (!swap_graph()) {
        if (failure != nullptr) *failure = PluginMutationFailure::kBadCommand;
        s->bypass = previous;
        std::string cleanup_error;
        (void)ensure_monitor_shadows(cleanup_error);
        (void)swap_graph();
        err = "PDC plan exceeds latency or memory safety limits";
        return false;
    }
    return true;
}

bool AudioEngine::set_monitor_bypass(std::uint32_t instance_id, bool bypass,
                                     std::string& err,
                                     PluginMutationFailure* failure) {
    if (failure != nullptr) *failure = PluginMutationFailure::kNone;
    RackSlot* s = find_slot_mut(instance_id);
    if (s == nullptr) {
        if (failure != nullptr) *failure = PluginMutationFailure::kNotFound;
        err = "unknown instanceId " + std::to_string(instance_id);
        return false;
    }
    if (s->monitor_bypass == bypass) return true;
    const bool previous = s->monitor_bypass;
    s->monitor_bypass = bypass;
    if (!prepare_monitor_variants(err)) {
        if (failure != nullptr) *failure = PluginMutationFailure::kStateFailed;
        s->monitor_bypass = previous;
        std::string cleanup_error;
        (void)ensure_monitor_shadows(cleanup_error);
        (void)swap_graph();
        return false;
    }
    if (!swap_graph()) {
        if (failure != nullptr) *failure = PluginMutationFailure::kBadCommand;
        s->monitor_bypass = previous;
        std::string cleanup_error;
        (void)ensure_monitor_shadows(cleanup_error);
        (void)swap_graph();  // running 時先前的全 bypass 過渡 graph 必須還原
        err = "PDC plan exceeds latency or memory safety limits";
        return false;
    }
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
    if (s->monitor_shadow) {
        s->monitor_ring->push({param_id, value});
        s->monitor_shadow->set_param_normalized(param_id, value);
    }
    return true;
}

bool AudioEngine::save_preset(std::uint32_t instance_id, const std::filesystem::path& file,
                              std::string& err) {
    const RackSlot* s = find_slot(instance_id);
    if (s == nullptr) {
        err = "unknown instanceId " + std::to_string(instance_id);
        return false;
    }
    if (s->plugin == nullptr) {
        err = "plugin not loaded (placeholder)";
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
                              std::string& err, PresetLoadFailure* failure) {
    if (failure != nullptr) *failure = PresetLoadFailure::kPresetIo;
    RackSlot* s = find_slot_mut(instance_id);
    if (s == nullptr) {
        if (failure != nullptr) *failure = PresetLoadFailure::kNotFound;
        err = "unknown instanceId " + std::to_string(instance_id);
        return false;
    }
    if (s->plugin == nullptr) {
        err = "plugin not loaded (placeholder)";
        return false;
    }
    // setState 與 RT process 不得併發:同 save_preset,先掛 bypass
    const bool orig_bypass = s->bypass;
    const auto original_params = s->param_values;
    const auto original_primary_latency = s->latency_samples;
    const auto original_monitor_latency = s->monitor_latency_samples;
    const auto original_primary_known = s->latency_known;
    const auto original_monitor_known = s->monitor_latency_known;
    const auto original_primary_state = s->primary_state;
    const auto original_monitor_state = s->monitor_state;
    s->bypass = true;
    (void)swap_graph();
    Sleep(60);
    Vst3RuntimeState original_primary;
    Vst3RuntimeState original_shadow;
    if (!s->plugin->capture_runtime_state(original_primary, err)) {
        s->bypass = orig_bypass;
        (void)swap_graph();
        return false;
    }
    if (s->monitor_shadow) {
        std::string capture_error;
        if (!s->monitor_shadow->capture_runtime_state(original_shadow, capture_error)) {
            s->bypass = orig_bypass;
            (void)swap_graph();
            err = "monitor shadow state capture failed: " + capture_error;
            return false;
        }
    }
    bool host_values_from_file = false;
    bool state_rejected = false;
    bool ok = s->plugin->load_preset(file, s->param_values, err,
                                     host_values_from_file, state_rejected);
    if (state_rejected && failure != nullptr)
        *failure = PresetLoadFailure::kPluginStateFailed;
    // 檔案無 RmxP(外部 host 存的 preset)時，以 primary controller 回報重建
    // host 權威參數；shadow 永遠跟隨這份權威值。
    if (ok && !host_values_from_file) {
        for (auto& [id, value] : s->param_values) {
            const double fresh = s->plugin->param_value(id);
            if (std::isfinite(fresh)) value = fresh;
        }
    }
    if (ok && s->monitor_shadow) {
        auto shadow_params = s->param_values;
        bool shadow_values_from_file = false;
        bool shadow_state_rejected = false;
        std::string shadow_err;
        if (!s->monitor_shadow->load_preset(file, shadow_params, shadow_err,
                                            shadow_values_from_file,
                                            shadow_state_rejected)) {
            if (shadow_state_rejected && failure != nullptr)
                *failure = PresetLoadFailure::kPluginStateFailed;
            err = "monitor shadow preset sync failed: " + shadow_err;
            ok = false;
        } else {
            s->monitor_latency_samples = s->monitor_shadow->latency_samples();
            s->monitor_latency_known = true;
            s->monitor_state = RackSlot::RuntimeState::kActive;
            for (const auto& [id, value] : s->param_values)
                s->monitor_shadow->set_param_normalized(id, value);
            if (device_.running() &&
                !pre_roll_shadow(*s->monitor_shadow, s->param_values,
                                 rt_sample_rate_.load(std::memory_order_relaxed),
                                 device_.block_size(), shadow_err)) {
                err = "monitor shadow preset pre-roll failed: " + shadow_err;
                ok = false;
            }
        }
    }
    s->bypass = orig_bypass;
    if (ok) {
        refresh_latency(*s);
        s->primary_state = RackSlot::RuntimeState::kActive;
        if (!prepare_monitor_variants(err)) ok = false;
    }
    if (ok) {
        ok = swap_graph();
        if (!ok) err = "PDC plan exceeds latency or memory safety limits";
    }
    if (!ok) {
        std::string rollback_error;
        const bool primary_restored =
            s->plugin->restore_runtime_state(original_primary, rollback_error);
        bool shadow_restored = true;
        if (s->monitor_shadow)
            shadow_restored =
                s->monitor_shadow->restore_runtime_state(original_shadow, rollback_error);
        s->param_values = original_params;
        s->latency_samples = original_primary_latency;
        s->monitor_latency_samples = original_monitor_latency;
        s->latency_known = original_primary_known;
        s->monitor_latency_known = original_monitor_known;
        s->primary_state = original_primary_state;
        s->monitor_state = original_monitor_state;
        for (const auto& [id, value] : s->param_values) {
            s->plugin->set_param_normalized(id, value);
            s->ring->push({id, value});
            if (s->monitor_shadow) {
                s->monitor_shadow->set_param_normalized(id, value);
                s->monitor_ring->push({id, value});
            }
        }
        (void)swap_graph();
        if (!primary_restored || !shadow_restored)
            err += "; rollback failed: " + rollback_error;
        return false;
    }
    // commit 後三路同步：host 權威表、RT ring、controller/editor。
    for (const auto& [id, value] : s->param_values) {
        s->ring->push({id, value});
        s->plugin->set_param_normalized(id, value);
        if (s->monitor_shadow) {
            s->monitor_ring->push({id, value});
            s->monitor_shadow->set_param_normalized(id, value);
        }
    }
    if (failure != nullptr) *failure = PresetLoadFailure::kNone;
    return true;
}

void AudioEngine::sync_controller_params(std::uint32_t instance_id) {
    // session 載入:set_param 只餵 RT ring,controller(editor GUI)不知道 ——
    // 開 GUI 會看到舊值/預設值。這裡把 host 權威值推給 controller 同步顯示
    RackSlot* s = find_slot_mut(instance_id);
    if (s == nullptr || s->plugin == nullptr) return;
    for (const auto& [id, v] : s->param_values) s->plugin->set_param_normalized(id, v);
}

bool AudioEngine::ensure_system_outputs() {
    if (!rmx::ensure_system_outputs(tracks_, next_track_id_)) return false;
    swap_graph();
    return true;
}

void AudioEngine::set_track_system_role(std::uint32_t track_id, SystemRole role) {
    TrackNode* t = find_track_mut(track_id);
    if (t != nullptr) {
        t->system_role = role;
        t->latency_policy = role == SystemRole::kMonitor ? OutputLatencyPolicy::kLowLatency
                                                         : OutputLatencyPolicy::kFullPdc;
    }
}

void AudioEngine::clear_all_tracks() {
    for (auto& t : tracks_) {
        for (auto& slot : t.chain)
            if (slot.plugin && slot.plugin->editor_open()) slot.plugin->close_editor();
        stop_capture(t);
        stop_render(t);
    }
    tracks_.clear();
    swap_graph();
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
    const std::uint64_t tsc0 = __rdtsc();  // callback load 量測(見 telemetry publish)
    const std::uint32_t frames =
        block.frames > kMaxBlockFrames ? kMaxBlockFrames : block.frames;
    TrackGraph* g = rt_graph_.load(std::memory_order_acquire);
    if (g == nullptr || frames == 0) return;  // 輸出已由 asio_device 清零 = 靜音

    // 1) 清所有軌的 summing bus(16 軌 @512f = 8K floats,可忽略)
    for (const auto& t : g->nodes) {
        if (t.buf != nullptr) {
            std::memset(t.buf->in[0], 0, frames * sizeof(float));
            std::memset(t.buf->in[1], 0, frames * sizeof(float));
            std::memset(t.buf->monitor_in[0], 0, frames * sizeof(float));
            std::memset(t.buf->monitor_in[1], 0, frames * sizeof(float));
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
        const RouteTrackPlan& route_plan = g->route_plan.tracks[idx];
        float* cur_l = n.buf->in[0];
        float* cur_r = n.buf->in[1];
        float* alt_l = n.buf->alt[0];
        float* alt_r = n.buf->alt[1];
        float* monitor_l = n.buf->monitor_in[0];
        float* monitor_r = n.buf->monitor_in[1];
        float* monitor_alt_l = n.buf->monitor_alt[0];
        float* monitor_alt_r = n.buf->monitor_alt[1];

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
        if (n.source.type != TrackSource::kNone) {
            std::memcpy(monitor_l, cur_l, frames * sizeof(float));
            std::memcpy(monitor_r, cur_r, frames * sizeof(float));
        }

        // RoutePlan 是唯一 slot 決策來源；同一迴圈依序執行 primary 與需要的
        // monitor variant，避免兩條分支各自重編 bypass/suspension 規則。
        for (std::size_t si = 0; si < n.chain.size(); ++si) {
            const RackSlot& slot = n.chain[si];
            const RouteSlotPlan& slot_plan = route_plan.slots[si];
            auto run_path = [&](const RoutePathPlan& path, Vst3Plugin* plugin,
                                ParamRing* ring, PdcDelayLine* dry_delay,
                                std::uint32_t cpu_index) {
                if (path.action == RouteSlotAction::kDry ||
                    path.action == RouteSlotAction::kReusePrimary)
                    return;
                float*& in_l = path.bus == RouteBus::kPrimary ? cur_l : monitor_l;
                float*& in_r = path.bus == RouteBus::kPrimary ? cur_r : monitor_r;
                float*& out_l = path.bus == RouteBus::kPrimary ? alt_l : monitor_alt_l;
                float*& out_r = path.bus == RouteBus::kPrimary ? alt_r : monitor_alt_r;
                float* dry_l = path.bus == RouteBus::kPrimary ? n.buf->dry[0]
                                                              : n.buf->monitor_dry[0];
                float* dry_r = path.bus == RouteBus::kPrimary ? n.buf->dry[1]
                                                              : n.buf->monitor_dry[1];
                if (path.action == RouteSlotAction::kDelayDry) {
                    if (dry_delay != nullptr) {
                        std::memset(dry_l, 0, frames * sizeof(float));
                        std::memset(dry_r, 0, frames * sizeof(float));
                        dry_delay->process_add(in_l, in_r, dry_l, dry_r, frames);
                        in_l = dry_l;
                        in_r = dry_r;
                    }
                    return;
                }
                if (plugin == nullptr || ring == nullptr) return;
                const std::size_t count = ring->pop_all(edits, kMaxParamEditsPerBlock);
                if (path.action == RouteSlotAction::kDrainParameters) {
                    if (count > 0)
                        (void)plugin->process(in_l, in_r, out_l, out_r,
                                              static_cast<std::int32_t>(frames), edits,
                                              count);
                    return;
                }

                float* failed_l = in_l;
                float* failed_r = in_r;
                if (dry_delay != nullptr) {
                    std::memset(dry_l, 0, frames * sizeof(float));
                    std::memset(dry_r, 0, frames * sizeof(float));
                    dry_delay->process_add(in_l, in_r, dry_l, dry_r, frames);
                    failed_l = dry_l;
                    failed_r = dry_r;
                }
                bool processed = false;
                if (cpu_index < kPluginLoadEntries) {
                    const auto plugin_t0 = plugin_tsc_begin();
                    processed = plugin->process(in_l, in_r, out_l, out_r,
                                                static_cast<std::int32_t>(frames), edits,
                                                count);
                    const auto elapsed = plugin_tsc_end() - plugin_t0;
                    meters_.add_plugin_cycles(
                        cpu_index, elapsed > plugin_timing_overhead_
                                       ? elapsed - plugin_timing_overhead_
                                       : 0);
                } else {
                    processed = plugin->process(in_l, in_r, out_l, out_r,
                                                static_cast<std::int32_t>(frames), edits,
                                                count);
                }
                if (processed) {
                    std::swap(in_l, out_l);
                    std::swap(in_r, out_r);
                } else {
                    in_l = failed_l;
                    in_r = failed_r;
                    rt_plugin_fails_.fetch_add(1, std::memory_order_relaxed);
                }
            };

            run_path(slot_plan.primary, slot.plugin.get(), slot.ring.get(),
                     slot.primary_dry_delay.get(), slot.primary_cpu_index);
            if (slot_plan.monitor.action == RouteSlotAction::kReusePrimary) {
                if (slot_plan.monitor.bus == RouteBus::kPrimary) {
                    monitor_l = cur_l;
                    monitor_r = cur_r;
                }
            } else {
                run_path(slot_plan.monitor, slot.monitor_shadow.get(),
                         slot.monitor_ring.get(), slot.shadow_dry_delay.get(),
                         slot.shadow_cpu_index);
            }
            if (si < n.chain_strips.size() && n.chain_strips[si] != kNoStrip)
                meter_band(meters_, n.chain_strips[si], cur_l, cur_r, frames);
        }

        if (!route_plan.monitor_diverged) {
            monitor_l = cur_l;
            monitor_r = cur_r;
        } else {
            n.buf->monitor_crossfade.blend(cur_l, cur_r, monitor_l, monitor_r, frames);
        }

        // gain/mute:post-fader,每 sample 套 target;值變時 block 內線性斜坡防爆音
        // (收斂時 inc = 0 = 常數乘;不能只斜坡一個 block — 穩態也要真的乘上 gain)
        const float target = route_plan.muted ? 0.0F : n.gain;
        const float g0 = n.buf->gain_state;
        const float inc = (target - g0) / static_cast<float>(frames);
        float gv = g0;
        for (std::uint32_t i = 0; i < frames; ++i) {
            gv += inc;
            cur_l[i] *= gv;
            cur_r[i] *= gv;
        }
        n.buf->gain_state = target;

        if (route_plan.monitor_diverged) {
            const float mg0 = n.buf->monitor_gain_state;
            const float minc = (target - mg0) / static_cast<float>(frames);
            float mgv = mg0;
            for (std::uint32_t i = 0; i < frames; ++i) {
                mgv += minc;
                monitor_l[i] *= mgv;
                monitor_r[i] *= mgv;
            }
            n.buf->monitor_gain_state = target;
        } else {
            n.buf->monitor_gain_state = target;
        }

        if (n.track_strip != kNoStrip) meter_band(meters_, n.track_strip, cur_l, cur_r, frames);

        // 目的地多選 = 加總
        for (std::size_t route = 0; route < route_plan.sends.size(); ++route) {
            const auto& send = route_plan.sends[route];
            const auto d = send.to_track_id;
            if (d >= g->id_index.size()) continue;
            const auto di = g->id_index[d];
            if (di == kNoStrip || di >= g->nodes.size()) continue;
            const auto& dst = g->nodes[di];
            if (dst.buf == nullptr) continue;
            const float* primary_l = send.primary_bus == RouteBus::kPrimary ? cur_l : monitor_l;
            const float* primary_r = send.primary_bus == RouteBus::kPrimary ? cur_r : monitor_r;
            const float* sent_monitor_l =
                send.monitor_bus == RouteBus::kPrimary ? cur_l : monitor_l;
            const float* sent_monitor_r =
                send.monitor_bus == RouteBus::kPrimary ? cur_r : monitor_r;
            if (route < n.dest_pdc.size() && n.dest_pdc[route] != nullptr)
                n.dest_pdc[route]->process_add(primary_l, primary_r, dst.buf->in[0],
                                               dst.buf->in[1], frames);
            else {
                bus_add(dst.buf->in[0], primary_l, frames);
                bus_add(dst.buf->in[1], primary_r, frames);
            }
            bus_add(dst.buf->monitor_in[0], sent_monitor_l, frames);
            bus_add(dst.buf->monitor_in[1], sent_monitor_r, frames);
        }

        const float* sink_l =
            route_plan.output_bus == RouteBus::kMonitor ? monitor_l : cur_l;
        const float* sink_r =
            route_plan.output_bus == RouteBus::kMonitor ? monitor_r : cur_r;

        // Sink(ASIO out scratch 已清零,直接 +=)
        if (n.output.type == TrackOutput::kAsioOut) {
            if (n.out_l >= 0 && static_cast<std::size_t>(n.out_l) < block.outputs.size()) {
                bus_add(block.outputs[static_cast<std::size_t>(n.out_l)], sink_l, frames);
                if (n.out_r >= 0 && static_cast<std::size_t>(n.out_r) < block.outputs.size())
                    bus_add(block.outputs[static_cast<std::size_t>(n.out_r)], sink_r, frames);
            }
            // strip 0(engine 輸出/頻譜)= 拓撲序最後一條有 ASIO out 的軌
            engine_l = sink_l;
            engine_r = sink_r;
        }
        // M5c:串流軌寫 render FIFO(滿 = 少寫;pump 端 drift 把 fill 拉回)
        if (n.output.type == TrackOutput::kWasapiRender && n.render != nullptr)
            n.render->write(sink_l, sink_r, frames);
    }

    if (engine_l != nullptr) {
        if (g->engine_strip != kNoStrip)
            meter_band(meters_, g->engine_strip, engine_l, engine_r, frames);
        meters_.append_spectrum(engine_l, engine_r, frames);  // 最終輸出進頻譜 ring
    }
    meters_.add_busy_cycles(__rdtsc() - tsc0);
}

}  // namespace rmx
