#include "loudness.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
constexpr double pi = 3.14159265358979323846;
static std::vector<float> tone(unsigned rate, double seconds, double frequency = 1000,
                               double left = .1, double right = .1, double phase = 0) {
    std::vector<float> data(static_cast<std::size_t>(rate * seconds) * 2);
    for (std::size_t i = 0; i < data.size() / 2; ++i) {
        const double sample = std::sin(2 * pi * frequency * i / rate + phase);
        data[i * 2] = static_cast<float>(sample * left);
        data[i * 2 + 1] = static_cast<float>(sample * right);
    }
    return data;
}
static rmx::LoudnessReport measure(unsigned rate, const std::vector<float>& data, std::size_t block) {
    rmx::LoudnessMeter meter(rate);
    for (std::size_t pos = 0; pos < data.size() / 2; pos += block)
        CHECK(meter.add(data.data() + pos * 2, (std::min)(block, data.size() / 2 - pos)));
    return meter.report();
}
static void near(std::optional<double> a, std::optional<double> b, double tolerance = .001) {
    CHECK(a && b);
    if (std::abs(*a - *b) > tolerance) std::fprintf(stderr, "values: %.8f %.8f\n", *a, *b);
    CHECK(std::abs(*a - *b) <= tolerance);
}
int main(int argc, char** argv) {
    // Reference runner: interleaved float32 PCM, no container decoding in the meter.
    if (argc == 3) {
        std::ifstream input(argv[2], std::ios::binary);
        CHECK(input.good());
        rmx::LoudnessMeter meter(static_cast<unsigned>(std::stoul(argv[1])));
        float block[8192];
        while (input.read(reinterpret_cast<char*>(block), sizeof(block)) || input.gcount()) {
            CHECK(input.gcount() % (sizeof(float) * 2) == 0);
            CHECK(meter.add(block, static_cast<std::size_t>(input.gcount()) / (sizeof(float) * 2)));
        }
        std::puts(rmx::loudness_report_json(meter.report()).dump().c_str());
        return 0;
    }
    for (unsigned rate : {44100u, 48000u, 96000u, 192000u}) {
        const auto data = tone(rate, 4);
        const auto whole = measure(rate, data, data.size() / 2);
        const auto split = measure(rate, data, 137);
        near(whole.momentary_lufs, split.momentary_lufs);
        near(whole.short_term_lufs, split.short_term_lufs);
        near(whole.integrated_lufs, split.integrated_lufs);
        near(whole.true_peak_dbtp, split.true_peak_dbtp);
        near(whole.loudness_range_lu, split.loudness_range_lu);
        near(whole.integrated_lufs, -20.0, .12);
        near(whole.true_peak_dbtp, -20.0, .3);
        near(whole.loudness_range_lu, 0.0, .1);
        const auto silent = measure(rate, std::vector<float>(rate * 8), 1024);
        CHECK(!silent.momentary_lufs && !silent.short_term_lufs && !silent.integrated_lufs);
        CHECK(!silent.true_peak_dbtp && !silent.loudness_range_lu);
        CHECK(silent.elapsed_seconds == 4);
    }
    const auto stereo = measure(48000, tone(48000, 4), 512);
    const auto one_channel = measure(48000, tone(48000, 4, 1000, .1, 0), 512);
    near(one_channel.integrated_lufs, *stereo.integrated_lufs - 3.0103, .1);
    const auto asymmetric = measure(48000, tone(48000, 4, 1000, .1, .05), 512);
    near(asymmetric.integrated_lufs, *stereo.integrated_lufs + 10 * std::log10(1.25 / 2), .1);

    rmx::LoudnessMeter warm(48000);
    auto data = tone(48000, 4);
    CHECK(warm.add(data.data(), 19199));
    CHECK(!warm.report().momentary_lufs);
    CHECK(warm.add(data.data() + 19199 * 2, 1));
    CHECK(warm.report().momentary_lufs && !warm.report().short_term_lufs);
    CHECK(warm.add(data.data() + 19200 * 2, 124799));
    CHECK(!warm.report().short_term_lufs && !warm.report().loudness_range_lu);
    CHECK(warm.add(data.data() + 143999 * 2, 1));
    CHECK(warm.report().short_term_lufs && warm.report().loudness_range_lu);
    rmx::LoudnessMeter fresh(48000);
    const auto empty = fresh.report();
    CHECK(empty.elapsed_seconds == 0 && !empty.true_peak_dbtp && !empty.integrated_lufs);
    CHECK(!empty.momentary_lufs && !empty.short_term_lufs && !empty.loudness_range_lu);

    auto steps = tone(48000, 10, 1000, .02, .02);
    auto louder = tone(48000, 10, 1000, .2, .2);
    steps.insert(steps.end(), louder.begin(), louder.end());
    auto step = measure(48000, steps, 791);
    CHECK(step.loudness_range_lu && *step.loudness_range_lu > 15);
    near(step.momentary_lufs, -13.98, .15);
    near(step.short_term_lufs, step.momentary_lufs, .05);
    const auto low = measure(48000, tone(48000, 4, 100), 512);
    const auto high = measure(48000, tone(48000, 4, 10000), 512);
    CHECK(*high.integrated_lufs > *low.integrated_lufs + 3);
    auto frequency_step = tone(48000, 4, 100);
    auto high_data = tone(48000, 4, 10000);
    frequency_step.insert(frequency_step.end(), high_data.begin(), high_data.end());
    near(measure(48000, frequency_step, 511).momentary_lufs, high.momentary_lufs, .01);

    const auto intersample = tone(48000, 1, 12000, .9, .9, pi / 4);
    const auto tp = measure(48000, intersample, 127);
    const double sample_peak = 20 * std::log10(.9 / std::sqrt(2.0));
    CHECK(tp.true_peak_dbtp && *tp.true_peak_dbtp > sample_peak + 2);

    rmx::LoudnessMeter maximum_peak(48000);
    const auto over = tone(48000, 1, 12000, 1.1, .1, pi / 4);
    CHECK(std::all_of(over.begin(), over.end(), [](float value) { return std::abs(value) < 1; }));
    CHECK(maximum_peak.add(over.data(), over.size() / 2));
    const auto maximum = maximum_peak.report().true_peak_dbtp;
    CHECK(maximum && *maximum > 0); // Inter-sample clipping despite every PCM sample being below 0 dBFS.
    const auto quiet = tone(48000, 4, 1000, .01, .01);
    CHECK(maximum_peak.add(quiet.data(), quiet.size() / 2));
    near(maximum_peak.report().true_peak_dbtp, maximum);

    for (float bad : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        rmx::LoudnessCapture capture("bad", 1, 48000, 32);
        float values[32]{}; values[10] = bad;
        capture.capture(values, values, 32);
        capture.stop();
        CHECK(capture.report().state == rmx::LoudnessReport::State::kError);
    }
    {
        // One write larger than the >=2s FIFO guarantees a short write regardless of scheduling.
        rmx::LoudnessCapture capture("overflow", 1, 48000, 262144);
        std::vector<float> large(262144);
        capture.capture(large.data(), large.data(), large.size());
        capture.stop();
        CHECK(capture.report().state == rmx::LoudnessReport::State::kError);
        CHECK(!capture.report().integrated_lufs);
    }
    std::vector<std::shared_ptr<rmx::LoudnessCapture>> retired;
    for (int i = 0; i < 80; ++i) {
        auto capture = std::make_shared<rmx::LoudnessCapture>(std::to_string(i), 1, 192000, 8192);
        capture->stop();
        capture->capture(nullptr, nullptr, 8192); // Old graph callback must not touch released resources.
        CHECK(capture->report().state == rmx::LoudnessReport::State::kStopped);
        retired.push_back(std::move(capture));
    }
    std::puts("loudness: rates, stereo, windows, steps, block splits, intersample peak, faults and retired shells passed");
}
