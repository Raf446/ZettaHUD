#pragma once

#include "etw/ZettaEtwPresentAnalyzer.hpp"
#include "etw/ZettaFrameRingBuffer.hpp"
#include "ipc/ZettaSharedMemoryManager.hpp"
#include "ipc/ZettaSharedState.h"
#include "telemetry/ZettaCpuRamCollector.hpp"
#include "telemetry/ZettaGpuVramCollector.hpp"
#include "telemetry/ZettaPowerBatteryCollector.hpp"
#include "telemetry/ZettaStorageNetCollector.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>

namespace zetta::telemetry {

struct TelemetrySnapshot {
    CpuRamMetrics                  cpuRam{};
    GpuVramMetrics                 gpuVram{};
    StorageNetMetrics              storageNet{};
    PowerBatteryMetrics            powerBattery{};
    zetta::etw::FrameStatsSnapshot frameStats{};
    uint64_t                       sampleIndex{0};
};

class ZettaTelemetryAggregator {
public:
    using TickCallback = std::function<void(const TelemetrySnapshot&, const zetta::ipc::ZettaSharedState&)>;

    explicit ZettaTelemetryAggregator(
        zetta::ipc::ZettaSharedMemoryManager& ipcManager,
        uint32_t intervalMs = 500,
        zetta::etw::ZettaEtwPresentAnalyzer* presentAnalyzer = nullptr
    ) noexcept
        : ipcManager_(ipcManager),
          intervalMs_(intervalMs),
          presentAnalyzer_(presentAnalyzer) {}

    ~ZettaTelemetryAggregator() noexcept {
        Stop();
    }

    ZettaTelemetryAggregator(const ZettaTelemetryAggregator&) = delete;
    ZettaTelemetryAggregator& operator=(const ZettaTelemetryAggregator&) = delete;

    void AttachPresentAnalyzer(zetta::etw::ZettaEtwPresentAnalyzer* analyzer) noexcept {
        presentAnalyzer_ = analyzer;
    }

    void SetTargetPid(uint32_t pid) noexcept {
        targetPid_.store(pid, std::memory_order_release);
        if (presentAnalyzer_ != nullptr) {
            presentAnalyzer_->SetTargetPid(pid);
        }
    }

    [[nodiscard]] const char* GetGpuAdapterName() const noexcept {
        return gpuVramCollector_.GetAdapterName();
    }

    void Start(TickCallback callback = nullptr) {
        if (workerThread_.joinable()) {
            return;
        }

        tickCallback_ = std::move(callback);

        gpuVramCollector_.Initialize();
        cpuRamCollector_.Initialize();
        storageNetCollector_.Initialize();
        powerBatteryCollector_.Initialize();

        SampleOnce();

        workerThread_ = std::jthread([this](std::stop_token stopToken) {
            WorkerLoop(stopToken);
        });
    }

    void Stop() noexcept {
        if (workerThread_.joinable()) {
            workerThread_.request_stop();
            wakeCv_.notify_all();
            workerThread_.join();
        }
        powerBatteryCollector_.Shutdown();
        storageNetCollector_.Shutdown();
        gpuVramCollector_.Shutdown();
        cpuRamCollector_.Shutdown();
    }

private:
    void WorkerLoop(std::stop_token stopToken) noexcept {
        std::stop_callback stopWake(stopToken, [this]() noexcept {
            wakeCv_.notify_all();
        });

        const auto interval = std::chrono::milliseconds(intervalMs_);
        std::unique_lock<std::mutex> lock(wakeMutex_);

        while (!stopToken.stop_requested()) {
            wakeCv_.wait_for(lock, interval, [&stopToken]() noexcept {
                return stopToken.stop_requested();
            });
            if (stopToken.stop_requested()) {
                break;
            }

            lock.unlock();
            SampleOnce();
            lock.lock();
        }
    }

    void SampleOnce() noexcept {
        const uint32_t pid = targetPid_.load(std::memory_order_acquire);

        gpuVramCollector_.Sample(snapshot_.gpuVram);
        cpuRamCollector_.Sample(pid, snapshot_.cpuRam);
        storageNetCollector_.Sample(snapshot_.storageNet);
        powerBatteryCollector_.Sample(snapshot_.powerBattery);

        if (presentAnalyzer_ != nullptr) {
            presentAnalyzer_->SampleFrameStats(snapshot_.frameStats);
        } else {
            snapshot_.frameStats = zetta::etw::FrameStatsSnapshot{};
        }

        ++snapshot_.sampleIndex;

        zetta::ipc::ZettaSharedState stateCopy{};
        const bool updated = ipcManager_.UpdateState([this, &stateCopy](zetta::ipc::ZettaSharedState& state) noexcept {
            PopulateSharedMetrics(snapshot_, state);
            stateCopy = state;
        });

        if (updated && tickCallback_) {
            tickCallback_(snapshot_, stateCopy);
        }
    }

    static void WriteMetricSlot(
        zetta::ipc::ZettaMetricEntry& slot,
        const char* id,
        const char* label,
        double numericValue,
        const char* formatted,
        uint32_t colorHex
    ) noexcept {
        zetta::ipc::CopyBoundedString(slot.id, id);
        zetta::ipc::CopyBoundedString(slot.label, label);
        slot.numericValue = numericValue;
        zetta::ipc::CopyBoundedString(slot.formatted, formatted);
        slot.colorHex  = colorHex;
        slot.isVisible = true;
    }

    static void PopulateSharedMetrics(
        const TelemetrySnapshot& snap,
        zetta::ipc::ZettaSharedState& state
    ) noexcept {
        char fmtBuf[32]{};

        // 1. gpu_load ("GPU", #22C55E)
        std::snprintf(fmtBuf, sizeof(fmtBuf), "%.1f %%", snap.gpuVram.gpuLoadPercent);
        WriteMetricSlot(
            state.metrics[0],
            "gpu_load",
            "GPU",
            snap.gpuVram.gpuLoadPercent,
            fmtBuf,
            0xFF22C55EU
        );

        // 2. cpu_load ("CPU", #38BDF8)
        std::snprintf(fmtBuf, sizeof(fmtBuf), "%.1f %%", snap.cpuRam.systemCpuPercent);
        WriteMetricSlot(
            state.metrics[1],
            "cpu_load",
            "CPU",
            snap.cpuRam.systemCpuPercent,
            fmtBuf,
            0xFF38BDF8U
        );

        // 3. vram_ded ("VRAM", #C084FC)
        std::snprintf(fmtBuf, sizeof(fmtBuf), "%.2f GB", snap.gpuVram.dedicatedVramUsedGb);
        WriteMetricSlot(
            state.metrics[2],
            "vram_ded",
            "VRAM",
            snap.gpuVram.dedicatedVramUsedGb,
            fmtBuf,
            0xFFC084FCU
        );

        // 4. vram_shared ("SH_VRAM", #A855F7)
        std::snprintf(fmtBuf, sizeof(fmtBuf), "%.2f GB", snap.gpuVram.sharedVramUsedGb);
        WriteMetricSlot(
            state.metrics[3],
            "vram_shared",
            "SH_VRAM",
            snap.gpuVram.sharedVramUsedGb,
            fmtBuf,
            0xFFA855F7U
        );

        // 5. ram_sys ("RAM", #F472B6)
        std::snprintf(
            fmtBuf,
            sizeof(fmtBuf),
            "%.1f/%.0f GB",
            snap.cpuRam.systemRamUsedGb,
            snap.cpuRam.systemRamTotalGb
        );
        WriteMetricSlot(
            state.metrics[4],
            "ram_sys",
            "RAM",
            snap.cpuRam.systemRamUsedGb,
            fmtBuf,
            0xFFF472B6U
        );

        // 6. proc_ram ("PROC_MEM", #E879F9)
        if (snap.cpuRam.processWorkingSetMb >= 1024.0) {
            std::snprintf(fmtBuf, sizeof(fmtBuf), "%.2f GB", snap.cpuRam.processWorkingSetMb / 1024.0);
        } else {
            std::snprintf(fmtBuf, sizeof(fmtBuf), "%.0f MB", snap.cpuRam.processWorkingSetMb);
        }
        WriteMetricSlot(
            state.metrics[5],
            "proc_ram",
            "PROC_MEM",
            snap.cpuRam.processWorkingSetMb,
            fmtBuf,
            0xFFE879F9U
        );

        // 7. disk_io ("NVME_IO", #FB923C)
        std::snprintf(
            fmtBuf,
            sizeof(fmtBuf),
            "R:%.1f W:%.1fM",
            snap.storageNet.diskReadMbps,
            snap.storageNet.diskWriteMbps
        );
        WriteMetricSlot(
            state.metrics[6],
            "disk_io",
            "NVME_IO",
            snap.storageNet.diskTotalMbps,
            fmtBuf,
            0xFFFB923CU
        );

        // 8. net_rate ("NET_IO", #2DD4BF)
        if (snap.storageNet.netTotalKbps >= 1024.0) {
            std::snprintf(fmtBuf, sizeof(fmtBuf), "%.2f MB/s", snap.storageNet.netTotalKbps / 1024.0);
        } else {
            std::snprintf(fmtBuf, sizeof(fmtBuf), "%.0f KB/s", snap.storageNet.netTotalKbps);
        }
        WriteMetricSlot(
            state.metrics[7],
            "net_rate",
            "NET_IO",
            snap.storageNet.netTotalKbps,
            fmtBuf,
            0xFF2DD4BFU
        );

        // 9. pwr_batt ("BATT_PWR", #FACC15)
        if (!snap.powerBattery.hasBattery) {
            std::snprintf(fmtBuf, sizeof(fmtBuf), "0.0 W (AC)");
        } else if (snap.powerBattery.isAcOnline && !snap.powerBattery.isDischarging) {
            std::snprintf(
                fmtBuf,
                sizeof(fmtBuf),
                "%.1fW %.0f%% AC",
                snap.powerBattery.powerWatts,
                snap.powerBattery.capacityPercent
            );
        } else {
            std::snprintf(
                fmtBuf,
                sizeof(fmtBuf),
                "%.1fW %.0f%% DC",
                snap.powerBattery.powerWatts,
                snap.powerBattery.capacityPercent
            );
        }
        WriteMetricSlot(
            state.metrics[8],
            "pwr_batt",
            "BATT_PWR",
            snap.powerBattery.powerWatts,
            fmtBuf,
            0xFFFACC15U
        );

        // 10. fps_engine ("FPS", #FB7185)
        std::snprintf(
            fmtBuf,
            sizeof(fmtBuf),
            "%.0f FPS [%s]",
            static_cast<double>(snap.frameStats.avgFps),
            snap.frameStats.apiTagString
        );
        WriteMetricSlot(
            state.metrics[9],
            "fps_engine",
            "FPS",
            static_cast<double>(snap.frameStats.avgFps),
            fmtBuf,
            0xFFFB7185U
        );

        // 11. fps_1low ("1% LOW", #34D399)
        std::snprintf(
            fmtBuf,
            sizeof(fmtBuf),
            "%.0f FPS (%.1fms)",
            static_cast<double>(snap.frameStats.onePercentLowFps),
            static_cast<double>(snap.frameStats.avgFrametimeMs)
        );
        WriteMetricSlot(
            state.metrics[10],
            "fps_1low",
            "1% LOW",
            static_cast<double>(snap.frameStats.onePercentLowFps),
            fmtBuf,
            0xFF34D399U
        );

        // 12. cpu_temp ("CPU_TMP", #FB923C)
        if (snap.cpuRam.cpuTempCelsius > 0.5) {
            std::snprintf(fmtBuf, sizeof(fmtBuf), "%.0f C", snap.cpuRam.cpuTempCelsius);
        } else {
            std::snprintf(fmtBuf, sizeof(fmtBuf), "0 C (N/A)");
        }
        WriteMetricSlot(
            state.metrics[11],
            "cpu_temp",
            "CPU_TMP",
            snap.cpuRam.cpuTempCelsius,
            fmtBuf,
            0xFFFB923CU
        );

        // 13. gpu_temp ("GPU_TMP", #34D399)
        if (snap.gpuVram.gpuTempCelsius > 0.5) {
            std::snprintf(fmtBuf, sizeof(fmtBuf), "%.0f C", snap.gpuVram.gpuTempCelsius);
        } else {
            std::snprintf(fmtBuf, sizeof(fmtBuf), "0 C (N/A)");
        }
        WriteMetricSlot(
            state.metrics[12],
            "gpu_temp",
            "GPU_TMP",
            snap.gpuVram.gpuTempCelsius,
            fmtBuf,
            0xFF34D399U
        );

        state.metricCount = 13U;

        std::memcpy(
            state.frameGraph.samples,
            snap.frameStats.samples,
            sizeof(state.frameGraph.samples)
        );
        state.frameGraph.headIndex        = snap.frameStats.headIndex;
        state.frameGraph.avgFrametimeMs   = snap.frameStats.avgFrametimeMs;
        state.frameGraph.onePercentLowFps = snap.frameStats.onePercentLowFps;
    }

    zetta::ipc::ZettaSharedMemoryManager& ipcManager_;
    uint32_t                              intervalMs_{500};
    zetta::etw::ZettaEtwPresentAnalyzer*  presentAnalyzer_{nullptr};
    std::atomic<uint32_t>                 targetPid_{0};

    ZettaCpuRamCollector                  cpuRamCollector_{};
    ZettaGpuVramCollector                 gpuVramCollector_{};
    ZettaStorageNetCollector              storageNetCollector_{};
    ZettaPowerBatteryCollector            powerBatteryCollector_{};
    TelemetrySnapshot                     snapshot_{};

    TickCallback                          tickCallback_{};
    std::mutex                            wakeMutex_{};
    std::condition_variable               wakeCv_{};
    std::jthread                          workerThread_{};
};

} // namespace zetta::telemetry
