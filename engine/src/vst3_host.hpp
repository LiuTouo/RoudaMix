// VST3 host 縮編(自 ProMixArea vst3_adapter):module 載入、stereo main bus、
// float process、參數枚舉/設定。無 editor、無 aux bus、無 state(M3+ 再說)。
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rmx {

struct Vst3ClassInfo {
    std::string uid;    // class ID hex
    std::string name;
    std::string vendor;
    std::string version;
    std::string subcategories;
};

// 列 module 內 Audio Effect classes(只載 factory,不 instantiate processor)
std::vector<Vst3ClassInfo> scan_vst3_module(const std::filesystem::path& module_path,
                                            std::string& error);

struct Vst3ModuleScan {
    std::filesystem::path path;
    std::vector<Vst3ClassInfo> classes;
};

// 掃多個根目錄(遞迴 *.vst3);載入失敗的 module 略過不整批 fail
std::vector<Vst3ModuleScan> scan_vst3_dirs(const std::vector<std::filesystem::path>& roots);

struct Vst3ParamInfo {
    uint32_t id{};
    std::string title;
    double default_normalized{};
    bool is_bypass{};
};

// 每 block 一次性參數事件(normalized 0..1,樣本 offset 0)
struct Vst3ParamEdit {
    uint32_t id{};
    double value{};
};

// 單一 plugin instance。載入/初始化/參數在控制面;process 只在 audio thread。
class Vst3Plugin {
public:
    Vst3Plugin(std::filesystem::path module_path, std::string class_id);
    ~Vst3Plugin();
    Vst3Plugin(const Vst3Plugin&) = delete;
    Vst3Plugin& operator=(const Vst3Plugin&) = delete;

    bool loaded() const noexcept;
    const std::string& name() const noexcept;      // class name
    const std::string& class_uid() const noexcept;  // 實際 instantiate 的 class ID
    const std::string& last_error() const noexcept;
    const std::vector<Vst3ParamInfo>& params() const noexcept;
    uint32_t latency_samples() const noexcept;

    bool initialize(double sample_rate, int32_t max_frames) noexcept;
    void terminate() noexcept;

    // in 為立體聲;out 可與 in 同 buffer(呼叫端保證非 alias 或允許就地)
    bool process(const float* in_l, const float* in_r, float* out_l, float* out_r,
                 int32_t frames, const Vst3ParamEdit* edits, size_t edit_count) noexcept;

    // 控制面讀 normalized 值;無此參數或無 controller 回 NaN
    double param_value(uint32_t id) const noexcept;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace rmx
