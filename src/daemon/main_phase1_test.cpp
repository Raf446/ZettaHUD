#include "ipc/ZettaSharedMemoryManager.hpp"
#include "ipc/ZettaSharedState.h"
#include "tracker/ZettaWindowTracker.hpp"

#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <string>

namespace {

std::atomic<DWORD> g_mainThreadId{0};
std::atomic<bool>  g_stopRequested{false};

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) noexcept {
    switch (ctrlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT: {
            g_stopRequested.store(true, std::memory_order_release);
            const DWORD threadId = g_mainThreadId.load(std::memory_order_acquire);
            if (threadId != 0) {
                // Wake the blocked GetMessageW loop on the main thread without polling.
                ::PostThreadMessageW(threadId, WM_QUIT, 0, 0);
            }
            return TRUE;
        }
        default:
            return FALSE;
    }
}

std::string FormatWallClockTimestamp() {
    using namespace std::chrono;
    const auto now        = system_clock::now();
    const auto millisPart = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t tt  = system_clock::to_time_t(now);

    std::tm localTm{};
    localtime_s(&localTm, &tt);

    char buffer[32]{};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%02d:%02d:%02d.%03lld",
        localTm.tm_hour,
        localTm.tm_min,
        localTm.tm_sec,
        static_cast<long long>(millisPart.count())
    );
    return std::string(buffer);
}

void SeedInitialMetrics(zetta::ipc::ZettaSharedState& state) noexcept {
    state.metricCount = 3;

    zetta::ipc::CopyBoundedString(state.metrics[0].id, "fps.avg");
    zetta::ipc::CopyBoundedString(state.metrics[0].label, "FPS");
    state.metrics[0].numericValue = 0.0;
    zetta::ipc::CopyBoundedString(state.metrics[0].formatted, "-- FPS");
    state.metrics[0].colorHex  = 0xFF3FB950U;
    state.metrics[0].isVisible = true;

    zetta::ipc::CopyBoundedString(state.metrics[1].id, "frametime.ms");
    zetta::ipc::CopyBoundedString(state.metrics[1].label, "FRAME");
    state.metrics[1].numericValue = 0.0;
    zetta::ipc::CopyBoundedString(state.metrics[1].formatted, "-- ms");
    state.metrics[1].colorHex  = 0xFF58A6FFU;
    state.metrics[1].isVisible = true;

    zetta::ipc::CopyBoundedString(state.metrics[2].id, "gpu.utilization");
    zetta::ipc::CopyBoundedString(state.metrics[2].label, "GPU");
    state.metrics[2].numericValue = 0.0;
    zetta::ipc::CopyBoundedString(state.metrics[2].formatted, "-- %");
    state.metrics[2].colorHex  = 0xFFA371F7U;
    state.metrics[2].isVisible = true;
}

} // namespace

int main() {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    g_mainThreadId.store(::GetCurrentThreadId(), std::memory_order_release);
    ::SetConsoleCtrlHandler(&ConsoleCtrlHandler, TRUE);

    // Ensure message queue exists on this thread prior to hook or PostThreadMessageW delivery.
    MSG initMsg{};
    ::PeekMessageW(&initMsg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    std::cout << "[ZettaHUD v1.0.0] Phase 1 Diagnostic Harness\n"
              << "  Shared Memory Map : Local\\ZettaTelemetry_SharedMem_v1 ("
              << sizeof(zetta::ipc::ZettaSharedState) << " bytes, 1-byte packed)\n"
              << "  DPI Context       : PerMonitorV2\n"
              << "  Press Ctrl+C to terminate gracefully.\n\n";

    zetta::ipc::ZettaSharedMemoryManager ipcHost(zetta::ipc::SharedMemoryRole::Host);
    if (!ipcHost.Initialize()) {
        std::cerr << "[ERROR] Failed to initialize shared memory host (Win32 error: "
                  << ::GetLastError() << ").\n";
        return 1;
    }

    ipcHost.UpdateState([](zetta::ipc::ZettaSharedState& state) noexcept {
        SeedInitialMetrics(state);
    });

    zetta::tracker::ZettaWindowTracker tracker;
    const bool started = tracker.Start([&ipcHost](const zetta::tracker::WindowTargetInfo& info) {
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

        const std::string timestamp = FormatWallClockTimestamp();
        std::printf(
            "[%s] PID: %-6u | %-28s | State: %-10s | Bounds: (%d, %d, %dx%d) | Elevated: %s\n",
            timestamp.c_str(),
            info.pid,
            info.processName.c_str(),
            zetta::tracker::ZettaWindowTracker::StateToString(info.state),
            info.x,
            info.y,
            info.width,
            info.height,
            info.isElevated ? "YES" : "NO"
        );
        std::fflush(stdout);
    });

    if (!started) {
        std::cerr << "[ERROR] Failed to install WinEvent hooks (Win32 error: "
                  << ::GetLastError() << ").\n";
        return 1;
    }

    MSG msg{};
    while (!g_stopRequested.load(std::memory_order_acquire)) {
        const BOOL gmResult = ::GetMessageW(&msg, nullptr, 0, 0);
        if (gmResult <= 0) {
            break;
        }
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    tracker.Stop();
    ipcHost.Shutdown();

    std::cout << "\n[ZettaHUD] Clean shutdown complete. All kernel handles and WinEvent hooks released.\n";
    return 0;
}
