#pragma once

#include "ipc/ZettaSharedMemoryManager.hpp"
#include "ipc/ZettaSharedState.h"
#include "overlay/ZettaD2DRenderer.hpp"
#include "overlay/ZettaLayoutConfig.hpp"

#include <windows.h>
#include <dwmapi.h>

#include <algorithm>
#include <cstdint>
#include <string>

namespace zetta::overlay {

class ZettaOverlayWindow {
public:
    static constexpr const wchar_t* kWindowClassName    = L"ZettaHUD_OverlayWindow_v1";
    static constexpr UINT_PTR       kRenderTimerId      = 101U;
    static constexpr UINT_PTR       kReloadTimerId      = 102U;
    static constexpr UINT           kWmForceReload      = WM_APP + 2U;
    static constexpr int            kHotkeyToggleVis    = 1;
    static constexpr int            kHotkeyCompact      = 2;

    explicit ZettaOverlayWindow(
        zetta::ipc::ZettaSharedMemoryManager& ipcManager,
        const char* captureModeBadge = "ETW"
    ) noexcept
        : ipcManager_(ipcManager),
          captureModeBadge_(captureModeBadge) {}

    ~ZettaOverlayWindow() noexcept {
        Shutdown();
    }

    ZettaOverlayWindow(const ZettaOverlayWindow&) = delete;
    ZettaOverlayWindow& operator=(const ZettaOverlayWindow&) = delete;

    [[nodiscard]] bool Initialize(const std::wstring& presetFilePath) noexcept {
        if (hwnd_ != nullptr) {
            return true;
        }

        layoutConfig_.LoadFromFile(presetFilePath);

        const HINSTANCE hInst = ::GetModuleHandleW(nullptr);
        const HICON appIcon   = ::LoadIconW(hInst, MAKEINTRESOURCEW(101));
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(WNDCLASSEXW);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = &ZettaOverlayWindow::StaticWndProc;
        wc.hInstance     = hInst;
        wc.hIcon         = appIcon;
        wc.hIconSm       = appIcon;
        wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = kWindowClassName;
        ::RegisterClassExW(&wc);

        constexpr DWORD kExStyle = WS_EX_LAYERED |
                                   WS_EX_TRANSPARENT |
                                   WS_EX_TOPMOST |
                                   WS_EX_TOOLWINDOW |
                                   WS_EX_NOACTIVATE;

        const LayoutConfigData& cfg = layoutConfig_.Get();
        lastX_      = cfg.offsetX;
        lastY_      = cfg.offsetY;
        lastWidth_  = 248;
        lastHeight_ = 310;

        hwnd_ = ::CreateWindowExW(
            kExStyle,
            kWindowClassName,
            L"ZettaHUD Transparent Telemetry Overlay",
            WS_POPUP,
            lastX_,
            lastY_,
            lastWidth_,
            lastHeight_,
            nullptr,
            nullptr,
            hInst,
            this
        );

        if (hwnd_ == nullptr) {
            return false;
        }

        ::SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);

        const MARGINS glassMargins = {-1, -1, -1, -1};
        ::DwmExtendFrameIntoClientArea(hwnd_, &glassMargins);

        if (!renderer_.Initialize(hwnd_, cfg)) {
            Shutdown();
            return false;
        }

        ::RegisterHotKey(hwnd_, kHotkeyToggleVis, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F12);
        ::RegisterHotKey(hwnd_, kHotkeyCompact, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F11);

        ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        ::UpdateWindow(hwnd_);

        ::SetTimer(hwnd_, kRenderTimerId, 16U, nullptr);
        ::SetTimer(hwnd_, kReloadTimerId, 500U, nullptr);

        OnRenderTick();
        return true;
    }

    void Shutdown() noexcept {
        if (hwnd_ != nullptr) {
            ::KillTimer(hwnd_, kRenderTimerId);
            ::KillTimer(hwnd_, kReloadTimerId);
            ::UnregisterHotKey(hwnd_, kHotkeyToggleVis);
            ::UnregisterHotKey(hwnd_, kHotkeyCompact);
        }

        renderer_.Shutdown();

        if (hwnd_ != nullptr) {
            ::DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

    void SetCaptureModeBadge(const char* badge) noexcept {
        captureModeBadge_ = badge;
    }

    [[nodiscard]] HWND GetHwnd() const noexcept { return hwnd_; }
    [[nodiscard]] uint64_t GetRenderedFrameCount() const noexcept {
        return renderer_.GetRenderedFrameCount();
    }
    [[nodiscard]] uint32_t GetDeviceRecreateCount() const noexcept {
        return renderer_.GetDeviceRecreateCount();
    }

private:
    static LRESULT CALLBACK StaticWndProc(
        HWND hwnd,
        UINT msg,
        WPARAM wParam,
        LPARAM lParam
    ) noexcept {
        ZettaOverlayWindow* self = nullptr;
        if (msg == WM_NCCREATE) {
            const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<ZettaOverlayWindow*>(cs->lpCreateParams);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<ZettaOverlayWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (self != nullptr) {
            return self->HandleMessage(hwnd, msg, wParam, lParam);
        }
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept {
        switch (msg) {
            case WM_NCHITTEST:
                // Guarantee 100% click-through transparency to underlying applications.
                return HTTRANSPARENT;

            case WM_MOUSEACTIVATE:
                return MA_NOACTIVATE;

            case WM_ERASEBKGND:
                return 1;

            case WM_TIMER:
                if (wParam == kRenderTimerId) {
                    OnRenderTick();
                } else if (wParam == kReloadTimerId) {
                    if (layoutConfig_.CheckAndReloadIfModified()) {
                        renderer_.RebuildTextFormats(layoutConfig_.Get());
                        OnRenderTick();
                    }
                }
                return 0;

            case kWmForceReload:
                if (layoutConfig_.ForceReload()) {
                    renderer_.RebuildTextFormats(layoutConfig_.Get());
                    OnRenderTick();
                }
                return 0;

            case WM_HOTKEY:
                if (wParam == kHotkeyToggleVis) {
                    userVisible_ = !userVisible_;
                    ::ShowWindow(hwnd, userVisible_ ? SW_SHOWNOACTIVATE : SW_HIDE);
                    if (userVisible_) {
                        OnRenderTick();
                    }
                } else if (wParam == kHotkeyCompact) {
                    compactMode_ = !compactMode_;
                    OnRenderTick();
                }
                return 0;

            case WM_PAINT: {
                PAINTSTRUCT ps{};
                ::BeginPaint(hwnd, &ps);
                OnRenderTick();
                ::EndPaint(hwnd, &ps);
                return 0;
            }

            default:
                return ::DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    void OnRenderTick() noexcept {
        if (hwnd_ == nullptr || !userVisible_) {
            return;
        }

        (void)ipcManager_.ReadSnapshot(cachedState_);

        const LayoutConfigData& cfg = layoutConfig_.Get();
        if (cfg.hideWhenIdle && !cachedState_.isGameProcess) {
            if (!autoHiddenForIdle_) {
                ::ShowWindow(hwnd_, SW_HIDE);
                autoHiddenForIdle_ = true;
            }
            return;
        }
        if (autoHiddenForIdle_) {
            ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
            autoHiddenForIdle_ = false;
        }

        uint32_t canvasW = 248;
        uint32_t canvasH = 310;
        renderer_.ComputeRequiredCanvasSize(cachedState_, layoutConfig_, compactMode_, canvasW, canvasH);

        int32_t desiredX = cfg.offsetX;
        int32_t desiredY = cfg.offsetY;

        if (cachedState_.isGameProcess &&
            cachedState_.windowWidth > 0 &&
            cachedState_.windowHeight > 0) {
            desiredX = cachedState_.windowX + cfg.offsetX;
            desiredY = cachedState_.windowY + cfg.offsetY;

            const int32_t maxRight  = cachedState_.windowX + cachedState_.windowWidth - static_cast<int32_t>(canvasW);
            const int32_t maxBottom = cachedState_.windowY + cachedState_.windowHeight - static_cast<int32_t>(canvasH);
            if (maxRight >= cachedState_.windowX) {
                desiredX = std::clamp(desiredX, cachedState_.windowX, maxRight);
            }
            if (maxBottom >= cachedState_.windowY) {
                desiredY = std::clamp(desiredY, cachedState_.windowY, maxBottom);
            }
        }

        const int32_t wInt = static_cast<int32_t>(canvasW);
        const int32_t hInt = static_cast<int32_t>(canvasH);
        if (desiredX != lastX_ || desiredY != lastY_ || wInt != lastWidth_ || hInt != lastHeight_) {
            lastX_      = desiredX;
            lastY_      = desiredY;
            lastWidth_  = wInt;
            lastHeight_ = hInt;

            ::SetWindowPos(
                hwnd_,
                HWND_TOPMOST,
                lastX_,
                lastY_,
                lastWidth_,
                lastHeight_,
                SWP_NOACTIVATE | SWP_NOSENDCHANGING | SWP_FRAMECHANGED
            );
            ::InvalidateRect(hwnd_, nullptr, TRUE);
        }

        renderer_.RenderFrame(
            cachedState_,
            layoutConfig_,
            compactMode_,
            captureModeBadge_,
            canvasW,
            canvasH
        );
    }

    zetta::ipc::ZettaSharedMemoryManager& ipcManager_;
    const char*                           captureModeBadge_{"ETW"};
    HWND                                  hwnd_{nullptr};

    ZettaLayoutConfig                     layoutConfig_{};
    ZettaD2DRenderer                      renderer_{};
    zetta::ipc::ZettaSharedState          cachedState_{};

    int32_t                               lastX_{20};
    int32_t                               lastY_{20};
    int32_t                               lastWidth_{248};
    int32_t                               lastHeight_{310};
    bool                                  userVisible_{true};
    bool                                  autoHiddenForIdle_{false};
    bool                                  compactMode_{false};
};

} // namespace zetta::overlay
