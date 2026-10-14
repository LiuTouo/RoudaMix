#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include <ebur128.h>

namespace rmx {

struct LoudnessReport {
    std::string measurement_id;
    std::uint32_t track_id{};
    enum class State { kMeasuring, kStopped, kError } state{State::kMeasuring};
    const char* error{};
    double elapsed_seconds{};
    std::optional<double> momentary_lufs, short_term_lufs, integrated_lufs;
    std::optional<double> true_peak_dbtp, loudness_range_lu;
};
nlohmann::json loudness_report_json(const LoudnessReport& report);

// Off-RT, bounded histogram meter. Splits at upstream's LRA gate boundaries.
class LoudnessMeter final {
public:
    explicit LoudnessMeter(std::uint32_t sample_rate);
    ~LoudnessMeter();
    LoudnessMeter(const LoudnessMeter&) = delete;
    LoudnessMeter& operator=(const LoudnessMeter&) = delete;
    bool add(const float* stereo, std::size_t frames);
    LoudnessReport report() const;
private:
    ebur128_state* meter_{};
    std::uint32_t rate_;
    std::uint64_t frames_{};
    std::uint64_t next_lra_frame_;
    bool gated_lra_{};
};

// Graphs retain only this small shell after stop; worker/FIFO/DSP are freed off-RT.
class LoudnessCapture final {
public:
    LoudnessCapture(std::string id, std::uint32_t track_id, std::uint32_t sample_rate,
                    std::size_t max_block_frames);
    ~LoudnessCapture();
    void capture(const float* left, const float* right, std::size_t frames) noexcept;
    void deactivate() noexcept { active_.store(false, std::memory_order_seq_cst); }
    void stop() noexcept;
    LoudnessReport report() const;
    const std::string& id() const noexcept { return id_; }
    std::uint32_t track_id() const noexcept { return track_id_; }
private:
    struct Resources;
    void run() noexcept;
    std::string id_;
    std::uint32_t track_id_;
    std::unique_ptr<Resources> resources_;
    std::atomic<bool> active_{true};
    std::atomic<unsigned> writers_{0};
    std::atomic<bool> exiting_{false};
    std::atomic<int> fault_{0};
    mutable std::mutex report_mutex_;
    LoudnessReport report_;
    std::thread worker_;
};

}  // namespace rmx
