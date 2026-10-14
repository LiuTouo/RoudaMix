#include "loudness.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <vector>

#include <ebur128.h>
#include "spsc_fifo.hpp"

namespace rmx {
namespace {
// libebur128 init rewrites shared histogram tables, even while another state exists.
std::mutex library_mutex;
std::optional<double> finite(double value) {
    return std::isfinite(value) ? std::optional<double>{value} : std::nullopt;
}
std::size_t capacity(std::uint32_t rate) {
    std::size_t result = 1;
    while (result < static_cast<std::size_t>(rate) * 2) result *= 2;
    return result;
}
}

nlohmann::json loudness_report_json(const LoudnessReport& r) {
    auto number = [](std::optional<double> v) -> nlohmann::json {
        return v && std::isfinite(*v) ? nlohmann::json(*v) : nlohmann::json(nullptr);
    };
    return {{"measurementId", r.measurement_id}, {"trackId", r.track_id},
            {"state", r.state == LoudnessReport::State::kError ? "error" :
                      r.state == LoudnessReport::State::kStopped ? "stopped" : "measuring"},
            {"error", r.error ? nlohmann::json(r.error) : nlohmann::json(nullptr)},
            {"elapsedSeconds", r.elapsed_seconds},
            {"momentaryLufs", number(r.momentary_lufs)},
            {"shortTermLufs", number(r.short_term_lufs)},
            {"integratedLufs", number(r.integrated_lufs)},
            {"truePeakDbtp", number(r.true_peak_dbtp)},
            {"loudnessRangeLu", number(r.loudness_range_lu)}};
}

LoudnessMeter::LoudnessMeter(std::uint32_t sample_rate)
    : rate_(sample_rate), next_lra_frame_(((sample_rate + 5) / 10) * 30ull) {
    if (rate_ < 8000 || rate_ > 384000) throw std::invalid_argument("unsupported loudness sample rate");
    std::lock_guard lock(library_mutex);
    meter_ = ebur128_init(2, rate_, EBUR128_MODE_I | EBUR128_MODE_LRA |
                                   EBUR128_MODE_TRUE_PEAK | EBUR128_MODE_HISTOGRAM);
    if (!meter_) throw std::bad_alloc();
}
LoudnessMeter::~LoudnessMeter() { ebur128_destroy(&meter_); }

bool LoudnessMeter::add(const float* stereo, std::size_t frames) {
    for (std::size_t i = 0; i < frames * 2; ++i)
        if (!std::isfinite(stereo[i])) return false;
    std::lock_guard lock(library_mutex);
    while (frames) {
        const auto count = (std::min)(frames, static_cast<std::size_t>(next_lra_frame_ - frames_));
        if (ebur128_add_frames_float(meter_, stereo, count) != EBUR128_SUCCESS) return false;
        frames_ += count;
        frames -= count;
        stereo += count * 2;
        if (frames_ == next_lra_frame_) {
            double value{};
            // Upstream stores 3s blocks at 3s,4s,... only if above its -70 LUFS gate.
            if (ebur128_loudness_shortterm(meter_, &value) == EBUR128_SUCCESS && value >= -70.0)
                gated_lra_ = true;
            next_lra_frame_ += ((rate_ + 5) / 10) * 10ull;
        }
    }
    return true;
}

LoudnessReport LoudnessMeter::report() const {
    std::lock_guard lock(library_mutex);
    LoudnessReport r;
    r.elapsed_seconds = static_cast<double>(frames_) / rate_;
    double value{};
    if (r.elapsed_seconds >= .4 && ebur128_loudness_momentary(meter_, &value) == EBUR128_SUCCESS)
        r.momentary_lufs = finite(value);
    if (r.elapsed_seconds >= 3 && ebur128_loudness_shortterm(meter_, &value) == EBUR128_SUCCESS)
        r.short_term_lufs = finite(value);
    if (ebur128_loudness_global(meter_, &value) == EBUR128_SUCCESS) r.integrated_lufs = finite(value);
    double peak = 0;
    for (unsigned channel = 0; channel < 2; ++channel)
        if (ebur128_true_peak(meter_, channel, &value) == EBUR128_SUCCESS) peak = (std::max)(peak, value);
    if (peak > 0) r.true_peak_dbtp = finite(20 * std::log10(peak));
    if (gated_lra_ && ebur128_loudness_range(meter_, &value) == EBUR128_SUCCESS)
        r.loudness_range_lu = finite(value);
    return r;
}

struct LoudnessCapture::Resources {
    LoudnessMeter meter;
    SpscFifo fifo;
    std::vector<float> interleave;
    std::vector<float> input;
    Resources(std::uint32_t rate, std::size_t block)
        : meter(rate), fifo(capacity(rate)), interleave(block * 2), input(8192) {}
};

LoudnessCapture::LoudnessCapture(std::string id, std::uint32_t track_id,
                               std::uint32_t rate, std::size_t max_block_frames)
    : id_(std::move(id)), track_id_(track_id),
      resources_(std::make_unique<Resources>(rate, max_block_frames)) {
    report_.measurement_id = id_;
    report_.track_id = track_id_;
    worker_ = std::thread([this] { run(); });
}
LoudnessCapture::~LoudnessCapture() { stop(); }

void LoudnessCapture::capture(const float* left, const float* right, std::size_t frames) noexcept {
    if (!active_.load(std::memory_order_seq_cst)) return;
    writers_.fetch_add(1, std::memory_order_seq_cst);
    // Paired SC gate allows stop to free resources even if an old graph survives.
    if (active_.load(std::memory_order_seq_cst) && !fault_.load(std::memory_order_relaxed)) {
        auto& r = *resources_;
        if (frames > r.interleave.size() / 2) {
            fault_.store(1, std::memory_order_release);
        } else {
            for (std::size_t i = 0; i < frames; ++i) {
                r.interleave[i * 2] = left[i];
                r.interleave[i * 2 + 1] = right[i];
            }
            if (r.fifo.write(r.interleave.data(), frames) != frames)
                fault_.store(1, std::memory_order_release);
        }
    }
    writers_.fetch_sub(1, std::memory_order_seq_cst);
}

void LoudnessCapture::run() noexcept {
    try {
        auto& r = *resources_;
        auto published = std::chrono::steady_clock::now();
        for (;;) {
            const bool exiting = exiting_.load(std::memory_order_acquire);
            const auto count = r.fifo.read(r.input.data(), r.input.size() / 2);
            if (count && !fault_.load(std::memory_order_acquire) && !r.meter.add(r.input.data(), count))
                fault_.store(2, std::memory_order_release);
            const auto now = std::chrono::steady_clock::now();
            const int fault = fault_.load(std::memory_order_acquire);
            const bool done = fault || (exiting && !count);
            if (done || now - published >= std::chrono::milliseconds(200)) {
                auto next = r.meter.report();
                next.measurement_id = id_;
                next.track_id = track_id_;
                if (fault) {
                    next.state = LoudnessReport::State::kError;
                    next.error = fault == 1 ? "Loudness capture overflow" : "Invalid audio or loudness processing failure";
                    next.momentary_lufs.reset(); next.short_term_lufs.reset(); next.integrated_lufs.reset();
                    next.true_peak_dbtp.reset(); next.loudness_range_lu.reset();
                } else if (done) next.state = LoudnessReport::State::kStopped;
                std::lock_guard lock(report_mutex_);
                report_ = std::move(next);
                published = now;
            }
            if (done) break;
            if (!count) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    } catch (...) {
        fault_.store(2, std::memory_order_release);
        std::lock_guard lock(report_mutex_);
        report_.state = LoudnessReport::State::kError;
        report_.error = "Loudness worker failure";
    }
}

void LoudnessCapture::stop() noexcept {
    deactivate();
    while (writers_.load(std::memory_order_seq_cst)) std::this_thread::yield();
    exiting_.store(true, std::memory_order_release);
    if (worker_.joinable()) worker_.join();
    resources_.reset();
}
LoudnessReport LoudnessCapture::report() const {
    std::lock_guard lock(report_mutex_);
    return report_;
}

}  // namespace rmx
