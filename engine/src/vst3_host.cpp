// VST3 host 縮編實作。依賴 Steinberg hosting(module/plugprovider/hostclasses/
// parameterchanges/processdata)+ pluginterfaces。ponytail:stereo main bus only,
// aux bus / Float64 不支援；main bus 可 stereo，或以 host downmix/duplicate 適配 mono。
// 需要時再開(ProMixArea vst3_adapter.cpp 有全版)。
// editor:M4a 加,Studio Pro 式重構後 —— view 生命週期在這(createView/attached/
// removed),視窗與 tab 列由 EditorHost 持有(見 editor_host.cpp);param 變更走
// performEdit → host callback(不廣播,set_param 同語意)。
#include "vst3_host.hpp"

#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/ivstplugview.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace rmx {

// UTF-8 → UTF-16(視窗 title / tab 文字用;StringConvert 只給 u16string,自轉)
std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                      nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

namespace {

using namespace Steinberg;
using namespace Steinberg::Vst;

constexpr int32 kMaxParamEventsPerBlock = 64;

// IComponentHandler:editor 內改參數 → performEdit → host callback。
// beginEdit/endEdit no-op(無 automation 錄製);restartComponent no-op(M4+ 再說)。
class EditComponentHandler final : public IComponentHandler {
public:
    std::function<void(ParamID, ParamValue)> on_edit;  // attach_editor 前設定一次
    std::function<void()> on_latency_changed;

    tresult PLUGIN_API queryInterface(const TUID requested_iid, void** obj) override {
        if (obj == nullptr) return kInvalidArgument;
        if (FUnknownPrivate::iidEqual(requested_iid, FUnknown::iid) ||
            FUnknownPrivate::iidEqual(requested_iid, IComponentHandler::iid)) {
            *obj = static_cast<IComponentHandler*>(this);
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++references_; }
    uint32 PLUGIN_API release() override {
        const auto remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }
    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue value) override {
        if (on_edit) on_edit(id, value);
        return kResultOk;
    }
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32 flags) override {
        if ((flags & kLatencyChanged) != 0 && on_latency_changed) on_latency_changed();
        return kResultOk;
    }

private:
    std::atomic<uint32> references_{1};
};

// IPlugFrame:搬到 editor_host.cpp(視窗由 EditorHost 持有)。
// ---- .vstpreset 容器(格式:VST3 Developer Portal「Preset Format」+ SDK
// vstpresetfile.cpp)----
// header 48B:'VST3' + int32 version(=1)+ char classID[32](32 hex ASCII,UID::toString
// 的 COM 形式)+ int64 chunkListOffset;chunk list:'List' + int32 count + per-entry
// { FourCC id, int64 offset, int64 size };chunk 'Comp' = component state、'Cont' =
// controller state(SDK kControllerState = {'C','o','n','t'})。全 little-endian。
constexpr std::int32_t kPresetVersion = 1;
constexpr std::uint32_t kPresetMagic = 0x33545356;    // 'VST3' little-endian
constexpr std::uint32_t kListMagic = 0x7473694C;      // 'List'
constexpr std::uint32_t kChunkComp = 0x706D6F43;      // 'Comp'
constexpr std::uint32_t kChunkCont = 0x746E6F43;      // 'Cont'(SDK 標準)
constexpr std::uint32_t kChunkCntcOld = 0x63746E43;   // 'Cntc'(本專案早期誤寫,讀相容)
constexpr std::uint32_t kChunkRmxP = 0x50786D52;      // 'RmxP'(私有:host 權威表)
constexpr std::size_t kPresetHeaderSize = 48;

void push_u32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&x);
    v.insert(v.end(), p, p + 4);
}
void push_u64(std::vector<std::uint8_t>& v, std::uint64_t x) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&x);
    v.insert(v.end(), p, p + 8);
}
std::uint32_t rd_u32(const std::uint8_t* p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
std::uint64_t rd_u64(const std::uint8_t* p) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}
std::uint64_t f64_bits(double v) noexcept {
    std::uint64_t b;
    std::memcpy(&b, &v, 8);
    return b;
}
double bits_f64(std::uint64_t b) noexcept {
    double v;
    std::memcpy(&v, &b, 8);
    return v;
}

// class ID 比對容錯:去 {}-、空白,收斂大寫;非 32 hex(規範形)回空
std::string normalize_uid(std::string s) {
    std::string out;
    for (const char c : s) {
        if (c == '{' || c == '}' || c == '-' || c == ' ') continue;
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    if (out.size() != 32) return {};
    for (const char c : out) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return {};
    }
    return out;
}

}  // namespace

std::vector<Vst3ClassInfo> scan_vst3_module(const std::filesystem::path& module_path,
                                            std::string& error) {
    std::vector<Vst3ClassInfo> result;
    auto module = VST3::Hosting::Module::create(module_path.string(), error);
    if (!module) return result;
    for (const auto& info : module->getFactory().classInfos()) {
        if (info.category() != kVstAudioEffectClass) continue;
        result.push_back({info.ID().toString(), info.name(), info.vendor(), info.version(),
                          info.subCategoriesString()});
    }
    if (result.empty()) error = "no VST3 audio-effect class in module";
    return result;
}

std::vector<Vst3ModuleScan> scan_vst3_dirs(const std::vector<std::filesystem::path>& roots) {
    std::vector<Vst3ModuleScan> result;
    for (const auto& root : roots) {
        std::error_code ec;
        if (!std::filesystem::exists(root, ec)) continue;
        for (std::filesystem::recursive_directory_iterator it{
                 root, std::filesystem::directory_options::skip_permission_denied, ec},
             end;
             it != end; it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            if (_wcsicmp(it->path().extension().c_str(), L".vst3") != 0) continue;
            std::error_code file_ec;
            const bool regular = it->is_regular_file(file_ec);
            const bool directory = !file_ec && !regular && it->is_directory(file_ec);
            if (file_ec || (!regular && !directory)) continue;
            if (directory) it.disable_recursion_pending();
            std::string error;
            auto classes = scan_vst3_module(it->path(), error);
            if (classes.empty()) continue;  // 壞 module / 非 audio effect:略過
            result.push_back({it->path(), std::move(classes)});
        }
    }
    return result;
}

struct Vst3Plugin::Impl {
    VST3::Hosting::Module::Ptr module;
    IPtr<HostApplication> host;
    IPtr<PlugProvider> provider;
    IPtr<IComponent> component;
    IPtr<IEditController> controller;
    IPtr<IComponentHandler> handler;
    IPtr<IAudioProcessor> processor;
    HostProcessData process_data;
    ProcessContext context{};
    ParameterChanges parameter_changes{kMaxParamEventsPerBlock};
    std::vector<Vst3ParamInfo> params;
    int32_t main_in_bus{-1};
    int32_t main_out_bus{-1};
    bool mono_main{};
    std::vector<float> mono_in;
    std::vector<float> mono_out;
    bool initialized{false};
    bool loaded{false};
    std::string name_;
    std::string class_uid_;
    std::string error;

    // ---- RT:slew 防 zipper(process() 專屬寫,僅 audio thread 觸碰)----
    // from = 同參數上次 slew 終值;首次編輯無快取 = 直接跳(拖動中才連續)。
    // setState(load_preset)會在 control thread 整批改參數 —— slew_gen_++ 讓
    // process() 下一個 block 清快取,不然下次編輯會從 pre-preset 舊值起 ramp
    // (sample 0 跳回舊值 = 正是 slew 要消的 zipper)
    struct SlewEntry {
        uint32_t id{};
        double value{};
        bool active{};
    };
    static constexpr size_t kSlewCacheSize = 64;
    SlewEntry slew_cache_[kSlewCacheSize]{};
    size_t slew_evict_{};
    int32_t slew_frames_{};  // initialize 時依 sample rate 算(15ms 線性)
    std::atomic<uint32_t> slew_gen_{0};   // control thread ++(setState 後)
    uint32_t slew_gen_seen_{};            // RT 專屬
    void invalidate_slew() noexcept { slew_gen_.fetch_add(1, std::memory_order_release); }

    // editor:view 生命週期全在 main thread(dispatch thread,跑 message loop)。
    // JUCE 系 plugin 假設 host 單一 UI thread —— createView/attached/removed 分拆到
    // 別 thread 會跨 thread 互等死鎖(實測:attached 等 condition_variable,
    // main thread 被 plugin wnd_proc 拉進 CreateWindowEx 卡 win32k send)。
    // 視窗本體由 EditorHost 持有(editor_host.cpp),這裡只管 view。
    EditComponentHandler* edit_handler{};  // 生命週期歸 handler(IPtr)持有
    IPtr<IPlugView> view;

    ~Impl() {
        close_editor();
        terminate();
    }

    // 冪等;EditorHost 關窗/切換/remove_plugin 或此呼叫同效。
    void close_editor() noexcept {
        if (view) {
            view->removed();
            view = nullptr;
        }
    }

    void load(const std::filesystem::path& path, const std::string& class_id) {
        module = VST3::Hosting::Module::create(path.string(), error);
        if (!module) return;
        const auto factory = module->getFactory();
        const auto classes = factory.classInfos();
        const auto found = std::find_if(classes.begin(), classes.end(), [&](const auto& info) {
            return info.category() == kVstAudioEffectClass &&
                   (class_id.empty() || info.ID().toString() == class_id);
        });
        if (found == classes.end()) {
            error = "VST3 audio-effect class was not found";
            return;
        }
        name_ = found->name();
        class_uid_ = found->ID().toString();
        host = owned(new HostApplication());
        factory.setHostContext(host);
        PluginContextFactory::instance().setPluginContext(host);
        provider = owned(new PlugProvider(factory, *found, /*useSeparateController=*/true));
        if (!provider->initialize()) {
            error = "VST3 component/controller initialization failed";
            return;
        }
        component = provider->getComponentPtr();
        controller = provider->getControllerPtr();
        if (!component) {
            error = "VST3 component is unavailable";
            return;
        }
        IAudioProcessor* proc{};
        component->queryInterface(IAudioProcessor::iid, reinterpret_cast<void**>(&proc));
        processor = owned(proc);
        if (!processor) {
            error = "VST3 IAudioProcessor is unavailable";
            return;
        }
        if (controller) {
            edit_handler = new EditComponentHandler();
            handler = owned(edit_handler);
            if (controller->setComponentHandler(handler) != kResultOk) {
                error = "VST3 controller rejected IComponentHandler";
                return;
            }
            enumerate_params();
        }
        loaded = true;
    }

    // ---- editor(全在 main thread;main.cpp 的 message loop 服務訊息)----

    // attach 到 EditorHost 提供的 parent(視窗/前景/尺寸都歸 host 管)。
    // caller 保證 main thread;成功後 out_w/out_h = clamp 後原生尺寸
    bool attach_editor(HWND parent, IPlugFrame* plug_frame, int& out_w, int& out_h) {
        if (!loaded || !controller) {
            error = "plugin has no controller for an editor";
            return false;
        }
        error.clear();
        IPlugView* raw = controller->createView(ViewType::kEditor);
        if (raw == nullptr) {
            error = "plugin has no editor view";
            return false;
        }
        view = owned(raw);
        ViewRect vr{};
        int w = 300, h = 200;  // getSize 失敗時的 fallback
        if (view->getSize(&vr) == kResultOk) {
            const int rw = vr.right - vr.left;
            const int rh = vr.bottom - vr.top;
            if (rw > 0 && rh > 0) {
                w = std::clamp(rw, 80, 4096);
                h = std::clamp(rh, 60, 4096);
            }
        }
        out_w = w;
        out_h = h;
        view->setFrame(plug_frame);
        if (view->attached(parent, kPlatformTypeHWND) != kResultOk) {
            // 從未 attached:removed 不該呼叫
            view->setFrame(nullptr);
            view = nullptr;
            error = "plugin editor attach failed";
            return false;
        }
        return true;
    }

    void editor_resize_view(int w, int h) noexcept {
        if (!view || w <= 0 || h <= 0) return;
        ViewRect r{0, 0, w, h};
        view->onSize(&r);
    }

    void enumerate_params() {
        const auto count = std::clamp<int32>(controller->getParameterCount(), 0, 4096);
        params.reserve(static_cast<size_t>(count));
        for (int32 i = 0; i < count; ++i) {
            ParameterInfo info{};
            if (controller->getParameterInfo(i, info) != kResultOk) continue;
            params.push_back({info.id, StringConvert::convert(info.title, 128),
                              info.defaultNormalizedValue,
                              (info.flags & ParameterInfo::kIsBypass) != 0});
        }
    }

    bool find_main_buses() {
        main_in_bus = main_out_bus = -1;
        for (int32 dir : {kInput, kOutput}) {
            const int32 count = component->getBusCount(kAudio, dir);
            for (int32 i = 0; i < count; ++i) {
                BusInfo info{};
                if (component->getBusInfo(kAudio, dir, i, info) == kResultOk &&
                    info.busType == kMain) {
                    if (dir == kInput) main_in_bus = i; else main_out_bus = i;
                    break;
                }
            }
        }
        if (main_in_bus < 0 || main_out_bus < 0) {
            error = "plugin has no main audio bus";
            return false;
        }
        return true;
    }

    bool negotiate_stereo_main() {
        if (!find_main_buses()) return false;
        const int32 in_count = component->getBusCount(kAudio, kInput);
        const int32 out_count = component->getBusCount(kAudio, kOutput);
        std::vector<SpeakerArrangement> ins(std::max<int32>(in_count, 1), SpeakerArr::kEmpty);
        std::vector<SpeakerArrangement> outs(std::max<int32>(out_count, 1), SpeakerArr::kEmpty);
        for (int32 i = 0; i < in_count; ++i)
            if (processor->getBusArrangement(kInput, i, ins[i]) != kResultOk)
                ins[i] = SpeakerArr::kEmpty;
        for (int32 i = 0; i < out_count; ++i)
            if (processor->getBusArrangement(kOutput, i, outs[i]) != kResultOk)
                outs[i] = SpeakerArr::kEmpty;
        const auto preferred_in = ins[main_in_bus];
        const auto preferred_out = outs[main_out_bus];
        ins[main_in_bus] = SpeakerArr::kStereo;
        outs[main_out_bus] = SpeakerArr::kStereo;
        if (processor->setBusArrangements(ins.data(), in_count, outs.data(), out_count) !=
            kResultOk) {
            // VST3 negotiation:host 提案被拒後，回送 plugin 的 preferred layout。
            // RoudaMix 內部仍是 stereo；mono effect 由 process() 下混/複製適配。
            ins[main_in_bus] = preferred_in;
            outs[main_out_bus] = preferred_out;
            if (SpeakerArr::getChannelCount(preferred_in) != 1 ||
                SpeakerArr::getChannelCount(preferred_out) != 1 ||
                processor->setBusArrangements(ins.data(), in_count, outs.data(), out_count) !=
                    kResultOk) {
                error = "plugin rejected stereo and preferred mono main-bus arrangements";
                return false;
            }
            mono_main = true;
        } else {
            mono_main = false;
        }
        BusInfo in_info{}, out_info{};
        if (component->getBusInfo(kAudio, kInput, main_in_bus, in_info) != kResultOk ||
            component->getBusInfo(kAudio, kOutput, main_out_bus, out_info) != kResultOk ||
            in_info.channelCount != (mono_main ? 1 : 2) ||
            out_info.channelCount != (mono_main ? 1 : 2)) {
            error = "plugin main bus does not match the negotiated mono/stereo layout";
            return false;
        }
        return true;
    }

    bool initialize(double sample_rate, int32_t max_frames) noexcept {
        if (!loaded || initialized || sample_rate <= 0 || max_frames <= 0) {
            error = "invalid VST3 initialization state";
            return false;
        }
        if (processor->canProcessSampleSize(kSample32) != kResultTrue) {
            error = "plugin does not support float32 processing";
            return false;
        }
        if (!negotiate_stereo_main()) return false;
        // 只有 main bus active;aux 一律關(ponytail:無 aux 資料來源)
        for (int32 dir : {kInput, kOutput}) {
            const int32 count = component->getBusCount(kAudio, dir);
            for (int32 i = 0; i < count; ++i)
                component->activateBus(kAudio, dir, i,
                                       dir == kInput ? i == main_in_bus : i == main_out_bus);
        }
        component->setIoMode(kAdvanced);
        if (!process_data.prepare(*component, /*bufferSamples=*/0, kSample32)) {
            error = "VST3 process buffers could not be prepared";
            return false;
        }
        const ProcessSetup setup{kRealtime, kSample32, max_frames, sample_rate};
        ProcessSetup setup_mut = setup;
        if (processor->setupProcessing(setup_mut) != kResultOk) {
            error = "VST3 setupProcessing was rejected";
            process_data.unprepare();
            return false;
        }
        if (component->setActive(true) != kResultOk) {
            error = "VST3 setActive was rejected";
            process_data.unprepare();
            return false;
        }
        if (mono_main) {
            try {
                mono_in.assign(static_cast<std::size_t>(max_frames), 0.0F);
                mono_out.assign(static_cast<std::size_t>(max_frames), 0.0F);
            } catch (...) {
                error = "mono adapter buffers could not be allocated";
                component->setActive(false);
                process_data.unprepare();
                return false;
            }
        } else {
            mono_in.clear();
            mono_out.clear();
        }
        slew_frames_ = static_cast<int32_t>(sample_rate * 0.015);  // 15ms 防 zipper
        // 部分 plugin 回 kNotImplemented 仍正常進 processing 狀態(ProMixArea 同款放行)
        processor->setProcessing(true);
        context = {};
        context.sampleRate = sample_rate;
        process_data.processContext = &context;
        process_data.inputParameterChanges = &parameter_changes;
        initialized = true;
        return true;
    }

    void terminate() noexcept {
        if (initialized) {
            processor->setProcessing(false);
            component->setActive(false);
            process_data.unprepare();
            initialized = false;
        }
    }

    // ---- preset:容器讀寫(控制面)----

    bool capture_runtime_state(Vst3RuntimeState& state, std::string& err) {
        state = {};
        if (!loaded || !component) {
            err = "plugin not loaded";
            return false;
        }
        MemoryStream component_stream;
        if (component->getState(&component_stream) != kResultOk) {
            err = "plugin component getState failed";
            return false;
        }
        const auto component_size = component_stream.getSize();
        if (component_size < 0 ||
            static_cast<std::uint64_t>(component_size) >
                static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
            err = "plugin component state is too large";
            return false;
        }
        const auto* component_data =
            reinterpret_cast<const std::uint8_t*>(component_stream.getData());
        if (component_size > 0 && component_data == nullptr) {
            err = "plugin component returned invalid state";
            return false;
        }
        if (component_size > 0)
            state.component.assign(component_data, component_data + component_size);

        if (controller) {
            MemoryStream controller_stream;
            // IEditController::getState 可回 kNotImplemented；component state
            // 仍是 DSP 權威，controller view 另外由 setComponentState + host
            // authoritative params 重建，與既有 .vstpreset 相容策略一致。
            if (controller->getState(&controller_stream) != kResultOk) return true;
            const auto controller_size = controller_stream.getSize();
            if (controller_size < 0 ||
                static_cast<std::uint64_t>(controller_size) >
                    static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
                err = "plugin controller state is too large";
                state = {};
                return false;
            }
            const auto* controller_data =
                reinterpret_cast<const std::uint8_t*>(controller_stream.getData());
            if (controller_size > 0 && controller_data == nullptr) {
                err = "plugin controller returned invalid state";
                state = {};
                return false;
            }
            if (controller_size > 0)
                state.controller.assign(controller_data, controller_data + controller_size);
        }
        return true;
    }

    bool restore_runtime_state(const Vst3RuntimeState& state, std::string& err) {
        if (!loaded || !component) {
            err = "plugin not loaded";
            return false;
        }
        if (state.component.empty() ||
            state.component.size() > static_cast<std::size_t>((std::numeric_limits<int32>::max)()) ||
            state.controller.size() > static_cast<std::size_t>((std::numeric_limits<int32>::max)())) {
            err = "runtime component state is empty or too large";
            return false;
        }
        MemoryStream component_stream(const_cast<std::uint8_t*>(state.component.data()),
                                      static_cast<int32>(state.component.size()));
        if (component->setState(&component_stream) != kResultOk) {
            err = "plugin rejected runtime component state";
            return false;
        }
        invalidate_slew();
        if (controller) {
            MemoryStream component_again(const_cast<std::uint8_t*>(state.component.data()),
                                         static_cast<int32>(state.component.size()));
            const bool controller_synced =
                controller->setComponentState(&component_again) == kResultOk;
            // 部分可正常處理的 plugin 不實作 controller state 同步；DSP
            // component 已完整恢復，controller 由 host params 重放即可。
            if (controller_synced && !state.controller.empty()) {
                MemoryStream controller_stream(
                    const_cast<std::uint8_t*>(state.controller.data()),
                    static_cast<int32>(state.controller.size()));
                if (controller->setState(&controller_stream) != kResultOk) {
                    err = "plugin controller rejected runtime controller state";
                    return false;
                }
            }
        }
        return true;
    }

    bool save_preset(const std::filesystem::path& file,
                     const std::vector<std::pair<std::uint32_t, double>>& host_params,
                     std::string& err) {
        if (!loaded || !component) {
            err = "plugin not loaded";
            return false;
        }
        MemoryStream comp_stream;
        if (component->getState(&comp_stream) != kResultOk) {
            err = "plugin component getState failed";
            return false;
        }
        MemoryStream ctrl_stream;
        bool have_ctrl = false;
        if (controller) have_ctrl = controller->getState(&ctrl_stream) == kResultOk;

        // RmxP:host 權威表(u32 count + {u32 id, f64 value}*),其他 host 略過
        std::vector<std::uint8_t> rmxp;
        if (!host_params.empty()) {
            push_u32(rmxp, static_cast<std::uint32_t>(host_params.size()));
            for (const auto& [id, v] : host_params) {
                push_u32(rmxp, id);
                push_u64(rmxp, f64_bits(v));
            }
        }

        const auto comp_data =
            reinterpret_cast<const std::uint8_t*>(comp_stream.getData());
        const auto ctrl_data =
            reinterpret_cast<const std::uint8_t*>(ctrl_stream.getData());
        const auto comp_size = static_cast<std::uint64_t>(comp_stream.getSize());
        const auto ctrl_size = static_cast<std::uint64_t>(have_ctrl ? ctrl_stream.getSize() : 0);
        const auto rmxp_size = static_cast<std::uint64_t>(rmxp.size());

        const std::uint64_t comp_off = kPresetHeaderSize;
        const std::uint64_t ctrl_off = comp_off + comp_size;
        const std::uint64_t rmxp_off = ctrl_off + ctrl_size;
        const std::uint64_t list_off = rmxp_off + rmxp_size;
        std::uint32_t chunk_count = 1;
        if (have_ctrl) ++chunk_count;
        if (!rmxp.empty()) ++chunk_count;

        std::vector<std::uint8_t> out;
        out.reserve(static_cast<std::size_t>(list_off) + 8 + 20 * chunk_count);
        push_u32(out, kPresetMagic);
        push_u32(out, static_cast<std::uint32_t>(kPresetVersion));
        char uid[32]{};
        std::memcpy(uid, class_uid_.data(), std::min<size_t>(32, class_uid_.size()));
        out.insert(out.end(), uid, uid + 32);
        push_u64(out, list_off);
        out.insert(out.end(), comp_data, comp_data + comp_size);
        if (have_ctrl) out.insert(out.end(), ctrl_data, ctrl_data + ctrl_size);
        if (!rmxp.empty()) out.insert(out.end(), rmxp.begin(), rmxp.end());
        push_u32(out, kListMagic);
        push_u32(out, chunk_count);
        push_u32(out, kChunkComp);
        push_u64(out, comp_off);
        push_u64(out, comp_size);
        if (have_ctrl) {
            push_u32(out, kChunkCont);
            push_u64(out, ctrl_off);
            push_u64(out, ctrl_size);
        }
        if (!rmxp.empty()) {
            push_u32(out, kChunkRmxP);
            push_u64(out, rmxp_off);
            push_u64(out, rmxp_size);
        }

        std::FILE* f = nullptr;
        if (_wfopen_s(&f, file.c_str(), L"wb") != 0 || f == nullptr) {
            err = "cannot open preset file for writing: " + file.string();
            return false;
        }
        const bool wrote = std::fwrite(out.data(), 1, out.size(), f) == out.size();
        std::fclose(f);
        if (!wrote) {
            err = "preset file write failed: " + file.string();
            return false;
        }
        return true;
    }

    bool load_preset(const std::filesystem::path& file,
                     std::vector<std::pair<std::uint32_t, double>>& host_params_inout,
                     std::string& err, bool& host_values_from_file) {
        host_values_from_file = false;
        if (!loaded || !component) {
            err = "plugin not loaded";
            return false;
        }
        std::FILE* f = nullptr;
        if (_wfopen_s(&f, file.c_str(), L"rb") != 0 || f == nullptr) {
            err = "cannot open preset file: " + file.string();
            return false;
        }
        std::vector<std::uint8_t> data;
        std::uint8_t buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
        std::fclose(f);

        const std::uint64_t total = data.size();
        if (total < kPresetHeaderSize + 8 + 20 || rd_u32(data.data()) != kPresetMagic ||
            rd_u32(data.data() + 4) != static_cast<std::uint32_t>(kPresetVersion)) {
            err = "not a VST3 preset file (bad header): " + file.string();
            return false;
        }
        // class ID 比對:header 32 字元 vs 本 instance(normalize 容錯 {}- 與大小寫)
        const char* raw_uid = reinterpret_cast<const char*>(data.data() + 8);
        const auto uid_len = strnlen(raw_uid, 32);
        std::string header_uid(raw_uid, uid_len);
        const std::string want = normalize_uid(class_uid_);
        const std::string got = normalize_uid(header_uid);
        if (want.empty() || got.empty() || want != got) {
            err = "preset class id mismatch (file targets " +
                  (got.empty() ? "?" : got) + ")";
            return false;
        }

        const std::uint64_t list_off = rd_u64(data.data() + 40);
        // 邊界全用減法形式:offset 為檔案內容(attacker-controlled),加法會繞回
        if (total < kPresetHeaderSize || list_off > total - 8 || total - list_off < 8 ||
            rd_u32(data.data() + list_off) != kListMagic) {
            err = "preset chunk list corrupt: " + file.string();
            return false;
        }
        const std::uint64_t count = rd_u32(data.data() + list_off + 4);
        if (count > 64) {
            err = "preset chunk list too large: " + file.string();
            return false;
        }
        std::uint64_t comp_off = 0, comp_size = 0, ctrl_off = 0, ctrl_size = 0, rmxp_off = 0,
                     rmxp_size = 0;
        bool have_comp = false, have_ctrl = false, have_rmxp = false;
        for (std::uint64_t i = 0; i < count; ++i) {
            const std::uint64_t e = list_off + 8 + i * 20;
            if (e > total || total - e < 20) {
                err = "preset chunk entry out of range: " + file.string();
                return false;
            }
            const std::uint32_t id = rd_u32(data.data() + e);
            const std::uint64_t off = rd_u64(data.data() + e + 4);
            const std::uint64_t size = rd_u64(data.data() + e + 12);
            if (off > total || total - off < size) {
                err = "preset chunk data out of range: " + file.string();
                return false;
            }
            if (id == kChunkComp) {
                comp_off = off;
                comp_size = size;
                have_comp = true;
            } else if (id == kChunkCont || id == kChunkCntcOld) {
                // 'Cont' = SDK 標準;'Cntc' = 本專案早期誤寫的檔,讀取相容
                ctrl_off = off;
                ctrl_size = size;
                have_ctrl = true;
            } else if (id == kChunkRmxP) {
                rmxp_off = off;
                rmxp_size = size;
                have_rmxp = true;
            }
        }
        if (!have_comp || comp_size == 0) {
            err = "preset has no component state chunk: " + file.string();
            return false;
        }

        MemoryStream comp_in(data.data() + comp_off, static_cast<int32>(comp_size));
        if (component->setState(&comp_in) != kResultOk) {
            err = "plugin rejected preset component state";
            return false;
        }
        invalidate_slew();  // 參數被整批改寫:slew 快取的 from 值全部作廢
        bool ctrl_synced = false;
        if (controller) {
            // setComponentState = controller 自 component state 重建參數視圖(VST3
            // 語意;檔案裡的 Cntc 是 controller 自己的佈景狀態,另一回事)。
            // 同步失敗不整體失敗:component 已套用,但 controller 值不可信
            MemoryStream comp_again(data.data() + comp_off, static_cast<int32>(comp_size));
            ctrl_synced = controller->setComponentState(&comp_again) == kResultOk;
        }
        if (have_ctrl && controller && ctrl_synced) {
            // controller 自身狀態(Cntc)最後套,蓋編輯器佈局等,不影響參數值
            MemoryStream ctrl_in(data.data() + ctrl_off, static_cast<int32>(ctrl_size));
            controller->setState(&ctrl_in);
        }

        // host 權威值:RmxP(save 時的 host 表)最準;淺實作 plugin 的 controller
        // setComponentState 回 OK 但 getParamNormalized 不動,不可當 truth
        if (have_rmxp && rmxp_size >= 4) {
            const std::uint64_t n_entries = rd_u32(data.data() + rmxp_off);
            if (n_entries <= (rmxp_size - 4) / 12) {
                for (std::uint64_t i = 0; i < n_entries; ++i) {
                    const std::uint8_t* e = data.data() + rmxp_off + 4 + i * 12;
                    const std::uint32_t id = rd_u32(e);
                    const double v = bits_f64(rd_u64(e + 4));
                    for (auto& [hid, hv] : host_params_inout) {
                        if (hid == id) {
                            hv = v;
                            break;
                        }
                    }
                }
                host_values_from_file = true;
            }
        }
        return true;
    }

    bool process(const float* in_l, const float* in_r, float* out_l, float* out_r,
                 int32_t frames, const Vst3ParamEdit* edits, size_t edit_count) noexcept {
        if (!initialized) return false;
        const uint32_t gen = slew_gen_.load(std::memory_order_acquire);
        if (gen != slew_gen_seen_) {
            slew_gen_seen_ = gen;
            for (auto& e : slew_cache_) e.active = false;
        }
        parameter_changes.clearQueue();
        for (size_t i = 0; i < edit_count; ++i) {
            int32 queue_index{};
            auto* queue = parameter_changes.addParameterData(edits[i].id, queue_index);
            if (queue == nullptr) return false;
            // slew 兩點線性(0 → slew_frames_)消 zipper;from = 上次終值,首次直接跳
            double from = edits[i].value;
            SlewEntry* entry = nullptr;
            for (auto& e : slew_cache_) {
                if (e.active && e.id == edits[i].id) {
                    entry = &e;
                    break;
                }
            }
            if (entry != nullptr) {
                from = entry->value;
            } else {
                for (auto& e : slew_cache_) {
                    if (!e.active) {
                        entry = &e;
                        e.active = true;
                        e.id = edits[i].id;
                        break;
                    }
                }
                if (entry == nullptr) {  // 滿:輪替覆寫(64 個同時 ramp 已遠超旋鈕場景)
                    entry = &slew_cache_[slew_evict_++ % kSlewCacheSize];
                    entry->active = true;
                    entry->id = edits[i].id;
                }
            }
            entry->value = edits[i].value;
            int32 point_index{};
            if (queue->addPoint(0, from, point_index) != kResultTrue) return false;
            const int32 off = std::min<int32_t>(frames > 0 ? frames - 1 : 0, slew_frames_);
            if (edits[i].value != from) {
                // off == 0(1-frame block)無處 ramp:同 offset 覆寫成目標值(直接跳)
                if (queue->addPoint(off > 0 ? off : 0, edits[i].value, point_index) !=
                    kResultTrue)
                    return false;
            }
        }
        process_data.numSamples = frames;
        context.projectTimeSamples += frames;
        context.state = 0;
        auto& in_bus = process_data.inputs[main_in_bus];
        auto& out_bus = process_data.outputs[main_out_bus];
        if (mono_main) {
            if (frames < 0 || static_cast<std::size_t>(frames) > mono_in.size()) return false;
            for (int32_t i = 0; i < frames; ++i)
                mono_in[static_cast<std::size_t>(i)] =
                    0.5F * ((in_l != nullptr ? in_l[i] : 0.0F) +
                            (in_r != nullptr ? in_r[i] : 0.0F));
            in_bus.channelBuffers32[0] = mono_in.data();
            out_bus.channelBuffers32[0] = mono_out.data();
            if (processor->process(process_data) != kResultOk) return false;
            for (int32_t i = 0; i < frames; ++i)
                out_l[i] = out_r[i] = mono_out[static_cast<std::size_t>(i)];
            return true;
        }
        in_bus.channelBuffers32[0] = const_cast<float*>(in_l);
        in_bus.channelBuffers32[1] = const_cast<float*>(in_r);
        out_bus.channelBuffers32[0] = out_l;
        out_bus.channelBuffers32[1] = out_r;
        return processor->process(process_data) == kResultOk;
    }
};

Vst3Plugin::Vst3Plugin(std::filesystem::path module_path, std::string class_id)
    : impl_(new Impl) {
    impl_->load(module_path, class_id);
}

Vst3Plugin::~Vst3Plugin() {
    delete impl_;
}

bool Vst3Plugin::loaded() const noexcept { return impl_ && impl_->loaded; }
const std::string& Vst3Plugin::name() const noexcept { return impl_->name_; }
const std::string& Vst3Plugin::class_uid() const noexcept { return impl_->class_uid_; }
const std::string& Vst3Plugin::last_error() const noexcept { return impl_->error; }
const std::vector<Vst3ParamInfo>& Vst3Plugin::params() const noexcept { return impl_->params; }

uint32_t Vst3Plugin::latency_samples() const noexcept {
    return impl_ && impl_->processor ? static_cast<uint32_t>(impl_->processor->getLatencySamples())
                                     : 0;
}

bool Vst3Plugin::initialize(double sample_rate, int32_t max_frames) noexcept {
    return impl_->initialize(sample_rate, max_frames);
}

void Vst3Plugin::terminate() noexcept { impl_->terminate(); }

bool Vst3Plugin::process(const float* in_l, const float* in_r, float* out_l, float* out_r,
                         int32_t frames, const Vst3ParamEdit* edits,
                         size_t edit_count) noexcept {
    return impl_->process(in_l, in_r, out_l, out_r, frames, edits, edit_count);
}

double Vst3Plugin::param_value(uint32_t id) const noexcept {
    if (!impl_ || !impl_->controller) return std::numeric_limits<double>::quiet_NaN();
    const double v = impl_->controller->getParamNormalized(id);
    return std::isfinite(v) ? v : std::numeric_limits<double>::quiet_NaN();
}

void Vst3Plugin::set_param_normalized(uint32_t id, double value) noexcept {
    // control thread 專屬;preset 載入後同步 controller → editor GUI 顯示跟著動
    if (impl_ && impl_->controller)
        impl_->controller->setParamNormalized(id, std::isfinite(value) ? value : 0.0);
}

bool Vst3Plugin::capture_runtime_state(Vst3RuntimeState& state, std::string& error) {
    return impl_ && impl_->capture_runtime_state(state, error);
}

bool Vst3Plugin::restore_runtime_state(const Vst3RuntimeState& state, std::string& error) {
    return impl_ && impl_->restore_runtime_state(state, error);
}

void Vst3Plugin::set_param_callback(std::function<void(uint32_t, double)> cb) noexcept {
    // 僅 main thread 在 attach_editor 前呼叫;performEdit 回呼也在 main thread(其
    // editor 訊息由 main 的 message loop 派發)—— 無並發
    if (impl_ && impl_->edit_handler) impl_->edit_handler->on_edit = std::move(cb);
}

void Vst3Plugin::set_latency_changed_callback(std::function<void()> cb) noexcept {
    if (impl_ && impl_->edit_handler)
        impl_->edit_handler->on_latency_changed = std::move(cb);
}

bool Vst3Plugin::save_preset(const std::filesystem::path& file,
                             const std::vector<std::pair<std::uint32_t, double>>& host_params,
                             std::string& error) {
    return impl_ && impl_->save_preset(file, host_params, error);
}

bool Vst3Plugin::load_preset(const std::filesystem::path& file,
                             std::vector<std::pair<std::uint32_t, double>>& host_params_inout,
                             std::string& error, bool& host_values_from_file) {
    host_values_from_file = false;
    return impl_ && impl_->load_preset(file, host_params_inout, error, host_values_from_file);
}

bool Vst3Plugin::editor_capable() const noexcept {
    return impl_ && impl_->loaded && impl_->controller != nullptr;
}

bool Vst3Plugin::attach_editor(void* parent_hwnd, void* plug_frame, int& out_w, int& out_h) {
    out_w = 0;
    out_h = 0;
    return impl_ && impl_->attach_editor(static_cast<HWND>(parent_hwnd),
                                         static_cast<Steinberg::IPlugFrame*>(plug_frame),
                                         out_w, out_h);
}

void Vst3Plugin::close_editor() noexcept {
    if (impl_ != nullptr) impl_->close_editor();
}

bool Vst3Plugin::editor_open() const noexcept {
    return impl_ && impl_->view != nullptr;
}

void Vst3Plugin::editor_resize_view(int w, int h) noexcept {
    if (impl_ != nullptr) impl_->editor_resize_view(w, h);
}

}  // namespace rmx
