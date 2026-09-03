// NOMINMAX 先定義,Windows.h 的 min/max 巨集不得污染 std::min/std::max
#define NOMINMAX
#include "asio_device.hpp"

#include <Objbase.h>
#include <Windows.h>

// ASIO 要求 asiosys.h 先定義 IEEE754_64FLOAT,asio.h 才能宣告 ASIOSampleType
#include "asiosys.h"
#include "asio.h"
#include "asiolist.h"
#include "iasiodrv.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>

namespace rmx {

// ASIO 通知位元(take_notifications 回傳)
enum NotifyBits : std::uint32_t {
    kNotifyResetRequest = 1U << 0,
    kNotifyResyncRequest = 1U << 1,
    kNotifyLatenciesChanged = 1U << 2,
    kNotifySampleRateChanged = 1U << 3,
    kNotifyBufferSizeChanged = 1U << 4,
    kNotifyOverload = 1U << 5,
};

namespace {

constexpr std::array<std::uint32_t, 10> kCommonSampleRates = {
    22'050, 32'000, 44'100, 48'000, 88'200, 96'000, 176'400, 192'000, 352'800, 384'000};

bool asio_ok(ASIOError e) noexcept { return e == ASE_OK || e == ASE_SUCCESS; }

std::string guid_text(const CLSID& v) {
    wchar_t wide[40]{};
    if (StringFromGUID2(v, wide, 40) <= 0) return {};
    char out[40]{};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, sizeof(out), nullptr, nullptr);
    return n > 1 ? std::string(out, n - 1) : std::string{};
}

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                      nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

struct EnumeratedDriver {
    std::string name;
    std::string clsid;
    std::string path;  // driver DLL 完整路徑(vendor 面板 exe fallback 用)
};

std::vector<EnumeratedDriver> enumerate(AsioDriverList& drivers) {
    std::vector<EnumeratedDriver> result;
    const LONG count = std::clamp<LONG>(drivers.asioGetNumDev(), 0, 512);
    for (LONG i = 0; i < count; ++i) {
        char name[MAXDRVNAMELEN]{};
        char path[MAXPATHLEN]{};
        CLSID clsid{};
        if (drivers.asioGetDriverName(i, name, sizeof(name)) != 0 ||
            drivers.asioGetDriverPath(i, path, sizeof(path)) != 0 ||
            drivers.asioGetDriverCLSID(i, &clsid) != 0)
            continue;
        result.push_back({name, guid_text(clsid), path});
    }
    return result;
}

}  // namespace

struct AsioDevice::Impl {
    AsioDevice* owner{};
    AsioDriverList driver_list_;
    IASIO* driver_{};
    int driver_index_{-1};
    HWND msg_window_{};

    bool buffers_created_{};
    std::size_t in_count_{}, out_count_{};
    std::vector<ASIOBufferInfo> buffer_infos_;
    std::vector<PcmType> input_types_, output_types_;  // 選定通道的型別
    std::vector<float> input_scratch_, output_scratch_;
    std::vector<float*> input_channels_, output_channels_;
    ASIOCallbacks asio_callbacks_{};
    std::uint64_t sample_position_{};

    // ---- RT 路徑(buffer_switch 呼叫,禁止配置/鎖/系統呼叫)----
    void process_buffer(long index) noexcept {
        if (index < 0 || index > 1 || !buffers_created_) {
            owner->xruns_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const std::size_t half = static_cast<std::size_t>(index);
        const std::size_t frames = owner->block_size_;
        for (std::size_t i = 0; i < in_count_; ++i) {
            const std::size_t bytes = pcm_bytes_per_sample(input_types_[i]) * frames;
            const auto* buffer = static_cast<const std::byte*>(buffer_infos_[i].buffers[half]);
            if (buffer == nullptr ||
                !pcm_to_float32(input_types_[i], {buffer, bytes}, {input_channels_[i], frames})) {
                std::fill_n(input_channels_[i], frames, 0.0F);
                owner->xruns_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        std::fill(output_scratch_.begin(), output_scratch_.end(), 0.0F);
        if (auto* cb = owner->callback_.load(std::memory_order_acquire)) {
            cb->process({static_cast<std::uint32_t>(frames), input_channels_, output_channels_});
        }
        for (std::size_t o = 0; o < out_count_; ++o) {
            const std::size_t bytes = pcm_bytes_per_sample(output_types_[o]) * frames;
            auto* buffer = static_cast<std::byte*>(buffer_infos_[in_count_ + o].buffers[half]);
            if (buffer == nullptr ||
                !float32_to_pcm(output_types_[o], {output_channels_[o], frames}, {buffer, bytes})) {
                if (buffer != nullptr) std::memset(buffer, 0, bytes);
                owner->xruns_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        sample_position_ += frames;
        owner->callbacks_.fetch_add(1, std::memory_order_relaxed);
        driver_->outputReady();
    }

    void notify(std::uint32_t bits) noexcept {
        owner->notifications_.fetch_or(bits, std::memory_order_release);
    }

    // ---- ASIO static callbacks ----
    static std::atomic<Impl*>& active() {
        static std::atomic<Impl*> instance{};
        return instance;
    }

    static void buffer_switch(long index, ASIOBool) noexcept {
        if (auto* impl = active().load(std::memory_order_acquire)) impl->process_buffer(index);
    }
    static void sample_rate_did_change(ASIOSampleRate) noexcept {
        if (auto* impl = active().load(std::memory_order_acquire))
            impl->notify(kNotifySampleRateChanged);
    }
    static long asio_message(long selector, long value, void*, double*) noexcept {
        auto* impl = active().load(std::memory_order_acquire);
        switch (selector) {
        case kAsioSelectorSupported:
            return value == kAsioResetRequest || value == kAsioResyncRequest ||
                           value == kAsioLatenciesChanged || value == kAsioEngineVersion ||
                           value == kAsioOverload
                       ? 1
                       : 0;
        case kAsioEngineVersion: return 2;
        case kAsioResetRequest:
            if (impl) impl->notify(kNotifyResetRequest);
            return 1;
        case kAsioBufferSizeChange:
            if (impl) impl->notify(kNotifyBufferSizeChanged);
            return 1;
        case kAsioResyncRequest:
            if (impl) impl->notify(kNotifyResyncRequest);
            return 1;
        case kAsioLatenciesChanged:
            if (impl) impl->notify(kNotifyLatenciesChanged);
            return 1;
        case kAsioOverload:
            if (impl) {
                impl->notify(kNotifyOverload);
                impl->owner->xruns_.fetch_add(1, std::memory_order_relaxed);
            }
            return 1;
        default: return 0;
        }
    }
    static ASIOTime* buffer_switch_time_info(ASIOTime* params, long index, ASIOBool) noexcept {
        if (auto* impl = active().load(std::memory_order_acquire)) impl->process_buffer(index);
        return params;
    }
};

std::vector<DriverEntry> enumerate_drivers() {
    AsioDriverList drivers;
    std::vector<DriverEntry> result;
    for (const auto& d : enumerate(drivers)) result.push_back({d.clsid, d.name});
    return result;
}

std::vector<std::uint32_t> DeviceCapability::buffer_options() const {
    std::vector<std::uint32_t> out;
    constexpr std::size_t kMaxOptions = 32;  // 下拉夠用;granularity=1 的 driver 截斷
    if (min_buffer == 0 || max_buffer < min_buffer) return out;
    auto push = [&](std::uint32_t v) {
        if (v >= min_buffer && v <= max_buffer && out.size() < kMaxOptions &&
            std::find(out.begin(), out.end(), v) == out.end())
            out.push_back(v);
    };
    if (buffer_granularity > 0) {
        for (std::uint64_t v = min_buffer; v <= max_buffer && out.size() < kMaxOptions;
             v += static_cast<std::uint64_t>(buffer_granularity))
            push(static_cast<std::uint32_t>(v));
    } else if (buffer_granularity == 0) {
        for (std::uint64_t v = min_buffer; v <= max_buffer && out.size() < kMaxOptions; v <<= 1)
            push(static_cast<std::uint32_t>(v));
    }
    // granularity < 0(任意值)或不連續展開:pow2 常見值過濾範圍
    if (out.empty()) {
        for (std::uint32_t v = 16; v <= 4096 && out.size() < kMaxOptions; v <<= 1) push(v);
    }
    // preferred 沒在清單 = 展開規則沒涵蓋(如 pref=128 但 granularity=0 從 16 起)—— 補進
    push(preferred_buffer);
    std::sort(out.begin(), out.end());
    return out;
}

AsioDevice::~AsioDevice() { close(); }

bool AsioDevice::probe(const std::string& clsid, std::string& err) {
    if (impl_ && clsid_ == clsid && !cap_.sample_rates.empty()) return true;  // 已 probe
    close();

    auto* impl = new Impl();
    impl->owner = this;

    const auto descriptors = enumerate(impl->driver_list_);
    const auto found = std::find_if(descriptors.begin(), descriptors.end(),
                                    [&](const EnumeratedDriver& d) { return d.clsid == clsid; });
    if (found == descriptors.end()) {
        err = "driver not found";
        delete impl;
        return false;
    }
    int index = -1;
    const LONG count = std::clamp<LONG>(impl->driver_list_.asioGetNumDev(), 0, 512);
    for (LONG i = 0; i < count; ++i) {
        CLSID candidate{};
        if (impl->driver_list_.asioGetDriverCLSID(i, &candidate) == 0 &&
            guid_text(candidate) == clsid) {
            index = static_cast<int>(i);
            break;
        }
    }
    if (index < 0) {
        err = "driver clsid mismatch";
        delete impl;
        return false;
    }
    void* instance{};
    if (impl->driver_list_.asioOpenDriver(index, &instance) != 0 || instance == nullptr) {
        err = "asioOpenDriver failed";
        delete impl;
        return false;
    }
    impl->driver_ = static_cast<IASIO*>(instance);
    impl->driver_index_ = index;
    impl->msg_window_ = CreateWindowExW(0, L"STATIC", L"RoudaMix ASIO Host", 0, 0, 0, 0, 0,
                                        HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (impl->driver_->init(impl->msg_window_ != nullptr ? impl->msg_window_
                                                         : GetDesktopWindow()) != ASIOTrue) {
        err = "driver init failed";
        close_impl_part(impl);
        delete impl;
        return false;
    }

    long in_count{}, out_count{}, min_b{}, max_b{}, preferred{}, granularity{};
    ASIOSampleRate current{};
    if (!asio_ok(impl->driver_->getChannels(&in_count, &out_count)) || in_count < 0 ||
        out_count < 0 ||
        !asio_ok(impl->driver_->getBufferSize(&min_b, &max_b, &preferred, &granularity)) ||
        min_b <= 0 || max_b < min_b || preferred < min_b || preferred > max_b ||
        !asio_ok(impl->driver_->getSampleRate(&current)) || !std::isfinite(current) ||
        current <= 0.0) {
        err = "invalid driver capability";
        close_impl_part(impl);
        delete impl;
        return false;
    }

    DeviceCapability cap;
    cap.max_in = static_cast<std::uint32_t>(in_count);
    cap.max_out = static_cast<std::uint32_t>(out_count);
    cap.min_buffer = static_cast<std::uint32_t>(min_b);
    cap.max_buffer = static_cast<std::uint32_t>(max_b);
    cap.preferred_buffer = static_cast<std::uint32_t>(preferred);
    cap.buffer_granularity = granularity;
    cap.current_sample_rate = static_cast<std::uint32_t>(std::llround(current));
    for (const auto rate : kCommonSampleRates) {
        if (asio_ok(impl->driver_->canSampleRate(static_cast<double>(rate))))
            cap.sample_rates.push_back(rate);
    }
    if (std::find(cap.sample_rates.begin(), cap.sample_rates.end(), cap.current_sample_rate) ==
        cap.sample_rates.end())
        cap.sample_rates.push_back(cap.current_sample_rate);
    long in_lat{}, out_lat{};
    if (asio_ok(impl->driver_->getLatencies(&in_lat, &out_lat)) && in_lat >= 0 && out_lat >= 0) {
        cap.input_latency = static_cast<std::uint32_t>(in_lat);
        cap.output_latency = static_cast<std::uint32_t>(out_lat);
        cap.latency_valid = true;
    }

    // per-channel sample types + 名稱
    bool types_ok = true;
    for (long i = 0; i < in_count && types_ok; ++i) {
        ASIOChannelInfo info{};
        info.channel = i;
        info.isInput = ASIOTrue;
        if (!asio_ok(impl->driver_->getChannelInfo(&info))) {
            types_ok = false;
            break;
        }
        cap.input_types.push_back(map_asio_sample_type(info.type));
        cap.input_names.push_back(info.name);
    }
    for (long o = 0; o < out_count && types_ok; ++o) {
        ASIOChannelInfo info{};
        info.channel = o;
        info.isInput = ASIOFalse;
        if (!asio_ok(impl->driver_->getChannelInfo(&info))) {
            types_ok = false;
            break;
        }
        cap.output_types.push_back(map_asio_sample_type(info.type));
        cap.output_names.push_back(info.name);
    }
    if (!types_ok) {
        err = "getChannelInfo failed";
        close_impl_part(impl);
        delete impl;
        return false;
    }

    impl_ = impl;
    cap_ = std::move(cap);
    clsid_ = clsid;
    name_ = found->name;
    driver_dll_path_ = found->path;
    return true;
}

bool AsioDevice::prepare(std::uint32_t sample_rate,
                         const std::vector<std::uint32_t>& in_channels,
                         const std::vector<std::uint32_t>& out_channels,
                         std::uint32_t buffer_size, std::string& err) {
    if (!impl_ || !impl_->driver_) {
        err = "device not probed";
        return false;
    }
    if (out_channels.empty()) {
        err = "no output channel";
        return false;
    }
    if (sample_rate != cap_.current_sample_rate &&
        std::find(cap_.sample_rates.begin(), cap_.sample_rates.end(), sample_rate) ==
            cap_.sample_rates.end()) {
        err = "unsupported sample rate";
        return false;
    }
    if (buffer_size != 0 &&
        (buffer_size < cap_.min_buffer || buffer_size > cap_.max_buffer)) {
        err = "buffer size out of range [" + std::to_string(cap_.min_buffer) + "," +
              std::to_string(cap_.max_buffer) + "]";
        return false;
    }
    if (impl_->buffers_created_) {
        stop();
        impl_->driver_->disposeBuffers();
        impl_->buffers_created_ = false;
    }
    if (sample_rate != cap_.current_sample_rate) {
        if (!asio_ok(impl_->driver_->setSampleRate(static_cast<double>(sample_rate)))) {
            err = "setSampleRate failed";
            return false;
        }
        // SSL 這類 driver:setSampleRate 回 OK 但硬體沒跟著換(callback 不會來)。
        // 讀回驗證,沒換成功就明確報錯(UI 顯示、引導用硬體面板改)
        ASIOSampleRate actual{};
        if (!asio_ok(impl_->driver_->getSampleRate(&actual)) ||
            std::llround(actual) != static_cast<long>(sample_rate)) {
            err = "driver did not apply sample rate " + std::to_string(sample_rate) +
                  " (actual " + std::to_string(static_cast<long>(std::llround(actual))) +
                  "); use hardware panel";
            return false;
        }
        cap_.current_sample_rate = sample_rate;
    }

    auto* impl = impl_;
    impl->buffer_infos_.clear();
    impl->input_types_.clear();
    impl->output_types_.clear();
    input_map_.clear();
    output_map_.clear();
    for (const auto ch : in_channels) {
        if (ch >= cap_.input_types.size()) {
            err = "input channel " + std::to_string(ch) + " out of range";
            return false;
        }
        if (pcm_bytes_per_sample(cap_.input_types[ch]) == 0) continue;  // 跳過不支援型別通道
        impl->buffer_infos_.push_back(
            {ASIOTrue, static_cast<long>(ch), {nullptr, nullptr}});
        impl->input_types_.push_back(cap_.input_types[ch]);
        input_map_.push_back(ch);
    }
    for (const auto ch : out_channels) {
        if (ch >= cap_.output_types.size()) {
            err = "output channel " + std::to_string(ch) + " out of range";
            return false;
        }
        if (pcm_bytes_per_sample(cap_.output_types[ch]) == 0) continue;
        impl->buffer_infos_.push_back(
            {ASIOFalse, static_cast<long>(ch), {nullptr, nullptr}});
        impl->output_types_.push_back(cap_.output_types[ch]);
        output_map_.push_back(ch);
    }
    if (impl->buffer_infos_.empty()) {
        err = "no usable channel pair";
        return false;
    }

    // createBuffers 用的要求值(0 = driver preferred)。driver 是最終權威:建完緩衝
    // 後讀回實際授予大小(getBufferSize 此時回單一現行值;granularity 不合時部分
    // driver 會 clamp),scratch 一併以授予值配置 —— 否則 RT 複製以 block_size_ 為
    // 步距,driver 緩衝比要求小時會越界。
    const std::uint32_t frames = buffer_size != 0 ? buffer_size : cap_.preferred_buffer;

    // 單例 gate:同 process 一條 ASIO stream
    AsioDevice::Impl* expected{};
    if (!AsioDevice::Impl::active().compare_exchange_strong(expected, impl,
                                                            std::memory_order_acq_rel)) {
        err = "another ASIO stream is active";
        return false;
    }
    impl->asio_callbacks_ = {&Impl::buffer_switch, &Impl::sample_rate_did_change,
                             &Impl::asio_message, &Impl::buffer_switch_time_info};
    const auto create_result = impl->driver_->createBuffers(
        impl->buffer_infos_.data(), static_cast<long>(impl->buffer_infos_.size()),
        static_cast<long>(frames), &impl->asio_callbacks_);
    if (!asio_ok(create_result)) {
        AsioDevice::Impl* mine = impl;
        AsioDevice::Impl::active().compare_exchange_strong(mine, nullptr,
                                                           std::memory_order_acq_rel);
        err = "createBuffers failed";
        return false;
    }
    impl->buffers_created_ = true;

    long amin{}, amax{}, agranted{}, agran{};
    std::uint32_t granted = frames;
    if (asio_ok(impl_->driver_->getBufferSize(&amin, &amax, &agranted, &agran)) &&
        agranted > 0)
        granted = static_cast<std::uint32_t>(agranted);
    block_size_ = granted;

    const std::size_t actual_in = impl->input_types_.size();
    const std::size_t actual_out = impl->output_types_.size();
    impl->input_scratch_.assign(actual_in * granted, 0.0F);
    impl->output_scratch_.assign(actual_out * granted, 0.0F);
    impl->input_channels_.resize(actual_in);
    impl->output_channels_.resize(actual_out);
    for (std::size_t i = 0; i < actual_in; ++i)
        impl->input_channels_[i] = impl->input_scratch_.data() + i * granted;
    for (std::size_t o = 0; o < actual_out; ++o)
        impl->output_channels_[o] = impl->output_scratch_.data() + o * granted;
    impl->in_count_ = actual_in;
    impl->out_count_ = actual_out;

    // 輸出緩衝清零(防啟動爆音)
    std::fill(impl->output_scratch_.begin(), impl->output_scratch_.end(), 0.0F);
    for (std::size_t o = 0; o < actual_out; ++o) {
        const std::size_t bytes = pcm_bytes_per_sample(impl->output_types_[o]) * granted;
        for (std::size_t half = 0; half < 2; ++half) {
            auto* buffer =
                static_cast<std::byte*>(impl->buffer_infos_[actual_in + o].buffers[half]);
            if (buffer != nullptr) std::memset(buffer, 0, bytes);
        }
    }
    return true;
}

bool AsioDevice::start(std::string& err) {
    if (!impl_ || !impl_->driver_ || !impl_->buffers_created_) {
        err = "device not prepared";
        return false;
    }
    AsioDevice::Impl* expected{};
    auto& active = AsioDevice::Impl::active();
    if (!active.compare_exchange_strong(expected, impl_, std::memory_order_acq_rel) &&
        expected != impl_) {
        err = "another ASIO stream is active";
        return false;
    }
    if (!asio_ok(impl_->driver_->start())) {
        AsioDevice::Impl* mine = impl_;
        active.compare_exchange_strong(mine, nullptr, std::memory_order_acq_rel);
        err = "driver start failed";
        return false;
    }
    running_.store(true, std::memory_order_release);
    return true;
}

void AsioDevice::stop() noexcept {
    if (impl_ && impl_->driver_) {
        if (running_.load(std::memory_order_acquire)) {
            impl_->driver_->stop();
            running_.store(false, std::memory_order_release);
        }
        if (impl_->buffers_created_) {
            AsioDevice::Impl* mine = impl_;
            AsioDevice::Impl::active().compare_exchange_strong(mine, nullptr,
                                                               std::memory_order_acq_rel);
        }
    }
}

bool AsioDevice::open_control_panel(std::string& err) {
    if (!impl_ || !impl_->driver_) {
        err = "device not open";
        return false;
    }
    // 1) 正規 ASIO controlPanel()。Thesycon 系(SSL 實測)是 stub:回 OK 不開視窗
    const bool driver_ok = asio_ok(impl_->driver_->controlPanel());
    // 2) vendor 面板 fallback:driver DLL 同目錄的 *cpl*.exe(如 SSLUsbAudioCpl.exe)
    if (!driver_dll_path_.empty()) {
        const std::size_t slash = driver_dll_path_.find_last_of("\\/");
        if (slash != std::string::npos) {
            const std::wstring dir = utf8_to_wide(driver_dll_path_.substr(0, slash + 1));
            WIN32_FIND_DATAW fd{};
            HANDLE found = FindFirstFileW((dir + L"*cpl*.exe").c_str(), &fd);
            if (found != INVALID_HANDLE_VALUE) {
                const std::wstring exe = dir + fd.cFileName;
                FindClose(found);
                STARTUPINFOW si{};
                si.cb = sizeof(si);
                PROCESS_INFORMATION pi{};
                // 獨立 process 面板:使用者直接操作,生死與 engine 無關。
                // 等它結束(呼叫端是 detach thread)= 面板關閉事件 + driver 設定已落地
                if (CreateProcessW(exe.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr,
                                   nullptr, &si, &pi)) {
                    CloseHandle(pi.hThread);
                    WaitForSingleObject(pi.hProcess, INFINITE);
                    CloseHandle(pi.hProcess);
                    return true;
                }
            }
        }
    }
    if (!driver_ok) {
        err = "driver controlPanel failed and no vendor panel exe found";
        return false;
    }
    return true;
}

// impl_ 的 driver/window 清理(probe 失敗路徑共用;不刪 impl_)
void AsioDevice::close_impl_part(Impl* impl) noexcept {
    if (impl->buffers_created_ && impl->driver_) {
        impl->driver_->disposeBuffers();
        impl->buffers_created_ = false;
    }
    AsioDevice::Impl* mine = impl;
    AsioDevice::Impl::active().compare_exchange_strong(mine, nullptr, std::memory_order_acq_rel);
    if (impl->driver_index_ >= 0) impl->driver_list_.asioCloseDriver(impl->driver_index_);
    impl->driver_ = nullptr;
    impl->driver_index_ = -1;
    if (impl->msg_window_ != nullptr) {
        DestroyWindow(impl->msg_window_);
        impl->msg_window_ = nullptr;
    }
}

void AsioDevice::close() noexcept {
    if (!impl_) return;
    stop();
    close_impl_part(impl_);
    delete impl_;
    impl_ = nullptr;
    clsid_.clear();
    name_.clear();
    driver_dll_path_.clear();
    cap_ = DeviceCapability{};
    block_size_ = 0;
}

void AsioDevice::attach_driver_for_test(IASIO* driver, DeviceCapability cap) {
    close();
    auto* impl = new Impl();
    impl->owner = this;
    impl->driver_ = driver;
    impl_ = impl;
    cap_ = std::move(cap);
    clsid_ = "test";
    name_ = "Test Driver";
}

std::uint64_t AsioDevice::xruns() const noexcept { return xruns_.load(std::memory_order_acquire); }
std::uint64_t AsioDevice::callbacks() const noexcept {
    return callbacks_.load(std::memory_order_acquire);
}
std::uint32_t AsioDevice::take_notifications() noexcept {
    return notifications_.exchange(0, std::memory_order_acq_rel);
}

}  // namespace rmx
