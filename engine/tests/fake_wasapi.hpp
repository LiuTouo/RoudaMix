// Test target only: replace Windows audio activation, keeping production pumps intact.
#pragma once
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <atomic>
#include <string>

namespace fake_wasapi {
extern std::atomic<bool> lost;
extern std::atomic<bool> fail_start;
extern std::atomic<bool> fail_buffer_size;
extern std::atomic<bool> stall_events;
extern std::atomic<int> clients;
extern std::wstring fail_endpoint;
float peak(const std::wstring& endpoint);
void clear_samples();
HRESULT WINAPI create(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
HRESULT WINAPI activate(LPCWSTR, REFIID, PROPVARIANT*,
    IActivateAudioInterfaceCompletionHandler*, IActivateAudioInterfaceAsyncOperation**);
HANDLE WINAPI mapping(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCWSTR);
}
#define CoCreateInstance fake_wasapi::create
#define ActivateAudioInterfaceAsync fake_wasapi::activate
#define CreateFileMappingW fake_wasapi::mapping
