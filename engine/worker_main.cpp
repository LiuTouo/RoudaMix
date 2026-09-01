// roudamix-worker — 隔離試爆 process。engine spawn 本程式驗證壞 module:
// 崩潰/掛死只死 worker(engine 讀 exit code + stdout 判結果,heap 污染不出本 process)。
//   --verify <module.vst3> [classId] <sampleRate> <blockSize>
//       載入 + instantiate + initialize + setActive(true→false);成功 exit 0。
//   --scan <root>
//       遞迴掃 *.vst3；fingerprint 未變時沿用 ROUDAMIX_VST_REGISTRY 快取，
//       新增/變更才載 module。每 module 輸出一行 JSONL 並 flush；單 module
//       SEH 攔硬體例外，worker 整體失敗時 engine 保留上一版 registry。
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "vst_registry.hpp"
#include "vst3_host.hpp"

namespace {

int verify(const std::vector<std::string>& args) {
    if (args.size() < 3) {
        std::fprintf(stderr, "usage: --verify <module.vst3> [classId] <rate> <block>\n");
        return 2;
    }
    const std::filesystem::path module_path{args[0]};
    std::string class_id;
    double rate = 48000.0;
    std::uint32_t block = 512;
    if (args.size() >= 4) {
        class_id = args[1];
        rate = std::atof(args[2].c_str());
        block = static_cast<std::uint32_t>(std::atoi(args[3].c_str()));
    } else {
        rate = std::atof(args[1].c_str());
        block = static_cast<std::uint32_t>(std::atoi(args[2].c_str()));
    }
    if (rate <= 0 || block == 0) {
        std::fprintf(stderr, "bad rate/block\n");
        return 2;
    }
    rmx::Vst3Plugin plugin(module_path, class_id);
    if (!plugin.loaded()) {
        std::fprintf(stderr, "load failed: %s\n", plugin.last_error().c_str());
        return 1;
    }
    if (!plugin.initialize(rate, static_cast<std::int32_t>(block))) {
        std::fprintf(stderr, "init failed: %s\n", plugin.last_error().c_str());
        return 1;
    }
    plugin.terminate();
    return 0;
}

// SEH 包裹層:__try 函式內不可有需 unwind 的 C++ 物件(main.cpp dispatch_seh 同款)
void scan_module_inner(const std::filesystem::path* path,
                       std::vector<rmx::Vst3ClassInfo>* out, std::string* error) {
    *out = rmx::scan_vst3_module(*path, *error);
}

std::uint32_t scan_module_seh(const std::filesystem::path* path,
                              std::vector<rmx::Vst3ClassInfo>* out, std::string* error) {
    __try {
        scan_module_inner(path, out, error);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

rmx::vst_registry::Entry scan_module(const std::filesystem::path& path,
                                     const rmx::vst_registry::Fingerprint& fingerprint) {
    std::string error;
    std::vector<rmx::Vst3ClassInfo> classes;
    // load 時硬體例外(AV)攔下繼續下一顆；heap 污染的延遲炸 = worker 整顆死，
    // engine 會把本輪視為失敗並保留上一版 registry。
    if (scan_module_seh(&path, &classes, &error) != 0) error = "SEH exception in module";
    rmx::vst_registry::Entry entry;
    entry.path = path;
    entry.fingerprint = fingerprint;
    if (classes.empty()) {
        entry.error = error.empty() ? "module contains no VST3 audio effect classes" : error;
        return entry;
    }
    for (const auto& c : classes) {
        entry.classes.push_back({
            {"uid", c.uid},
            {"name", c.name},
            {"vendor", c.vendor},
            {"version", c.version},
            {"subcategories", c.subcategories},
        });
    }
    return entry;
}

void emit_entry(const rmx::vst_registry::Entry& entry) {
    const std::string line = rmx::vst_registry::entry_json(entry).dump() + "\n";
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);  // 增量契約:每 module 一行即沖
}

int scan(const std::string& root) {
    rmx::vst_registry::Registry cache;
    std::string cache_error;
    (void)rmx::vst_registry::load(rmx::vst_registry::cache_path_from_env(), cache,
                                  cache_error);
    std::unordered_map<std::wstring, const rmx::vst_registry::Entry*> cache_by_path;
    cache_by_path.reserve(cache.entries.size());
    for (const auto& entry : cache.entries)
        cache_by_path.emplace(rmx::vst_registry::path_key(entry.path), &entry);
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it{
        std::filesystem::path{root},
        std::filesystem::directory_options::skip_permission_denied, ec};
    if (ec) {
        std::fprintf(stderr, "root not accessible: %s\n", root.c_str());
        return 2;
    }
    for (decltype(it) end; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (_wcsicmp(it->path().extension().c_str(), L".vst3") != 0) continue;
        std::error_code fec;
        const bool regular = it->is_regular_file(fec);
        const bool directory = !fec && !regular && it->is_directory(fec);
        if (fec || (!regular && !directory)) continue;
        // VST3 bundle 目錄本身就是 module；不可再把 Contents 內二進位掃成第二顆。
        if (directory) it.disable_recursion_pending();
        rmx::vst_registry::Fingerprint value;
        std::string error;
        if (!rmx::vst_registry::fingerprint(it->path(), value, error)) {
            rmx::vst_registry::Entry failed;
            failed.path = it->path();
            failed.error = std::move(error);
            emit_entry(failed);
            continue;
        }
        const auto cached_it = cache_by_path.find(rmx::vst_registry::path_key(it->path()));
        if (cached_it != cache_by_path.end() &&
            cached_it->second->fingerprint == value) {
            const auto* cached = cached_it->second;
            auto reused = *cached;
            reused.path = it->path();
            emit_entry(reused);
        } else {
            emit_entry(scan_module(it->path(), value));
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        std::fprintf(stderr, "CoInitializeEx failed\n");
        return 3;
    }
    std::vector<std::string> args(argv + (argc > 0 ? 1 : 0), argv + argc);
    int code = 2;
    if (args.size() >= 2 && args[0] == "--verify") {
        code = verify(std::vector<std::string>(args.begin() + 1, args.end()));
    } else if (args.size() == 2 && args[0] == "--scan") {
        code = scan(args[1]);
    } else {
        std::fprintf(stderr, "usage: roudamix-worker --verify <module.vst3> [classId] <rate> <block>\n"
                             "                --scan <root>\n");
    }
    CoUninitialize();
    return code;
}
