// pipe transport 認證 prelude 單元測試(issue #16):真 pipe pair 驅動
// auth_client(逾時讀 + 常數時間比較 + ack),以及 secret_matches / hex_decode
// 純函數案例、FIRST_PIPE_INSTANCE 名稱佔位(命名空間碰撞 fail closed)。
// 對 remediation 案例的映射:secret 不符 → 拒絕且不回 ok、未認證 client
// 卡住不送話 → bounded timeout、碰撞建立 → 失敗。
// 遠端(REJECT_REMOTE_CLIENTS)無法單機重現,由 engine main.cpp 的旗標保證。
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "frame_io.hpp"

#define CHECK(x)                                                                  \
    do {                                                                          \
        if (!(x)) {                                                               \
            std::fprintf(stderr, "CHECK FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            std::abort();                                                         \
        }                                                                         \
    } while (0)

namespace {

// 建 overlapped duplex pipe pair:server 帶 production 同款旗標
// (FIRST_PIPE_INSTANCE + REJECT_REMOTE_CLIENTS),client 以 overlapped 開。
struct PipePair {
    HANDLE server{};
    HANDLE client{};
    ~PipePair() {
        if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
        if (server != INVALID_HANDLE_VALUE) CloseHandle(server);
    }
};

PipePair make_pipe(const wchar_t* name) {
    PipePair p;
    p.server = CreateNamedPipeW(
        name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0,
        nullptr);
    CHECK(p.server != INVALID_HANDLE_VALUE);
    OVERLAPPED connect{};
    connect.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CHECK(connect.hEvent != nullptr);
    // overlapped:FALSE + ERROR_IO_PENDING = 連線等待中;ERROR_PIPE_CONNECTED =
    // client 已搶先接上(直接算成功)。錯誤碼須立刻取用 —— 下面的 CreateFileW
    // 會蓋掉 thread 的 LastError。
    const BOOL connected_ret = ConnectNamedPipe(p.server, &connect);
    const DWORD connect_err = connected_ret ? ERROR_SUCCESS : GetLastError();
    CHECK(connect_err == ERROR_IO_PENDING || connect_err == ERROR_PIPE_CONNECTED);
    p.client = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, nullptr);
    CHECK(p.client != INVALID_HANDLE_VALUE);
    if (connect_err == ERROR_IO_PENDING) {
        CHECK(WaitForSingleObject(connect.hEvent, 5000) == WAIT_OBJECT_0);
    }
    CloseHandle(connect.hEvent);
    return p;
}

// client 端寫 n bytes(overlapped,等完成)。
void client_write(HANDLE client, const void* data, size_t n) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CHECK(ov.hEvent != nullptr);
    DWORD written = 0;
    CHECK(WriteFile(client, data, static_cast<DWORD>(n), &written, &ov) ||
          (GetLastError() == ERROR_IO_PENDING &&
           WaitForSingleObject(ov.hEvent, 5000) == WAIT_OBJECT_0));
    CHECK(GetOverlappedResult(client, &ov, &written, FALSE));
    CHECK(written == n);
    CloseHandle(ov.hEvent);
}

// client 端讀 1 byte ack(overlapped;逾時回 0xFF)。
int client_read_ack(HANDLE client) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CHECK(ov.hEvent != nullptr);
    uint8_t ack = 0xFF;
    DWORD read = 0;
    if (ReadFile(client, &ack, 1, &read, &ov)) {
        // 同步完成
    } else if (GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, 5000) != WAIT_OBJECT_0) {
            CancelIoEx(client, &ov);
            CloseHandle(ov.hEvent);
            return 0xFF;
        }
        if (!GetOverlappedResult(client, &ov, &read, FALSE)) {
            CloseHandle(ov.hEvent);
            return 0xFF;
        }
    } else {
        CloseHandle(ov.hEvent);
        return 0xFF;
    }
    CloseHandle(ov.hEvent);
    return read == 1 ? ack : 0xFF;
}

void test_secret_matches() {
    const uint8_t a[] = {1, 2, 3, 4};
    const uint8_t b[] = {1, 2, 3, 4};
    const uint8_t c[] = {1, 2, 3, 5};
    const uint8_t d[] = {1, 2, 3, 4, 5};
    CHECK(rmx::secret_matches(a, 4, b, 4));
    CHECK(!rmx::secret_matches(a, 4, c, 4));   // 單 bit 差即拒
    CHECK(!rmx::secret_matches(a, 4, d, 5));   // 長度差即拒
    CHECK(!rmx::secret_matches(nullptr, 0, a, 4));
    CHECK(rmx::secret_matches(nullptr, 0, nullptr, 0));
    std::printf("ok secret_matches\n");
}

void test_hex_decode() {
    CHECK((rmx::hex_decode("") == std::vector<uint8_t>{}));
    CHECK((rmx::hex_decode("00ff7F") == std::vector<uint8_t>{0x00, 0xFF, 0x7F}));
    CHECK((rmx::hex_decode("deadBEEF") == std::vector<uint8_t>{0xDE, 0xAD, 0xBE, 0xEF}));
    CHECK(!rmx::hex_decode("abc").has_value());    // 奇數長
    CHECK(!rmx::hex_decode("zz").has_value());     // 非 hex
    std::printf("ok hex_decode\n");
}

void test_auth_ok() {
    PipePair p = make_pipe(L"\\\\.\\pipe\\roudamix-test-frameio-ok");
    const std::vector<uint8_t> secret = {0x11, 0x22, 0x33};
    client_write(p.client, secret.data(), secret.size());
    CHECK(rmx::auth_client(p.server, secret, 5000));
    CHECK(client_read_ack(p.client) == 1);
    std::printf("ok auth_ok\n");
}

void test_auth_wrong_secret_rejected() {
    PipePair p = make_pipe(L"\\\\.\\pipe\\roudamix-test-frameio-bad");
    const std::vector<uint8_t> secret = {0x11, 0x22, 0x33};
    const uint8_t forged[] = {0x11, 0x22, 0x34};
    client_write(p.client, forged, sizeof(forged));
    CHECK(!rmx::auth_client(p.server, secret, 5000));
    CHECK(client_read_ack(p.client) == 0);
    std::printf("ok auth_wrong_secret_rejected\n");
}

void test_auth_stall_times_out() {
    PipePair p = make_pipe(L"\\\\.\\pipe\\roudamix-test-frameio-stall");
    const std::vector<uint8_t> secret = {0x11, 0x22, 0x33};
    // client 連上但不送話:auth_client 須在 timeout 內放棄,不是永等
    const auto started = std::chrono::steady_clock::now();
    CHECK(!rmx::auth_client(p.server, secret, 300));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    CHECK(ms >= 250 && ms < 3000);
    std::printf("ok auth_stall_times_out (%lld ms)\n", static_cast<long long>(ms));
}

void test_first_instance_collision_fails() {
    const wchar_t* name = L"\\\\.\\pipe\\roudamix-test-frameio-collision";
    PipePair p = make_pipe(name);
    HANDLE second = CreateNamedPipeW(
        name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0,
        nullptr);
    CHECK(second == INVALID_HANDLE_VALUE);
    // 實測 Win11 回 ERROR_PIPE_BUSY(MSDN 寫 ERROR_ACCESS_DENIED);
    // 重點是「同名第二個 FIRST instance 建立必敗」= engine fail closed
    {
        const DWORD err = GetLastError();
        CHECK(err == ERROR_ACCESS_DENIED || err == ERROR_PIPE_BUSY);
    }
    std::printf("ok first_instance_collision_fails\n");
}

}  // namespace

int main() {
    test_secret_matches();
    test_hex_decode();
    test_auth_ok();
    test_auth_wrong_secret_rejected();
    test_auth_stall_times_out();
    test_first_instance_collision_fails();
    std::printf("frame_io_test: all passed\n");
    return 0;
}
