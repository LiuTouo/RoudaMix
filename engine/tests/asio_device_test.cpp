// asio_device 測試:假 IASIO driver 走真 prepare/start/RT 路徑,驗證
// 「createBuffers 後以 driver 授予的 buffer 大小為準」——部分 driver 會把
// granularity 不合的要求 clamp 成別的合法值,host 報「要求的」大小 = 顯示 ≠ 實際。
#define NOMINMAX
#include "asio_device.hpp"

#include <Objbase.h>
#include <Windows.h>

#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define CHECK(x)                                                              \
    do {                                                                      \
        if (!(x)) {                                                           \
            std::fprintf(stderr, "CHECK FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            std::abort();                                                     \
        }                                                                     \
    } while (0)

namespace {

using rmx::AsioDevice;
using rmx::DeviceCapability;
using rmx::PcmType;

// granularity=0(2 的冪)driver:要求非冪值 → clamp 成 granted(預設 512)
class FakeAsio final : public IASIO {
public:
    long granted = 512;  // createBuffers 後 getBufferSize 回報的「現行單一值」
    bool fail_grant_query = false;
    long requested = 0;
    ASIOCallbacks* callbacks = nullptr;
    std::vector<std::vector<float>> channel_buffers;  // per-info [half0, half1]

    // IUnknown(假物件永不經 base 指標刪除;AddRef 為假)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    ASIOBool init(void*) override { return ASIOTrue; }
    void getDriverName(char* name) override { std::snprintf(name, 32, "Fake"); }
    long getDriverVersion() override { return 1; }
    void getErrorMessage(char* s) override { s[0] = '\0'; }
    ASIOError start() override { return ASE_OK; }
    ASIOError stop() override { return ASE_OK; }
    ASIOError getChannels(long* in, long* out) override {
        *in = 1; *out = 1; return ASE_OK;
    }
    ASIOError getLatencies(long* in, long* out) override {
        *in = 6; *out = 12; return ASE_OK;
    }
    ASIOError getBufferSize(long* mn, long* mx, long* pf, long* gr) override {
        if (callbacks != nullptr) {  // createBuffers 後 = 單一現行值(規格)
            if (fail_grant_query) return ASE_NotPresent;
            *mn = *mx = *pf = granted; *gr = 0;
        } else {  // 能力查詢
            *mn = 64; *mx = 2048; *pf = 256; *gr = 0;
        }
        return ASE_OK;
    }
    ASIOError canSampleRate(ASIOSampleRate) override { return ASE_OK; }
    ASIOError getSampleRate(ASIOSampleRate* rate) override {
        *rate = 48000.0; return ASE_OK;
    }
    ASIOError setSampleRate(ASIOSampleRate) override { return ASE_OK; }
    ASIOError getClockSources(ASIOClockSource* clocks, long* n) override {
        *n = 0; (void)clocks; return ASE_OK;
    }
    ASIOError setClockSource(long) override { return ASE_OK; }
    ASIOError getSamplePosition(ASIOSamples* pos, ASIOTimeStamp*) override {
        pos->lo = 0; pos->hi = 0; return ASE_OK;
    }
    ASIOError getChannelInfo(ASIOChannelInfo* info) override {
        info->type = ASIOSTFloat32LSB;
        std::snprintf(info->name, sizeof(info->name), "Fake %ld", info->channel);
        return ASE_OK;
    }
    ASIOError createBuffers(ASIOBufferInfo* infos, long n, long,
                                             ASIOCallbacks* cbs) override {
        requested = 0;
        callbacks = cbs;
        channel_buffers.assign(static_cast<std::size_t>(n) * 2,
                               std::vector<float>(static_cast<std::size_t>(granted), 0.0F));
        for (long i = 0; i < n; ++i) {
            infos[i].buffers[0] = channel_buffers[static_cast<std::size_t>(i) * 2].data();
            infos[i].buffers[1] = channel_buffers[static_cast<std::size_t>(i) * 2 + 1].data();
        }
        return ASE_OK;
    }
    ASIOError disposeBuffers() override {
        callbacks = nullptr;
        channel_buffers.clear();
        return ASE_OK;
    }
    ASIOError controlPanel() override { return ASE_OK; }
    ASIOError future(long, void*) override { return ASE_OK; }
    ASIOError outputReady() override { return ASE_OK; }

    // 以 driver 名義發 bufferSwitch(模擬 RT callback)
    void fire(long index) {
        if (callbacks != nullptr) callbacks->bufferSwitch(index, ASIOTrue);
    }
    // input ch0 的兩個 half 都填 marker(fire(0)/fire(1) 各讀一個)
    void fill_input_markers() {
        for (std::size_t half = 0; half < 2; ++half) {
            auto& buf = channel_buffers[half];
            for (std::size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<float>(i + 1);
        }
    }
};

DeviceCapability test_capability() {
    DeviceCapability cap{};
    cap.max_in = 1;
    cap.max_out = 1;
    cap.min_buffer = 64;
    cap.max_buffer = 2048;
    cap.preferred_buffer = 256;
    cap.buffer_granularity = 0;
    cap.current_sample_rate = 48000;
    cap.sample_rates = {48000};
    cap.input_types = {PcmType::Float32Lsb};
    cap.output_types = {PcmType::Float32Lsb};
    cap.input_names = {"Fake In 1"};
    cap.output_names = {"Fake Out 1"};
    return cap;
}

struct Recorder final : rmx::IAudioCallback {
    std::uint32_t frames = 0;
    std::vector<float> in0;
    void process(const rmx::AudioBlock& block) noexcept override {
        frames = block.frames;
        in0.assign(block.inputs[0], block.inputs[0] + block.frames);
    }
};

}  // namespace

int main() {
    std::string err;

    // --- 1:要求 576(非 2 的冪),driver clamp 成 512 → block_size 須 = 授予值 ---
    {
        AsioDevice dev;
        FakeAsio fake;
        dev.attach_driver_for_test(&fake, test_capability());
        CHECK(dev.prepare(48000, {0}, {0}, 576, err));
        CHECK(dev.block_size() == 512);  // 授予值,不是要求值 576

        Recorder rec;
        dev.set_callback(&rec);
        CHECK(dev.start(err));
        fake.fill_input_markers();
        fake.fire(0);
        fake.fire(1);
        CHECK(rec.frames == 512);        // RT 步距跟授予值(舊碼 = 576 → 越界讀假 driver 緩衝)
        CHECK(rec.in0.size() == 512);
        CHECK(rec.in0[0] == 1.0F && rec.in0[511] == 512.0F);
    }

    // --- 2:要求 = 授予(一般路徑回歸)+ preferred 路徑(0)---
    {
        AsioDevice dev;
        FakeAsio fake;
        dev.attach_driver_for_test(&fake, test_capability());
        CHECK(dev.prepare(48000, {0}, {0}, 512, err));
        CHECK(dev.block_size() == 512);
        fake.granted = 256;
        CHECK(dev.prepare(48000, {0}, {0}, 0, err));  // 0 = driver preferred
        CHECK(dev.block_size() == 256);
    }

    // --- 3:授予查詢失敗 = 退回要求值(行為不劣化)---
    {
        AsioDevice dev;
        FakeAsio fake;
        fake.granted = 480;  // 假 driver 這次「授予」要求值本身
        dev.attach_driver_for_test(&fake, test_capability());
        CHECK(dev.prepare(48000, {0}, {0}, 480, err));
        fake.fail_grant_query = true;
        CHECK(dev.prepare(48000, {0}, {0}, 480, err));  // re-prepare,查詢失敗
        CHECK(dev.block_size() == 480);
    }

    std::printf("asio_device_test PASSED\n");
    return 0;
}
