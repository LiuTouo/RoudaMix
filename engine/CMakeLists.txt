cmake_minimum_required(VERSION 3.24)
project(roudamix-engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(RMX_COMMAND_CONTRACT "${CMAKE_CURRENT_SOURCE_DIR}/../contracts/command_contract.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${RMX_COMMAND_CONTRACT}")
file(READ "${RMX_COMMAND_CONTRACT}" RMX_COMMAND_CONTRACT_JSON)
string(LENGTH "${RMX_COMMAND_CONTRACT_JSON}" RMX_COMMAND_CONTRACT_LENGTH)
set(RMX_COMMAND_CONTRACT_LITERALS "")
set(RMX_COMMAND_CONTRACT_OFFSET 0)
while(RMX_COMMAND_CONTRACT_OFFSET LESS RMX_COMMAND_CONTRACT_LENGTH)
    math(EXPR RMX_COMMAND_CONTRACT_REMAINING
         "${RMX_COMMAND_CONTRACT_LENGTH} - ${RMX_COMMAND_CONTRACT_OFFSET}")
    if(RMX_COMMAND_CONTRACT_REMAINING GREATER 8000)
        set(RMX_COMMAND_CONTRACT_CHUNK_LENGTH 8000)
    else()
        set(RMX_COMMAND_CONTRACT_CHUNK_LENGTH ${RMX_COMMAND_CONTRACT_REMAINING})
    endif()
    string(SUBSTRING "${RMX_COMMAND_CONTRACT_JSON}" ${RMX_COMMAND_CONTRACT_OFFSET}
           ${RMX_COMMAND_CONTRACT_CHUNK_LENGTH} RMX_COMMAND_CONTRACT_CHUNK)
    string(APPEND RMX_COMMAND_CONTRACT_LITERALS
           "R\"RMX_CONTRACT(${RMX_COMMAND_CONTRACT_CHUNK})RMX_CONTRACT\"\n")
    math(EXPR RMX_COMMAND_CONTRACT_OFFSET
         "${RMX_COMMAND_CONTRACT_OFFSET} + ${RMX_COMMAND_CONTRACT_CHUNK_LENGTH}")
endwhile()
configure_file(src/command_contract_embed.hpp.in
               "${CMAKE_CURRENT_BINARY_DIR}/generated/command_contract_embed.hpp" @ONLY)

# Portable 發佈不得要求目標電腦先安裝 Visual C++ Redistributable。
# Debug 仍使用 debug CRT；Release 與其他非 Debug 組態使用靜態 release CRT。
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

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
    src/command_contract.cpp
    src/protocol.cpp
    src/frame_io.cpp
    src/pcm_convert.cpp
    src/asio_device.cpp
    src/telemetry.cpp
    src/pdc_planner.cpp
    src/route_planner.cpp
    src/pdc_delay_line.cpp
    src/rt_crossfade.cpp
    src/track_graph.cpp
    src/audio_engine.cpp
    src/app_capture.cpp
    src/render_sink.cpp
    src/vst3_host.cpp
    src/editor_host.cpp
    src/session.cpp
    src/sandbox.cpp
    src/vst_registry.cpp
)
target_include_directories(roudamix-engine PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
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
    src/vst_registry.cpp
)
target_include_directories(roudamix-worker PRIVATE src)
target_include_directories(roudamix-worker SYSTEM PRIVATE third_party)
target_link_libraries(roudamix-worker PRIVATE vst3_sdk user32 ole32)
if(MSVC)
    target_compile_options(roudamix-worker PRIVATE /W4 /utf-8)
endif()

enable_testing()

# 協議 conformance(純 fixture,不依賴 ASIO/Windows SDK)
add_executable(conformance_test tests/conformance_test.cpp src/protocol.cpp src/command_contract.cpp)
target_include_directories(conformance_test PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(conformance_test SYSTEM PRIVATE third_party)
if(MSVC)
    target_compile_options(conformance_test PRIVATE /W4 /utf-8)
endif()
add_test(NAME protocol_conformance
    COMMAND conformance_test "${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/protocol")

add_executable(protocol_probe tests/protocol_probe.cpp src/protocol.cpp src/command_contract.cpp)
target_include_directories(protocol_probe PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(protocol_probe SYSTEM PRIVATE third_party)
if(MSVC)
    target_compile_options(protocol_probe PRIVATE /W4 /utf-8)
endif()
add_test(NAME protocol_parity
    COMMAND node "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/protocol-parity.mjs")

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
    src/pdc_planner.cpp src/route_planner.cpp src/pdc_delay_line.cpp src/rt_crossfade.cpp src/track_graph.cpp src/app_capture.cpp src/render_sink.cpp
    src/asio_device.cpp src/pcm_convert.cpp src/telemetry.cpp src/vst3_host.cpp
    src/sandbox.cpp)
target_include_directories(session_test PRIVATE src)
target_include_directories(session_test SYSTEM PRIVATE third_party)
target_link_libraries(session_test PRIVATE asio_sdk vst3_sdk user32 ole32 mmdevapi psapi)
if(MSVC)
    target_compile_options(session_test PRIVATE /W4 /utf-8)
endif()
add_test(NAME session COMMAND session_test)

# track graph 純邏輯(system role 補齊/去重、ASIO channel 聯集;無音訊裝置)
add_executable(track_graph_test tests/track_graph_test.cpp src/track_graph.cpp)
target_include_directories(track_graph_test PRIVATE src)
target_include_directories(track_graph_test SYSTEM PRIVATE third_party)
if(MSVC)
    target_compile_options(track_graph_test PRIVATE /W4 /utf-8)
endif()
add_test(NAME track_graph COMMAND track_graph_test)

# Plugin Delay Compensation planner:純 DAG/資源規劃，不依賴 audio device/VST。
add_executable(pdc_planner_test tests/pdc_planner_test.cpp src/pdc_planner.cpp)
target_include_directories(pdc_planner_test PRIVATE src)
if(MSVC)
    target_compile_options(pdc_planner_test PRIVATE /W4 /utf-8)
endif()
add_test(NAME pdc_planner COMMAND pdc_planner_test)

# RoutePlan:slot/bus/shadow/dry-delay 決策；純 value，不依賴 audio device/VST。
add_executable(route_planner_test tests/route_planner_test.cpp
    src/route_planner.cpp src/pdc_planner.cpp)
target_include_directories(route_planner_test PRIVATE src)
if(MSVC)
    target_compile_options(route_planner_test PRIVATE /W4 /utf-8)
endif()
add_test(NAME route_planner COMMAND route_planner_test)

add_executable(pdc_delay_line_test tests/pdc_delay_line_test.cpp src/pdc_delay_line.cpp)
target_include_directories(pdc_delay_line_test PRIVATE src)
if(MSVC)
    target_compile_options(pdc_delay_line_test PRIVATE /W4 /utf-8)
endif()
add_test(NAME pdc_delay_line COMMAND pdc_delay_line_test)

add_executable(rt_crossfade_test tests/rt_crossfade_test.cpp src/rt_crossfade.cpp)
target_include_directories(rt_crossfade_test PRIVATE src)
if(MSVC)
    target_compile_options(rt_crossfade_test PRIVATE /W4 /utf-8)
endif()
add_test(NAME rt_crossfade COMMAND rt_crossfade_test)

add_executable(pdc_audio_path_test tests/pdc_audio_path_test.cpp
    src/route_planner.cpp src/pdc_planner.cpp src/pdc_delay_line.cpp)
target_include_directories(pdc_audio_path_test PRIVATE src)
if(MSVC)
    target_compile_options(pdc_audio_path_test PRIVATE /W4 /utf-8)
endif()
add_test(NAME pdc_audio_path COMMAND pdc_audio_path_test)

add_executable(telemetry_test tests/telemetry_test.cpp src/telemetry.cpp)
target_include_directories(telemetry_test PRIVATE src)
if(MSVC)
    target_compile_options(telemetry_test PRIVATE /W4 /utf-8)
endif()
add_test(NAME telemetry COMMAND telemetry_test)

# Deterministic test-only VST3：可切 latency、送 kLatencyChanged、耗 CPU、process fail。
add_library(rmx_latency_fixture MODULE
    tests/latency_fixture_vst3.cpp
    ${VST3_SDK_DIR}/base/source/fobject.cpp
    ${VST3_SDK_DIR}/base/source/fstring.cpp
    ${VST3_SDK_DIR}/base/source/updatehandler.cpp
    ${VST3_SDK_DIR}/base/thread/source/flock.cpp
    ${VST3_SDK_DIR}/public.sdk/source/common/pluginview.cpp
    ${VST3_SDK_DIR}/public.sdk/source/main/dllmain.cpp
    ${VST3_SDK_DIR}/public.sdk/source/main/pluginfactory.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/vstaudioeffect.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/vstbus.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/vstcomponent.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/vstcomponentbase.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/vsteditcontroller.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/vstparameters.cpp)
target_include_directories(rmx_latency_fixture SYSTEM PRIVATE ${VST3_SDK_DIR})
target_link_libraries(rmx_latency_fixture PRIVATE vst3_sdk)
set_target_properties(rmx_latency_fixture PROPERTIES PREFIX "" SUFFIX ".vst3")
if(MSVC)
    target_compile_options(rmx_latency_fixture PRIVATE /W0 /utf-8 /Zc:char8_t-)
endif()

add_executable(latency_fixture_test tests/latency_fixture_test.cpp src/vst3_host.cpp)
target_include_directories(latency_fixture_test PRIVATE src)
target_link_libraries(latency_fixture_test PRIVATE vst3_sdk user32)
add_dependencies(latency_fixture_test rmx_latency_fixture)
if(MSVC)
    target_compile_options(latency_fixture_test PRIVATE /W4 /utf-8)
endif()
add_test(NAME latency_fixture COMMAND latency_fixture_test $<TARGET_FILE:rmx_latency_fixture>)

# VST registry 純檔案邏輯：fingerprint、增量命中、去重、原子持久化。
add_executable(vst_registry_test tests/vst_registry_test.cpp src/vst_registry.cpp)
target_include_directories(vst_registry_test PRIVATE src)
target_include_directories(vst_registry_test SYSTEM PRIVATE third_party)
if(MSVC)
    target_compile_options(vst_registry_test PRIVATE /W4 /utf-8)
endif()
add_test(NAME vst_registry COMMAND vst_registry_test)
