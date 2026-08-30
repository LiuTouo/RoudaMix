cmake_minimum_required(VERSION 3.24)
project(roudamix-engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ASIO_SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio-sdk)

# Steinberg ASIO SDK(只編 host 端 asiolist.cpp;/W0 壓 SDK 警告)
add_library(asio_sdk STATIC ${ASIO_SDK_DIR}/host/pc/asiolist.cpp)
target_include_directories(asio_sdk SYSTEM PUBLIC
    ${ASIO_SDK_DIR} ${ASIO_SDK_DIR}/common ${ASIO_SDK_DIR}/host ${ASIO_SDK_DIR}/host/pc)
target_link_libraries(asio_sdk PRIVATE ole32)
if(MSVC)
    target_compile_options(asio_sdk PRIVATE /W0)
endif()

# Steinberg VST3 SDK(hosting 子集,自 ProMixArea 縮編;/W0 壓 SDK 警告)
set(VST3_SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vst3-sdk)
add_library(vst3_sdk STATIC
    ${VST3_SDK_DIR}/base/source/baseiids.cpp
    ${VST3_SDK_DIR}/pluginterfaces/base/funknown.cpp
    ${VST3_SDK_DIR}/pluginterfaces/base/coreiids.cpp
    ${VST3_SDK_DIR}/pluginterfaces/base/conststringtable.cpp
    ${VST3_SDK_DIR}/pluginterfaces/base/ustring.cpp
    ${VST3_SDK_DIR}/public.sdk/source/common/commoniids.cpp
    ${VST3_SDK_DIR}/public.sdk/source/common/commonstringconvert.cpp
    ${VST3_SDK_DIR}/public.sdk/source/common/memorystream.cpp
    ${VST3_SDK_DIR}/public.sdk/source/common/threadchecker_win32.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/vstinitiids.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/connectionproxy.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/hostclasses.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_win32.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/parameterchanges.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/processdata.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/utility/stringconvert.cpp
)
target_include_directories(vst3_sdk SYSTEM PUBLIC ${VST3_SDK_DIR})
if(MSVC)
    target_compile_options(vst3_sdk PRIVATE /W0 /Zc:char8_t-)
endif()

add_executable(roudamix-engine
    main.cpp
    src/protocol.cpp
    src/frame_io.cpp
    src/pcm_convert.cpp
    src/asio_device.cpp
    src/telemetry.cpp
    src/track_graph.cpp
    src/audio_engine.cpp
    src/app_capture.cpp
    src/render_sink.cpp
    src/vst3_host.cpp
    src/editor_host.cpp
    src/session.cpp
    src/sandbox.cpp
)
target_include_directories(roudamix-engine PRIVATE src)
target_include_directories(roudamix-engine SYSTEM PRIVATE third_party)
target_link_libraries(roudamix-engine PRIVATE asio_sdk vst3_sdk user32 ole32 gdi32 gdiplus dwmapi mmdevapi psapi)
if(MSVC)
    target_compile_options(roudamix-engine PRIVATE /W4 /utf-8)
    # 診斷用:SEH address 反查(roudamix-engine.map)
    target_compile_options(roudamix-engine PRIVATE /Zi)
    target_link_options(roudamix-engine PRIVATE /MAP /DEBUG)
endif()

# 隔離試爆 worker:module 掃描/載入在獨立 process 試爆,壞 DLL 污染不了 engine
add_executable(roudamix-worker
    worker_main.cpp
    src/vst3_host.cpp
)
target_include_directories(roudamix-worker PRIVATE src)
target_include_directories(roudamix-worker SYSTEM PRIVATE third_party)
target_link_libraries(roudamix-worker PRIVATE vst3_sdk user32 ole32)
if(MSVC)
    target_compile_options(roudamix-worker PRIVATE /W4 /utf-8)
endif()

enable_testing()

# 協議 conformance(純 fixture,不依賴 ASIO/Windows SDK)
add_executable(conformance_test tests/conformance_test.cpp src/protocol.cpp)
target_include_directories(conformance_test PRIVATE src)
target_include_directories(conformance_test SYSTEM PRIVATE third_party)
if(MSVC)
    target_compile_options(conformance_test PRIVATE /W4 /utf-8)
endif()
add_test(NAME protocol_conformance
    COMMAND conformance_test "${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/protocol")

# 轉換表 round-trip + meter accumulate 離線測試
add_executable(audio_test tests/audio_test.cpp src/pcm_convert.cpp)
target_include_directories(audio_test PRIVATE src)
if(MSVC)
    target_compile_options(audio_test PRIVATE /W4 /utf-8)
endif()
add_test(NAME audio_convert_roundtrip COMMAND audio_test)

# VST3 host 縮編:scan 壞路徑必敗;本機有 plugin 才 load+process(否則 SKIP)
add_executable(vst3_host_test tests/vst3_host_test.cpp src/vst3_host.cpp)
target_include_directories(vst3_host_test PRIVATE src)
target_link_libraries(vst3_host_test PRIVATE vst3_sdk user32)
if(MSVC)
    target_compile_options(vst3_host_test PRIVATE /W4 /utf-8)
endif()
add_test(NAME vst3_host COMMAND vst3_host_test)

# session 檔案序列化/載入(無 plugin 環境;真 plugin roundtrip 走 m5a-engine-tracks.ps1)
add_executable(session_test tests/session_test.cpp src/session.cpp src/audio_engine.cpp
    src/track_graph.cpp src/app_capture.cpp src/render_sink.cpp
    src/asio_device.cpp src/pcm_convert.cpp src/telemetry.cpp src/vst3_host.cpp)
target_include_directories(session_test PRIVATE src)
target_include_directories(session_test SYSTEM PRIVATE third_party)
target_link_libraries(session_test PRIVATE asio_sdk vst3_sdk user32 ole32 mmdevapi psapi)
if(MSVC)
    target_compile_options(session_test PRIVATE /W4 /utf-8)
endif()
add_test(NAME session COMMAND session_test)
