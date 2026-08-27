// VST3 host 縮編實作。依賴 Steinberg hosting(module/plugprovider/hostclasses/
// parameterchanges/processdata)+ pluginterfaces。ponytail:stereo main bus only,
// aux bus / Float64 / editor / state 不支援 —— mono-only 或多 bus plugin 進不來,
// 需要時再開(ProMixArea vst3_adapter.cpp 有全版)。
#include "vst3_host.hpp"

#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

namespace rmx {

namespace {

using namespace Steinberg;
using namespace Steinberg::Vst;

constexpr int32 kMaxParamEventsPerBlock = 64;

// IComponentHandler:no-op。RoudaMix 沒有 plugin editor,正常沒有 performEdit 來源;
// plugin 內部執行緒若呼叫也安全(ref-counted,生命週期歸 plugin 持有)。
class NullComponentHandler final : public IComponentHandler {
public:
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
    tresult PLUGIN_API performEdit(ParamID, ParamValue) override { return kResultOk; }
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32) override { return kResultOk; }

private:
    std::atomic<uint32> references_{1};
};

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
            std::error_code file_ec;
            if (!it->is_regular_file(file_ec) || it->path().extension() != ".vst3") continue;
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
    bool initialized{false};
    bool loaded{false};
    std::string name_;
    std::string class_uid_;
    std::string error;

    ~Impl() { terminate(); }

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
            handler = owned(new NullComponentHandler());
            if (controller->setComponentHandler(handler) != kResultOk) {
                error = "VST3 controller rejected IComponentHandler";
                return;
            }
            enumerate_params();
        }
        loaded = true;
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
        ins[main_in_bus] = SpeakerArr::kStereo;
        outs[main_out_bus] = SpeakerArr::kStereo;
        if (processor->setBusArrangements(ins.data(), in_count, outs.data(), out_count) != kResultOk) {
            error = "plugin rejected stereo main-bus arrangement";
            return false;
        }
        BusInfo in_info{}, out_info{};
        if (component->getBusInfo(kAudio, kInput, main_in_bus, in_info) != kResultOk ||
            component->getBusInfo(kAudio, kOutput, main_out_bus, out_info) != kResultOk ||
            in_info.channelCount != 2 || out_info.channelCount != 2) {
            error = "plugin main bus is not stereo after negotiation";
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

    bool process(const float* in_l, const float* in_r, float* out_l, float* out_r,
                 int32_t frames, const Vst3ParamEdit* edits, size_t edit_count) noexcept {
        if (!initialized) return false;
        parameter_changes.clearQueue();
        for (size_t i = 0; i < edit_count; ++i) {
            int32 queue_index{};
            auto* queue = parameter_changes.addParameterData(edits[i].id, queue_index);
            int32 point_index{};
            if (!queue ||
                queue->addPoint(0, edits[i].value, point_index) != kResultTrue)
                return false;
        }
        process_data.numSamples = frames;
        context.projectTimeSamples += frames;
        context.state = 0;
        auto& in_bus = process_data.inputs[main_in_bus];
        auto& out_bus = process_data.outputs[main_out_bus];
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

}  // namespace rmx
