#pragma once

#include <windows.h>
#include <dwmapi.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace zetta::tracker {

enum class ViewportState : uint8_t {
    Idle = 0,
    Windowed = 1,
    Fullscreen = 2
};

struct WindowTargetInfo {
    uint32_t      pid{0};
    std::string   processName{"Idle"};
    ViewportState state{ViewportState::Idle};
    bool          isGameProcess{false};
    bool          isElevated{false};
    bool          isFullscreen{false};
    int32_t       x{0};
    int32_t       y{0};
    int32_t       width{0};
    int32_t       height{0};

    [[nodiscard]] bool operator==(const WindowTargetInfo& other) const noexcept {
        return pid == other.pid &&
               state == other.state &&
               isGameProcess == other.isGameProcess &&
               isElevated == other.isElevated &&
               isFullscreen == other.isFullscreen &&
               x == other.x &&
               y == other.y &&
               width == other.width &&
               height == other.height &&
               processName == other.processName;
    }

    [[nodiscard]] bool operator!=(const WindowTargetInfo& other) const noexcept {
        return !(*this == other);
    }
};

class ScopedKernelHandle {
public:
    ScopedKernelHandle() noexcept = default;
    explicit ScopedKernelHandle(HANDLE handle) noexcept : handle_(handle) {}

    ~ScopedKernelHandle() noexcept {
        Reset();
    }

    ScopedKernelHandle(const ScopedKernelHandle&) = delete;
    ScopedKernelHandle& operator=(const ScopedKernelHandle&) = delete;

    ScopedKernelHandle(ScopedKernelHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    ScopedKernelHandle& operator=(ScopedKernelHandle&& other) noexcept {
        if (this != &other) {
            Reset();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    void Reset(HANDLE newHandle = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle_);
        }
        handle_ = newHandle;
    }

    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }
    [[nodiscard]] HANDLE* AddressOf() noexcept {
        Reset();
        return &handle_;
    }
    [[nodiscard]] bool IsValid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_{nullptr};
};

class ZettaWindowTracker {
public:
    using StateCallback = std::function<void(const WindowTargetInfo&)>;

    ZettaWindowTracker() noexcept = default;

    ~ZettaWindowTracker() noexcept {
        Stop();
    }

    ZettaWindowTracker(const ZettaWindowTracker&) = delete;
    ZettaWindowTracker& operator=(const ZettaWindowTracker&) = delete;

    [[nodiscard]] bool Start(StateCallback callback) noexcept {
        if (foregroundHook_ != nullptr || locationHook_ != nullptr) {
            return true;
        }

        callback_ = std::move(callback);
        instance_ = this;

        constexpr DWORD kHookFlags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;

        foregroundHook_ = ::SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND,
            EVENT_SYSTEM_FOREGROUND,
            nullptr,
            &ZettaWindowTracker::WinEventProc,
            0,
            0,
            kHookFlags
        );

        minimizeHook_ = ::SetWinEventHook(
            EVENT_SYSTEM_MINIMIZESTART,
            EVENT_SYSTEM_MINIMIZEEND,
            nullptr,
            &ZettaWindowTracker::WinEventProc,
            0,
            0,
            kHookFlags
        );

        locationHook_ = ::SetWinEventHook(
            EVENT_OBJECT_LOCATIONCHANGE,
            EVENT_OBJECT_LOCATIONCHANGE,
            nullptr,
            &ZettaWindowTracker::WinEventProc,
            0,
            0,
            kHookFlags
        );

        if (foregroundHook_ == nullptr || locationHook_ == nullptr) {
            Stop();
            return false;
        }

        HandleWindowFocusChange(::GetForegroundWindow(), true);
        return true;
    }

    void Stop() noexcept {
        if (foregroundHook_ != nullptr) {
            ::UnhookWinEvent(foregroundHook_);
            foregroundHook_ = nullptr;
        }
        if (minimizeHook_ != nullptr) {
            ::UnhookWinEvent(minimizeHook_);
            minimizeHook_ = nullptr;
        }
        if (locationHook_ != nullptr) {
            ::UnhookWinEvent(locationHook_);
            locationHook_ = nullptr;
        }
        if (instance_ == this) {
            instance_ = nullptr;
        }
    }

    [[nodiscard]] static const char* StateToString(ViewportState state) noexcept {
        switch (state) {
            case ViewportState::Fullscreen: return "Fullscreen";
            case ViewportState::Windowed:   return "Windowed";
            case ViewportState::Idle:
            default:                        return "Idle";
        }
    }

private:
    struct InternalTrackerState {
        HWND         hwnd{nullptr};
        DWORD        pid{0};
        bool         isGameProcess{false};
        bool         isElevated{false};
        bool         isFullscreen{false};
        std::wstring processName{L"Idle"};
        std::string  utf8ProcessName{"Idle"};
        RECT         rect{0, 0, 0, 0};
    };

    static void CALLBACK WinEventProc(
        HWINEVENTHOOK /*hWinEventHook*/,
        DWORD event,
        HWND hwnd,
        LONG idObject,
        LONG idChild,
        DWORD /*idEventThread*/,
        DWORD /*dwmsEventTime*/
    ) noexcept {
        if (instance_ == nullptr) {
            return;
        }
        instance_->HandleWinEvent(event, hwnd, idObject, idChild);
    }

    void HandleWinEvent(DWORD event, HWND hwnd, LONG idObject, LONG idChild) noexcept {
        if (event == EVENT_SYSTEM_FOREGROUND) {
            HandleWindowFocusChange(hwnd != nullptr ? hwnd : ::GetForegroundWindow(), false);
            return;
        }

        if (event == EVENT_SYSTEM_MINIMIZESTART || event == EVENT_SYSTEM_MINIMIZEEND) {
            if (idObject == OBJID_WINDOW && idChild == CHILDID_SELF) {
                HandleWindowFocusChange(::GetForegroundWindow(), false);
            }
            return;
        }

        if (event == EVENT_OBJECT_LOCATIONCHANGE) {
            // Ignore cursor/caret/child control notifications to keep CPU load near zero.
            if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || hwnd == nullptr) {
                return;
            }
            if (hwnd != currentState.hwnd) {
                return;
            }
            UpdateWindowCoordinates(hwnd, false);
        }
    }

    void ResetToIdle(bool forceEmit = false) noexcept {
        currentState.hwnd            = nullptr;
        currentState.pid             = 0;
        currentState.isGameProcess   = false;
        currentState.isElevated      = false;
        currentState.isFullscreen    = false;
        currentState.processName     = L"Idle";
        currentState.utf8ProcessName = "Idle";
        currentState.rect            = {0, 0, 0, 0};

        WindowTargetInfo idleInfo{};
        DispatchIfChanged(idleInfo, forceEmit);
    }

    void HandleWindowFocusChange(HWND hwnd, bool forceEmit = false) noexcept {
        if (hwnd == nullptr || !::IsWindow(hwnd) || !::IsWindowVisible(hwnd) || ::IsIconic(hwnd)) {
            ResetToIdle(forceEmit);
            return;
        }

        DWORD pid = 0;
        ::GetWindowThreadProcessId(hwnd, &pid);
        if (pid == 0 || pid == ::GetCurrentProcessId()) {
            ResetToIdle(forceEmit);
            return;
        }

        std::wstring fullPath{};
        std::wstring procName{};
        bool isGame = false;

        const auto cachedIt = knownGamePids.find(pid);
        if (cachedIt != knownGamePids.end()) {
            isGame = cachedIt->second;
            if (!isGame) {
                ResetToIdle(forceEmit);
                return;
            }
            if (currentState.pid == pid &&
                !currentState.processName.empty() &&
                currentState.processName != L"Idle") {
                procName = currentState.processName;
            } else {
                fullPath = GetProcessFullPathByPid(pid);
                procName = ExtractProcessNameFromPath(fullPath);
                if (procName.empty()) {
                    knownGamePids.erase(cachedIt);
                    ResetToIdle(forceEmit);
                    return;
                }
            }
        } else {
            fullPath = GetProcessFullPathByPid(pid);
            procName = ExtractProcessNameFromPath(fullPath);
            if (procName.empty()) {
                ResetToIdle(forceEmit);
                return;
            }

            if (IsBlacklistedSystemProcess(procName)) {
                isGame = false;
            } else {
                isGame = AnalyzeProcessSignature(pid, fullPath, procName);
            }

            if (knownGamePids.size() >= 4096U) {
                knownGamePids.clear();
            }
            knownGamePids[pid] = isGame;

            if (!isGame) {
                ResetToIdle(forceEmit);
                return;
            }
        }

        currentState.hwnd            = hwnd;
        currentState.pid             = pid;
        currentState.processName     = procName;
        currentState.utf8ProcessName = WideToUtf8(procName);
        currentState.isGameProcess   = true;
        currentState.isElevated      = QueryProcessElevation(pid);
        currentState.rect            = {0, 0, 0, 0};

        UpdateWindowCoordinates(hwnd, forceEmit);
    }

    void UpdateWindowCoordinates(HWND hwnd, bool forceEmit = false) noexcept {
        if (hwnd == nullptr ||
            hwnd != currentState.hwnd ||
            !::IsWindow(hwnd) ||
            !::IsWindowVisible(hwnd) ||
            ::IsIconic(hwnd)) {
            ResetToIdle(forceEmit);
            return;
        }

        RECT bounds{};
        if (!QueryVisibleWindowBounds(hwnd, bounds)) {
            ResetToIdle(forceEmit);
            return;
        }

        const int32_t width  = static_cast<int32_t>(bounds.right - bounds.left);
        const int32_t height = static_cast<int32_t>(bounds.bottom - bounds.top);
        if (width <= 0 || height <= 0) {
            ResetToIdle(forceEmit);
            return;
        }

        if (!forceEmit && ::EqualRect(&bounds, &currentState.rect)) {
            return;
        }

        currentState.rect         = bounds;
        currentState.isFullscreen = IsWindowFullscreenOnMonitor(hwnd, bounds);

        WindowTargetInfo info{};
        info.pid           = static_cast<uint32_t>(currentState.pid);
        info.processName   = currentState.utf8ProcessName;
        info.isGameProcess = currentState.isGameProcess;
        info.isElevated    = currentState.isElevated;
        info.isFullscreen  = currentState.isFullscreen;
        info.state         = currentState.isFullscreen ? ViewportState::Fullscreen : ViewportState::Windowed;
        info.x             = static_cast<int32_t>(bounds.left);
        info.y             = static_cast<int32_t>(bounds.top);
        info.width         = width;
        info.height        = height;

        DispatchIfChanged(info, forceEmit);
    }

    void DispatchIfChanged(const WindowTargetInfo& nextState, bool forceEmit) noexcept {
        if (!forceEmit && nextState == lastState_) {
            return;
        }
        lastState_ = nextState;
        if (callback_) {
            callback_(lastState_);
        }
    }

    [[nodiscard]] static bool QueryVisibleWindowBounds(HWND hwnd, RECT& outRect) noexcept {
        // DWMWA_EXTENDED_FRAME_BOUNDS strips invisible Windows 11 resize and shadow borders.
        const HRESULT hr = ::DwmGetWindowAttribute(
            hwnd,
            DWMWA_EXTENDED_FRAME_BOUNDS,
            &outRect,
            static_cast<DWORD>(sizeof(RECT))
        );
        if (SUCCEEDED(hr)) {
            return true;
        }
        return ::GetWindowRect(hwnd, &outRect) != FALSE;
    }

    [[nodiscard]] static bool IsWindowFullscreenOnMonitor(HWND hwnd, const RECT& windowBounds) noexcept {
        HMONITOR monitor = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (monitor == nullptr) {
            return false;
        }

        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(MONITORINFO);
        if (!::GetMonitorInfoW(monitor, &monitorInfo)) {
            return false;
        }

        const RECT& mon = monitorInfo.rcMonitor;
        return windowBounds.left <= mon.left &&
               windowBounds.top <= mon.top &&
               windowBounds.right >= mon.right &&
               windowBounds.bottom >= mon.bottom;
    }

    [[nodiscard]] static std::wstring GetProcessFullPathByPid(DWORD pid) noexcept {
        ScopedKernelHandle processHandle(
            ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)
        );
        if (!processHandle.IsValid()) {
            return {};
        }

        wchar_t pathBuffer[MAX_PATH * 2]{};
        DWORD bufferChars = static_cast<DWORD>(std::size(pathBuffer));
        if (!::QueryFullProcessImageNameW(processHandle.Get(), 0, pathBuffer, &bufferChars)) {
            return {};
        }

        return std::wstring(pathBuffer, bufferChars);
    }

    [[nodiscard]] static std::wstring ExtractProcessNameFromPath(std::wstring_view fullPath) noexcept {
        if (fullPath.empty()) {
            return {};
        }
        const size_t lastSlash = fullPath.find_last_of(L"\\/");
        if (lastSlash != std::wstring_view::npos && lastSlash + 1 < fullPath.size()) {
            return std::wstring(fullPath.substr(lastSlash + 1));
        }
        return std::wstring(fullPath);
    }

    [[nodiscard]] static bool QueryProcessElevation(DWORD pid) noexcept {
        ScopedKernelHandle processHandle(
            ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)
        );
        if (!processHandle.IsValid()) {
            return ::GetLastError() == ERROR_ACCESS_DENIED;
        }

        ScopedKernelHandle tokenHandle;
        if (!::OpenProcessToken(processHandle.Get(), TOKEN_QUERY, tokenHandle.AddressOf())) {
            return ::GetLastError() == ERROR_ACCESS_DENIED;
        }

        TOKEN_ELEVATION elevation{};
        DWORD returnLength = 0;
        if (!::GetTokenInformation(
                tokenHandle.Get(),
                TokenElevation,
                &elevation,
                sizeof(elevation),
                &returnLength)) {
            return false;
        }

        return elevation.TokenIsElevated != 0;
    }

    [[nodiscard]] static std::wstring ToLowerWide(std::wstring str) noexcept {
        std::transform(
            str.begin(),
            str.end(),
            str.begin(),
            [](wchar_t c) noexcept {
                return static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
            }
        );
        return str;
    }

    [[nodiscard]] static bool IsBlacklistedSystemProcess(std::wstring procName) noexcept {
        const std::wstring lowerName = ToLowerWide(std::move(procName));

        static constexpr std::array<std::wstring_view, 31> kBlacklistedProcesses = {
            // Browsers
            L"brave.exe",
            L"chrome.exe",
            L"msedge.exe",
            L"firefox.exe",
            L"opera.exe",
            L"vivaldi.exe",
            // Electron / CEF / Productivity
            L"discord.exe",
            L"spotify.exe",
            L"slack.exe",
            L"teams.exe",
            L"obs64.exe",
            L"obs.exe",
            L"code.exe",
            L"notepad.exe",
            L"steamwebhelper.exe",
            // System & Shell
            L"explorer.exe",
            L"taskmgr.exe",
            L"dwm.exe",
            L"cmd.exe",
            L"powershell.exe",
            L"conhost.exe",
            L"searchhost.exe",
            L"shellexperiencehost.exe",
            L"applicationframehost.exe",
            L"textinputhost.exe",
            // ZettaHUD & Diagnostic Binaries
            L"zetta_hud_live.exe",
            L"zetta_studio.exe",
            L"zetta_overlay.exe",
            L"zetta_phase1_test.exe",
            L"zetta_phase2_test.exe",
            L"zetta_phase3_test.exe"
        };

        for (const auto& entry : kBlacklistedProcesses) {
            if (lowerName == entry) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] static bool AnalyzeProcessSignature(
        DWORD pid,
        const std::wstring& fullPath,
        const std::wstring& procName
    ) noexcept {
        if (pid == 0 || fullPath.empty() || procName.empty()) {
            return false;
        }

        const std::wstring lowerPath = ToLowerWide(fullPath);

        // 1. Directory Whitelist: Known game launcher libraries.
        static constexpr std::array<std::wstring_view, 5> kLauncherDirs = {
            L"\\steamapps\\common\\",
            L"\\epic games\\",
            L"\\xboxgames\\",
            L"\\ea games\\",
            L"\\ubisoft game launcher\\"
        };
        for (const auto& dir : kLauncherDirs) {
            if (lowerPath.find(dir) != std::wstring::npos) {
                return true;
            }
        }

        // 2. System Directory Reject: Exclude Windows OS binaries.
        if (lowerPath.find(L"\\windows\\system32\\") != std::wstring::npos ||
            lowerPath.find(L"\\windows\\syswow64\\") != std::wstring::npos) {
            return false;
        }

        // 3. Module Signature Scan: Inspect loaded DLLs for game engine/runtime APIs.
        ScopedKernelHandle snapshot(
            ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
        );
        if (snapshot.IsValid()) {
            MODULEENTRY32W me{};
            me.dwSize = sizeof(MODULEENTRY32W);

            static constexpr std::array<std::wstring_view, 9> kSignatureDlls = {
                L"unityplayer.dll",
                L"steam_api64.dll",
                L"steam_api.dll",
                L"bink2w64.dll",
                L"fmodstudio.dll",
                L"fmod.dll",
                L"vulkan-1.dll",
                L"d3d12.dll",
                L"dxvk_d3d11.dll"
            };

            if (::Module32FirstW(snapshot.Get(), &me)) {
                do {
                    const std::wstring lowerMod = ToLowerWide(me.szModule);
                    for (const auto& sigDll : kSignatureDlls) {
                        if (lowerMod == sigDll) {
                            return true;
                        }
                    }
                } while (::Module32NextW(snapshot.Get(), &me));
            }
        }

        // 4. Fallback Reject: Non-game desktop application.
        return false;
    }

    [[nodiscard]] static std::string WideToUtf8(std::wstring_view wideStr) noexcept {
        if (wideStr.empty()) {
            return {};
        }
        const int requiredBytes = ::WideCharToMultiByte(
            CP_UTF8,
            0,
            wideStr.data(),
            static_cast<int>(wideStr.size()),
            nullptr,
            0,
            nullptr,
            nullptr
        );
        if (requiredBytes <= 0) {
            return {};
        }

        std::string utf8Str(static_cast<size_t>(requiredBytes), '\0');
        ::WideCharToMultiByte(
            CP_UTF8,
            0,
            wideStr.data(),
            static_cast<int>(wideStr.size()),
            utf8Str.data(),
            requiredBytes,
            nullptr,
            nullptr
        );
        return utf8Str;
    }

    inline static ZettaWindowTracker* instance_{nullptr};

    HWINEVENTHOOK                   foregroundHook_{nullptr};
    HWINEVENTHOOK                   minimizeHook_{nullptr};
    HWINEVENTHOOK                   locationHook_{nullptr};
    std::unordered_map<DWORD, bool> knownGamePids{};
    InternalTrackerState            currentState{};
    WindowTargetInfo                lastState_{};
    StateCallback                   callback_{};
};

} // namespace zetta::tracker
