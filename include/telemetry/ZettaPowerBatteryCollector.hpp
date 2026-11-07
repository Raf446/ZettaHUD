#pragma once

#include "tracker/ZettaWindowTracker.hpp"

#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <batclass.h>
#include <powrprof.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace zetta::telemetry {

struct PowerBatteryMetrics {
    double powerWatts{0.0};
    double capacityPercent{100.0};
    bool   hasBattery{false};
    bool   isAcOnline{true};
    bool   isCharging{false};
    bool   isDischarging{false};
};

class ZettaPowerBatteryCollector {
public:
    ZettaPowerBatteryCollector() noexcept = default;

    ~ZettaPowerBatteryCollector() noexcept {
        Shutdown();
    }

    ZettaPowerBatteryCollector(const ZettaPowerBatteryCollector&) = delete;
    ZettaPowerBatteryCollector& operator=(const ZettaPowerBatteryCollector&) = delete;

    void Initialize() noexcept {
        OpenBatteryDevice();
    }

    void Shutdown() noexcept {
        batteryHandle_.Reset();
        batteryTag_ = 0;
    }

    void Sample(PowerBatteryMetrics& outMetrics) noexcept {
        outMetrics = PowerBatteryMetrics{};

        SYSTEM_POWER_STATUS sps{};
        if (::GetSystemPowerStatus(&sps)) {
            outMetrics.isAcOnline = (sps.ACLineStatus == 1);
            outMetrics.hasBattery = ((sps.BatteryFlag & 128) == 0 && sps.BatteryFlag != 255);
            if (sps.BatteryLifePercent <= 100) {
                outMetrics.capacityPercent = static_cast<double>(sps.BatteryLifePercent);
            }
        }

        if (SampleBatteryIoctl(outMetrics)) {
            return;
        }

        SampleNtPowerFallback(outMetrics);
    }

private:
    void OpenBatteryDevice() noexcept {
        // GUID_DEVCLASS_BATTERY: {72631E54-78A4-11D0-BCF7-00AA00B7B32A}
        static constexpr GUID kBatteryClassGuid = {
            0x72631e54U, 0x78a4U, 0x11d0U, {0xbc, 0xf7, 0x00, 0xaa, 0x00, 0xb7, 0xb3, 0x2a}
        };

        HDEVINFO devInfo = ::SetupDiGetClassDevsW(
            &kBatteryClassGuid,
            nullptr,
            nullptr,
            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
        );
        if (devInfo == INVALID_HANDLE_VALUE) {
            return;
        }

        SP_DEVICE_INTERFACE_DATA interfaceData{};
        interfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

        if (::SetupDiEnumDeviceInterfaces(devInfo, nullptr, &kBatteryClassGuid, 0, &interfaceData)) {
            DWORD requiredSize = 0;
            ::SetupDiGetDeviceInterfaceDetailW(devInfo, &interfaceData, nullptr, 0, &requiredSize, nullptr);

            if (requiredSize > 0 && requiredSize <= 1024) {
                alignas(SP_DEVICE_INTERFACE_DETAIL_DATA_W) uint8_t detailBuf[1024]{};
                auto* detailData = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuf);
                detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

                if (::SetupDiGetDeviceInterfaceDetailW(
                        devInfo,
                        &interfaceData,
                        detailData,
                        requiredSize,
                        nullptr,
                        nullptr)) {
                    batteryHandle_.Reset(::CreateFileW(
                        detailData->DevicePath,
                        GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr,
                        OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL,
                        nullptr
                    ));
                }
            }
        }

        ::SetupDiDestroyDeviceInfoList(devInfo);
        QueryBatteryTagAndFullCapacity();
    }

    void QueryBatteryTagAndFullCapacity() noexcept {
        if (!batteryHandle_.IsValid()) {
            return;
        }

        ULONG waitTimeout = 0;
        DWORD bytesReturned = 0;
        ULONG tag = 0;
        if (!::DeviceIoControl(
                batteryHandle_.Get(),
                IOCTL_BATTERY_QUERY_TAG,
                &waitTimeout,
                sizeof(waitTimeout),
                &tag,
                sizeof(tag),
                &bytesReturned,
                nullptr) || tag == 0) {
            return;
        }

        batteryTag_ = tag;

        BATTERY_QUERY_INFORMATION bqi{};
        bqi.BatteryTag       = batteryTag_;
        bqi.InformationLevel = BatteryInformation;

        BATTERY_INFORMATION bi{};
        if (::DeviceIoControl(
                batteryHandle_.Get(),
                IOCTL_BATTERY_QUERY_INFORMATION,
                &bqi,
                sizeof(bqi),
                &bi,
                sizeof(bi),
                &bytesReturned,
                nullptr)) {
            fullChargedCapacityMwh_ = bi.FullChargedCapacity;
        }
    }

    [[nodiscard]] bool SampleBatteryIoctl(PowerBatteryMetrics& outMetrics) noexcept {
        if (!batteryHandle_.IsValid()) {
            return false;
        }

        if (batteryTag_ == 0) {
            QueryBatteryTagAndFullCapacity();
            if (batteryTag_ == 0) {
                return false;
            }
        }

        BATTERY_WAIT_STATUS bws{};
        bws.BatteryTag = batteryTag_;

        BATTERY_STATUS bs{};
        DWORD bytesReturned = 0;
        if (!::DeviceIoControl(
                batteryHandle_.Get(),
                IOCTL_BATTERY_QUERY_STATUS,
                &bws,
                sizeof(bws),
                &bs,
                sizeof(bs),
                &bytesReturned,
                nullptr)) {
            batteryTag_ = 0;
            return false;
        }

        outMetrics.hasBattery    = true;
        outMetrics.isAcOnline    = (bs.PowerState & BATTERY_POWER_ON_LINE) != 0;
        outMetrics.isCharging    = (bs.PowerState & BATTERY_CHARGING) != 0;
        outMetrics.isDischarging = (bs.PowerState & BATTERY_DISCHARGING) != 0;

        if (bs.Rate != static_cast<LONG>(BATTERY_UNKNOWN_RATE)) {
            outMetrics.powerWatts = std::abs(static_cast<double>(bs.Rate)) / 1000.0;
        } else {
            outMetrics.powerWatts = 0.0;
        }

        if (fullChargedCapacityMwh_ > 0 && bs.Capacity != BATTERY_UNKNOWN_CAPACITY) {
            const double pct = (static_cast<double>(bs.Capacity) * 100.0) /
                               static_cast<double>(fullChargedCapacityMwh_);
            outMetrics.capacityPercent = std::clamp(pct, 0.0, 100.0);
        }

        return true;
    }

    static void SampleNtPowerFallback(PowerBatteryMetrics& outMetrics) noexcept {
        SYSTEM_BATTERY_STATE sbs{};
        if (::CallNtPowerInformation(
                SystemBatteryState,
                nullptr,
                0,
                &sbs,
                sizeof(sbs)) != 0) {
            return;
        }

        outMetrics.hasBattery    = sbs.BatteryPresent != 0;
        outMetrics.isAcOnline    = sbs.AcOnLine != 0;
        outMetrics.isCharging    = sbs.Charging != 0;
        outMetrics.isDischarging = sbs.Discharging != 0;

        if (static_cast<DWORD>(sbs.Rate) != static_cast<DWORD>(BATTERY_UNKNOWN_RATE)) {
            outMetrics.powerWatts = std::abs(static_cast<double>(static_cast<LONG>(sbs.Rate))) / 1000.0;
        }
        if (sbs.MaxCapacity > 0) {
            const double pct = (static_cast<double>(sbs.RemainingCapacity) * 100.0) /
                               static_cast<double>(sbs.MaxCapacity);
            outMetrics.capacityPercent = std::clamp(pct, 0.0, 100.0);
        }
    }

    zetta::tracker::ScopedKernelHandle batteryHandle_{};
    ULONG                              batteryTag_{0};
    ULONG                              fullChargedCapacityMwh_{0};
};

} // namespace zetta::telemetry
