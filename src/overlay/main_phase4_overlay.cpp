#include "etw/ZettaEtwPresentAnalyzer.hpp"
#include "ipc/ZettaSharedMemoryManager.hpp"
#include "ipc/ZettaSharedState.h"
#include "overlay/ZettaOverlayWindow.hpp"
#include "telemetry/ZettaTelemetryAggregator.hpp"
#include "tracker/ZettaWindowTracker.hpp"

#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

namespace {

std::atomic<DWORD> g_mainThreadId{0};
std::atomic<bool>  g_stopRequested{false};

class ScopedComInitializer {
public:
    ScopedComInitializer() noexcept {
        // Direct2D single-threaded UI + WIC/DirectWrite works best with STA on the main GUI thread.
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
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

std::wstring ToFullPath(const std::wstring& path) {
    wchar_t fullBuf[MAX_PATH * 2]{};
    const DWORD len = ::GetFullPathNameW(path.c_str(), static_cast<DWORD>(std::size(fullBuf)), fullBuf, nullptr);
    if (len > 0 && len < std::size(fullBuf)) {
        return std::wstring(fullBuf, len);
    }
    return path;
}

std::wstring ResolvePresetFilePath() {
    wchar_t exePath[MAX_PATH * 2]{};
    const DWORD len = ::GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
    if (len > 0) {
        std::wstring dir(exePath, len);
        const size_t slashPos = dir.find_last_of(L"\\/");
        if (slashPos != std::wstring::npos) {
            dir.resize(slashPos + 1);
            const std::wstring candidate1 = dir + L"..\\resources\\presets\\default_layout.json";
            if (::GetFileAttributesW(candidate1.c_str()) != INVALID_FILE_ATTRIBUTES) {
                return ToFullPath(candidate1);
            }
            const std::wstring candidate2 = dir + L"resources\\presets\\default_layout.json";
            if (::GetFileAttributesW(candidate2.c_str()) != INVALID_FILE_ATTRIBUTES) {
                return ToFullPath(candidate2);
            }
            const std::wstring candidate3 = dir + L"presets\\default_layout.json";
            if (::GetFileAttributesW(candidate3.c_str()) != INVALID_FILE_ATTRIBUTES) {
                return ToFullPath(candidate3);
            }
        }
    }

    const std::wstring localPath = L"resources\\presets\\default_layout.json";
    if (::GetFileAttributesW(localPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return ToFullPath(localPath);
    }
    return localPath;
}

} // namespace

int main(int argc, char* argv[]) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ScopedComInitializer comInit;

    bool clientOnlyMode    = false;
    uint32_t durationSec   = 0;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--client") == 0) {
            clientOnlyMode = true;
        } else if (std::strncmp(argv[i], "--duration=", 11) == 0) {
            const long val = std::strtol(argv[i] + 11, nullptr, 10);
            if (val > 0) {
                durationSec = static_cast<uint32_t>(val);
            }
        }
    }

    g_mainThreadId.store(::GetCurrentThreadId(), std::memory_order_release);
    ::SetConsoleCtrlHandler(&ConsoleCtrlHandler, TRUE);

    MSG initMsg{};
    ::PeekMessageW(&initMsg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    const zetta::ipc::SharedMemoryRole ipcRole =
        clientOnlyMode ? zetta::ipc::SharedMemoryRole::Client : zetta::ipc::SharedMemoryRole::Host;

    zetta::ipc::ZettaSharedMemoryManager ipcManager(ipcRole);
    if (!ipcManager.Initialize()) {
        std::cerr << "[ERROR] Failed to initialize shared memory ("
                  << (clientOnlyMode ? "Client" : "Host")
                  << " mode, Win32 error: " << ::GetLastError() << ").\n";
        return 1;
    }

    std::unique_ptr<zetta::etw::ZettaEtwPresentAnalyzer>      presentAnalyzer;
    std::unique_ptr<zetta::telemetry::ZettaTelemetryAggregator> telemetry;
    std::unique_ptr<zetta::tracker::ZettaWindowTracker>       tracker;

    const char* captureBadge = "IPC";

    if (!clientOnlyMode) {
        presentAnalyzer = std::make_unique<zetta::etw::ZettaEtwPresentAnalyzer>();
        presentAnalyzer->Start();
        captureBadge = presentAnalyzer->IsEtwActive() ? "ETW" : "DWM";

        telemetry = std::make_unique<zetta::telemetry::ZettaTelemetryAggregator>(
            ipcManager,
            500,
            presentAnalyzer.get()
        );
        tracker = std::make_unique<zetta::tracker::ZettaWindowTracker>();

        if (!tracker->Start([&ipcManager, &telemetry](const zetta::tracker::WindowTargetInfo& info) {
                telemetry->SetTargetPid(info.pid);
                ipcManager.UpdateState([&info](zetta::ipc::ZettaSharedState& state) noexcept {
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
            })) {
            std::cerr << "[ERROR] Failed to start WinEvent window tracker.\n";
            return 1;
        }
    }

    const std::wstring presetPath = ResolvePresetFilePath();
    zetta::overlay::ZettaOverlayWindow overlayWindow(ipcManager, captureBadge);
    if (!overlayWindow.Initialize(presetPath)) {
        std::cerr << "[ERROR] Failed to initialize Direct2D overlay window (Win32 error: "
                  << ::GetLastError() << ").\n";
        return 1;
    }

    std::cout << "[ZettaHUD v3.3] Direct2D Transparent Click-Through Overlay\n"
              << "  Execution Mode    : " << (clientOnlyMode ? "Client-Only (--client)" : "Self-Contained Daemon + Overlay") << "\n"
              << "  Capture Engine    : [" << captureBadge << "]\n"
              << "  Hotkeys           : Ctrl+Shift+F12 (Show/Hide HUD) | Ctrl+Shift+F11 (Compact/Full Mode)\n"
              << "  Hot-Reload Preset : resources/presets/default_layout.json (500ms file watcher + IPC signal)\n"
              << "  Press Ctrl+C to exit cleanly.\n\n";

    if (telemetry) {
        telemetry->Start([&overlayWindow](
                             const zetta::telemetry::TelemetrySnapshot& snap,
                             const zetta::ipc::ZettaSharedState& state) {
            if (snap.sampleIndex != 1 && (snap.sampleIndex % 2) != 0) {
                return;
            }
            std::printf(
                "[HUD LIVE] D2D Frames: %-5llu | Target: %-18s | FPS: %5.1f (1%%L: %5.1f) | "
                "CPU: %4.1f%% (%s) | GPU: %4.1f%% (%s) | VRAM: %.2fG/%.2fG | RAM: %s\n",
                static_cast<unsigned long long>(overlayWindow.GetRenderedFrameCount()),
                state.targetProcessName[0] != '\0' ? state.targetProcessName : "Desktop",
                static_cast<double>(snap.frameStats.avgFps),
                static_cast<double>(snap.frameStats.onePercentLowFps),
                snap.cpuRam.systemCpuPercent,
                state.metrics[11].formatted,
                snap.gpuVram.gpuLoadPercent,
                state.metrics[12].formatted,
                snap.gpuVram.dedicatedVramUsedGb,
                snap.gpuVram.sharedVramUsedGb,
                state.metrics[4].formatted
            );
            std::fflush(stdout);
        });
    }

    constexpr UINT_PTR kDurationExitTimerId = 999U;
    if (durationSec > 0) {
        ::SetTimer(overlayWindow.GetHwnd(), kDurationExitTimerId, durationSec * 1000U, nullptr);
    }

    const auto startTime = std::chrono::steady_clock::now();
    MSG msg{};
    while (!g_stopRequested.load(std::memory_order_acquire)) {
        const BOOL gmResult = ::GetMessageW(&msg, nullptr, 0, 0);
        if (gmResult <= 0) {
            break;
        }
        if (msg.message == WM_TIMER && msg.wParam == kDurationExitTimerId) {
            break;
        }
        if (durationSec > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - startTime
            ).count();
            if (elapsed >= static_cast<long long>(durationSec)) {
                break;
            }
        }
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    const uint64_t totalRenderedFrames = overlayWindow.GetRenderedFrameCount();
    const uint32_t totalRecreates      = overlayWindow.GetDeviceRecreateCount();

    overlayWindow.Shutdown();
    if (telemetry) {
        telemetry->Stop();
    }
    if (presentAnalyzer) {
        presentAnalyzer->Stop();
    }
    if (tracker) {
        tracker->Stop();
    }
    ipcManager.Shutdown();

    std::cout << "\n[ZettaHUD] Phase 4 Overlay exited cleanly. Rendered "
              << totalRenderedFrames << " Direct2D frames (Device losses recovered: "
              << totalRecreates << ").\n";
    return 0;
}
