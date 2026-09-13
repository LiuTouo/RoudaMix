// 音訊執行緒進場慣例:denormal 遮罩(FTZ/DAZ)+ 執行緒優先權(MMCSS)。
// denormal:quiet tail(VST3 release、meter 平方累加)會踩 denormal,受罰可達
// 10-100x;FTZ/DAZ 對常規數值 bit-exact,只壓 denormal 路徑,故不動 /fp:fast。
// 優先權:MMCSS "Pro Audio" 為主;查表失敗退 SetThreadPriority。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <avrt.h>
#include <xmmintrin.h>
#include <pmmintrin.h>

#pragma comment(lib, "avrt.lib")

namespace rt {

// denormal 遮罩。成本 = MXCSR 兩次寫;process_buffer 每塊重設 — 外掛若自行
// 清旗標,下一塊補回。thread 進場處只需一次。
inline void denormals_off() noexcept {
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
}

// 執行緒等級:critical = 驅動引擎 RT 的線(ASIO callback thread / WasapiClock
// pump);其餘 capture/render pump 用 normal。每執行緒一次(thread_local 擋
// 重複 syscall);thread 結束 MMCSS 關聯自動失效,不需 RAII 退場。
inline void boost(bool critical) noexcept {
    static thread_local bool done = false;
    if (done) return;
    done = true;
    DWORD idx = 0;
    if (HANDLE h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &idx)) {
        AvSetMmThreadPriority(h, critical ? AVRT_PRIORITY_CRITICAL
                                          : AVRT_PRIORITY_NORMAL);
        return;
    }
    SetThreadPriority(GetCurrentThread(), critical ? THREAD_PRIORITY_TIME_CRITICAL
                                                   : THREAD_PRIORITY_ABOVE_NORMAL);
}

}  // namespace rt
