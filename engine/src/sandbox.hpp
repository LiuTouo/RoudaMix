// 隔離 worker(roudamix-worker.exe)spawn 介面。engine 只靠 exit code + stdout
// 判結果;worker 崩潰/掛死都不污染 engine(超時 TerminateJobObject)。
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rmx::sandbox {

struct WorkerResult {
    bool spawned{false};      // false = worker exe 不在(呼叫端自行 fallback)
    bool timed_out{false};
    bool cancelled{false};    // 外部 cancel flag 被設 → 提前砍
    int exit_code{-1};        // spawned 時有效;0 = 成功
    std::string output;       // stdout+stderr 合流(UTF-8;逐行解析)
};

// worker exe 探查:engine exe 同目錄 roudamix-worker.exe;空 = 無
std::filesystem::path worker_path();

// 跑一次 worker(args 不含 exe 本身);timeout 到即砍 job,exit_code = -1、timed_out。
// cancel 非 null:wait 期間 flag 被設 = 立刻砍(cancelled = true),長掃描可中斷
WorkerResult run_worker(const std::vector<std::string>& args, std::uint32_t timeout_ms,
                        std::atomic<bool>* cancel = nullptr);

// add_plugin 前試爆:module 載入 + class instantiate + initialize + setActive。
// 成功 true；worker 缺失／spawn 失敗／驗證失敗皆 false，err 帶原因。
bool verify_module(const std::filesystem::path& module_path, const std::string& class_id,
                   double sample_rate, std::uint32_t block_size, std::string& err);

enum class PreflightFailure {
    kNone,
    kWorkerUnavailable,
    kVerificationFailed,
};

// 所有會把 module 載入 engine process 的路徑共用此 fail-closed preflight。
PreflightFailure preflight_module(const std::filesystem::path& module_path,
                                  const std::string& class_id, double sample_rate,
                                  std::uint32_t block_size, std::string& err);

}  // namespace rmx::sandbox
