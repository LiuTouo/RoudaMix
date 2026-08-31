#include "sandbox.hpp"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <thread>

namespace rmx::sandbox {

namespace {

constexpr std::size_t kMaxOutputBytes = 4u * 1024 * 1024;  // 壞 module 狂噴 stdout 的上限

// 引號含空格路徑(command line 重組;寬字元版)
std::wstring quote(const std::wstring& s) { return L"\"" + s + L"\""; }

// UTF-8 → UTF-16(路徑/command line 用;非 ANSI 路徑才不炸)
std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                      nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

struct PipePair {
    HANDLE read_handle{nullptr};
    HANDLE write_handle{nullptr};
    bool ok() const noexcept { return read_handle != nullptr && write_handle != nullptr; }
};

PipePair make_inherit_pipe() {
    PipePair p;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    if (!CreatePipe(&p.read_handle, &p.write_handle, &sa, 0)) return {};
    // 讀端不可被 child 繼承
    SetHandleInformation(p.read_handle, HANDLE_FLAG_INHERIT, 0);
    return p;
}

// worker 輸出可能混 ANSI(SDK 錯誤訊息)—— 非 UTF-8 byte 換 '?',免得 nlohmann
// JSON 序列化 throw(json.exception.type_error.316,曾讓 dispatch 炸掉不回 reply)
std::string sanitize_utf8(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size();) {
        const auto c = static_cast<unsigned char>(in[i]);
        std::size_t len = 1;
        if (c >= 0x80) {
            len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
            bool valid = len >= 2 && i + len <= in.size();
            for (std::size_t k = 1; valid && k < len; ++k)
                if ((static_cast<unsigned char>(in[i + k]) & 0xC0) != 0x80) valid = false;
            if (!valid) len = 1;
        }
        if (len == 1 && c >= 0x80)
            out.push_back('?');
        else
            out.append(in, i, len);
        i += len;
    }
    return out;
}

}  // namespace

std::filesystem::path worker_path() {
    wchar_t exe[MAX_PATH]{};
    const UINT n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::filesystem::path dir = std::filesystem::path(exe).parent_path();
    std::error_code ec;
    const std::filesystem::path candidate = dir / L"roudamix-worker.exe";
    if (std::filesystem::exists(candidate, ec)) return candidate;
    return {};
}

WorkerResult run_worker(const std::vector<std::string>& args, std::uint32_t timeout_ms,
                        std::atomic<bool>* cancel) {
    WorkerResult result;
    const std::filesystem::path exe = worker_path();
    if (exe.empty()) return result;  // spawned = false:呼叫端 fallback in-process
    result.spawned = true;
    std::fprintf(stderr, "[sandbox] spawn worker args0=%s\n", args.empty() ? "?" : args[0].c_str());

    std::wstring cmdline = quote(exe.wstring());
    for (const auto& a : args) cmdline += L" " + quote(to_wide(a));

    PipePair pipe = make_inherit_pipe();
    if (!pipe.ok()) {
        result.exit_code = -1;
        result.output = "worker pipe creation failed";
        return result;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nullptr;
    si.hStdOutput = pipe.write_handle;
    si.hStdError = pipe.write_handle;  // stdout+stderr 合流,逐行解析時容錯
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutable_cmd(cmdline.begin(), cmdline.end());
    mutable_cmd.push_back(L'\0');
    if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        std::fprintf(stderr, "[sandbox] CreateProcessW failed: %lu\n", GetLastError());
        result.output = "worker spawn failed: " + std::to_string(GetLastError());
        CloseHandle(pipe.read_handle);
        CloseHandle(pipe.write_handle);
        return result;
    }
    std::fprintf(stderr, "[sandbox] spawned pid=%lu\n", pi.dwProcessId);
    // KILL_ON_JOB_CLOSE:超時/父死 worker 必死,不留孤兒。job 進不去(沙箱/測試
    // 環境會失敗)= 超時改用 TerminateProcess,不留「以為砍了其實沒砍」的空洞
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    bool in_job = false;
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        in_job = SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit,
                                         sizeof(limit)) &&
                 AssignProcessToJobObject(job, pi.hProcess);
    }
    CloseHandle(pipe.write_handle);  // 讀端 EOF 依賴寫端全關
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    // 讀者 thread:PeekNamedPipe 輪詢(非 blocking),cancel flag 收尾 —— 保證可
    // join,絕不 detach(捕 stack reference 的 thread detach = use-after-free)。
    // 上限 cap 防 module 狂噴 stdout 灌爆記憶體
    std::atomic<bool> reader_done{false};
    std::atomic<bool> reader_cancel{false};
    std::thread reader([&result, &reader_done, &reader_cancel, h = pipe.read_handle] {
        char buf[4096];
        for (;;) {
            if (reader_cancel.load(std::memory_order_acquire)) break;
            DWORD avail = 0;
            // pipe broken(寫端全關)= FALSE/ERROR_BROKEN_PIPE;grandchild 持寫端
            // 時這裡永遠 avail==0,靠 cancel 收尾
            if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) break;
            if (avail == 0) {
                Sleep(15);
                continue;
            }
            DWORD n = 0;
            const DWORD want = static_cast<DWORD>(sizeof(buf)) < avail
                                   ? static_cast<DWORD>(sizeof(buf))
                                   : avail;
            if (!ReadFile(h, buf, want, &n, nullptr) || n == 0) break;
            if (result.output.size() < kMaxOutputBytes) result.output.append(buf, n);
        }
        reader_done.store(true, std::memory_order_release);
    });
    // 分段等:timeout 或外部 cancel(掃描可中斷)都會提前砍
    DWORD wait = WaitForSingleObject(pi.hProcess, 100);
    DWORD waited = 100;
    while (wait == WAIT_TIMEOUT && waited < timeout_ms) {
        if (cancel != nullptr && cancel->load(std::memory_order_acquire)) break;
        wait = WaitForSingleObject(pi.hProcess, 100);
        waited += 100;
    }
    if (cancel != nullptr && cancel->load(std::memory_order_acquire) &&
        wait == WAIT_TIMEOUT) {
        result.cancelled = true;
    } else if (wait == WAIT_TIMEOUT) {
        result.timed_out = true;
    }
    if (wait == WAIT_TIMEOUT) {
        if (in_job)
            TerminateJobObject(job, UINT32_MAX);
        else
            TerminateProcess(pi.hProcess, UINT32_MAX);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    result.exit_code = static_cast<int>(code);
    // bounded 等 reader 收乾 pipe buffer(正常 < 一個 poll 週期);逾時 = EOF 永不
    // 來(grandchild 持繼承寫端)→ cancel,reader 15ms 內退出後 join(必成)
    for (int i = 0; i < 40 && !reader_done.load(std::memory_order_acquire); ++i) Sleep(25);
    reader_cancel.store(true, std::memory_order_release);
    reader.join();
    result.output = sanitize_utf8(result.output);
    std::fprintf(stderr, "[sandbox] wait=%lu exit=%lu out=%zu\n", wait,
                 static_cast<unsigned long>(result.exit_code), result.output.size());
    CloseHandle(pi.hProcess);
    if (job != nullptr) CloseHandle(job);  // KILL_ON_JOB_CLOSE 兜底清殘留
    CloseHandle(pipe.read_handle);
    return result;
}

bool verify_module(const std::filesystem::path& module_path, const std::string& class_id,
                   double sample_rate, std::uint32_t block_size, std::string& err) {
    std::vector<std::string> args = {"--verify", module_path.string()};
    if (!class_id.empty()) args.push_back(class_id);
    args.push_back(std::to_string(sample_rate));
    args.push_back(std::to_string(block_size));

    // ponytail:20s 固定上限 —— 惡意 plugin 掛死只浪費一次 add 的 20s;要縮再調
    const WorkerResult r = run_worker(args, 20000);
    if (!r.spawned) {
        err = {};  // worker 不在:呼叫端 fallback in-process 載入(不擋)
        return true;
    }
    if (r.timed_out) {
        err = "plugin load verification timed out (worker killed)";
        return false;
    }
    if (r.exit_code != 0) {
        err = "plugin failed in sandbox worker (exit " + std::to_string(r.exit_code) + ")";
        if (!r.output.empty()) {
            // 只帶最後一行 stderr 摘要,別把 crash dump 塞進 reply
            std::string last;
            for (const char c : r.output)
                if (c == '\n' || c == '\r') {
                    if (!last.empty()) break;
                } else {
                    last.push_back(c);
                }
            if (!last.empty()) err += ": " + last;
        }
        return false;
    }
    return true;
}

}  // namespace rmx::sandbox
