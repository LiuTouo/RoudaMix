// vst3_host 單測:壞路徑 scan 必敗;本機有任一 *.vst3 就 load + init + process + 設參數。
// 沒裝 plugin 的環境印 SKIP(不自動失敗)。
#include "vst3_host.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>

namespace {

int failures = 0;
#define CHECK(cond, msg)                             \
    do {                                             \
        if (!(cond)) {                               \
            std::printf("FAIL: %s\n", msg);          \
            ++failures;                              \
        }                                            \
    } while (0)

std::filesystem::path find_any_vst3() {
    for (const char* dir : {"C:\\Program Files\\Common Files\\VST3", "C:\\Program Files\\VST3"}) {
        std::error_code ec;
        const std::filesystem::path root{dir};
        if (!std::filesystem::exists(root, ec)) continue;
        for (std::filesystem::recursive_directory_iterator it{
                 root, std::filesystem::directory_options::skip_permission_denied, ec},
             end;
             it != end; it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            std::error_code file_ec;
            if (it->is_regular_file(file_ec) && it->path().extension() == ".vst3")
                return it->path();
        }
    }
    return {};
}

}  // namespace

int main() {
    {  // 1. 壞路徑:scan 失敗且帶 error
        std::string err;
        const auto classes = rmx::scan_vst3_module("Z:\\definitely\\not\\here.vst3", err);
        CHECK(classes.empty(), "scan of missing path should return no classes");
        CHECK(!err.empty(), "scan of missing path should set error");
    }

    const auto plugin_path = find_any_vst3();
    if (plugin_path.empty()) {
        std::printf("SKIP: no VST3 plugin installed\n");
        return failures == 0 ? 0 : 1;
    }
    std::printf("using plugin: %s\n", plugin_path.string().c_str());

    // 2. scan 列出 class
    std::string err;
    const auto classes = rmx::scan_vst3_module(plugin_path, err);
    CHECK(!classes.empty(), "scan should find an audio-effect class");
    if (classes.empty()) {
        std::printf("scan error: %s\n", err.c_str());
        return 1;
    }
    std::printf("class: %s (%s %s)\n", classes[0].name.c_str(), classes[0].vendor.c_str(),
                classes[0].version.c_str());

    // 3. load + init + process:440Hz sine 進 8 block,輸出必 finite、有能量
    rmx::Vst3Plugin plugin{plugin_path, classes[0].uid};
    CHECK(plugin.loaded(), "plugin should load");
    if (!plugin.loaded()) {
        std::printf("load error: %s\n", plugin.last_error().c_str());
        return 1;
    }
    const bool initialized = plugin.initialize(48000.0, 256);
    CHECK(initialized, "plugin should initialize at 48k/256");
    if (!initialized) {
        std::printf("init error: %s\n", plugin.last_error().c_str());
        return 1;
    }
    std::printf("params=%zu latency=%u\n", plugin.params().size(), plugin.latency_samples());

    const rmx::Vst3ParamInfo* wiggle = nullptr;
    for (const auto& info : plugin.params())
        if (!info.is_bypass) {
            wiggle = &info;
            break;
        }

    constexpr int kFrames = 256;
    float in_l[kFrames], in_r[kFrames], out_l[kFrames], out_r[kFrames];
    double phase = 0.0;
    bool all_processed = true;
    bool all_finite = true;
    double peak = 0.0;
    for (int block = 0; block < 8; ++block) {
        for (int i = 0; i < kFrames; ++i) {
            in_l[i] = in_r[i] = 0.25f * static_cast<float>(std::sin(phase));
            phase += 2.0 * 3.14159265358979323846 * 440.0 / 48000.0;
        }
        rmx::Vst3ParamEdit edits[1];
        size_t edit_count = 0;
        if (wiggle != nullptr && block < 2) {
            edits[0] = {wiggle->id, 0.5};
            edit_count = 1;
        }
        all_processed &= plugin.process(in_l, in_r, out_l, out_r, kFrames, edits, edit_count);
        for (int i = 0; i < kFrames; ++i) {
            all_finite &= std::isfinite(out_l[i]) && std::isfinite(out_r[i]);
            peak = std::max(peak, std::max(std::abs(static_cast<double>(out_l[i])),
                                           std::abs(static_cast<double>(out_r[i]))));
        }
    }
    CHECK(all_processed, "process should succeed on every block");
    CHECK(all_finite, "output must be finite everywhere");
    CHECK(peak > 0.0, "output should carry some energy");

    if (wiggle != nullptr)
        std::printf("param %u (%s) -> %f\n", wiggle->id, wiggle->title.c_str(),
                    plugin.param_value(wiggle->id));

    if (failures == 0) std::printf("vst3_host_test PASSED (peak=%.6f)\n", peak);
    return failures == 0 ? 0 : 1;
}
