// roudamix-worker — 隔離試爆 process。engine spawn 本程式驗證壞 module:
// 崩潰/掛死只死 worker(engine 讀 exit code + stdout 判結果,heap 污染不出本 process)。
//   --verify <module.vst3> [classId] <sampleRate> <blockSize>
//       載入 + instantiate + initialize + setActive(true→false);成功 exit 0。
//   --scan <root>
//       遞迴掃 *.vst3;每 module 一行「OK\t<path>\t<classes JSON>」或
//       「FAIL\t<path>\t<error>」,逐行 flush —— worker 中途崩潰時 engine 保留
//       已 flush 的增量結果。單 module SEH 攔硬體例外,能續就續。
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "vst3_host.hpp"

namespace {

// path → UTF-8(預設 narrow string 是 ANSI;engine 端 JSON 需要 UTF-8)
std::string path_utf8(const std::filesystem::path& p) {
    const std::wstring w = p.wstring();
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n,
                        nullptr, nullptr);
    return s;
}

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

void scan_module(const std::filesystem::path& path) {
    std::string error;
    std::vector<rmx::Vst3ClassInfo> classes;
    // load 時硬體例外(AV)攔下繼續下一顆;heap 污染的延遲炸 = worker 整顆死,
    // engine 靠已 flush 行數兜底 —— 兩層都成立
    if (scan_module_seh(&path, &classes, &error) != 0) error = "SEH exception in module";
    if (classes.empty()) {
        const std::string p8 = path_utf8(path);
        std::fwrite("FAIL\t", 1, 5, stdout);
        std::fwrite(p8.c_str(), 1, p8.size(), stdout);
        std::fwrite("\t", 1, 1, stdout);
        std::fwrite(error.c_str(), 1, error.size(), stdout);
        std::fwrite("\n", 1, 1, stdout);
    } else {
        std::string line = "OK\t" + path_utf8(path) + "\t[";
        for (std::size_t i = 0; i < classes.size(); ++i) {
            const auto& c = classes[i];
            if (i > 0) line += ",";
            // JSON 字串:路徑/name 內的 " \ 控制字元跳脫(最小實作)
            const auto json_str = [](const std::string& s) {
                std::string out = "\"";
                for (const char ch : s) {
                    if (ch == '"' || ch == '\\') out += '\\';
                    if (static_cast<unsigned char>(ch) < 0x20) continue;
                    out += ch;
                }
                return out + "\"";
            };
            line += "{\"uid\":" + json_str(c.uid) + ",\"name\":" + json_str(c.name) +
                    ",\"vendor\":" + json_str(c.vendor) + ",\"version\":" + json_str(c.version) +
                    ",\"subcategories\":" + json_str(c.subcategories) + "}";
        }
        line += "]\n";
        std::fwrite(line.data(), 1, line.size(), stdout);
    }
    std::fflush(stdout);  // 增量契約:每 module 一行即沖
}

int scan(const std::string& root) {
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
        std::error_code fec;
        if (!it->is_regular_file(fec) || it->path().extension() != ".vst3") continue;
        scan_module(it->path());
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
