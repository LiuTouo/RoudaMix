#include "audio_engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

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
}  // namespace

AudioEngine::AudioEngine() {
    sine_freq_bits_.store(f32_bits(440.0F), std::memory_order_relaxed);
}

AudioEngine::~AudioEngine() {
    exiting_.store(true, std::memory_order_release);
    if (publish_thread_.joinable()) publish_thread_.join();
    device_.close();
    delete rt_rack_.load(std::memory_order_relaxed);
    for (auto& r : retired_) delete r.chain;
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
            sum.preferred_buffer = cap.preferred_buffer;
            sum.sample_rates = cap.sample_rates;
        }
        probe_device.close();
        result.push_back(std::move(sum));
    }
    return result;
}

bool AudioEngine::start(const std::string& device_key,
                        std::optional<std::uint32_t> sample_rate, std::string& err) {
    if (device_.running()) {
        err = "already running";
        return false;
    }
    if (!device_.probe(device_key, err)) return false;
    const auto& cap = device_.capability();
    const std::uint32_t rate = sample_rate.value_or(cap.current_sample_rate);

    if (!device_.prepare(rate, /*in=*/2, /*out=*/2, err)) {
        device_.close();
        return false;
    }
    device_.set_callback(this);

    // rack plugin 先 initialize + 進 RT 鏈,再開 device —— callback 一啟動就拿到
    // 已就緒的 plugin(順序反了 RT 會拿到未 initialize 的鏈,process 全 fail)。
    // terminate 先跑:前次 start 失敗殘留的 initialized 狀態會讓 initialize 拒絕
    for (auto& slot : rack_) {
        slot.plugin->terminate();
        if (!slot.plugin->initialize(static_cast<double>(rate), device_.block_size())) {
            device_.close();
            err = "plugin '" + slot.name + "' init failed: " + slot.plugin->last_error();
            return false;
        }
    }
    swap_rack();

    if (!device_.start(err)) {
        device_.close();
        err = std::string("ASIO start failed after rack ready: ") + err;
        return false;
    }

    rt_sample_rate_.store(rate, std::memory_order_relaxed);
    rt_phase_.store(0, std::memory_order_relaxed);
    last_device_key_ = device_key;  // session 用:stop 後存檔仍記得裝置
    last_sample_rate_ = rate;
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
                // strip instance ids:0 = engine 輸出(0xFFFFFFFF)、1.. = rack slots
                std::uint32_t ids[kTelemetryStrips] = {0xFFFFFFFFu};
                std::size_t count = 1;
                const RackChain* chain = rt_rack_.load(std::memory_order_acquire);
                if (chain != nullptr) {
                    for (const auto& slot : chain->slots) {
                        if (count >= kTelemetryStrips) break;
                        ids[count++] = slot.instance_id;
                    }
                }
                meters_.publish(*shm_, device_.xruns(), ids, count);
            }
        });
    }
    return true;
}

void AudioEngine::stop() noexcept {
    device_.stop();
    // RT 停 callback 後退鏈、卸 plugin(下次 start 依新 rate 重建)
    retire_rack();
    for (auto& slot : rack_) slot.plugin->terminate();
    clear_expired_retired(/*force=*/true);
}

// ---- rack ----

void AudioEngine::swap_rack() noexcept {
    auto* fresh = new RackChain{rack_};  // 淺拷貝:plugin/ring shared、值欄位快照
    RackChain* old = rt_rack_.exchange(fresh, std::memory_order_acq_rel);
    if (old != nullptr) retired_.push_back({old, GetTickCount64()});
    clear_expired_retired(false);
}

void AudioEngine::retire_rack() noexcept {
    RackChain* old = rt_rack_.exchange(nullptr, std::memory_order_acq_rel);
    if (old != nullptr) retired_.push_back({old, GetTickCount64()});
}

void AudioEngine::clear_expired_retired(bool force) noexcept {
    const std::uint64_t now = GetTickCount64();
    std::vector<Retired> still;
    still.reserve(retired_.size());
    for (auto& r : retired_) {
        if (force || (now - r.tick) > 500)
            delete r.chain;
        else
            still.push_back(std::move(r));
    }
    retired_ = std::move(still);
}

RackSlot* AudioEngine::find_slot_mut(std::uint32_t instance_id) noexcept {
    for (auto& s : rack_)
        if (s.instance_id == instance_id) return &s;
    return nullptr;
}

const RackSlot* AudioEngine::find_slot(std::uint32_t instance_id) const noexcept {
    for (const auto& s : rack_)
        if (s.instance_id == instance_id) return &s;
    return nullptr;
}

bool AudioEngine::add_plugin(const std::string& module_path, const std::string& class_id,
                             std::uint32_t& instance_id, std::string& err) {
    if (rack_.size() >= kMaxRackSlots) {
        err = "rack full (15 slots)";
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
    rack_.push_back(std::move(slot));
    swap_rack();
    return true;
}

bool AudioEngine::remove_plugin(std::uint32_t instance_id, std::string& err) {
    for (auto it = rack_.begin(); it != rack_.end(); ++it) {
        if (it->instance_id == instance_id) {
            rack_.erase(it);
            swap_rack();
            return true;
        }
    }
    err = "unknown instanceId " + std::to_string(instance_id);
    return false;
}

bool AudioEngine::move_plugin(std::uint32_t instance_id, std::size_t to_index,
                              std::string& err) {
    if (to_index >= rack_.size()) {
        err = "toIndex out of range";
        return false;
    }
    const auto from = std::find_if(rack_.begin(), rack_.end(),
                                   [&](const RackSlot& s) { return s.instance_id == instance_id; });
    if (from == rack_.end()) {
        err = "unknown instanceId " + std::to_string(instance_id);
        return false;
    }
    RackSlot moved = std::move(*from);
    rack_.erase(from);
    rack_.insert(rack_.begin() + static_cast<std::ptrdiff_t>(to_index), std::move(moved));
    swap_rack();
    return true;
}

bool AudioEngine::set_bypass(std::uint32_t instance_id, bool bypass, std::string& err) {
    RackSlot* s = find_slot_mut(instance_id);
    if (s == nullptr) {
        err = "unknown instanceId " + std::to_string(instance_id);
        return false;
    }
    s->bypass = bypass;
    swap_rack();
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

bool AudioEngine::set_source(bool passthrough, float sine_freq, std::string& err) {
    if (sine_freq < 20.0F || sine_freq > 20000.0F) {
        err = "sineFreq out of range [20,20000]";
        return false;
    }
    source_passthrough_.store(passthrough ? 1u : 0u, std::memory_order_relaxed);
    sine_freq_bits_.store(f32_bits(sine_freq), std::memory_order_relaxed);
    return true;
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
    s.source = source_passthrough_.load(std::memory_order_relaxed) ? "passthrough" : "sine";
    s.sine_freq = bits_f32(sine_freq_bits_.load(std::memory_order_relaxed));
    if (s.sine_freq == 0.0F) s.sine_freq = 440.0F;
    s.plugin_fails = rt_plugin_fails_.load(std::memory_order_relaxed);
    return s;
}

// ---- RT:audio callback(禁配置/鎖/系統呼叫)----
void AudioEngine::process(const AudioBlock& block) noexcept {
    const std::uint32_t frames =
        block.frames > kMaxBlockFrames ? kMaxBlockFrames : block.frames;
    const bool passthrough = source_passthrough_.load(std::memory_order_relaxed) != 0;
    const float freq = bits_f32(sine_freq_bits_.load(std::memory_order_relaxed));
    const float rate = static_cast<float>(rt_sample_rate_.load(std::memory_order_relaxed));

    float* out_l = block.outputs.size() > 0 ? block.outputs[0] : nullptr;
    float* out_r = block.outputs.size() > 1 ? block.outputs[1] : nullptr;
    const float* in_l = block.inputs.size() > 0 ? block.inputs[0] : nullptr;
    const float* in_r = block.inputs.size() > 1 ? block.inputs[1] : nullptr;

    std::uint64_t phase = rt_phase_.load(std::memory_order_relaxed);
    const std::uint64_t step =
        rate > 0.0F
            ? static_cast<std::uint64_t>(4294967296.0 * static_cast<double>(freq) / rate)
            : 0;

    // 來源進 ping bus,再過 rack 鏈(每 slot 就地 ping-pong),最後 copy 到輸出
    float* cur_l = rt_bus_[0][0];
    float* cur_r = rt_bus_[0][1];
    float* alt_l = rt_bus_[1][0];
    float* alt_r = rt_bus_[1][1];
    for (std::uint32_t i = 0; i < frames; ++i) {
        float l, r;
        if (passthrough) {
            l = in_l != nullptr ? in_l[i] : 0.0F;
            r = in_r != nullptr ? in_r[i] : l;
        } else {
            // 相位表 sine:2π = 2^32
            const double a = static_cast<double>(phase >> 8) *
                             (2.0 * 3.14159265358979323846 / 16777216.0);
            const float s = 0.25F * static_cast<float>(std::sin(a));  // -12 dBFS 防爆
            l = s;
            r = s;
            phase += step;
        }
        cur_l[i] = l;
        cur_r[i] = r;
    }
    rt_phase_.store(phase, std::memory_order_relaxed);

    RackChain* chain = rt_rack_.load(std::memory_order_acquire);
    if (chain != nullptr) {
        Vst3ParamEdit edits[kMaxParamEditsPerBlock];
        std::size_t strip = 1;  // strip 0 = engine 輸出
        for (const auto& slot : chain->slots) {
            if (strip > kMaxRackSlots) break;
            Vst3Plugin* plugin = slot.plugin.get();
            if (!slot.bypass && plugin != nullptr) {
                const std::size_t n = slot.ring->pop_all(edits, kMaxParamEditsPerBlock);
                if (plugin->process(cur_l, cur_r, alt_l, alt_r,
                                    static_cast<std::int32_t>(frames), edits, n)) {
                    std::swap(cur_l, alt_l);
                    std::swap(cur_r, alt_r);
                } else {
                    // plugin 拒絕本 block:維持原樣(bypass 效果)、計失敗
                    rt_plugin_fails_.fetch_add(1, std::memory_order_relaxed);
                }
            }
            meter_band(meters_, strip, cur_l, cur_r, frames);
            ++strip;
        }
    }

    if (out_l != nullptr) std::memcpy(out_l, cur_l, frames * sizeof(float));
    if (out_r != nullptr) std::memcpy(out_r, cur_r, frames * sizeof(float));
    // 超出 bus 上限的尾巴(異常巨 block):補靜音,不出垃圾
    for (std::uint32_t i = frames; i < block.frames; ++i) {
        if (out_l != nullptr) out_l[i] = 0.0F;
        if (out_r != nullptr) out_r[i] = 0.0F;
    }
    meter_band(meters_, 0, cur_l, cur_r, frames);
}

}  // namespace rmx
