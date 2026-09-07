#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/vsteditcontroller.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using namespace Steinberg;
using namespace Steinberg::Vst;

constexpr ParamID kLatencyParam = 100;
constexpr ParamID kCpuParam = 101;
constexpr ParamID kFailParam = 102;
constexpr std::uint32_t kMaxLatency = 192000;
const FUID kProcessorUid(0x52584C50, 0x44434658, 0x54555245, 0x30303031);
const FUID kControllerUid(0x52584C43, 0x44434658, 0x54555245, 0x30303031);
std::atomic<IComponentHandler*> g_component_handlers[16]{};
std::atomic<std::uint32_t> g_next_opaque_state{1};

bool write_exact(IBStream* stream, const void* data, int32 bytes) {
    int32 written = 0;
    return stream != nullptr && stream->write(const_cast<void*>(data), bytes, &written) == kResultOk &&
           written == bytes;
}

bool read_exact(IBStream* stream, void* data, int32 bytes) {
    int32 read = 0;
    return stream != nullptr && stream->read(data, bytes, &read) == kResultOk && read == bytes;
}

class LatencyFixtureProcessor final : public AudioEffect {
public:
    LatencyFixtureProcessor() { setControllerClass(kControllerUid); }

    static FUnknown* create_instance(void*) { return static_cast<IAudioProcessor*>(new LatencyFixtureProcessor); }

    tresult PLUGIN_API initialize(FUnknown* context) override {
        if (AudioEffect::initialize(context) != kResultOk) return kResultFalse;
        addAudioInput(STR16("Stereo In"), SpeakerArr::kStereo);
        addAudioOutput(STR16("Stereo Out"), SpeakerArr::kStereo);
        return kResultOk;
    }

    tresult PLUGIN_API setupProcessing(ProcessSetup& setup) override {
        if (AudioEffect::setupProcessing(setup) != kResultOk || setup.maxSamplesPerBlock <= 0)
            return kResultFalse;
        const auto capacity = static_cast<std::size_t>(kMaxLatency) +
                              static_cast<std::size_t>(setup.maxSamplesPerBlock) + 1u;
        ring_l_.assign(capacity, 0.0F);
        ring_r_.assign(capacity, 0.0F);
        write_ = 0;
        return kResultOk;
    }

    uint32 PLUGIN_API getLatencySamples() override {
        return latency_.load(std::memory_order_acquire);
    }

    tresult PLUGIN_API process(ProcessData& data) override {
        bool latency_changed = false;
        if (data.inputParameterChanges != nullptr) {
            const int32 queues = data.inputParameterChanges->getParameterCount();
            for (int32 i = 0; i < queues; ++i) {
                IParamValueQueue* queue = data.inputParameterChanges->getParameterData(i);
                if (queue == nullptr || queue->getPointCount() == 0) continue;
                int32 offset = 0;
                ParamValue value = 0.0;
                if (queue->getPoint(queue->getPointCount() - 1, offset, value) != kResultTrue)
                    continue;
                value = std::clamp(value, 0.0, 1.0);
                if (queue->getParameterId() == kLatencyParam) {
                    const auto next = static_cast<std::uint32_t>(std::llround(value * kMaxLatency));
                    latency_changed = latency_changed || next != latency_.load(std::memory_order_relaxed);
                    latency_.store(next, std::memory_order_release);
                } else if (queue->getParameterId() == kCpuParam) {
                    cpu_.store(value, std::memory_order_release);
                } else if (queue->getParameterId() == kFailParam) {
                    fail_.store(value >= 0.5, std::memory_order_release);
                }
            }
        }
        if (latency_changed) {
            for (auto& entry : g_component_handlers)
                if (auto* handler = entry.load(std::memory_order_acquire))
                    (void)handler->restartComponent(kLatencyChanged);
        }
        const auto work = static_cast<std::uint32_t>(cpu_.load(std::memory_order_acquire) * 20000.0);
        volatile double sink = 0.0;
        for (std::uint32_t i = 0; i < work; ++i) sink += std::sin(static_cast<double>(i));
        (void)sink;
        if (fail_.load(std::memory_order_acquire)) return kResultFalse;
        if (data.numInputs < 1 || data.numOutputs < 1 || data.symbolicSampleSize != kSample32 ||
            ring_l_.empty())
            return kResultFalse;
        auto& input = data.inputs[0];
        auto& output = data.outputs[0];
        if (input.numChannels < 2 || output.numChannels < 2) return kResultFalse;
        const auto delay = static_cast<std::size_t>(latency_.load(std::memory_order_acquire));
        for (int32 i = 0; i < data.numSamples; ++i) {
            ring_l_[write_] = input.channelBuffers32[0][i];
            ring_r_[write_] = input.channelBuffers32[1][i];
            const auto read = (write_ + ring_l_.size() - delay) % ring_l_.size();
            output.channelBuffers32[0][i] = ring_l_[read];
            output.channelBuffers32[1][i] = ring_r_[read];
            write_ = (write_ + 1u) % ring_l_.size();
        }
        return kResultOk;
    }

    tresult PLUGIN_API getState(IBStream* state) override {
        const auto latency = latency_.load(std::memory_order_acquire);
        const auto cpu = cpu_.load(std::memory_order_acquire);
        const std::uint8_t fail = fail_.load(std::memory_order_acquire) ? 1u : 0u;
        return write_exact(state, &latency, sizeof(latency)) &&
                       write_exact(state, &cpu, sizeof(cpu)) &&
                       write_exact(state, &fail, sizeof(fail)) &&
                       write_exact(state, &opaque_state_, sizeof(opaque_state_))
                   ? kResultOk
                   : kResultFalse;
    }

    tresult PLUGIN_API setState(IBStream* state) override {
        std::uint32_t latency = 0;
        double cpu = 0.0;
        std::uint8_t fail = 0;
        if (!read_exact(state, &latency, sizeof(latency)) ||
            !read_exact(state, &cpu, sizeof(cpu)) ||
            !read_exact(state, &fail, sizeof(fail)) || latency > kMaxLatency)
            return kResultFalse;
        latency_.store(latency, std::memory_order_release);
        cpu_.store(std::clamp(cpu, 0.0, 1.0), std::memory_order_release);
        fail_.store(fail != 0, std::memory_order_release);
        // 舊 fixture preset 沒有此尾欄也可載入；新實例的預設值彼此不同。
        std::uint32_t opaque = 0;
        if (read_exact(state, &opaque, sizeof(opaque))) opaque_state_ = opaque;
        std::fill(ring_l_.begin(), ring_l_.end(), 0.0F);
        std::fill(ring_r_.begin(), ring_r_.end(), 0.0F);
        write_ = 0;
        return kResultOk;
    }

private:
    // 不暴露成參數，驗證複製有還原 component state，而非只重放 params。
    std::uint32_t opaque_state_{g_next_opaque_state.fetch_add(1)};
    std::atomic<std::uint32_t> latency_{};
    std::atomic<double> cpu_{};
    std::atomic<bool> fail_{};
    std::vector<float> ring_l_, ring_r_;
    std::size_t write_{};
};

class LatencyFixtureController final : public EditController {
public:
    ~LatencyFixtureController() override {
        for (auto& entry : g_component_handlers) {
            auto* expected = registered_handler_;
            (void)entry.compare_exchange_strong(expected, nullptr);
        }
    }
    static FUnknown* create_instance(void*) { return static_cast<IEditController*>(new LatencyFixtureController); }

    tresult PLUGIN_API initialize(FUnknown* context) override {
        if (EditController::initialize(context) != kResultOk) return kResultFalse;
        parameters.addParameter(STR16("Latency"), nullptr, 0, 0.0,
                                ParameterInfo::kCanAutomate, kLatencyParam);
        parameters.addParameter(STR16("CPU"), nullptr, 0, 0.0,
                                ParameterInfo::kCanAutomate, kCpuParam);
        parameters.addParameter(STR16("Fail"), nullptr, 1, 0.0,
                                ParameterInfo::kCanAutomate, kFailParam);
        return kResultOk;
    }

    tresult PLUGIN_API setComponentHandler(IComponentHandler* handler) override {
        const auto result = EditController::setComponentHandler(handler);
        if (result == kResultOk) {
            registered_handler_ = handler;
            for (auto& entry : g_component_handlers) {
                IComponentHandler* expected = nullptr;
                if (entry.compare_exchange_strong(expected, handler)) break;
            }
        }
        return result;
    }

    tresult PLUGIN_API setComponentState(IBStream* state) override {
        std::uint32_t latency = 0;
        double cpu = 0.0;
        std::uint8_t fail = 0;
        if (!read_exact(state, &latency, sizeof(latency)) ||
            !read_exact(state, &cpu, sizeof(cpu)) ||
            !read_exact(state, &fail, sizeof(fail)))
            return kResultFalse;
        setParamNormalized(kLatencyParam,
                           static_cast<double>(latency) / static_cast<double>(kMaxLatency));
        setParamNormalized(kCpuParam, cpu);
        setParamNormalized(kFailParam, fail != 0 ? 1.0 : 0.0);
        return kResultOk;
    }

    tresult PLUGIN_API getState(IBStream* state) override {
        const double values[]{getParamNormalized(kLatencyParam), getParamNormalized(kCpuParam),
                              getParamNormalized(kFailParam)};
        return write_exact(state, values, sizeof(values)) ? kResultOk : kResultFalse;
    }

    tresult PLUGIN_API setState(IBStream* state) override {
        double values[3]{};
        if (!read_exact(state, values, sizeof(values))) return kResultFalse;
        setParamNormalized(kLatencyParam, values[0]);
        setParamNormalized(kCpuParam, values[1]);
        setParamNormalized(kFailParam, values[2]);
        return kResultOk;
    }

private:
    IComponentHandler* registered_handler_{};
};

}  // namespace

bool InitModule() { return true; }
bool DeinitModule() { return true; }

BEGIN_FACTORY("RoudaMix", "https://roudamix.invalid", "test@roudamix.invalid", 0)
DEF_CLASS2(INLINE_UID(0x52584C50, 0x44434658, 0x54555245, 0x30303031),
           PClassInfo::kManyInstances, kVstAudioEffectClass, "RoudaMix Latency Fixture", 0,
           Vst::PlugType::kFx, "1.0.0", kVstVersionString,
           LatencyFixtureProcessor::create_instance)
DEF_CLASS2(INLINE_UID(0x52584C43, 0x44434658, 0x54555245, 0x30303031),
           PClassInfo::kManyInstances, kVstComponentControllerClass,
           "RoudaMix Latency Fixture Controller", 0, "", "1.0.0", kVstVersionString,
           LatencyFixtureController::create_instance)
END_FACTORY
