#pragma once

#include "ipc/ZettaSharedState.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>

namespace zetta::etw {

enum class GraphicsApiTag : uint8_t {
    Idle = 0,
    DxgiDx11 = 1,
    Dx12 = 2,
    VulkanDxvk = 3,
    OpenGl = 4,
    DwmComp = 5
};

struct FrameStatsSnapshot {
    float          samples[zetta::ipc::kFrameHistorySize]{};
    uint32_t       headIndex{0};
    float          instantFps{0.0f};
    float          avgFps{0.0f};
    float          avgFrametimeMs{0.0f};
    float          onePercentLowFps{0.0f};
    float          pointOnePercentLowFps{0.0f};
    GraphicsApiTag apiTag{GraphicsApiTag::Idle};
    char           apiTagString[16]{"IDLE"};
};

class ZettaFrameRingBuffer {
public:
    static constexpr size_t kPercentileWindowSize = 256U;

    ZettaFrameRingBuffer() noexcept {
        ::InitializeSRWLock(&srwLock_);
        LARGE_INTEGER freq{};
        if (::QueryPerformanceFrequency(&freq) && freq.QuadPart > 0) {
            qpcFrequency_ = static_cast<uint64_t>(freq.QuadPart);
        }
    }

    ~ZettaFrameRingBuffer() noexcept = default;

    ZettaFrameRingBuffer(const ZettaFrameRingBuffer&) = delete;
    ZettaFrameRingBuffer& operator=(const ZettaFrameRingBuffer&) = delete;

    [[nodiscard]] static const char* ApiTagToString(GraphicsApiTag tag) noexcept {
        switch (tag) {
            case GraphicsApiTag::DxgiDx11:   return "DXGI/DX11";
            case GraphicsApiTag::Dx12:       return "DX12";
            case GraphicsApiTag::VulkanDxvk: return "VK/DXVK";
            case GraphicsApiTag::OpenGl:     return "OPENGL";
            case GraphicsApiTag::DwmComp:    return "DWM_COMP";
            case GraphicsApiTag::Idle:
            default:                         return "IDLE";
        }
    }

    void RecordPresentTimestamp(
        uint64_t qpcTimestamp,
        uint32_t pid,
        GraphicsApiTag apiTag
    ) noexcept {
        if (qpcTimestamp == 0 || qpcFrequency_ == 0) {
            return;
        }

        ::AcquireSRWLockExclusive(&srwLock_);

        if (pid != activePid_ || lastQpcTimestamp_ == 0 || qpcTimestamp <= lastQpcTimestamp_) {
            activePid_        = pid;
            lastQpcTimestamp_ = qpcTimestamp;
            activeApiTag_     = apiTag;
            lastUpdateMs_     = CurrentSteadyMs();
            ::ReleaseSRWLockExclusive(&srwLock_);
            return;
        }

        const uint64_t qpcDelta = qpcTimestamp - lastQpcTimestamp_;
        const double deltaMs = (static_cast<double>(qpcDelta) * 1000.0) /
                               static_cast<double>(qpcFrequency_);

        // Filter duplicate flip callbacks (< 0.1 ms) without advancing baseline.
        if (deltaMs < 0.1) {
            ::ReleaseSRWLockExclusive(&srwLock_);
            return;
        }

        lastQpcTimestamp_ = qpcTimestamp;
        lastUpdateMs_     = CurrentSteadyMs();
        activeApiTag_     = apiTag;

        if (deltaMs <= 1000.0) {
            PushFrameSampleLocked(static_cast<float>(deltaMs));
        }

        ::ReleaseSRWLockExclusive(&srwLock_);
    }

    void RecordFrameDeltaMs(
        float deltaMs,
        uint32_t pid,
        GraphicsApiTag apiTag
    ) noexcept {
        if (deltaMs < 0.1f || deltaMs > 1000.0f) {
            return;
        }

        ::AcquireSRWLockExclusive(&srwLock_);
        activePid_    = pid;
        activeApiTag_ = apiTag;
        lastUpdateMs_ = CurrentSteadyMs();
        PushFrameSampleLocked(deltaMs);
        ::ReleaseSRWLockExclusive(&srwLock_);
    }

    void ComputeSnapshot(FrameStatsSnapshot& outSnapshot) noexcept {
        std::array<float, kPercentileWindowSize> windowCopy{};
        uint32_t countCopy = 0;
        float latestDeltaMs = 0.0f;
        uint64_t lastUpdateCopy = 0;

        ::AcquireSRWLockShared(&srwLock_);

        std::memcpy(
            outSnapshot.samples,
            historySamples_.data(),
            sizeof(outSnapshot.samples)
        );
        outSnapshot.headIndex = historyHead_;
        outSnapshot.apiTag    = activeApiTag_;
        zetta::ipc::CopyBoundedString(outSnapshot.apiTagString, ApiTagToString(activeApiTag_));

        countCopy      = windowCount_;
        latestDeltaMs  = lastFrameDeltaMs_;
        lastUpdateCopy = lastUpdateMs_;
        if (countCopy > 0) {
            std::memcpy(
                windowCopy.data(),
                percentileWindow_.data(),
                countCopy * sizeof(float)
            );
        }

        ::ReleaseSRWLockShared(&srwLock_);

        const uint64_t nowMs = CurrentSteadyMs();
        // Decay to zero if no present events arrived within the last 1500 ms.
        if (countCopy == 0 || lastUpdateCopy == 0 || (nowMs > lastUpdateCopy && (nowMs - lastUpdateCopy) > 1500ULL)) {
            outSnapshot.instantFps            = 0.0f;
            outSnapshot.avgFps                = 0.0f;
            outSnapshot.avgFrametimeMs        = 0.0f;
            outSnapshot.onePercentLowFps      = 0.0f;
            outSnapshot.pointOnePercentLowFps = 0.0f;
            outSnapshot.apiTag                = GraphicsApiTag::Idle;
            zetta::ipc::CopyBoundedString(outSnapshot.apiTagString, "IDLE");
            return;
        }

        double sumMs = 0.0;
        for (uint32_t i = 0; i < countCopy; ++i) {
            sumMs += static_cast<double>(windowCopy[i]);
        }

        const float avgMs = static_cast<float>(sumMs / static_cast<double>(countCopy));
        outSnapshot.avgFrametimeMs = avgMs;
        outSnapshot.avgFps         = (avgMs > 0.01f) ? (1000.0f / avgMs) : 0.0f;
        outSnapshot.instantFps     = (latestDeltaMs > 0.01f) ? (1000.0f / latestDeltaMs) : outSnapshot.avgFps;

        std::sort(windowCopy.begin(), windowCopy.begin() + countCopy);

        const uint32_t p99Index = std::min(
            countCopy - 1U,
            static_cast<uint32_t>((countCopy * 99U) / 100U)
        );
        const float p99FrametimeMs = windowCopy[p99Index];
        const float worstFrametimeMs = windowCopy[countCopy - 1U];

        outSnapshot.onePercentLowFps = (p99FrametimeMs > 0.01f)
                                           ? (1000.0f / p99FrametimeMs)
                                           : 0.0f;
        outSnapshot.pointOnePercentLowFps = (worstFrametimeMs > 0.01f)
                                                ? (1000.0f / worstFrametimeMs)
                                                : 0.0f;
    }

private:
    void PushFrameSampleLocked(float deltaMs) noexcept {
        lastFrameDeltaMs_ = deltaMs;

        historySamples_[historyHead_] = deltaMs;
        historyHead_ = static_cast<uint32_t>((historyHead_ + 1U) % zetta::ipc::kFrameHistorySize);

        percentileWindow_[windowHead_] = deltaMs;
        windowHead_ = static_cast<uint32_t>((windowHead_ + 1U) % kPercentileWindowSize);
        if (windowCount_ < kPercentileWindowSize) {
            ++windowCount_;
        }
    }

    [[nodiscard]] static uint64_t CurrentSteadyMs() noexcept {
        using namespace std::chrono;
        return static_cast<uint64_t>(
            duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count()
        );
    }

    SRWLOCK                                        srwLock_{};
    uint64_t                                       qpcFrequency_{10000000ULL};
    uint64_t                                       lastQpcTimestamp_{0};
    uint64_t                                       lastUpdateMs_{0};
    uint32_t                                       activePid_{0};
    float                                          lastFrameDeltaMs_{0.0f};
    GraphicsApiTag                                 activeApiTag_{GraphicsApiTag::Idle};

    std::array<float, zetta::ipc::kFrameHistorySize> historySamples_{};
    uint32_t                                         historyHead_{0};

    std::array<float, kPercentileWindowSize>         percentileWindow_{};
    uint32_t                                         windowHead_{0};
    uint32_t                                         windowCount_{0};
};

} // namespace zetta::etw
