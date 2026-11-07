#pragma once

#include "ipc/ZettaSharedState.h"

#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>

namespace zetta::ipc {

enum class SharedMemoryRole : uint8_t {
    Host,
    Client
};

class ZettaSharedMemoryManager {
public:
    explicit ZettaSharedMemoryManager(SharedMemoryRole role = SharedMemoryRole::Host) noexcept
        : role_(role) {}

    ~ZettaSharedMemoryManager() noexcept {
        Shutdown();
    }

    ZettaSharedMemoryManager(const ZettaSharedMemoryManager&) = delete;
    ZettaSharedMemoryManager& operator=(const ZettaSharedMemoryManager&) = delete;

    ZettaSharedMemoryManager(ZettaSharedMemoryManager&& other) noexcept
        : role_(other.role_),
          mappingHandle_(other.mappingHandle_),
          mutexHandle_(other.mutexHandle_),
          mappedView_(other.mappedView_) {
        other.mappingHandle_ = nullptr;
        other.mutexHandle_   = nullptr;
        other.mappedView_    = nullptr;
    }

    ZettaSharedMemoryManager& operator=(ZettaSharedMemoryManager&& other) noexcept {
        if (this != &other) {
            Shutdown();
            role_                = other.role_;
            mappingHandle_       = other.mappingHandle_;
            mutexHandle_         = other.mutexHandle_;
            mappedView_          = other.mappedView_;
            other.mappingHandle_ = nullptr;
            other.mutexHandle_   = nullptr;
            other.mappedView_    = nullptr;
        }
        return *this;
    }

    [[nodiscard]] bool Initialize() noexcept {
        if (mappedView_ != nullptr) {
            return true;
        }

        if (role_ == SharedMemoryRole::Host) {
            mutexHandle_ = ::CreateMutexW(nullptr, FALSE, kSharedMutexName);
        } else {
            mutexHandle_ = ::OpenMutexW(SYNCHRONIZE, FALSE, kSharedMutexName);
        }

        if (mutexHandle_ == nullptr) {
            return false;
        }

        const DWORD mappingSize = static_cast<DWORD>(sizeof(ZettaSharedState));
        if (role_ == SharedMemoryRole::Host) {
            mappingHandle_ = ::CreateFileMappingW(
                INVALID_HANDLE_VALUE,
                nullptr,
                PAGE_READWRITE,
                0,
                mappingSize,
                kSharedMemoryName
            );
        } else {
            mappingHandle_ = ::OpenFileMappingW(
                FILE_MAP_READ,
                FALSE,
                kSharedMemoryName
            );
        }

        if (mappingHandle_ == nullptr) {
            Shutdown();
            return false;
        }

        const DWORD desiredAccess = (role_ == SharedMemoryRole::Host) ? FILE_MAP_ALL_ACCESS : FILE_MAP_READ;
        void* rawView = ::MapViewOfFile(mappingHandle_, desiredAccess, 0, 0, sizeof(ZettaSharedState));
        if (rawView == nullptr) {
            Shutdown();
            return false;
        }

        mappedView_ = static_cast<ZettaSharedState*>(rawView);

        if (role_ == SharedMemoryRole::Host) {
            ScopedInterprocessLock lock(mutexHandle_);
            if (lock.IsLocked()) {
                std::memset(mappedView_, 0, sizeof(ZettaSharedState));
                mappedView_->magicHeader = kZettaMagicHeader;
                mappedView_->version     = kZettaSchemaVersion;
                mappedView_->timestampMs = CurrentTimestampMs();
                std::atomic_thread_fence(std::memory_order_release);
            }
        }

        return true;
    }

    void Shutdown() noexcept {
        if (mappedView_ != nullptr) {
            ::UnmapViewOfFile(mappedView_);
            mappedView_ = nullptr;
        }
        if (mappingHandle_ != nullptr) {
            ::CloseHandle(mappingHandle_);
            mappingHandle_ = nullptr;
        }
        if (mutexHandle_ != nullptr) {
            ::CloseHandle(mutexHandle_);
            mutexHandle_ = nullptr;
        }
    }

    [[nodiscard]] bool IsInitialized() const noexcept {
        return mappedView_ != nullptr;
    }

    template <typename MutatorFn>
    bool UpdateState(MutatorFn&& mutator) noexcept {
        if (mappedView_ == nullptr || role_ != SharedMemoryRole::Host) {
            return false;
        }

        ScopedInterprocessLock lock(mutexHandle_);
        if (!lock.IsLocked()) {
            return false;
        }

        mutator(*mappedView_);
        mappedView_->magicHeader = kZettaMagicHeader;
        mappedView_->version     = kZettaSchemaVersion;
        mappedView_->timestampMs = CurrentTimestampMs();
        std::atomic_thread_fence(std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool ReadSnapshot(ZettaSharedState& outState) const noexcept {
        if (mappedView_ == nullptr) {
            return false;
        }

        ScopedInterprocessLock lock(mutexHandle_);
        if (!lock.IsLocked()) {
            return false;
        }

        std::atomic_thread_fence(std::memory_order_acquire);
        if (mappedView_->magicHeader != kZettaMagicHeader ||
            mappedView_->version != kZettaSchemaVersion) {
            return false;
        }

        std::memcpy(&outState, mappedView_, sizeof(ZettaSharedState));
        return true;
    }

    [[nodiscard]] static uint64_t CurrentTimestampMs() noexcept {
        using namespace std::chrono;
        return static_cast<uint64_t>(
            duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count()
        );
    }

private:
    class ScopedInterprocessLock {
    public:
        explicit ScopedInterprocessLock(HANDLE mutexHandle, DWORD timeoutMs = 50) noexcept
            : mutexHandle_(mutexHandle) {
            if (mutexHandle_ != nullptr) {
                const DWORD waitResult = ::WaitForSingleObject(mutexHandle_, timeoutMs);
                // Accept WAIT_ABANDONED to recover gracefully if a peer terminated mid-operation.
                locked_ = (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED);
            }
        }

        ~ScopedInterprocessLock() noexcept {
            if (locked_ && mutexHandle_ != nullptr) {
                ::ReleaseMutex(mutexHandle_);
            }
        }

        ScopedInterprocessLock(const ScopedInterprocessLock&) = delete;
        ScopedInterprocessLock& operator=(const ScopedInterprocessLock&) = delete;

        [[nodiscard]] bool IsLocked() const noexcept {
            return locked_;
        }

    private:
        HANDLE mutexHandle_{nullptr};
        bool   locked_{false};
    };

    SharedMemoryRole  role_{SharedMemoryRole::Host};
    HANDLE            mappingHandle_{nullptr};
    HANDLE            mutexHandle_{nullptr};
    ZettaSharedState* mappedView_{nullptr};
};

} // namespace zetta::ipc
