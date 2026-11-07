#pragma once

#include "tracker/ZettaWindowTracker.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winioctl.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <chrono>
#include <cstdint>

namespace zetta::telemetry {

struct StorageNetMetrics {
    double diskReadMbps{0.0};
    double diskWriteMbps{0.0};
    double diskTotalMbps{0.0};
    double netRxKbps{0.0};
    double netTxKbps{0.0};
    double netTotalKbps{0.0};
};

class ZettaStorageNetCollector {
public:
    ZettaStorageNetCollector() noexcept = default;

    ~ZettaStorageNetCollector() noexcept {
        Shutdown();
    }

    ZettaStorageNetCollector(const ZettaStorageNetCollector&) = delete;
    ZettaStorageNetCollector& operator=(const ZettaStorageNetCollector&) = delete;

    void Initialize() noexcept {
        // Desired access 0 allows querying IOCTL_DISK_PERFORMANCE without elevation.
        driveHandle_.Reset(::CreateFileW(
            L"\\\\.\\PhysicalDrive0",
            0,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
        ));

        InitializePdhDiskFallback();

        StorageNetMetrics dummy{};
        Sample(dummy);
    }

    void Shutdown() noexcept {
        if (pdhDiskQuery_ != nullptr) {
            ::PdhCloseQuery(pdhDiskQuery_);
            pdhDiskQuery_     = nullptr;
            pdhReadCounter_   = nullptr;
            pdhWriteCounter_  = nullptr;
        }
        driveHandle_.Reset();
    }

    void Sample(StorageNetMetrics& outMetrics) noexcept {
        using namespace std::chrono;
        const auto now = steady_clock::now();
        double elapsedSec = 0.0;
        if (hasTimestamp_) {
            elapsedSec = duration<double>(now - prevSampleTime_).count();
        }
        prevSampleTime_ = now;
        hasTimestamp_   = true;

        SampleDiskThroughput(elapsedSec, outMetrics);
        SampleNetworkThroughput(elapsedSec, outMetrics);
    }

private:
    void InitializePdhDiskFallback() noexcept {
        if (::PdhOpenQueryW(nullptr, 0, &pdhDiskQuery_) != ERROR_SUCCESS) {
            pdhDiskQuery_ = nullptr;
            return;
        }

        const PDH_STATUS s1 = ::PdhAddEnglishCounterW(
            pdhDiskQuery_,
            L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec",
            0,
            &pdhReadCounter_
        );
        const PDH_STATUS s2 = ::PdhAddEnglishCounterW(
            pdhDiskQuery_,
            L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec",
            0,
            &pdhWriteCounter_
        );

        if (s1 != ERROR_SUCCESS || s2 != ERROR_SUCCESS) {
            ::PdhCloseQuery(pdhDiskQuery_);
            pdhDiskQuery_    = nullptr;
            pdhReadCounter_  = nullptr;
            pdhWriteCounter_ = nullptr;
            return;
        }

        ::PdhCollectQueryData(pdhDiskQuery_);
    }

    void SampleDiskThroughput(double elapsedSec, StorageNetMetrics& outMetrics) noexcept {
        outMetrics.diskReadMbps  = 0.0;
        outMetrics.diskWriteMbps = 0.0;
        outMetrics.diskTotalMbps = 0.0;

        constexpr double kBytesPerMb = 1024.0 * 1024.0;

        if (driveHandle_.IsValid()) {
            DISK_PERFORMANCE diskPerf{};
            DWORD bytesReturned = 0;
            if (::DeviceIoControl(
                    driveHandle_.Get(),
                    IOCTL_DISK_PERFORMANCE,
                    nullptr,
                    0,
                    &diskPerf,
                    sizeof(diskPerf),
                    &bytesReturned,
                    nullptr)) {
                const uint64_t readBytes  = static_cast<uint64_t>(diskPerf.BytesRead.QuadPart);
                const uint64_t writeBytes = static_cast<uint64_t>(diskPerf.BytesWritten.QuadPart);

                if (hasDiskBaseline_ && elapsedSec > 1e-4) {
                    const uint64_t readDelta  = (readBytes >= prevDiskReadBytes_)
                                                    ? (readBytes - prevDiskReadBytes_)
                                                    : 0ULL;
                    const uint64_t writeDelta = (writeBytes >= prevDiskWriteBytes_)
                                                    ? (writeBytes - prevDiskWriteBytes_)
                                                    : 0ULL;

                    outMetrics.diskReadMbps  = (static_cast<double>(readDelta) / kBytesPerMb) / elapsedSec;
                    outMetrics.diskWriteMbps = (static_cast<double>(writeDelta) / kBytesPerMb) / elapsedSec;
                    outMetrics.diskTotalMbps = outMetrics.diskReadMbps + outMetrics.diskWriteMbps;
                }

                prevDiskReadBytes_  = readBytes;
                prevDiskWriteBytes_ = writeBytes;
                hasDiskBaseline_    = true;
                return;
            }
        }

        if (pdhDiskQuery_ != nullptr && pdhReadCounter_ != nullptr && pdhWriteCounter_ != nullptr) {
            if (::PdhCollectQueryData(pdhDiskQuery_) == ERROR_SUCCESS) {
                PDH_FMT_COUNTERVALUE readVal{};
                PDH_FMT_COUNTERVALUE writeVal{};
                if (::PdhGetFormattedCounterValue(pdhReadCounter_, PDH_FMT_DOUBLE, nullptr, &readVal) == ERROR_SUCCESS) {
                    outMetrics.diskReadMbps = readVal.doubleValue / kBytesPerMb;
                }
                if (::PdhGetFormattedCounterValue(pdhWriteCounter_, PDH_FMT_DOUBLE, nullptr, &writeVal) == ERROR_SUCCESS) {
                    outMetrics.diskWriteMbps = writeVal.doubleValue / kBytesPerMb;
                }
                outMetrics.diskTotalMbps = outMetrics.diskReadMbps + outMetrics.diskWriteMbps;
            }
        }
    }

    void SampleNetworkThroughput(double elapsedSec, StorageNetMetrics& outMetrics) noexcept {
        outMetrics.netRxKbps    = 0.0;
        outMetrics.netTxKbps    = 0.0;
        outMetrics.netTotalKbps = 0.0;

        PMIB_IF_TABLE2 ifTable = nullptr;
        if (::GetIfTable2(&ifTable) != NO_ERROR || ifTable == nullptr) {
            return;
        }

        uint64_t totalRxBytes = 0;
        uint64_t totalTxBytes = 0;

        for (ULONG i = 0; i < ifTable->NumEntries; ++i) {
            const MIB_IF_ROW2& row = ifTable->Table[i];
            if (row.OperStatus != IfOperStatusUp) {
                continue;
            }
            if (row.InterfaceAndOperStatusFlags.FilterInterface) {
                continue;
            }
            if (row.Type != IF_TYPE_ETHERNET_CSMACD && row.Type != IF_TYPE_IEEE80211) {
                continue;
            }

            totalRxBytes += row.InOctets;
            totalTxBytes += row.OutOctets;
        }

        ::FreeMibTable(ifTable);

        if (hasNetBaseline_ && elapsedSec > 1e-4) {
            constexpr double kBytesPerKb = 1024.0;
            const uint64_t rxDelta = (totalRxBytes >= prevNetRxBytes_)
                                         ? (totalRxBytes - prevNetRxBytes_)
                                         : 0ULL;
            const uint64_t txDelta = (totalTxBytes >= prevNetTxBytes_)
                                         ? (totalTxBytes - prevNetTxBytes_)
                                         : 0ULL;

            outMetrics.netRxKbps    = (static_cast<double>(rxDelta) / kBytesPerKb) / elapsedSec;
            outMetrics.netTxKbps    = (static_cast<double>(txDelta) / kBytesPerKb) / elapsedSec;
            outMetrics.netTotalKbps = outMetrics.netRxKbps + outMetrics.netTxKbps;
        }

        prevNetRxBytes_ = totalRxBytes;
        prevNetTxBytes_ = totalTxBytes;
        hasNetBaseline_ = true;
    }

    zetta::tracker::ScopedKernelHandle driveHandle_{};
    PDH_HQUERY                         pdhDiskQuery_{nullptr};
    PDH_HCOUNTER                       pdhReadCounter_{nullptr};
    PDH_HCOUNTER                       pdhWriteCounter_{nullptr};

    std::chrono::steady_clock::time_point prevSampleTime_{};
    uint64_t prevDiskReadBytes_{0};
    uint64_t prevDiskWriteBytes_{0};
    uint64_t prevNetRxBytes_{0};
    uint64_t prevNetTxBytes_{0};
    bool     hasTimestamp_{false};
    bool     hasDiskBaseline_{false};
    bool     hasNetBaseline_{false};
};

} // namespace zetta::telemetry
