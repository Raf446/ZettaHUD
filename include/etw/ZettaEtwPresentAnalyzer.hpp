#pragma once

#include "etw/ZettaFrameRingBuffer.hpp"
#include "tracker/ZettaWindowTracker.hpp"

#include <windows.h>
#include <wmistr.h>
#include <evntrace.h>
#include <evntcons.h>
#include <dwmapi.h>
#include <tlhelp32.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <stop_token>
#include <string_view>
#include <thread>

namespace zetta::etw {

enum class CaptureMode : uint8_t {
    Inactive = 0,
    EtwKernel = 1,
    DwmPassiveFallback = 2
};

class ZettaEtwPresentAnalyzer {
public:
    static constexpr const wchar_t* kEtwSessionName = L"ZettaHUD_PassivePresentTrace_v1";

    ZettaEtwPresentAnalyzer() noexcept = default;

    ~ZettaEtwPresentAnalyzer() noexcept {
        Stop();
    }

    ZettaEtwPresentAnalyzer(const ZettaEtwPresentAnalyzer&) = delete;
    ZettaEtwPresentAnalyzer& operator=(const ZettaEtwPresentAnalyzer&) = delete;

    void Start() {
        if (captureMode_.load(std::memory_order_acquire) != CaptureMode::Inactive) {
            return;
        }

        if (TryStartEtwSession()) {
            captureMode_.store(CaptureMode::EtwKernel, std::memory_order_release);
            etwWorkerThread_ = std::jthread([this](std::stop_token stopToken) {
                EtwConsumerLoop(stopToken);
            });
            return;
        }

        captureMode_.store(CaptureMode::DwmPassiveFallback, std::memory_order_release);
        dwmWorkerThread_ = std::jthread([this](std::stop_token stopToken) {
            DwmFallbackLoop(stopToken);
        });
    }

    void Stop() noexcept {
        const CaptureMode mode = captureMode_.exchange(CaptureMode::Inactive, std::memory_order_acq_rel);
        if (mode == CaptureMode::Inactive) {
            return;
        }

        if (dwmWorkerThread_.joinable()) {
            dwmWorkerThread_.request_stop();
            wakeCv_.notify_all();
            dwmWorkerThread_.join();
        }

        if (consumerHandle_ != INVALID_PROCESSTRACE_HANDLE && consumerHandle_ != 0) {
            ::CloseTrace(consumerHandle_);
            consumerHandle_ = INVALID_PROCESSTRACE_HANDLE;
        }

        StopEtwSessionKernel();

        if (etwWorkerThread_.joinable()) {
            etwWorkerThread_.request_stop();
            etwWorkerThread_.join();
        }
    }

    void SetTargetPid(uint32_t pid) noexcept {
        const uint32_t prevPid = targetPid_.exchange(pid, std::memory_order_acq_rel);
        if (pid != prevPid) {
            const GraphicsApiTag tag = InspectProcessGraphicsApi(pid);
            cachedApiTag_.store(tag, std::memory_order_release);
        }
    }

    void SampleFrameStats(FrameStatsSnapshot& outSnapshot) noexcept {
        ringBuffer_.ComputeSnapshot(outSnapshot);
    }

    [[nodiscard]] bool IsEtwActive() const noexcept {
        return captureMode_.load(std::memory_order_acquire) == CaptureMode::EtwKernel;
    }

    [[nodiscard]] bool IsDwmFallbackActive() const noexcept {
        return captureMode_.load(std::memory_order_acquire) == CaptureMode::DwmPassiveFallback;
    }

    [[nodiscard]] const char* GetCaptureModeTag() const noexcept {
        switch (captureMode_.load(std::memory_order_acquire)) {
            case CaptureMode::EtwKernel:          return "ETW_KERNEL";
            case CaptureMode::DwmPassiveFallback: return "DWM_PASSIVE";
            case CaptureMode::Inactive:
            default:                              return "INACTIVE";
        }
    }

private:
    struct TracePropertiesBuffer {
        EVENT_TRACE_PROPERTIES props{};
        wchar_t                sessionName[64]{};
    };

    // Microsoft-Windows-DXGI: {CA11C036-0102-4A2D-A6AD-F03CFED5D3C9}
    static constexpr GUID kDxgiProviderGuid = {
        0xCA11C036U, 0x0102U, 0x4A2DU, {0xA6, 0xAD, 0xF0, 0x3C, 0xFE, 0xD5, 0xD3, 0xC9}
    };

    // Microsoft-Windows-DxgKrnl: {802EC45A-1E99-4B83-9920-87C98277BA9D}
    static constexpr GUID kDxgKrnlProviderGuid = {
        0x802EC45AU, 0x1E99U, 0x4B83U, {0x99, 0x20, 0x87, 0xC9, 0x82, 0x77, 0xBA, 0x9D}
    };

    static void InitializePropertiesBuffer(TracePropertiesBuffer& buf) noexcept {
        std::memset(&buf, 0, sizeof(TracePropertiesBuffer));
        buf.props.Wnode.BufferSize    = sizeof(TracePropertiesBuffer);
        buf.props.Wnode.Flags         = WNODE_FLAG_TRACED_GUID;
        buf.props.Wnode.ClientContext = 1; // QPC clock resolution
        buf.props.LogFileMode         = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_NO_PER_PROCESSOR_BUFFERING;
        buf.props.BufferSize          = 64;
        buf.props.MinimumBuffers      = 4;
        buf.props.MaximumBuffers      = 16;
        buf.props.LoggerNameOffset    = offsetof(TracePropertiesBuffer, sessionName);
    }

    void StopEtwSessionKernel() noexcept {
        TracePropertiesBuffer stopBuf{};
        InitializePropertiesBuffer(stopBuf);
        ::ControlTraceW(sessionHandle_, kEtwSessionName, &stopBuf.props, EVENT_TRACE_CONTROL_STOP);
        sessionHandle_ = 0;
    }

    [[nodiscard]] bool TryStartEtwSession() noexcept {
        // Always clean up any orphaned trace session before starting a fresh one.
        TracePropertiesBuffer cleanupBuf{};
        InitializePropertiesBuffer(cleanupBuf);
        ::ControlTraceW(0, kEtwSessionName, &cleanupBuf.props, EVENT_TRACE_CONTROL_STOP);

        TracePropertiesBuffer startBuf{};
        InitializePropertiesBuffer(startBuf);

        const ULONG startStatus = ::StartTraceW(&sessionHandle_, kEtwSessionName, &startBuf.props);
        if (startStatus != ERROR_SUCCESS) {
            sessionHandle_ = 0;
            return false;
        }

        const ULONG dxgiStatus = ::EnableTraceEx2(
            sessionHandle_,
            &kDxgiProviderGuid,
            EVENT_CONTROL_CODE_ENABLE_PROVIDER,
            TRACE_LEVEL_INFORMATION,
            0x20ULL,
            0,
            0,
            nullptr
        );

        const ULONG dxgkStatus = ::EnableTraceEx2(
            sessionHandle_,
            &kDxgKrnlProviderGuid,
            EVENT_CONTROL_CODE_ENABLE_PROVIDER,
            TRACE_LEVEL_INFORMATION,
            0x1ULL,
            0,
            0,
            nullptr
        );

        if (dxgiStatus != ERROR_SUCCESS && dxgkStatus != ERROR_SUCCESS) {
            StopEtwSessionKernel();
            return false;
        }

        EVENT_TRACE_LOGFILEW logFile{};
        logFile.LoggerName          = const_cast<LPWSTR>(kEtwSessionName);
        logFile.ProcessTraceMode    = PROCESS_TRACE_MODE_REAL_TIME |
                                      PROCESS_TRACE_MODE_EVENT_RECORD |
                                      PROCESS_TRACE_MODE_RAW_TIMESTAMP;
        logFile.EventRecordCallback = &ZettaEtwPresentAnalyzer::StaticEventRecordCallback;
        logFile.Context             = this;

        consumerHandle_ = ::OpenTraceW(&logFile);
        if (consumerHandle_ == INVALID_PROCESSTRACE_HANDLE || consumerHandle_ == 0) {
            consumerHandle_ = INVALID_PROCESSTRACE_HANDLE;
            StopEtwSessionKernel();
            return false;
        }

        return true;
    }

    static void WINAPI StaticEventRecordCallback(PEVENT_RECORD record) noexcept {
        if (record == nullptr || record->UserContext == nullptr) {
            return;
        }
        auto* self = static_cast<ZettaEtwPresentAnalyzer*>(record->UserContext);
        self->OnEventRecord(record);
    }

    void OnEventRecord(PEVENT_RECORD record) noexcept {
        const uint32_t eventPid  = record->EventHeader.ProcessId;
        const uint32_t targetPid = targetPid_.load(std::memory_order_acquire);

        if (targetPid != 0 && eventPid != targetPid) {
            return;
        }

        const USHORT eventId = record->EventHeader.EventDescriptor.Id;
        const GUID& provider = record->EventHeader.ProviderId;

        if (IsEqualGUID(provider, kDxgiProviderGuid)) {
            // Event ID 42 = DXGI Present_Start
            if (eventId == 42) {
                GraphicsApiTag tag = cachedApiTag_.load(std::memory_order_acquire);
                if (tag == GraphicsApiTag::Idle || tag == GraphicsApiTag::DwmComp) {
                    tag = GraphicsApiTag::DxgiDx11;
                }
                ringBuffer_.RecordPresentTimestamp(
                     static_cast<uint64_t>(record->EventHeader.TimeStamp.QuadPart),
                    eventPid,
                    tag
                );
            }
            return;
        }

        if (IsEqualGUID(provider, kDxgKrnlProviderGuid)) {
            // Event ID 184 = DxgKrnl PresentHistory
            if (eventId == 184) {
                const GraphicsApiTag tag = cachedApiTag_.load(std::memory_order_acquire);
                ringBuffer_.RecordPresentTimestamp(
                    static_cast<uint64_t>(record->EventHeader.TimeStamp.QuadPart),
                    eventPid,
                    tag != GraphicsApiTag::Idle ? tag : GraphicsApiTag::DwmComp
                );
            }
        }
    }

    void EtwConsumerLoop(std::stop_token /*stopToken*/) noexcept {
        if (consumerHandle_ != INVALID_PROCESSTRACE_HANDLE && consumerHandle_ != 0) {
            ::ProcessTrace(&consumerHandle_, 1, nullptr, nullptr);
        }
    }

    void DwmFallbackLoop(std::stop_token stopToken) noexcept {
        std::stop_callback stopWake(stopToken, [this]() noexcept {
            wakeCv_.notify_all();
        });

        uint64_t prevRefreshCount = 0;
        uint64_t prevPresentCount = 0;
        bool     hasBaseline      = false;

        while (!stopToken.stop_requested()) {
            // DwmFlush blocks until the next DWM composition VBlank without busy-waiting.
            const HRESULT flushHr = ::DwmFlush();
            if (FAILED(flushHr)) {
                std::unique_lock<std::mutex> lock(wakeMutex_);
                wakeCv_.wait_for(lock, std::chrono::milliseconds(16), [&stopToken]() noexcept {
                    return stopToken.stop_requested();
                });
                if (stopToken.stop_requested()) {
                    break;
                }
            }

            LARGE_INTEGER qpcNow{};
            ::QueryPerformanceCounter(&qpcNow);

            const uint32_t pid = targetPid_.load(std::memory_order_acquire);
            GraphicsApiTag tag = cachedApiTag_.load(std::memory_order_acquire);
            if (tag == GraphicsApiTag::Idle) {
                tag = GraphicsApiTag::DwmComp;
            }

            DWM_TIMING_INFO timingInfo{};
            timingInfo.cbSize = sizeof(DWM_TIMING_INFO);
            if (SUCCEEDED(::DwmGetCompositionTimingInfo(nullptr, &timingInfo))) {
                const uint64_t refreshCount = timingInfo.cRefresh;
                const uint64_t presentCount = timingInfo.cDXPresent;

                if (!hasBaseline) {
                    prevRefreshCount = refreshCount;
                    prevPresentCount = presentCount;
                    hasBaseline      = true;
                    ringBuffer_.RecordPresentTimestamp(
                        static_cast<uint64_t>(qpcNow.QuadPart),
                        pid,
                        tag
                    );
                    continue;
                }

                if (refreshCount > prevRefreshCount || presentCount > prevPresentCount) {
                    prevRefreshCount = refreshCount;
                    prevPresentCount = presentCount;
                    const uint64_t composeQpc = (timingInfo.qpcCompose > 0)
                                                    ? static_cast<uint64_t>(timingInfo.qpcCompose)
                                                    : static_cast<uint64_t>(qpcNow.QuadPart);
                    ringBuffer_.RecordPresentTimestamp(composeQpc, pid, tag);
                }
            } else {
                ringBuffer_.RecordPresentTimestamp(
                    static_cast<uint64_t>(qpcNow.QuadPart),
                    pid,
                    tag
                );
            }
        }
    }

    [[nodiscard]] static bool ContainsCaseInsensitive(
        std::wstring_view haystack,
        std::wstring_view needle
    ) noexcept {
        if (needle.size() > haystack.size()) {
            return false;
        }
        for (size_t i = 0; i <= haystack.size() - needle.size(); ++i) {
            bool match = true;
            for (size_t j = 0; j < needle.size(); ++j) {
                if (std::towlower(haystack[i + j]) != std::towlower(needle[j])) {
                    match = false;
                    break;
                }
            }
            if (match) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] static GraphicsApiTag InspectProcessGraphicsApi(uint32_t pid) noexcept {
        if (pid == 0) {
            return GraphicsApiTag::DwmComp;
        }

        zetta::tracker::ScopedKernelHandle snapshot(
            ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
        );
        if (!snapshot.IsValid()) {
            return GraphicsApiTag::DwmComp;
        }

        MODULEENTRY32W me{};
        me.dwSize = sizeof(MODULEENTRY32W);
        if (!::Module32FirstW(snapshot.Get(), &me)) {
            return GraphicsApiTag::DwmComp;
        }

        bool hasDx12   = false;
        bool hasVulkan = false;
        bool hasDx11   = false;
        bool hasOpenGl = false;

        do {
            const std::wstring_view modName(me.szModule);
            if (ContainsCaseInsensitive(modName, L"vulkan-1.dll") ||
                ContainsCaseInsensitive(modName, L"dxvk")) {
                hasVulkan = true;
                break;
            }
            if (ContainsCaseInsensitive(modName, L"d3d12.dll") ||
                ContainsCaseInsensitive(modName, L"d3d12core.dll")) {
                hasDx12 = true;
            } else if (ContainsCaseInsensitive(modName, L"d3d11.dll")) {
                hasDx11 = true;
            } else if (ContainsCaseInsensitive(modName, L"opengl32.dll")) {
                hasOpenGl = true;
            }
        } while (::Module32NextW(snapshot.Get(), &me));

        if (hasVulkan) return GraphicsApiTag::VulkanDxvk;
        if (hasDx12)   return GraphicsApiTag::Dx12;
        if (hasDx11)   return GraphicsApiTag::DxgiDx11;
        if (hasOpenGl) return GraphicsApiTag::OpenGl;
        return GraphicsApiTag::DwmComp;
    }

    ZettaFrameRingBuffer        ringBuffer_{};
    TRACEHANDLE                 sessionHandle_{0};
    TRACEHANDLE                 consumerHandle_{INVALID_PROCESSTRACE_HANDLE};
    std::atomic<CaptureMode>    captureMode_{CaptureMode::Inactive};
    std::atomic<uint32_t>       targetPid_{0};
    std::atomic<GraphicsApiTag> cachedApiTag_{GraphicsApiTag::DwmComp};

    std::mutex                  wakeMutex_{};
    std::condition_variable     wakeCv_{};
    std::jthread                etwWorkerThread_{};
    std::jthread                dwmWorkerThread_{};
};

} // namespace zetta::etw
