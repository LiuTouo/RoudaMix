// roudamix-engine — pipe transport、單一 instance、client 斷線續跑等重連。
// Engine command 語意、臨界區、revision/event/reply envelope 全由 Router 擁有。
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <optional>
#include <thread>

#include "editor_host.hpp"
#include "frame_io.hpp"
#include "protocol.hpp"
#include "router.hpp"

namespace {

using rmx::Command;
using rmx::Frame;

std::atomic<bool> g_exiting{false};
std::atomic<ULONGLONG> g_last_activity{GetTickCount64()};
rmx::Router g_router;

constexpr UINT WM_APP_TASK = WM_APP + 1;
constexpr UINT WM_APP_QUIT = WM_APP + 2;
constexpr UINT WM_APP_CAPTURE_ERR = WM_APP + 4;
constexpr UINT WM_APP_LATENCY_CHANGED = WM_APP + 5;
constexpr wchar_t kMainWndClass[] = L"RmxEngineMain";
HWND g_main_hwnd = nullptr;

std::mutex g_write_mutex;
std::atomic<HANDLE> g_active_pipe{nullptr};
std::atomic<std::uint64_t> g_conn_gen{0};

struct Task {
    std::uint64_t generation;
    Command command;
};

void send_frame(HANDLE client, const nlohmann::json& frame) {
    std::lock_guard<std::mutex> lock(g_write_mutex);
    if (!rmx::write_frame(client, frame))
        std::fprintf(stderr, "[engine] send_frame write failed: %lu\n", GetLastError());
}

struct DispatchSehResult {
    bool shutdown{};
    DWORD exception_code{};
};

DispatchSehResult dispatch_seh_raw(std::uint64_t generation,
                                   const Command* command) {
    DispatchSehResult result;
    __try {
        result.shutdown = g_router.dispatch_guarded(generation, *command);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result.exception_code = GetExceptionCode();
    }
    return result;
}

bool dispatch_seh(std::uint64_t generation, const Command& command) {
    const auto result = dispatch_seh_raw(generation, &command);
    if (result.exception_code != 0) {
        std::fprintf(stderr, "[engine] SEH exception while dispatching %s: %lX\n",
                     command.kind.c_str(), result.exception_code);
        char message[64]{};
        std::snprintf(message, sizeof(message), "structured exception 0x%08lX",
                      result.exception_code);
        g_router.recover_from_structured_exception(generation, command.id, message);
    }
    return result.shutdown;
}

LRESULT CALLBACK main_wnd_proc(HWND window, UINT message, WPARAM word_param,
                               LPARAM long_param) noexcept {
    if (message == WM_APP_TASK) {
        auto* task = reinterpret_cast<Task*>(long_param);
        if (task != nullptr) {
            if (task->generation != g_conn_gen.load(std::memory_order_acquire)) {
                delete task;
                return 0;
            }
            if (dispatch_seh(task->generation, task->command))
                PostMessageW(window, WM_APP_QUIT, 0, 0);
            delete task;
        }
        return 0;
    }
    if (message == rmx::WM_APP_HOSTCMD) {
        auto* command = reinterpret_cast<rmx::EditorHostCmd*>(long_param);
        if (command != nullptr) {
            g_router.handle_host_command(*command);
            delete command;
        }
        return 0;
    }
    if (message == WM_APP_CAPTURE_ERR) {
        g_router.handle_track_failed(static_cast<std::uint32_t>(word_param));
        return 0;
    }
    if (message == WM_APP_LATENCY_CHANGED) {
        g_router.handle_latency_changed(static_cast<std::uint32_t>(word_param),
                                        long_param != 0);
        return 0;
    }
    if (message == WM_APP_QUIT) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, word_param, long_param);
}

void serve_client(HANDLE pipe, std::uint64_t generation) {
    g_last_activity.store(GetTickCount64());
    g_router.connect(generation,
                     [pipe](const nlohmann::json& frame) { send_frame(pipe, frame); });
    for (;;) {
        std::optional<std::vector<std::uint8_t>> frame;
        try {
            frame = rmx::read_frame(pipe);
        } catch (const rmx::FrameIoError& error) {
            std::fprintf(stderr, "[engine] bad frame: %s\n", error.what());
            break;
        }
        if (!frame) break;
        g_last_activity.store(GetTickCount64());
        const auto json = nlohmann::json::parse(*frame, nullptr, false);
        if (json.is_discarded()) {
            std::fprintf(stderr, "[engine] invalid JSON payload\n");
            break;
        }
        try {
            Frame parsed = rmx::parse_frame(json, false);
            if (auto* command = std::get_if<Command>(&parsed)) {
                auto* task = new Task{generation, std::move(*command)};
                if (!PostMessageW(g_main_hwnd, WM_APP_TASK, 0,
                                  reinterpret_cast<LPARAM>(task))) {
                    delete task;
                    break;
                }
            }
        } catch (const rmx::ParseError& error) {
            g_router.send_protocol_error(generation, 0, error.code, error.what());
        }
    }
}

void pipe_serve_inner() {
    while (!g_exiting.load()) {
        HANDLE pipe = CreateNamedPipeW(
            L"\\\\.\\pipe\\roudamix-engine",
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_READMODE_BYTE | PIPE_WAIT, 1, 64 * 1024, 64 * 1024, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            std::fprintf(stderr, "[engine] CreateNamedPipeW failed: %lu\n", GetLastError());
            break;
        }
        g_active_pipe.store(pipe);
        OVERLAPPED connection{};
        connection.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BOOL connected = FALSE;
        if (ConnectNamedPipe(pipe, &connection) ||
            GetLastError() == ERROR_PIPE_CONNECTED) {
            connected = TRUE;
        } else if (GetLastError() == ERROR_IO_PENDING && connection.hEvent != nullptr) {
            while (!g_exiting.load()) {
                if (WaitForSingleObject(connection.hEvent, 500) == WAIT_OBJECT_0) {
                    DWORD transferred = 0;
                    if (GetOverlappedResult(pipe, &connection, &transferred, FALSE))
                        connected = TRUE;
                    break;
                }
            }
        }
        if (connection.hEvent != nullptr) CloseHandle(connection.hEvent);
        if (!connected) {
            g_active_pipe.store(nullptr);
            CloseHandle(pipe);
            continue;
        }

        std::printf("[engine] client connected\n");
        const auto generation = g_conn_gen.fetch_add(1) + 1;
        __try {
            serve_client(pipe, generation);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            std::fprintf(stderr, "[engine] SEH while serving client: %lX\n",
                         GetExceptionCode());
        }
        g_router.disconnect(generation);
        g_active_pipe.store(nullptr);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        g_last_activity.store(GetTickCount64());
        std::printf("[engine] client disconnected\n");
    }
}

void idle_watchdog() {
    constexpr ULONGLONG kIdleTimeoutMs = 12ull * 60 * 60 * 1000;
    for (;;) {
        if (g_exiting.load()) return;
        for (int index = 0; index < 240 && !g_exiting.load(); ++index) Sleep(250);
        if (g_exiting.load()) return;
        if (GetTickCount64() - g_last_activity.load() > kIdleTimeoutMs) {
            if (HANDLE pipe = g_active_pipe.load(); pipe != nullptr)
                CancelIoEx(pipe, nullptr);
            PostMessageW(g_main_hwnd, WM_APP_QUIT, 0, 0);
            return;
        }
    }
}

}  // namespace

int main() {
    HANDLE singleton = CreateMutexW(nullptr, TRUE, rmx::kSingletonMutex);
    if (singleton == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
        std::fprintf(stderr, "[engine] another instance already running, exiting\n");
        return 0;
    }

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
        std::fprintf(stderr, "[engine] CoInitializeEx failed\n");

    g_router.load_registry();

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &main_wnd_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = kMainWndClass;
    RegisterClassExW(&window_class);
    g_main_hwnd = CreateWindowExW(0, kMainWndClass, L"", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, nullptr, window_class.hInstance, nullptr);
    std::fprintf(stderr, "[engine] main window hwnd=%p\n",
                 static_cast<void*>(g_main_hwnd));

    g_router.set_editor_command_target(g_main_hwnd);
    g_router.set_capture_failed_callback([](std::uint32_t track_id) {
        PostMessageW(g_main_hwnd, WM_APP_CAPTURE_ERR, track_id, 0);
    });
    g_router.set_latency_changed_callback(
        [](std::uint32_t instance_id, bool monitor_shadow) {
            PostMessageW(g_main_hwnd, WM_APP_LATENCY_CHANGED, instance_id,
                         monitor_shadow ? 1 : 0);
        });

    std::thread pipe_thread(pipe_serve_inner);
    std::thread watchdog(idle_watchdog);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    g_exiting.store(true);
    g_router.shutdown();
    Sleep(150);
    if (HANDLE pipe = g_active_pipe.load(); pipe != nullptr) CancelIoEx(pipe, nullptr);
    if (pipe_thread.joinable()) pipe_thread.join();
    if (watchdog.joinable()) watchdog.join();
    if (g_main_hwnd != nullptr) DestroyWindow(g_main_hwnd);
    CoUninitialize();
    ReleaseMutex(singleton);
    CloseHandle(singleton);
    return 0;
}
