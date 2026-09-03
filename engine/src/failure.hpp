// 型別化失敗值:protocol error code 的 enum + 人讀 message。
// engine 核心、capture/render pump、Router 共用;enum → wire code 字串的
// 對照表只存在 Router(router.cpp to_code)。成功以 std::optional<Failure>
// 的 nullopt 表達;failure() 是造失敗值的唯一入口。
#pragma once

#include <optional>
#include <string>
#include <utility>

namespace rmx {

// 集合 = contracts/command_contract.json errorCodes(新增 code 時兩邊同步)
enum class Err {
    kUnsupportedVersion,
    kBadFrame,
    kBadCommand,
    kNotRunning,
    kAlreadyRunning,
    kDeviceOpenFailed,
    kDeviceLost,
    kTrackNotFound,
    kCycleDetected,
    kDeviceBusy,
    kAppNotFound,
    kUnsupportedWindows,
    kPluginNotFound,
    kPluginLoadFailed,
    kPluginNoEditor,
    kParamNotFound,
    kSessionIo,
    kPresetIo,
    kPluginStateFailed,
    kInternal,
};

struct Failure {
    Err code{Err::kInternal};
    std::string message;
};

inline std::optional<Failure> failure(Err code, std::string message) {
    return Failure{code, std::move(message)};
}

}  // namespace rmx
