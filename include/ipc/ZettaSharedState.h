#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace zetta::ipc {

inline constexpr const wchar_t* kSharedMemoryName = L"Local\\ZettaTelemetry_SharedMem_v1";
inline constexpr const wchar_t* kSharedMutexName  = L"Local\\ZettaTelemetry_SharedMtx_v1";

inline constexpr uint32_t kZettaMagicHeader   = 0x5A455454U; // 'ZETT'
inline constexpr uint32_t kZettaSchemaVersion = 310U;
inline constexpr size_t   kMaxMetrics         = 48U;
inline constexpr size_t   kFrameHistorySize   = 120U;

// Canonical v3.1 Metric Slot Identifiers (Slots 0..12 in ZettaSharedState::metrics)
inline constexpr const char* kMetricIdGpuLoad    = "gpu_load";
inline constexpr const char* kMetricIdCpuLoad    = "cpu_load";
inline constexpr const char* kMetricIdVramDed    = "vram_ded";
inline constexpr const char* kMetricIdVramShared = "vram_shared";
inline constexpr const char* kMetricIdRamSys     = "ram_sys";
inline constexpr const char* kMetricIdProcRam    = "proc_ram";
inline constexpr const char* kMetricIdDiskIo     = "disk_io";
inline constexpr const char* kMetricIdNetRate    = "net_rate";
inline constexpr const char* kMetricIdPwrBatt    = "pwr_batt";
inline constexpr const char* kMetricIdFpsEngine  = "fps_engine";
inline constexpr const char* kMetricIdFps1Low    = "fps_1low";
inline constexpr const char* kMetricIdCpuTemp    = "cpu_temp";
inline constexpr const char* kMetricIdGpuTemp    = "gpu_temp";

inline constexpr uint32_t kSlotIndexCpuTemp = 11U;
inline constexpr uint32_t kSlotIndexGpuTemp = 12U;
inline constexpr uint32_t kActiveMetricSlots = 13U;

#pragma pack(push, 1)

struct ZettaMetricEntry {
    char     id[32];
    char     label[16];
    double   numericValue;
    char     formatted[32];
    uint32_t colorHex;
    bool     isVisible;
};

struct ZettaFrameHistory {
    float    samples[kFrameHistorySize];
    uint32_t headIndex;
    float    avgFrametimeMs;
    float    onePercentLowFps;
};

struct ZettaSharedState {
    uint32_t          magicHeader;
    uint32_t          version;
    uint64_t          timestampMs;

    uint32_t          targetPid;
    char              targetProcessName[64];
    bool              isGameProcess;
    bool              isElevated;
    bool              isFullscreen;

    int32_t           windowX;
    int32_t           windowY;
    int32_t           windowWidth;
    int32_t           windowHeight;

    uint32_t          metricCount;
    ZettaMetricEntry  metrics[kMaxMetrics];
    ZettaFrameHistory frameGraph;
};

#pragma pack(pop)

static_assert(sizeof(ZettaMetricEntry) == 93, "ZettaMetricEntry must be tightly packed to 93 bytes.");
static_assert(sizeof(ZettaFrameHistory) == 492, "ZettaFrameHistory must be tightly packed to 492 bytes.");
static_assert(sizeof(ZettaSharedState) == 5063, "ZettaSharedState must be tightly packed to 5063 bytes.");
static_assert(std::is_standard_layout_v<ZettaSharedState>, "ZettaSharedState requires standard layout for IPC.");
static_assert(std::is_trivially_copyable_v<ZettaSharedState>, "ZettaSharedState must be trivially copyable.");

template <size_t N>
inline void CopyBoundedString(char (&dest)[N], const char* src) noexcept {
    if (!src) {
        std::memset(dest, 0, N);
        return;
    }
    std::memset(dest, 0, N);
    for (size_t i = 0; i < N - 1 && src[i] != '\0'; ++i) {
        dest[i] = src[i];
    }
}

} // namespace zetta::ipc
