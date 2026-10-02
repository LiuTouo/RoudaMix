#include "fake_wasapi.hpp"
#undef CoCreateInstance
#undef ActivateAudioInterfaceAsync
#undef CreateFileMappingW
#include <propsys.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace fake_wasapi {
std::atomic<bool> lost{false}, fail_start{false};
std::atomic<bool> fail_buffer_size{false}, stall_events{false};
std::atomic<int> clients{0};
std::wstring fail_endpoint;
static std::mutex samples_mutex;
static std::map<std::wstring, float> peaks;
float peak(const std::wstring& endpoint) {
    std::lock_guard lock(samples_mutex); return peaks[endpoint];
}
void clear_samples() { std::lock_guard lock(samples_mutex); peaks.clear(); }

template<class I> class Com : public I {
    std::atomic<ULONG> refs{1};
public:
    virtual ~Com() = default;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        *out = nullptr;
        if (id != __uuidof(IUnknown) && id != __uuidof(I)) return E_NOINTERFACE;
        *out = static_cast<I*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto n = --refs; if (!n) delete this; return n;
    }
};
struct Stream {
    std::wstring id;
    HANDLE event{};
    std::vector<float> buffer = std::vector<float>(256);
    bool packet{true};
    bool invalid() const { return id == L"master" && lost.load(); }
};
class Render final : public Com<IAudioRenderClient> {
    std::shared_ptr<Stream> s;
public:
    explicit Render(std::shared_ptr<Stream> stream) : s(std::move(stream)) {}
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT32, BYTE** data) override {
        if (s->invalid()) return AUDCLNT_E_DEVICE_INVALIDATED;
        *data = reinterpret_cast<BYTE*>(s->buffer.data()); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 n, DWORD) override {
        if (s->invalid()) return AUDCLNT_E_DEVICE_INVALIDATED;
        { std::lock_guard lock(samples_mutex);
          for (UINT32 i = 0; i < n * 2; ++i)
              peaks[s->id] = (std::max)(peaks[s->id], std::abs(s->buffer[i])); }
        if (s->id != L"master" || !stall_events) SetEvent(s->event);
        return S_OK;
    }
};
class Capture final : public Com<IAudioCaptureClient> {
    std::shared_ptr<Stream> s;
public:
    explicit Capture(std::shared_ptr<Stream> stream) : s(std::move(stream)) {
        std::fill(s->buffer.begin(), s->buffer.end(),
            s->id == L"app" ? .75F : s->id == L"mic2" ? .5F : .25F);
    }
    HRESULT STDMETHODCALLTYPE GetBuffer(BYTE** data, UINT32* n, DWORD* flags,
                                        UINT64*, UINT64*) override {
        *data = reinterpret_cast<BYTE*>(s->buffer.data()); *n = 128; *flags = 0; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32) override { s->packet = false; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetNextPacketSize(UINT32* n) override {
        *n = s->packet ? 128 : 0;
        if (!s->packet) { Sleep(3); s->packet = true; SetEvent(s->event); }
        return S_OK;
    }
};
class Client final : public Com<IAudioClient> {
    std::shared_ptr<Stream> s = std::make_shared<Stream>();
public:
    explicit Client(std::wstring id) { s->id = std::move(id); ++clients; }
    ~Client() override { --clients; }
    HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE, DWORD, REFERENCE_TIME,
        REFERENCE_TIME, const WAVEFORMATEX*, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32* n) override {
        *n = 128; return fail_buffer_size ? E_FAIL : S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME* n) override { *n = 26667; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32* n) override {
        Sleep(3); *n = 0; return s->invalid() ? AUDCLNT_E_DEVICE_INVALIDATED : S_OK;
    }
    HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE, const WAVEFORMATEX*, WAVEFORMATEX**) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX** out) override {
        auto* f = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
        *f = {WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 384000, 8, 32, 0}; *out = f; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME* d, REFERENCE_TIME* m) override {
        if (d) *d = 26667; if (m) *m = 26667; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Start() override {
        if (fail_start || s->id == fail_endpoint) return E_FAIL;
        SetEvent(s->event); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Stop() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Reset() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE h) override { s->event = h; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetService(REFIID id, void** out) override {
        if (id == __uuidof(IAudioRenderClient)) *out = new Render(s);
        else if (id == __uuidof(IAudioCaptureClient)) *out = new Capture(s);
        else { *out = nullptr; return E_NOINTERFACE; }
        return S_OK;
    }
};
class Properties final : public Com<IPropertyStore> {
public:
    HRESULT STDMETHODCALLTYPE GetCount(DWORD* n) override { *n = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetAt(DWORD, PROPERTYKEY*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetValue(REFPROPERTYKEY, PROPVARIANT* v) override {
        PropVariantInit(v); v->vt = VT_LPWSTR;
        v->pwszVal = static_cast<wchar_t*>(CoTaskMemAlloc(11 * sizeof(wchar_t)));
        wcscpy_s(v->pwszVal, 11, L"Test audio"); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetValue(REFPROPERTYKEY, REFPROPVARIANT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Commit() override { return S_OK; }
};
class Device final : public Com<IMMDevice> {
    std::wstring id;
public:
    explicit Device(std::wstring name) : id(std::move(name)) {}
    HRESULT STDMETHODCALLTYPE Activate(REFIID iid, DWORD, PROPVARIANT*, void** out) override {
        if (iid != __uuidof(IAudioClient)) return E_NOINTERFACE;
        *out = new Client(id); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD, IPropertyStore** out) override { *out = new Properties; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetId(LPWSTR* out) override {
        *out = static_cast<wchar_t*>(CoTaskMemAlloc((id.size() + 1) * sizeof(wchar_t)));
        wcscpy_s(*out, id.size() + 1, id.c_str()); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetState(DWORD* out) override { *out = DEVICE_STATE_ACTIVE; return S_OK; }
};
class Collection final : public Com<IMMDeviceCollection> {
    std::vector<std::wstring> ids;
public:
    explicit Collection(EDataFlow flow) : ids(flow == eCapture
        ? std::vector<std::wstring>{L"mic", L"mic2"}
        : std::vector<std::wstring>{L"master", L"speaker", L"headphones"}) {}
    HRESULT STDMETHODCALLTYPE GetCount(UINT* n) override { *n = static_cast<UINT>(ids.size()); return S_OK; }
    HRESULT STDMETHODCALLTYPE Item(UINT n, IMMDevice** out) override {
        if (n >= ids.size()) return E_INVALIDARG; *out = new Device(ids[n]); return S_OK;
    }
};
class Enumerator final : public Com<IMMDeviceEnumerator> {
public:
    HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(EDataFlow flow, DWORD, IMMDeviceCollection** out) override { *out = new Collection(flow); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(EDataFlow flow, ERole, IMMDevice** out) override { *out = new Device(flow == eCapture ? L"mic" : L"master"); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDevice(LPCWSTR id, IMMDevice** out) override { *out = new Device(id); return S_OK; }
    HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(IMMNotificationClient*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(IMMNotificationClient*) override { return S_OK; }
};
class Activation final : public Com<IActivateAudioInterfaceAsyncOperation> {
public:
    HRESULT STDMETHODCALLTYPE GetActivateResult(HRESULT* hr, IUnknown** out) override {
        *hr = S_OK; *out = new Client(L"app"); return S_OK;
    }
};
HRESULT WINAPI create(REFCLSID cls, LPUNKNOWN outer, DWORD ctx, REFIID iid, LPVOID* out) {
    if (cls != __uuidof(MMDeviceEnumerator)) return CoCreateInstance(cls, outer, ctx, iid, out);
    *out = new Enumerator; return S_OK;
}
HRESULT WINAPI activate(LPCWSTR, REFIID, PROPVARIANT*,
    IActivateAudioInterfaceCompletionHandler* handler, IActivateAudioInterfaceAsyncOperation** out) {
    *out = new Activation; return handler->ActivateCompleted(*out);
}
HANDLE WINAPI mapping(HANDLE file, LPSECURITY_ATTRIBUTES attrs, DWORD protect, DWORD hi, DWORD lo, LPCWSTR) {
    // No named mapping: never overwrite the running desktop application's meters.
    return CreateFileMappingW(file, attrs, protect, hi, lo, nullptr);
}
}
