// VST3 host 縮編(自 ProMixArea vst3_adapter):module 載入、stereo main bus、
// float process、參數枚舉/設定。無 editor、無 aux bus、無 state(M3+ 再說)。
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
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

    // ---- preset(.vstpreset 檔案式 state 存取;控制面呼叫)----
    // 寫 VST3 容器:'VST3' header + 'Comp'(component state)+ 'Cntc'(controller
    // state,controller getState OK 時)+ 'RmxP'(host 權威表,私有 chunk,其他
    // host 會略過)。成功回 true;err 帶原因
    bool save_preset(const std::filesystem::path& file,
                     const std::vector<std::pair<std::uint32_t, double>>& host_params,
                     std::string& error);
    // 讀容器套用(component setState → controller setComponentState)。
    // class ID 不符或缺 Comp chunk 回 false。host_params_inout:檔案帶 RmxP chunk
    // 時就地更新(save 時的 host 權威值,優先於一切);host_values_from_file =
    // 已從檔案更新,呼叫端不要再拿 controller 值覆寫(淺實作 plugin 的 controller
    // setComponentState 回 OK 但值不動,不可信)
    bool load_preset(const std::filesystem::path& file,
                     std::vector<std::pair<std::uint32_t, double>>& host_params_inout,
                     std::string& error, bool& host_values_from_file);

    // ---- editor(plugin 自帶 GUI;top-level 視窗住 engine process)----
    // performEdit(editor 內改參數)→ cb(paramId, normalized);open_editor 前設定
    void set_param_callback(std::function<void(uint32_t, double)> cb) noexcept;
    // 建 editor 視窗 + attach;無 view 或 attach 失敗回 false(err 帶原因)
    bool open_editor();
    // 冪等;使用者按視窗 X 或此呼叫同效。remove/析構前必呼
    void close_editor() noexcept;
    bool editor_open() const noexcept;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace rmx
