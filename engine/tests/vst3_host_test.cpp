// vst3_host 單測:壞路徑 scan 必敗;本機有任一 *.vst3 就 load + init + process + 設參數。
// 沒裝 plugin 的環境印 SKIP(不自動失敗)。
#include "vst3_host.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

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

std::filesystem::path find_installed_vst3(const char* filename) {
    const auto path = std::filesystem::path{"C:\\Program Files\\Common Files\\VST3"} / filename;
    std::error_code ec;
    return std::filesystem::exists(path, ec) ? path : std::filesystem::path{};
}

// 最小合法 .vst3 容器:48B header('VST3'+v1+uid32+list_off)+ Comp chunk + chunk list
std::vector<std::uint8_t> make_preset(const std::string& uid32,
                                      const std::vector<std::uint8_t>& comp) {
    std::vector<std::uint8_t> out;
    const auto push32 = [&out](std::uint32_t x) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(&x);
        out.insert(out.end(), p, p + 4);
    };
    const auto push64 = [&out](std::uint64_t x) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(&x);
        out.insert(out.end(), p, p + 8);
    };
    push32(0x33545356);  // 'VST3'
    push32(1);           // version
    char uid[32]{};
    std::memcpy(uid, uid32.data(), std::min<size_t>(32, uid32.size()));
    out.insert(out.end(), uid, uid + 32);
    push64(48 + comp.size());  // chunk list offset(= Comp chunk 結尾)
    out.insert(out.end(), comp.begin(), comp.end());
    push32(0x7473694C);  // 'List'
    push32(1);           // count
    push32(0x706D6F43);  // 'Comp'
    push64(48);          // offset
    push64(comp.size());
    return out;
}

}  // namespace

int main() {
    {  // 1. 壞路徑:scan 失敗且帶 error
        std::string err;
        const auto classes = rmx::scan_vst3_module("Z:\\definitely\\not\\here.vst3", err);
        CHECK(classes.empty(), "scan of missing path should return no classes");
        CHECK(!err.empty(), "scan of missing path should set error");
    }

    {  // 1b. #14 validate_preset_file:pure 檔案驗證(不需 plugin)——header/class/
        //     chunk 邊界;整檔 byte cap 在動 live plugin(先 bypass)之前擋下
        const auto uid = std::string(32, 'A');  // normalize_uid:32 hex 大寫
        const std::vector<std::uint8_t> comp{'C', 'S', 'T'};
        const auto dir = std::filesystem::temp_directory_path() / "rmx-vst3-host-test";
        std::filesystem::create_directories(dir);
        const auto write = [](const std::filesystem::path& p,
                              const std::vector<std::uint8_t>& bytes) {
            std::FILE* f = nullptr;
            if (_wfopen_s(&f, p.c_str(), L"wb") != 0 || f == nullptr) return;
            std::fwrite(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
        };

        // 合法容器 → 過
        const auto ok = dir / "ok.vstpreset";
        write(ok, make_preset(uid, comp));
        std::string v_err;
        CHECK(rmx::validate_preset_file(ok, uid, v_err), "valid preset should pass validation");

        // class ID 不符 → 拒
        const auto wrong_uid = dir / "wrong-uid.vstpreset";
        write(wrong_uid, make_preset(std::string(31, 'B') + "C", comp));
        CHECK(!rmx::validate_preset_file(wrong_uid, uid, v_err),
              "preset with mismatched class id should be rejected");

        // chunk list offset 超界 → 拒
        {
            auto bad = make_preset(uid, comp);
            const std::uint64_t bogus = 0xFFFFFFFFFFFFFF00ull;
            std::memcpy(bad.data() + 40, &bogus, 8);
            const auto bad_list = dir / "bad-list.vstpreset";
            write(bad_list, bad);
            CHECK(!rmx::validate_preset_file(bad_list, uid, v_err),
                  "out-of-range chunk list should be rejected");
        }

        // 非 preset 檔(隨機短檔)→ 拒
        const auto garbage = dir / "garbage.vstpreset";
        write(garbage, {'x', 'y', 'z'});
        CHECK(!rmx::validate_preset_file(garbage, uid, v_err),
              "non-preset file should be rejected");

        // 整檔 byte cap:超上限的大檔在讀入階段就拒(不整檔吃進記憶體)
        {
            const auto huge = dir / "huge.vstpreset";
            std::FILE* f = nullptr;
            if (_wfopen_s(&f, huge.c_str(), L"wb") == 0 && f != nullptr) {
                const std::vector<std::uint8_t> zero(1 << 20, 0);
                for (int i = 0; i < 65; ++i)  // 65MiB > cap
                    std::fwrite(zero.data(), 1, zero.size(), f);
                std::fclose(f);
            }
            CHECK(!rmx::validate_preset_file(huge, uid, v_err),
                  "oversized preset should be rejected by byte cap");
            std::filesystem::remove(huge);
        }
        std::filesystem::remove_all(dir);
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

    // Monitor shadow transaction 使用記憶體 state，不得經過暫存檔。
    rmx::Vst3RuntimeState runtime_state;
    std::string runtime_state_error;
    const bool runtime_state_captured =
        plugin.capture_runtime_state(runtime_state, runtime_state_error);
    if (runtime_state_captured && !runtime_state.component.empty())
        CHECK(plugin.restore_runtime_state(runtime_state, runtime_state_error),
              "captured runtime state should restore to the same instance");
    else
        std::printf("runtime state SKIP for this plugin: %s\n",
                    runtime_state_error.c_str());

    if (wiggle != nullptr)
        std::printf("param %u (%s) -> %f\n", wiggle->id, wiggle->title.c_str(),
                    plugin.param_value(wiggle->id));

    // 4. Mono main-bus regression:THE VOID ULTRA 可在 Studio Pro 使用，但 preferred
    // arrangement 是 mono。安裝時必須 initialize + process，未安裝環境跳過。
    const auto mono_path = find_installed_vst3("THE VOID ULTRA.vst3");
    if (!mono_path.empty()) {
        std::string mono_err;
        const auto mono_classes = rmx::scan_vst3_module(mono_path, mono_err);
        CHECK(!mono_classes.empty(), "mono plugin scan should find a class");
        if (!mono_classes.empty()) {
            rmx::Vst3Plugin mono{mono_path, mono_classes[0].uid};
            CHECK(mono.loaded(), "mono plugin should load");
            const bool mono_initialized = mono.initialize(48000.0, 256);
            CHECK(mono_initialized, "mono plugin should initialize");
            if (mono.loaded() && mono_initialized) {
                float mono_in_l[256]{}, mono_in_r[256]{}, mono_out_l[256]{}, mono_out_r[256]{};
                mono_in_l[0] = 1.0F;
                mono_in_r[0] = 0.5F;
                CHECK(mono.process(mono_in_l, mono_in_r, mono_out_l, mono_out_r, 256,
                                   nullptr, 0),
                      "mono plugin should process stereo host buffers");
                for (int i = 0; i < 256; ++i)
                    CHECK(std::isfinite(mono_out_l[i]) && mono_out_l[i] == mono_out_r[i],
                          "mono plugin output should be finite and duplicated to L/R");
            }
        }
    }

    if (failures == 0) std::printf("vst3_host_test PASSED (peak=%.6f)\n", peak);
    return failures == 0 ? 0 : 1;
}
