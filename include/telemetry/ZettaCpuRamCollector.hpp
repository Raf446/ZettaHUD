#pragma once

#include "telemetry/ZettaGpuVramCollector.hpp"
#include "tracker/ZettaWindowTracker.hpp"

#include <windows.h>
#include <objbase.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <wbemidl.h>

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace zetta::telemetry {

struct CpuRamMetrics {
    double   systemCpuPercent{0.0};
    double   cpuTempCelsius{0.0};
    bool     hasCpuTemp{false};
    double   systemRamUsedGb{0.0};
    double   systemRamTotalGb{0.0};
    double   systemRamPercent{0.0};
    double   systemCommitUsedGb{0.0};
    double   processWorkingSetMb{0.0};
    double   processPrivateMb{0.0};
    uint32_t processHandleCount{0};
};

class ZettaCpuRamCollector {
public:
    ZettaCpuRamCollector() noexcept = default;

    ~ZettaCpuRamCollector() noexcept {
        Shutdown();
    }

    ZettaCpuRamCollector(const ZettaCpuRamCollector&) = delete;
    ZettaCpuRamCollector& operator=(const ZettaCpuRamCollector&) = delete;

    void Initialize() noexcept {
        (void)SampleCpuLoad();
        InitializeThermalSensors();
    }

    void Shutdown() noexcept {
        if (thermalPdhQuery_ != nullptr) {
            ::PdhCloseQuery(thermalPdhQuery_);
            thermalPdhQuery_        = nullptr;
            thermalCounterHighPrec_ = nullptr;
            thermalCounterKelvin_   = nullptr;
        }
        if (wbemServicesCimv2_ != nullptr) {
            wbemServicesCimv2_->Release();
            wbemServicesCimv2_ = nullptr;
        }
        if (wbemServicesWmi_ != nullptr) {
            wbemServicesWmi_->Release();
            wbemServicesWmi_ = nullptr;
        }
        if (wbemLocator_ != nullptr) {
            wbemLocator_->Release();
            wbemLocator_ = nullptr;
        }
    }

    void Sample(uint32_t targetPid, CpuRamMetrics& outMetrics) noexcept {
        outMetrics.systemCpuPercent = SampleCpuLoad();
        SampleCpuTemperatureThrottled(outMetrics);
        SampleSystemMemory(outMetrics);
        SampleTargetProcessMemory(targetPid, outMetrics);
    }

private:
    using SysAllocStringFn = BSTR(WINAPI*)( const OLECHAR* );
    using SysFreeStringFn  = void(WINAPI*)( BSTR );
    using VariantInitFn    = void(WINAPI*)( VARIANTARG* );
    using VariantClearFn   = HRESULT(WINAPI*)( VARIANTARG* );

    struct ScopedBstr {
        BSTR             str{nullptr};
        SysFreeStringFn  freeFn{nullptr};

        ScopedBstr(const wchar_t* text, SysAllocStringFn allocFn, SysFreeStringFn fFn) noexcept
            : str(allocFn ? allocFn(text) : nullptr),
              freeFn(fFn) {}

        ~ScopedBstr() noexcept {
            if (str != nullptr && freeFn != nullptr) {
                freeFn(str);
            }
        }

        ScopedBstr(const ScopedBstr&) = delete;
        ScopedBstr& operator=(const ScopedBstr&) = delete;
    };

    void InitializeThermalSensors() noexcept {
        HMODULE hOleAut = ::GetModuleHandleW(L"oleaut32.dll");
        if (hOleAut == nullptr) {
            hOleAut = ::LoadLibraryW(L"oleaut32.dll");
        }
        if (hOleAut != nullptr) {
            sysAllocString_ = reinterpret_cast<SysAllocStringFn>(
                reinterpret_cast<void*>(::GetProcAddress(hOleAut, "SysAllocString"))
            );
            sysFreeString_ = reinterpret_cast<SysFreeStringFn>(
                reinterpret_cast<void*>(::GetProcAddress(hOleAut, "SysFreeString"))
            );
            variantInit_ = reinterpret_cast<VariantInitFn>(
                reinterpret_cast<void*>(::GetProcAddress(hOleAut, "VariantInit"))
            );
            variantClear_ = reinterpret_cast<VariantClearFn>(
                reinterpret_cast<void*>(::GetProcAddress(hOleAut, "VariantClear"))
            );
        }

        if (::PdhOpenQueryW(nullptr, 0, &thermalPdhQuery_) == ERROR_SUCCESS) {
            constexpr const wchar_t* kHighPrecPath =
                L"\\Thermal Zone Information(*)\\High Precision Temperature";
            constexpr const wchar_t* kTempPath =
                L"\\Thermal Zone Information(*)\\Temperature";

            if (::PdhAddEnglishCounterW(thermalPdhQuery_, kHighPrecPath, 0, &thermalCounterHighPrec_) != ERROR_SUCCESS) {
                ::PdhAddCounterW(thermalPdhQuery_, kHighPrecPath, 0, &thermalCounterHighPrec_);
            }
            if (::PdhAddEnglishCounterW(thermalPdhQuery_, kTempPath, 0, &thermalCounterKelvin_) != ERROR_SUCCESS) {
                ::PdhAddCounterW(thermalPdhQuery_, kTempPath, 0, &thermalCounterKelvin_);
            }
            ::PdhCollectQueryData(thermalPdhQuery_);
        }
    }

    void EnsureWmiConnected() noexcept {
        if (wmiInitAttempted_) {
            return;
        }
        wmiInitAttempted_ = true;

        if (!sysAllocString_ || !sysFreeString_ || !variantInit_ || !variantClear_) {
            return;
        }

        static constexpr GUID kClsidWbemLocator = {
            0x4590f811, 0x1d3a, 0x11d0, {0x89, 0x1f, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24}
        };
        static constexpr GUID kIidIWbemLocator = {
            0xdc12a687, 0x737f, 0x11cf, {0x88, 0x4d, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24}
        };

        const HRESULT hrLoc = ::CoCreateInstance(
            kClsidWbemLocator,
            nullptr,
            CLSCTX_INPROC_SERVER,
            kIidIWbemLocator,
            reinterpret_cast<void**>(&wbemLocator_)
        );
        if (FAILED(hrLoc) || wbemLocator_ == nullptr) {
            wbemLocator_ = nullptr;
            return;
        }

        {
            ScopedBstr nsWmi(L"ROOT\\WMI", sysAllocString_, sysFreeString_);
            if (nsWmi.str != nullptr) {
                if (SUCCEEDED(wbemLocator_->ConnectServer(
                        nsWmi.str,
                        nullptr,
                        nullptr,
                        nullptr,
                        WBEM_FLAG_CONNECT_USE_MAX_WAIT,
                        nullptr,
                        nullptr,
                        &wbemServicesWmi_)) &&
                    wbemServicesWmi_ != nullptr) {
                    (void)::CoSetProxyBlanket(
                        wbemServicesWmi_,
                        RPC_C_AUTHN_WINNT,
                        RPC_C_AUTHZ_NONE,
                        nullptr,
                        RPC_C_AUTHN_LEVEL_CALL,
                        RPC_C_IMP_LEVEL_IMPERSONATE,
                        nullptr,
                        EOAC_NONE
                    );
                }
            }
        }

        {
            ScopedBstr nsCim(L"ROOT\\CIMV2", sysAllocString_, sysFreeString_);
            if (nsCim.str != nullptr) {
                if (SUCCEEDED(wbemLocator_->ConnectServer(
                        nsCim.str,
                        nullptr,
                        nullptr,
                        nullptr,
                        WBEM_FLAG_CONNECT_USE_MAX_WAIT,
                        nullptr,
                        nullptr,
                        &wbemServicesCimv2_)) &&
                    wbemServicesCimv2_ != nullptr) {
                    (void)::CoSetProxyBlanket(
                        wbemServicesCimv2_,
                        RPC_C_AUTHN_WINNT,
                        RPC_C_AUTHZ_NONE,
                        nullptr,
                        RPC_C_AUTHN_LEVEL_CALL,
                        RPC_C_IMP_LEVEL_IMPERSONATE,
                        nullptr,
                        EOAC_NONE
                    );
                }
            }
        }
    }

    void SampleCpuTemperatureThrottled(CpuRamMetrics& outMetrics) noexcept {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastTempPollTime_
        ).count();

        if (hasPolledTempOnce_ && elapsedMs < 2000) {
            if (cachedCpuTempC_ <= 0.5) {
                const double sharedApuTemp = g_sharedApuPackageTempCelsius.load(std::memory_order_acquire);
                if (sharedApuTemp >= 10.0 && sharedApuTemp <= 125.0) {
                    cachedCpuTempC_ = sharedApuTemp;
                }
            }
            outMetrics.cpuTempCelsius = cachedCpuTempC_;
            outMetrics.hasCpuTemp     = (cachedCpuTempC_ > 0.5);
            return;
        }

        hasPolledTempOnce_ = true;
        lastTempPollTime_  = now;

        double tempC = QueryWmiThermalTemperature();
        if (tempC <= 0.5) {
            tempC = QueryPdhThermalTemperature();
        }
        // Fallback to the shared AMD APU / GPU thermal package reading when Ring 3 WMI is blocked.
        if (tempC <= 0.5) {
            tempC = g_sharedApuPackageTempCelsius.load(std::memory_order_acquire);
        }

        cachedCpuTempC_           = (tempC >= 10.0 && tempC <= 125.0) ? tempC : 0.0;
        outMetrics.cpuTempCelsius = cachedCpuTempC_;
        outMetrics.hasCpuTemp     = (cachedCpuTempC_ > 0.5);
    }

    [[nodiscard]] double QueryWmiThermalTemperature() noexcept {
        EnsureWmiConnected();
        if (!sysAllocString_ || !sysFreeString_ || !variantInit_ || !variantClear_) {
            return 0.0;
        }

        ScopedBstr wqlLang(L"WQL", sysAllocString_, sysFreeString_);
        if (wqlLang.str == nullptr) {
            return 0.0;
        }

        // 1. MSAcpi_ThermalZoneTemperature (tenths of Kelvin)
        if (wbemServicesWmi_ != nullptr) {
            ScopedBstr queryStr(
                L"SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature",
                sysAllocString_,
                sysFreeString_
            );
            IEnumWbemClassObject* enumerator = nullptr;
            if (queryStr.str != nullptr &&
                SUCCEEDED(wbemServicesWmi_->ExecQuery(
                    wqlLang.str,
                    queryStr.str,
                    WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                    nullptr,
                    &enumerator)) &&
                enumerator != nullptr) {
                IWbemClassObject* clsObj = nullptr;
                ULONG returned = 0;
                double celsius = 0.0;
                if (SUCCEEDED(enumerator->Next(100, 1, &clsObj, &returned)) &&
                    returned > 0 && clsObj != nullptr) {
                    VARIANT vtProp{};
                    variantInit_(&vtProp);
                    if (SUCCEEDED(clsObj->Get(L"CurrentTemperature", 0, &vtProp, nullptr, nullptr))) {
                        double rawTenthsKelvin = 0.0;
                        if (vtProp.vt == VT_I4 || vtProp.vt == VT_UI4) {
                            rawTenthsKelvin = static_cast<double>(vtProp.ulVal);
                        } else if (vtProp.vt == VT_I2 || vtProp.vt == VT_UI2) {
                            rawTenthsKelvin = static_cast<double>(vtProp.uiVal);
                        }
                        if (rawTenthsKelvin > 2732.0 && rawTenthsKelvin < 4000.0) {
                            celsius = (rawTenthsKelvin / 10.0) - 273.15;
                        }
                    }
                    (void)variantClear_(&vtProp);
                    clsObj->Release();
                }
                enumerator->Release();
                if (celsius >= 10.0 && celsius <= 125.0) {
                    return celsius;
                }
            }
        }

        // 2. Win32_TemperatureProbe fallback
        if (wbemServicesCimv2_ != nullptr) {
            ScopedBstr queryStr(
                L"SELECT CurrentReading FROM Win32_TemperatureProbe",
                sysAllocString_,
                sysFreeString_
            );
            IEnumWbemClassObject* enumerator = nullptr;
            if (queryStr.str != nullptr &&
                SUCCEEDED(wbemServicesCimv2_->ExecQuery(
                    wqlLang.str,
                    queryStr.str,
                    WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                    nullptr,
                    &enumerator)) &&
                enumerator != nullptr) {
                IWbemClassObject* clsObj = nullptr;
                ULONG returned = 0;
                double celsius = 0.0;
                if (SUCCEEDED(enumerator->Next(100, 1, &clsObj, &returned)) &&
                    returned > 0 && clsObj != nullptr) {
                    VARIANT vtProp{};
                    variantInit_(&vtProp);
                    if (SUCCEEDED(clsObj->Get(L"CurrentReading", 0, &vtProp, nullptr, nullptr))) {
                        if (vtProp.vt == VT_I4 || vtProp.vt == VT_UI4) {
                            const double raw = static_cast<double>(vtProp.lVal);
                            celsius = (raw > 200.0) ? (raw / 10.0) : raw;
                        }
                    }
                    (void)variantClear_(&vtProp);
                    clsObj->Release();
                }
                enumerator->Release();
                if (celsius >= 10.0 && celsius <= 125.0) {
                    return celsius;
                }
            }
        }

        return 0.0;
    }

    [[nodiscard]] double QueryPdhThermalTemperature() noexcept {
        if (thermalPdhQuery_ == nullptr) {
            return 0.0;
        }
        if (::PdhCollectQueryData(thermalPdhQuery_) != ERROR_SUCCESS) {
            return 0.0;
        }

        if (thermalCounterHighPrec_ != nullptr) {
            PDH_FMT_COUNTERVALUE fmtVal{};
            if (::PdhGetFormattedCounterValue(thermalCounterHighPrec_, PDH_FMT_DOUBLE, nullptr, &fmtVal) == ERROR_SUCCESS &&
                (fmtVal.CStatus == PDH_CSTATUS_VALID_DATA || fmtVal.CStatus == PDH_CSTATUS_NEW_DATA)) {
                const double c = (fmtVal.doubleValue / 10.0) - 273.15;
                if (c >= 10.0 && c <= 125.0) {
                    return c;
                }
            }
        }

        if (thermalCounterKelvin_ != nullptr) {
            PDH_FMT_COUNTERVALUE fmtVal{};
            if (::PdhGetFormattedCounterValue(thermalCounterKelvin_, PDH_FMT_DOUBLE, nullptr, &fmtVal) == ERROR_SUCCESS &&
                (fmtVal.CStatus == PDH_CSTATUS_VALID_DATA || fmtVal.CStatus == PDH_CSTATUS_NEW_DATA)) {
                const double c = fmtVal.doubleValue - 273.15;
                if (c >= 10.0 && c <= 125.0) {
                    return c;
                }
            }
        }

        return 0.0;
    }

    [[nodiscard]] static uint64_t FileTimeToUint64(const FILETIME& ft) noexcept {
        ULARGE_INTEGER uli{};
        uli.LowPart  = ft.dwLowDateTime;
        uli.HighPart = ft.dwHighDateTime;
        return uli.QuadPart;
    }

    [[nodiscard]] double SampleCpuLoad() noexcept {
        FILETIME idleFt{};
        FILETIME kernelFt{};
        FILETIME userFt{};

        if (!::GetSystemTimes(&idleFt, &kernelFt, &userFt)) {
            return lastCpuPercent_;
        }

        const uint64_t idle   = FileTimeToUint64(idleFt);
        const uint64_t kernel = FileTimeToUint64(kernelFt);
        const uint64_t user   = FileTimeToUint64(userFt);

        if (!hasBaseline_) {
            prevIdle_    = idle;
            prevKernel_  = kernel;
            prevUser_    = user;
            hasBaseline_ = true;
            return 0.0;
        }

        const uint64_t idleDelta   = (idle >= prevIdle_) ? (idle - prevIdle_) : 0ULL;
        const uint64_t kernelDelta = (kernel >= prevKernel_) ? (kernel - prevKernel_) : 0ULL;
        const uint64_t userDelta   = (user >= prevUser_) ? (user - prevUser_) : 0ULL;

        prevIdle_   = idle;
        prevKernel_ = kernel;
        prevUser_   = user;

        // GetSystemTimes includes idle time inside kernel time.
        const uint64_t totalDelta = kernelDelta + userDelta;
        if (totalDelta == 0ULL) {
            return lastCpuPercent_;
        }

        const uint64_t busyDelta = (totalDelta > idleDelta) ? (totalDelta - idleDelta) : 0ULL;
        const double rawPercent  = (static_cast<double>(busyDelta) * 100.0) / static_cast<double>(totalDelta);
        lastCpuPercent_          = std::clamp(rawPercent, 0.0, 100.0);
        return lastCpuPercent_;
    }

    static void SampleSystemMemory(CpuRamMetrics& outMetrics) noexcept {
        MEMORYSTATUSEX memStatus{};
        memStatus.dwLength = sizeof(MEMORYSTATUSEX);
        if (!::GlobalMemoryStatusEx(&memStatus)) {
            return;
        }

        constexpr double kBytesPerGb = 1024.0 * 1024.0 * 1024.0;
        const uint64_t usedPhys = (memStatus.ullTotalPhys > memStatus.ullAvailPhys)
                                      ? (memStatus.ullTotalPhys - memStatus.ullAvailPhys)
                                      : 0ULL;
        const uint64_t usedPage = (memStatus.ullTotalPageFile > memStatus.ullAvailPageFile)
                                      ? (memStatus.ullTotalPageFile - memStatus.ullAvailPageFile)
                                      : 0ULL;

        outMetrics.systemRamTotalGb   = static_cast<double>(memStatus.ullTotalPhys) / kBytesPerGb;
        outMetrics.systemRamUsedGb    = static_cast<double>(usedPhys) / kBytesPerGb;
        outMetrics.systemRamPercent   = static_cast<double>(memStatus.dwMemoryLoad);
        outMetrics.systemCommitUsedGb = static_cast<double>(usedPage) / kBytesPerGb;
    }

    static void SampleTargetProcessMemory(uint32_t targetPid, CpuRamMetrics& outMetrics) noexcept {
        outMetrics.processWorkingSetMb = 0.0;
        outMetrics.processPrivateMb    = 0.0;
        outMetrics.processHandleCount  = 0;

        if (targetPid == 0) {
            return;
        }

        zetta::tracker::ScopedKernelHandle processHandle(
            ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, targetPid)
        );
        if (!processHandle.IsValid()) {
            processHandle.Reset(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, targetPid));
        }
        if (!processHandle.IsValid()) {
            return;
        }

        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(PROCESS_MEMORY_COUNTERS_EX);
        if (::K32GetProcessMemoryInfo(
                processHandle.Get(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                sizeof(pmc))) {
            constexpr double kBytesPerMb   = 1024.0 * 1024.0;
            outMetrics.processWorkingSetMb = static_cast<double>(pmc.WorkingSetSize) / kBytesPerMb;
            outMetrics.processPrivateMb    = static_cast<double>(pmc.PrivateUsage) / kBytesPerMb;
        }

        DWORD handleCount = 0;
        if (::GetProcessHandleCount(processHandle.Get(), &handleCount)) {
            outMetrics.processHandleCount = static_cast<uint32_t>(handleCount);
        }
    }

    uint64_t prevIdle_{0};
    uint64_t prevKernel_{0};
    uint64_t prevUser_{0};
    double   lastCpuPercent_{0.0};
    bool     hasBaseline_{false};

    PDH_HQUERY                          thermalPdhQuery_{nullptr};
    PDH_HCOUNTER                        thermalCounterHighPrec_{nullptr};
    PDH_HCOUNTER                        thermalCounterKelvin_{nullptr};

    IWbemLocator*                       wbemLocator_{nullptr};
    IWbemServices*                      wbemServicesWmi_{nullptr};
    IWbemServices*                      wbemServicesCimv2_{nullptr};
    bool                                wmiInitAttempted_{false};

    SysAllocStringFn                    sysAllocString_{nullptr};
    SysFreeStringFn                     sysFreeString_{nullptr};
    VariantInitFn                       variantInit_{nullptr};
    VariantClearFn                      variantClear_{nullptr};

    std::chrono::steady_clock::time_point lastTempPollTime_{};
    bool                                hasPolledTempOnce_{false};
    double                              cachedCpuTempC_{0.0};
};

} // namespace zetta::telemetry
