#include "etw/ZettaEtwPresentAnalyzer.hpp"
#include "ipc/ZettaSharedMemoryManager.hpp"
#include "ipc/ZettaSharedState.h"
#include "telemetry/ZettaTelemetryAggregator.hpp"
#include "tracker/ZettaWindowTracker.hpp"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>

namespace {

std::atomic<DWORD> g_mainThreadId{0};
std::atomic<bool>  g_stopRequested{false};

class ScopedComInitializer {
public:
    ScopedComInitializer() noexcept {
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        initialized_ = SUCCEEDED(hr);
    }

    ~ScopedComInitializer() noexcept {
        if (initialized_) {
            ::CoUninitialize();
        }
    }

    ScopedComInitializer(const ScopedComInitializer&) = delete;
    ScopedComInitializer& operator=(const ScopedComInitializer&) = delete;

private:
    bool initialized_{false};
};

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) noexcept {
    switch (ctrlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT: {
            g_stopRequested.store(true, std::memory_order_release);
            const DWORD threadId = g_mainThreadId.load(std::memory_order_acquire);
            if (threadId != 0) {
                ::PostThreadMessageW(threadId, WM_QUIT, 0, 0);
            }
            return TRUE;
        }
        default:
            return FALSE;
    }
}

void FormatWallClockTimestamp(char (&buffer)[32]) noexcept {
    using namespace std::chrono;
    const auto now        = system_clock::now();
    const auto millisPart = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t tt  = system_clock::to_time_t(now);

    std::tm localTm{};
    localtime_s(&localTm, &tt);

    std::snprintf(
        buffer,
        sizeof(buffer),
        "%02d:%02d:%02d.%03lld",
        localTm.tm_hour,
        localTm.tm_min,
        localTm.tm_sec,
        static_cast<long long>(millisPart.count())
    );
}

void BuildAsciiPulseBar(const zetta::ipc::ZettaFrameHistory& fg, char (&outBar)[13]) noexcept {
    static constexpr const char kLevels[] = "._-=+*#%@";
    constexpr size_t kNumLevels = sizeof(kLevels) - 1U;
    constexpr size_t kBarWidth  = 12U;

    for (size_t i = 0; i < kBarWidth; ++i) {
        const size_t ringIdx = (fg.headIndex + zetta::ipc::kFrameHistorySize - kBarWidth + i) %
                               zetta::ipc::kFrameHistorySize;
        const float ms = fg.samples[ringIdx];
        if (ms <= 0.05f) {
            outBar[i] = '.';
            continue;
        }

        // Normalize [4.0 ms .. 33.3 ms] across the 9 ASCII levels.
        const float norm = std::clamp((ms - 4.0f) / 29.3f, 0.0f, 1.0f);
        const size_t lvl = std::min(
            kNumLevels - 1U,
            static_cast<size_t>(norm * static_cast<float>(kNumLevels - 1U))
        );
        outBar[i] = kLevels[lvl];
    }
    outBar[kBarWidth] = '\0';
}

uint64_t ParseMaxTicksArg(int argc, char* argv[]) noexcept {
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--ticks=", 8) == 0) {
            const long val = std::strtol(argv[i] + 8, nullptr, 10);
            if (val > 0) {
                return static_cast<uint64_t>(val);
            }
        }
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ScopedComInitializer comInit;

    const uint64_t maxTicks = ParseMaxTicksArg(argc, argv);

    g_mainThreadId.store(::GetCurrentThreadId(), std::memory_order_release);
    ::SetConsoleCtrlHandler(&ConsoleCtrlHandler, TRUE);

    MSG initMsg{};
    ::PeekMessageW(&initMsg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    zetta::ipc::ZettaSharedMemoryManager ipcHost(zetta::ipc::SharedMemoryRole::Host);
    if (!ipcHost.Initialize()) {
        std::cerr << "[ERROR] Failed to initialize shared memory host (Win32 error: "
                  << ::GetLastError() << ").\n";
        return 1;
    }

    zetta::etw::ZettaEtwPresentAnalyzer presentAnalyzer;
    presentAnalyzer.Start();

    zetta::telemetry::ZettaTelemetryAggregator telemetry(ipcHost, 500, &presentAnalyzer);
    zetta::tracker::ZettaWindowTracker tracker;

    const bool trackerStarted = tracker.Start([&ipcHost, &telemetry](const zetta::tracker::WindowTargetInfo& info) {
        telemetry.SetTargetPid(info.pid);
        ipcHost.UpdateState([&info](zetta::ipc::ZettaSharedState& state) noexcept {
            state.targetPid = info.pid;
            zetta::ipc::CopyBoundedString(state.targetProcessName, info.processName.c_str());
            state.isGameProcess = info.isGameProcess;
            state.isElevated    = info.isElevated;
            state.isFullscreen  = info.isFullscreen;
            state.windowX       = info.x;
            state.windowY       = info.y;
            state.windowWidth   = info.width;
            state.windowHeight  = info.height;
        });
    });

    if (!trackerStarted) {
        std::cerr << "[ERROR] Failed to install WinEvent hooks (Win32 error: "
                  << ::GetLastError() << ").\n";
        presentAnalyzer.Stop();
        return 1;
    }

    std::cout << "[ZettaHUD v1.0.0] Phase 3 Passive Frame & Hardware Telemetry Daemon\n"
              << "  Capture Engine    : [" << presentAnalyzer.GetCaptureModeTag() << "] "
              << (presentAnalyzer.IsEtwActive()
                      ? "Real-Time ETW Kernel Session (Microsoft-Windows-DXGI + DxgKrnl)"
                      : "Non-Admin DWM Composition Pacing + Module Inspector Fallback")
              << "\n"
              << "  Shared Memory Map : Local\\ZettaTelemetry_SharedMem_v1 ("
              << sizeof(zetta::ipc::ZettaSharedState) << " bytes, 11 active sensor slots + 120-frame ring)\n"
              << "  Polling Interval  : 500ms (Console output every 1000ms)\n"
              << "  Press Ctrl+C to terminate gracefully.\n\n";

    telemetry.Start([maxTicks, &presentAnalyzer](
                        const zetta::telemetry::TelemetrySnapshot& snap,
                        const zetta::ipc::ZettaSharedState& state) {
        if (snap.sampleIndex == 1) {
            std::printf("  GPU Adapter       : %s\n\n", snap.gpuVram.adapterName);
        }

        if (snap.sampleIndex != 1 && (snap.sampleIndex % 2) != 0) {
            return;
        }

        char ts[32]{};
        FormatWallClockTimestamp(ts);

        char pulseBar[13]{};
        BuildAsciiPulseBar(state.frameGraph, pulseBar);

        std::printf(
            "[%s] [%s] PID %-5u %-18s [%-9s] | "
            "FPS: %5.1f (1%%L: %5.1f | %4.1fms) [%s] | "
            "CPU: %4.1f%% | GPU: %4.1f%% | VRAM: %.2fG/%.2fG | RAM: %s | NVMe: %s | Net: %s | Batt: %s\n",
            ts,
            presentAnalyzer.GetCaptureModeTag(),
            state.targetPid,
            state.targetProcessName[0] != '\0' ? state.targetProcessName : "Idle",
            snap.frameStats.apiTagString,
            static_cast<double>(snap.frameStats.avgFps),
            static_cast<double>(snap.frameStats.onePercentLowFps),
            static_cast<double>(snap.frameStats.avgFrametimeMs),
            pulseBar,
            snap.cpuRam.systemCpuPercent,
            snap.gpuVram.gpuLoadPercent,
            snap.gpuVram.dedicatedVramUsedGb,
            snap.gpuVram.sharedVramUsedGb,
            state.metrics[4].formatted,
            state.metrics[6].formatted,
            state.metrics[7].formatted,
            state.metrics[8].formatted
        );
        std::fflush(stdout);

        if (maxTicks > 0 && snap.sampleIndex >= maxTicks) {
            g_stopRequested.store(true, std::memory_order_release);
            const DWORD threadId = g_mainThreadId.load(std::memory_order_acquire);
            if (threadId != 0) {
                ::PostThreadMessageW(threadId, WM_QUIT, 0, 0);
            }
        }
    });

    MSG msg{};
    while (!g_stopRequested.load(std::memory_order_acquire)) {
        const BOOL gmResult = ::GetMessageW(&msg, nullptr, 0, 0);
        if (gmResult <= 0) {
            break;
        }
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    telemetry.Stop();
    presentAnalyzer.Stop();
    tracker.Stop();
    ipcHost.Shutdown();

    std::cout << "\n[ZettaHUD] Phase 3 daemon stopped cleanly. All ETW/DWM, DXGI, PDH, WinEvent, and IPC handles released.\n";
    return 0;
}
