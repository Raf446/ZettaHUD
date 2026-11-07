#pragma once

#include "ipc/ZettaSharedState.h"

#include <windows.h>
#include <dxgi1_4.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace zetta::telemetry {

// Shared atomic APU/GPU package temperature (in Celsius) for monolithic APU fallback.
inline std::atomic<double> g_sharedApuPackageTempCelsius{0.0};

struct ADLTemperature {
    int iSize;
    int iTemperature;
};

struct ADLPMLogSensorEntry {
    unsigned int supported;
    int          value;
};

struct ADLPMLogDataOutput {
    int                 size;
    ADLPMLogSensorEntry sensors[256];
};

using ADL_MAIN_MALLOC_CALLBACK            = void* (__stdcall*)(int);
using ADL_Main_Control_Create_Fn          = int (*)(ADL_MAIN_MALLOC_CALLBACK, int);
using ADL_Main_Control_Destroy_Fn         = int (*)();
using ADL_Adapter_NumberOfAdapters_Get_Fn = int (*)(int*);
using ADL_Overdrive5_Temperature_Get_Fn   = int (*)(int, int, ADLTemperature*);
using ADL_Overdrive6_Temperature_Get_Fn   = int (*)(int, int*);
using ADL2_Main_Control_Create_Fn         = int (*)(ADL_MAIN_MALLOC_CALLBACK, int, void**);
using ADL2_Main_Control_Destroy_Fn        = int (*)(void*);
using ADL2_OverdriveN_Temperature_Get_Fn  = int (*)(void*, int, int, int*);
using ADL2_New_QueryPMLogData_Get_Fn      = int (*)(void*, int, ADLPMLogDataOutput*);

inline void* __stdcall AdlMainMemoryAlloc(int size) noexcept {
    if (size <= 0) {
        return nullptr;
    }
    return std::malloc(static_cast<size_t>(size));
}

template <typename T>
class ScopedComPtr {
public:
    ScopedComPtr() noexcept = default;
    explicit ScopedComPtr(T* ptr) noexcept : ptr_(ptr) {}

    ~ScopedComPtr() noexcept {
        Reset();
    }

    ScopedComPtr(const ScopedComPtr&) = delete;
    ScopedComPtr& operator=(const ScopedComPtr&) = delete;

    ScopedComPtr(ScopedComPtr&& other) noexcept : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }

    ScopedComPtr& operator=(ScopedComPtr&& other) noexcept {
        if (this != &other) {
            Reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    void Reset(T* newPtr = nullptr) noexcept {
        if (ptr_ != nullptr) {
            ptr_->Release();
        }
        ptr_ = newPtr;
    }

    [[nodiscard]] T* Get() const noexcept { return ptr_; }
    [[nodiscard]] T* operator->() const noexcept { return ptr_; }
    [[nodiscard]] T** AddressOf() noexcept {
        Reset();
        return &ptr_;
    }
    [[nodiscard]] void** VoidAddressOf() noexcept {
        Reset();
        return reinterpret_cast<void**>(&ptr_);
    }
    [[nodiscard]] bool IsValid() const noexcept { return ptr_ != nullptr; }

private:
    T* ptr_{nullptr};
};

struct GpuVramMetrics {
    double gpuLoadPercent{0.0};
    double gpuTempCelsius{0.0};
    bool   hasGpuTemp{false};
    double dedicatedVramUsedGb{0.0};
    double dedicatedVramBudgetGb{0.0};
    double sharedVramUsedGb{0.0};
    double sharedVramBudgetGb{0.0};
    char   adapterName[128]{};
};

class ZettaGpuVramCollector {
public:
    ZettaGpuVramCollector() noexcept = default;

    ~ZettaGpuVramCollector() noexcept {
        Shutdown();
    }

    ZettaGpuVramCollector(const ZettaGpuVramCollector&) = delete;
    ZettaGpuVramCollector& operator=(const ZettaGpuVramCollector&) = delete;

    void Initialize() noexcept {
        InitializeDxgiAdapter();
        InitializePdhCounter();
        InitializeNvml();
        InitializeAdl();
        GpuVramMetrics initialMetrics{};
        SampleGpuTemperatureThrottled(initialMetrics);
    }

    void Shutdown() noexcept {
        if (pdhQuery_ != nullptr) {
            ::PdhCloseQuery(pdhQuery_);
            pdhQuery_              = nullptr;
            pdhCounter3D_          = nullptr;
            pdhCounterDedVram_     = nullptr;
            pdhCounterSharedVram_  = nullptr;
            pdhCounterGpuTemp_     = nullptr;
        }
        ShutdownAdl();
        ShutdownNvml();
        activeAdapter3_.Reset();
        dxgiFactory_.Reset();
    }

    void Sample(GpuVramMetrics& outMetrics) noexcept {
        zetta::ipc::CopyBoundedString(outMetrics.adapterName, adapterName_);
        outMetrics.dedicatedVramUsedGb   = 0.0;
        outMetrics.dedicatedVramBudgetGb = 0.0;
        outMetrics.sharedVramUsedGb      = 0.0;
        outMetrics.sharedVramBudgetGb    = 0.0;

        constexpr double kBytesPerGb = 1024.0 * 1024.0 * 1024.0;

        if (activeAdapter3_.IsValid()) {
            DXGI_QUERY_VIDEO_MEMORY_INFO localInfo{};
            if (SUCCEEDED(activeAdapter3_->QueryVideoMemoryInfo(
                    0,
                    DXGI_MEMORY_SEGMENT_GROUP_LOCAL,
                    &localInfo))) {
                outMetrics.dedicatedVramUsedGb   = static_cast<double>(localInfo.CurrentUsage) / kBytesPerGb;
                outMetrics.dedicatedVramBudgetGb = static_cast<double>(localInfo.Budget) / kBytesPerGb;
            }

            DXGI_QUERY_VIDEO_MEMORY_INFO nonLocalInfo{};
            if (SUCCEEDED(activeAdapter3_->QueryVideoMemoryInfo(
                    0,
                    DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,
                    &nonLocalInfo))) {
                outMetrics.sharedVramUsedGb   = static_cast<double>(nonLocalInfo.CurrentUsage) / kBytesPerGb;
                outMetrics.sharedVramBudgetGb = static_cast<double>(nonLocalInfo.Budget) / kBytesPerGb;
            }
        }

        SampleGpuPdhCounters(outMetrics);
        SampleGpuTemperatureThrottled(outMetrics);
    }

    [[nodiscard]] const char* GetAdapterName() const noexcept {
        return adapterName_;
    }

private:
    void InitializeDxgiAdapter() noexcept {
        zetta::ipc::CopyBoundedString(adapterName_, "Unknown DXGI Adapter");

        if (FAILED(::CreateDXGIFactory1(__uuidof(IDXGIFactory1), dxgiFactory_.VoidAddressOf()))) {
            return;
        }

        UINT bestIndex = 0;
        SIZE_T bestScore = 0;
        ScopedComPtr<IDXGIAdapter1> bestAdapter;

        for (UINT idx = 0;; ++idx) {
            ScopedComPtr<IDXGIAdapter1> candidate;
            if (dxgiFactory_->EnumAdapters1(idx, candidate.AddressOf()) == DXGI_ERROR_NOT_FOUND) {
                break;
            }

            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(candidate->GetDesc1(&desc))) {
                continue;
            }

            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
                continue;
            }

            // Score by dedicated + shared memory to pick the primary discrete or APU graphics adapter.
            const SIZE_T score = (desc.DedicatedVideoMemory * 2U) + desc.SharedSystemMemory + 1U;
            if (!bestAdapter.IsValid() || score > bestScore) {
                bestScore   = score;
                bestIndex   = idx;
                bestAdapter = std::move(candidate);

                char utf8Desc[128]{};
                ::WideCharToMultiByte(
                    CP_UTF8,
                    0,
                    desc.Description,
                    -1,
                    utf8Desc,
                    static_cast<int>(sizeof(utf8Desc)),
                    nullptr,
                    nullptr
                );
                zetta::ipc::CopyBoundedString(adapterName_, utf8Desc);
            }
        }

        (void)bestIndex;
        if (bestAdapter.IsValid()) {
            bestAdapter->QueryInterface(__uuidof(IDXGIAdapter3), activeAdapter3_.VoidAddressOf());
        }
    }

    void InitializePdhCounter() noexcept {
        if (::PdhOpenQueryW(nullptr, 0, &pdhQuery_) != ERROR_SUCCESS) {
            pdhQuery_ = nullptr;
            return;
        }

        constexpr const wchar_t* kGpu3DCounterPath =
            L"\\GPU Engine(*engtype_3D)\\Utilization Percentage";
        constexpr const wchar_t* kGpuDedVramPath =
            L"\\GPU Adapter Memory(*)\\Dedicated Usage";
        constexpr const wchar_t* kGpuSharedVramPath =
            L"\\GPU Adapter Memory(*)\\Shared Usage";
        constexpr const wchar_t* kGpuThermalPath =
            L"\\GPU Thermal Zone(*)\\Temperature";

        if (::PdhAddEnglishCounterW(pdhQuery_, kGpu3DCounterPath, 0, &pdhCounter3D_) != ERROR_SUCCESS) {
            ::PdhAddCounterW(pdhQuery_, kGpu3DCounterPath, 0, &pdhCounter3D_);
        }
        if (::PdhAddEnglishCounterW(pdhQuery_, kGpuDedVramPath, 0, &pdhCounterDedVram_) != ERROR_SUCCESS) {
            ::PdhAddCounterW(pdhQuery_, kGpuDedVramPath, 0, &pdhCounterDedVram_);
        }
        if (::PdhAddEnglishCounterW(pdhQuery_, kGpuSharedVramPath, 0, &pdhCounterSharedVram_) != ERROR_SUCCESS) {
            ::PdhAddCounterW(pdhQuery_, kGpuSharedVramPath, 0, &pdhCounterSharedVram_);
        }
        if (::PdhAddEnglishCounterW(pdhQuery_, kGpuThermalPath, 0, &pdhCounterGpuTemp_) != ERROR_SUCCESS) {
            ::PdhAddCounterW(pdhQuery_, kGpuThermalPath, 0, &pdhCounterGpuTemp_);
        }

        ::PdhCollectQueryData(pdhQuery_);
    }

    void InitializeNvml() noexcept {
        nvmlModule_ = ::LoadLibraryW(L"nvml.dll");
        if (nvmlModule_ == nullptr) {
            wchar_t sysDir[MAX_PATH]{};
            if (::GetSystemDirectoryW(sysDir, MAX_PATH) > 0) {
                std::wstring nvmlPath(sysDir);
                nvmlPath += L"\\nvml.dll";
                nvmlModule_ = ::LoadLibraryW(nvmlPath.c_str());
            }
        }
        if (nvmlModule_ == nullptr) {
            return;
        }

        using NvmlInitFn      = int (*)();
        using NvmlGetDeviceFn = int (*)(unsigned int, void**);
        using NvmlGetTempFn   = int (*)(void*, int, unsigned int*);
        using NvmlShutdownFn  = int (*)();

        auto initFn = reinterpret_cast<NvmlInitFn>(
            reinterpret_cast<void*>(::GetProcAddress(nvmlModule_, "nvmlInit_v2"))
        );
        if (initFn == nullptr) {
            initFn = reinterpret_cast<NvmlInitFn>(
                reinterpret_cast<void*>(::GetProcAddress(nvmlModule_, "nvmlInit"))
            );
        }
        auto getDevFn = reinterpret_cast<NvmlGetDeviceFn>(
            reinterpret_cast<void*>(::GetProcAddress(nvmlModule_, "nvmlDeviceGetHandleByIndex_v2"))
        );
        if (getDevFn == nullptr) {
            getDevFn = reinterpret_cast<NvmlGetDeviceFn>(
                reinterpret_cast<void*>(::GetProcAddress(nvmlModule_, "nvmlDeviceGetHandleByIndex"))
            );
        }
        nvmlGetTempFn_ = reinterpret_cast<NvmlGetTempFn>(
            reinterpret_cast<void*>(::GetProcAddress(nvmlModule_, "nvmlDeviceGetTemperature"))
        );
        nvmlShutdownFn_ = reinterpret_cast<NvmlShutdownFn>(
            reinterpret_cast<void*>(::GetProcAddress(nvmlModule_, "nvmlShutdown"))
        );

        if (initFn != nullptr && getDevFn != nullptr && nvmlGetTempFn_ != nullptr) {
            if (initFn() == 0) {
                nvmlInitialized_ = true;
                (void)getDevFn(0U, &nvmlDeviceHandle_);
            }
        }
    }

    void ShutdownNvml() noexcept {
        if (nvmlInitialized_ && nvmlShutdownFn_ != nullptr) {
            (void)nvmlShutdownFn_();
        }
        nvmlInitialized_  = false;
        nvmlDeviceHandle_ = nullptr;
        nvmlGetTempFn_    = nullptr;
        nvmlShutdownFn_   = nullptr;
        if (nvmlModule_ != nullptr) {
            ::FreeLibrary(nvmlModule_);
            nvmlModule_ = nullptr;
        }
    }

    void InitializeAdl() noexcept {
        adlModule_ = ::LoadLibraryW(L"atiadlxx.dll");
        if (adlModule_ == nullptr) {
            adlModule_ = ::LoadLibraryW(L"atiadlxy.dll");
        }
        if (adlModule_ == nullptr) {
            return;
        }

        adlMainControlCreate_ = reinterpret_cast<ADL_Main_Control_Create_Fn>(
            reinterpret_cast<void*>(::GetProcAddress(adlModule_, "ADL_Main_Control_Create"))
        );
        adlMainControlDestroy_ = reinterpret_cast<ADL_Main_Control_Destroy_Fn>(
            reinterpret_cast<void*>(::GetProcAddress(adlModule_, "ADL_Main_Control_Destroy"))
        );
        adlNumberOfAdaptersGet_ = reinterpret_cast<ADL_Adapter_NumberOfAdapters_Get_Fn>(
            reinterpret_cast<void*>(::GetProcAddress(adlModule_, "ADL_Adapter_NumberOfAdapters_Get"))
        );
        adlOverdrive5TempGet_ = reinterpret_cast<ADL_Overdrive5_Temperature_Get_Fn>(
            reinterpret_cast<void*>(::GetProcAddress(adlModule_, "ADL_Overdrive5_Temperature_Get"))
        );
        adlOverdrive6TempGet_ = reinterpret_cast<ADL_Overdrive6_Temperature_Get_Fn>(
            reinterpret_cast<void*>(::GetProcAddress(adlModule_, "ADL_Overdrive6_Temperature_Get"))
        );
        adl2MainControlCreate_ = reinterpret_cast<ADL2_Main_Control_Create_Fn>(
            reinterpret_cast<void*>(::GetProcAddress(adlModule_, "ADL2_Main_Control_Create"))
        );
        adl2MainControlDestroy_ = reinterpret_cast<ADL2_Main_Control_Destroy_Fn>(
            reinterpret_cast<void*>(::GetProcAddress(adlModule_, "ADL2_Main_Control_Destroy"))
        );
        adl2OverdriveNTempGet_ = reinterpret_cast<ADL2_OverdriveN_Temperature_Get_Fn>(
            reinterpret_cast<void*>(::GetProcAddress(adlModule_, "ADL2_OverdriveN_Temperature_Get"))
        );
        adl2QueryPmLogDataGet_ = reinterpret_cast<ADL2_New_QueryPMLogData_Get_Fn>(
            reinterpret_cast<void*>(::GetProcAddress(adlModule_, "ADL2_New_QueryPMLogData_Get"))
        );

        if (adlMainControlCreate_ != nullptr) {
            if (adlMainControlCreate_(&AdlMainMemoryAlloc, 1) == 0) {
                adlInitialized_ = true;
                int numAdapters = 0;
                if (adlNumberOfAdaptersGet_ != nullptr &&
                    adlNumberOfAdaptersGet_(&numAdapters) == 0 &&
                    numAdapters > 0) {
                    adlAdapterCount_ = numAdapters;
                } else {
                    adlAdapterCount_ = 1;
                }
            }
        }

        if (adl2MainControlCreate_ != nullptr) {
            if (adl2MainControlCreate_(&AdlMainMemoryAlloc, 1, &adlContext_) == 0 && adlContext_ != nullptr) {
                adlInitialized_ = true;
                if (adlAdapterCount_ <= 0) {
                    adlAdapterCount_ = 1;
                }
            }
        }
    }

    void ShutdownAdl() noexcept {
        if (adlContext_ != nullptr && adl2MainControlDestroy_ != nullptr) {
            (void)adl2MainControlDestroy_(adlContext_);
            adlContext_ = nullptr;
        }
        if (adlInitialized_ && adlMainControlDestroy_ != nullptr) {
            (void)adlMainControlDestroy_();
        }
        adlInitialized_         = false;
        adlAdapterCount_        = 0;
        adlMainControlCreate_   = nullptr;
        adlMainControlDestroy_  = nullptr;
        adlNumberOfAdaptersGet_ = nullptr;
        adlOverdrive5TempGet_   = nullptr;
        adlOverdrive6TempGet_   = nullptr;
        adl2MainControlCreate_  = nullptr;
        adl2MainControlDestroy_ = nullptr;
        adl2OverdriveNTempGet_  = nullptr;
        adl2QueryPmLogDataGet_  = nullptr;
        if (adlModule_ != nullptr) {
            ::FreeLibrary(adlModule_);
            adlModule_ = nullptr;
        }
    }

    [[nodiscard]] double SampleAdlTemperature() const noexcept {
        if (!adlInitialized_) {
            return 0.0;
        }

        const int maxAdapters = std::clamp(adlAdapterCount_, 1, 16);
        for (int idx = 0; idx < maxAdapters; ++idx) {
            // 1. Standard Overdrive5 temperature query (millidegrees Celsius, e.g. 45000 -> 45.0 C)
            if (adlOverdrive5TempGet_ != nullptr) {
                ADLTemperature tempData{};
                tempData.iSize = static_cast<int>(sizeof(ADLTemperature));
                if (adlOverdrive5TempGet_(idx, 0, &tempData) == 0 && tempData.iTemperature > 0) {
                    const double c = (tempData.iTemperature >= 1000)
                                         ? (static_cast<double>(tempData.iTemperature) / 1000.0)
                                         : static_cast<double>(tempData.iTemperature);
                    if (c >= 15.0 && c <= 125.0) {
                        return c;
                    }
                }
            }

            // 2. OverdriveN temperature query for Ryzen APUs (millidegrees Celsius, e.g. 71750 -> 71.75 C)
            if (adl2OverdriveNTempGet_ != nullptr) {
                int odnTemp = 0;
                if (adl2OverdriveNTempGet_(adlContext_, idx, 1, &odnTemp) == 0 && odnTemp > 0) {
                    const double c = (odnTemp >= 1000)
                                         ? (static_cast<double>(odnTemp) / 1000.0)
                                         : static_cast<double>(odnTemp);
                    if (c >= 15.0 && c <= 125.0) {
                        return c;
                    }
                }
            }

            // 3. Overdrive6 temperature query
            if (adlOverdrive6TempGet_ != nullptr) {
                int od6Temp = 0;
                if (adlOverdrive6TempGet_(idx, &od6Temp) == 0 && od6Temp > 0) {
                    const double c = (od6Temp >= 1000)
                                         ? (static_cast<double>(od6Temp) / 1000.0)
                                         : static_cast<double>(od6Temp);
                    if (c >= 15.0 && c <= 125.0) {
                        return c;
                    }
                }
            }

            // 4. PMLog sensor table fallback (indices 28 = GFX Temp, 29 = SoC Temp in whole degrees Celsius)
            if (adl2QueryPmLogDataGet_ != nullptr) {
                ADLPMLogDataOutput pmOut{};
                pmOut.size = static_cast<int>(sizeof(ADLPMLogDataOutput));
                if (adl2QueryPmLogDataGet_(adlContext_, idx, &pmOut) == 0) {
                    constexpr int kPmTempSensorIds[] = {28, 29};
                    for (int sensorId : kPmTempSensorIds) {
                        if (pmOut.sensors[sensorId].supported != 0U) {
                            const int rawVal = pmOut.sensors[sensorId].value;
                            const double c = (rawVal >= 1000)
                                                 ? (static_cast<double>(rawVal) / 1000.0)
                                                 : static_cast<double>(rawVal);
                            if (c >= 15.0 && c <= 125.0) {
                                return c;
                            }
                        }
                    }
                }
            }
        }
        return 0.0;
    }

    void SampleGpuTemperatureThrottled(GpuVramMetrics& outMetrics) noexcept {
        const ULONGLONG nowMs = ::GetTickCount64();
        if (lastGpuTempPollMs_ == 0ULL || (nowMs - lastGpuTempPollMs_) >= 2000ULL) {
            lastGpuTempPollMs_ = nowMs;

            double sampledC = 0.0;

            // 1. Try AMD Display Library (atiadlxx.dll) for Radeon GPUs and Ryzen APUs.
            sampledC = SampleAdlTemperature();

            // 2. Try NVIDIA NVML dynamic interface if ADL did not return a reading.
            if (sampledC <= 0.5 && nvmlInitialized_ && nvmlDeviceHandle_ != nullptr && nvmlGetTempFn_ != nullptr) {
                unsigned int tempVal = 0U;
                if (nvmlGetTempFn_(nvmlDeviceHandle_, 0, &tempVal) == 0 && tempVal > 0U && tempVal < 130U) {
                    sampledC = static_cast<double>(tempVal);
                }
            }

            // 3. Try PDH GPU Thermal Zone counter fallback.
            if (sampledC <= 0.5 && pdhCounterGpuTemp_ != nullptr) {
                const double rawTemp = SumCounterArray(pdhCounterGpuTemp_);
                if (rawTemp > 200.0 && rawTemp < 420.0) {
                    sampledC = rawTemp - 273.15;
                } else if (rawTemp >= 15.0 && rawTemp <= 125.0) {
                    sampledC = rawTemp;
                }
            }

            if (sampledC >= 15.0 && sampledC <= 125.0) {
                cachedGpuTempCelsius_ = sampledC;
                hasGpuTemp_           = true;
                g_sharedApuPackageTempCelsius.store(sampledC, std::memory_order_release);
            }
        }

        outMetrics.gpuTempCelsius = cachedGpuTempCelsius_;
        outMetrics.hasGpuTemp     = hasGpuTemp_;
    }

    [[nodiscard]] double SumCounterArray(PDH_HCOUNTER counter) noexcept {
        if (counter == nullptr) {
            return 0.0;
        }

        DWORD bufferSize = static_cast<DWORD>(pdhScratchBuffer_.size());
        DWORD itemCount  = 0;
        auto* items      = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(pdhScratchBuffer_.data());

        const PDH_STATUS status = ::PdhGetFormattedCounterArrayW(
            counter,
            PDH_FMT_DOUBLE,
            &bufferSize,
            &itemCount,
            items
        );

        const PDH_STATUS moreDataStatus = static_cast<PDH_STATUS>(PDH_MORE_DATA);
        if (status != ERROR_SUCCESS && status != moreDataStatus) {
            return 0.0;
        }
        if (status == moreDataStatus) {
            return 0.0;
        }

        double sum = 0.0;
        for (DWORD i = 0; i < itemCount; ++i) {
            if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA ||
                items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA) {
                sum += items[i].FmtValue.doubleValue;
            }
        }
        return sum;
    }

    void SampleGpuPdhCounters(GpuVramMetrics& outMetrics) noexcept {
        if (pdhQuery_ == nullptr) {
            outMetrics.gpuLoadPercent = 0.0;
            return;
        }

        if (::PdhCollectQueryData(pdhQuery_) != ERROR_SUCCESS) {
            outMetrics.gpuLoadPercent = 0.0;
            return;
        }

        if (pdhCounter3D_ != nullptr) {
            lastGpuLoad_ = std::clamp(SumCounterArray(pdhCounter3D_), 0.0, 100.0);
        }
        outMetrics.gpuLoadPercent = lastGpuLoad_;

        constexpr double kBytesPerGb = 1024.0 * 1024.0 * 1024.0;
        if (pdhCounterDedVram_ != nullptr) {
            const double dedBytes = SumCounterArray(pdhCounterDedVram_);
            if (dedBytes > 0.0) {
                outMetrics.dedicatedVramUsedGb = dedBytes / kBytesPerGb;
            }
        }
        if (pdhCounterSharedVram_ != nullptr) {
            const double sharedBytes = SumCounterArray(pdhCounterSharedVram_);
            if (sharedBytes > 0.0) {
                outMetrics.sharedVramUsedGb = sharedBytes / kBytesPerGb;
            }
        }
    }

    ScopedComPtr<IDXGIFactory1> dxgiFactory_{};
    ScopedComPtr<IDXGIAdapter3> activeAdapter3_{};
    char                        adapterName_[128]{};

    PDH_HQUERY                  pdhQuery_{nullptr};
    PDH_HCOUNTER                pdhCounter3D_{nullptr};
    PDH_HCOUNTER                pdhCounterDedVram_{nullptr};
    PDH_HCOUNTER                pdhCounterSharedVram_{nullptr};
    PDH_HCOUNTER                pdhCounterGpuTemp_{nullptr};
    double                      lastGpuLoad_{0.0};
    alignas(PDH_FMT_COUNTERVALUE_ITEM_W) std::array<uint8_t, 65536> pdhScratchBuffer_{};

    HMODULE                     nvmlModule_{nullptr};
    void*                       nvmlDeviceHandle_{nullptr};
    int                         (*nvmlGetTempFn_)(void*, int, unsigned int*){nullptr};
    int                         (*nvmlShutdownFn_)(){nullptr};
    bool                        nvmlInitialized_{false};

    HMODULE                               adlModule_{nullptr};
    void*                                 adlContext_{nullptr};
    ADL_Main_Control_Create_Fn            adlMainControlCreate_{nullptr};
    ADL_Main_Control_Destroy_Fn           adlMainControlDestroy_{nullptr};
    ADL_Adapter_NumberOfAdapters_Get_Fn   adlNumberOfAdaptersGet_{nullptr};
    ADL_Overdrive5_Temperature_Get_Fn     adlOverdrive5TempGet_{nullptr};
    ADL_Overdrive6_Temperature_Get_Fn     adlOverdrive6TempGet_{nullptr};
    ADL2_Main_Control_Create_Fn           adl2MainControlCreate_{nullptr};
    ADL2_Main_Control_Destroy_Fn          adl2MainControlDestroy_{nullptr};
    ADL2_OverdriveN_Temperature_Get_Fn    adl2OverdriveNTempGet_{nullptr};
    ADL2_New_QueryPMLogData_Get_Fn        adl2QueryPmLogDataGet_{nullptr};
    bool                                  adlInitialized_{false};
    int                                   adlAdapterCount_{0};

    ULONGLONG                   lastGpuTempPollMs_{0ULL};
    double                      cachedGpuTempCelsius_{0.0};
    bool                        hasGpuTemp_{false};
};

} // namespace zetta::telemetry
