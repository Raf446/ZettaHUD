#include "ipc/ZettaSharedMemoryManager.hpp"
#include "ipc/ZettaSharedState.h"
#include "studio/ZettaStudioBridgeServer.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace {

#ifndef IDI_APP_ICON
#define IDI_APP_ICON 101
#endif

constexpr UINT     kWmTrayCallback = WM_APP + 1U;
constexpr UINT_PTR kCmdOpenStudio  = 1001U;
constexpr UINT_PTR kCmdToggleHud   = 1002U;
constexpr UINT_PTR kCmdExitApp     = 1003U;

std::atomic<bool> g_stopRequested{false};

struct TrayContext {
    zetta::studio::ZettaStudioBridgeServer* bridge{nullptr};
    zetta::ipc::ZettaSharedMemoryManager*   ipc{nullptr};
    uint16_t                                port{19450};
};

bool SetAutostartRegistry(bool enable) noexcept {
    constexpr const wchar_t* kRunSubKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr const wchar_t* kValueName = L"ZettaHUD";

    HKEY hKey = nullptr;
    const LSTATUS openStatus = ::RegCreateKeyExW(
        HKEY_CURRENT_USER,
        kRunSubKey,
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE | KEY_QUERY_VALUE,
        nullptr,
        &hKey,
        nullptr
    );
    if (openStatus != ERROR_SUCCESS || hKey == nullptr) {
        return false;
    }

    bool ok = false;
    if (enable) {
        wchar_t exePath[MAX_PATH * 2]{};
        const DWORD len = ::GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
        if (len > 0) {
            std::wstring cmdValue = L"\"";
            cmdValue.append(exePath, len);
            cmdValue.append(L"\" --background");

            const DWORD byteCount = static_cast<DWORD>((cmdValue.size() + 1U) * sizeof(wchar_t));
            const LSTATUS setStatus = ::RegSetValueExW(
                hKey,
                kValueName,
                0,
                REG_SZ,
                reinterpret_cast<const BYTE*>(cmdValue.c_str()),
                byteCount
            );
            ok = (setStatus == ERROR_SUCCESS);
        }
    } else {
        const LSTATUS delStatus = ::RegDeleteValueW(hKey, kValueName);
        ok = (delStatus == ERROR_SUCCESS || delStatus == ERROR_FILE_NOT_FOUND);
    }

    ::RegCloseKey(hKey);
    return ok;
}

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) noexcept {
    switch (ctrlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_stopRequested.store(true, std::memory_order_release);
            return TRUE;
        default:
            return FALSE;
    }
}

std::wstring GetExecutableDirectory() {
    wchar_t buf[MAX_PATH * 2]{};
    const DWORD len = ::GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (len == 0) {
        return L".\\";
    }
    std::wstring path(buf, len);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        path.resize(slash + 1);
    }
    return path;
}

std::wstring ToFullPath(const std::wstring& path) {
    wchar_t fullBuf[MAX_PATH * 2]{};
    const DWORD len = ::GetFullPathNameW(path.c_str(), static_cast<DWORD>(std::size(fullBuf)), fullBuf, nullptr);
    if (len > 0 && len < std::size(fullBuf)) {
        return std::wstring(fullBuf, len);
    }
    return path;
}

std::wstring ResolveWorkspaceFile(const std::wstring& relativeSubpath) {
    const std::wstring exeDir = GetExecutableDirectory();
    const std::wstring candidate2 = exeDir + L"..\\" + relativeSubpath;
    if (::GetFileAttributesW(candidate2.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return ToFullPath(candidate2);
    }
    const std::wstring candidate1 = exeDir + relativeSubpath;
    if (::GetFileAttributesW(candidate1.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return ToFullPath(candidate1);
    }
    if (::GetFileAttributesW(relativeSubpath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return ToFullPath(relativeSubpath);
    }
    return relativeSubpath;
}

std::wstring ResolveOverlayExecutable() {
    const std::wstring localBuild = ResolveWorkspaceFile(L"build\\zetta_hud_live.exe");
    if (::GetFileAttributesW(localBuild.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return localBuild;
    }
    const std::wstring siblingExe = GetExecutableDirectory() + L"zetta_hud_live.exe";
    if (::GetFileAttributesW(siblingExe.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return siblingExe;
    }
    return localBuild;
}

void OpenStudioAppWindow(uint16_t port) noexcept {
    wchar_t url[128]{};
    std::swprintf(url, std::size(url), L"http://127.0.0.1:%u/?v=3.3", static_cast<unsigned>(port));

    wchar_t edgeArgs[256]{};
    std::swprintf(
        edgeArgs,
        std::size(edgeArgs),
        L"--app=%ls --window-size=1380,860",
        url
    );

    const HINSTANCE edgeInst = ::ShellExecuteW(
        nullptr,
        L"open",
        L"msedge.exe",
        edgeArgs,
        nullptr,
        SW_SHOWNORMAL
    );

    if (reinterpret_cast<INT_PTR>(edgeInst) <= 32) {
        ::ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void ToggleHudOverlayViaIpc(TrayContext* ctx) noexcept {
    // 1. If the Direct2D overlay window is active, send it the visibility toggle hotkey message.
    const HWND overlayHwnd = ::FindWindowW(L"ZettaHUD_OverlayWindow_v1", nullptr);
    if (overlayHwnd != nullptr) {
        ::PostMessageW(overlayHwnd, WM_HOTKEY, 1, 0);
        if (ctx != nullptr && ctx->ipc != nullptr) {
            (void)ctx->ipc->UpdateState([](zetta::ipc::ZettaSharedState& state) noexcept {
                state.timestampMs = static_cast<uint64_t>(::GetTickCount64());
            });
        }
        return;
    }

    // 2. Otherwise, launch the HUD overlay daemon via the Studio bridge controller.
    if (ctx != nullptr && ctx->bridge != nullptr) {
        (void)ctx->bridge->EnsureOverlayRunning();
    }
}

void ExecuteTrayMenuCommand(HWND hwnd, UINT cmdId, TrayContext* ctx) noexcept {
    switch (cmdId) {
        case kCmdOpenStudio:
            OpenStudioAppWindow(ctx != nullptr ? ctx->port : 19450U);
            break;

        case kCmdToggleHud:
            ToggleHudOverlayViaIpc(ctx);
            break;

        case kCmdExitApp:
            if (ctx != nullptr && ctx->bridge != nullptr) {
                (void)ctx->bridge->StopOverlayRunning();
            }
            g_stopRequested.store(true, std::memory_order_release);
            ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
            break;

        default:
            break;
    }
}

void ShowTrayContextMenu(HWND hwnd, TrayContext* ctx) noexcept {
    POINT pt{};
    ::GetCursorPos(&pt);

    const HMENU hMenu = ::CreatePopupMenu();
    if (hMenu == nullptr) {
        return;
    }

    ::AppendMenuW(hMenu, MF_STRING, kCmdOpenStudio, L"Open Studio UI");
    ::AppendMenuW(hMenu, MF_STRING, kCmdToggleHud, L"Toggle HUD Overlay");
    ::AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(hMenu, MF_STRING, kCmdExitApp, L"Exit");

    ::SetForegroundWindow(hwnd);
    const UINT selectedCmd = static_cast<UINT>(::TrackPopupMenu(
        hMenu,
        TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
        pt.x,
        pt.y,
        0,
        hwnd,
        nullptr
    ));
    ::PostMessageW(hwnd, WM_NULL, 0, 0);
    ::DestroyMenu(hMenu);

    if (selectedCmd != 0U) {
        ExecuteTrayMenuCommand(hwnd, selectedCmd, ctx);
    }
}

LRESULT CALLBACK SystemTrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept {
    TrayContext* ctx = nullptr;
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        ctx = static_cast<TrayContext*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
    } else {
        ctx = reinterpret_cast<TrayContext*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    switch (msg) {
        case kWmTrayCallback: {
            const UINT mouseMsg = LOWORD(lParam);
            if (mouseMsg == WM_LBUTTONDBLCLK) {
                OpenStudioAppWindow(ctx != nullptr ? ctx->port : 19450U);
                return 0;
            }
            if (mouseMsg == WM_RBUTTONUP || mouseMsg == WM_CONTEXTMENU) {
                ShowTrayContextMenu(hwnd, ctx);
                return 0;
            }
            return 0;
        }

        case WM_COMMAND:
            ExecuteTrayMenuCommand(hwnd, LOWORD(wParam), ctx);
            return 0;

        case WM_CLOSE:
            ::DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            g_stopRequested.store(true, std::memory_order_release);
            ::PostQuitMessage(0);
            return 0;

        default:
            return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

std::string PerformLoopbackRequest(uint16_t port, const char* rawHttpRequest) {
    SOCKET sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        return {};
    }

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    addr.sin_port        = ::htons(port);

    if (::connect(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        ::closesocket(sock);
        return {};
    }

    ::send(sock, rawHttpRequest, static_cast<int>(std::strlen(rawHttpRequest)), 0);

    std::string response;
    char buf[4096]{};
    int received = 0;
    while ((received = ::recv(sock, buf, sizeof(buf), 0)) > 0) {
        response.append(buf, static_cast<size_t>(received));
    }

    ::closesocket(sock);
    return response;
}

bool RunEndToEndBridgeSelfTest(uint16_t port) {
    const std::string telemetryResp = PerformLoopbackRequest(
        port,
        "GET /api/telemetry HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n"
    );
    if (telemetryResp.find("HTTP/1.1 200 OK") == std::string::npos ||
        telemetryResp.find("\"metrics\":[") == std::string::npos) {
        std::cerr << "[SELF-TEST FAIL] /api/telemetry did not return valid JSON.\n";
        return false;
    }

    const std::string layoutResp = PerformLoopbackRequest(
        port,
        "GET /api/layout HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n"
    );
    const size_t bodyPos = layoutResp.find("\r\n\r\n");
    if (layoutResp.find("HTTP/1.1 200 OK") == std::string::npos || bodyPos == std::string::npos) {
        std::cerr << "[SELF-TEST FAIL] /api/layout did not return preset JSON.\n";
        return false;
    }

    const std::string settingsResp = PerformLoopbackRequest(
        port,
        "GET /api/settings HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n"
    );
    if (settingsResp.find("HTTP/1.1 200 OK") == std::string::npos ||
        settingsResp.find("\"toggleHotkey\"") == std::string::npos) {
        std::cerr << "[SELF-TEST FAIL] /api/settings did not return valid settings JSON.\n";
        return false;
    }

    const std::string existingJson = layoutResp.substr(bodyPos + 4U);

    // 1. Test switching to NVIDIA_HORIZONTAL via POST /api/layout
    const std::string nvidiaPayload =
        "{\n"
        "  \"version\": \"3.3.0\",\n"
        "  \"layoutMode\": \"NVIDIA_HORIZONTAL\",\n"
        "  \"offsetX\": 24,\n"
        "  \"offsetY\": 28,\n"
        "  \"bgOpacity\": 0.88,\n"
        "  \"cornerRadius\": 8,\n"
        "  \"fontSize\": 13,\n"
        "  \"showGraph\": true,\n"
        "  \"hideWhenIdle\": false\n"
        "}";
    char postHeader[256]{};
    std::snprintf(
        postHeader,
        sizeof(postHeader),
        "POST /api/layout HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        nvidiaPayload.size()
    );
    const std::string nvidiaPost = std::string(postHeader) + nvidiaPayload;
    const std::string nvidiaResp = PerformLoopbackRequest(port, nvidiaPost.c_str());
    if (nvidiaResp.find("\"status\":\"hot_reloaded\"") == std::string::npos) {
        std::cerr << "[SELF-TEST FAIL] POST /api/layout (NVIDIA_HORIZONTAL) failed.\n";
        return false;
    }

    // 2. Test fault-tolerant payload without layoutMode field (non-fatal fallback to CARD_VERTICAL)
    const std::string fallbackPayload =
        "{\n"
        "  \"version\": \"3.3.0\",\n"
        "  \"offsetX\": 24,\n"
        "  \"offsetY\": 28\n"
        "}";
    std::snprintf(
        postHeader,
        sizeof(postHeader),
        "POST /api/layout HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        fallbackPayload.size()
    );
    const std::string fallbackPost = std::string(postHeader) + fallbackPayload;
    const std::string fallbackResp = PerformLoopbackRequest(port, fallbackPost.c_str());
    if (fallbackResp.find("\"status\":\"hot_reloaded\"") == std::string::npos) {
        std::cerr << "[SELF-TEST FAIL] POST /api/layout (non-fatal fallback) failed.\n";
        return false;
    }

    // 3. Restore original preset JSON
    std::snprintf(
        postHeader,
        sizeof(postHeader),
        "POST /api/layout HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        existingJson.size()
    );
    const std::string fullPost = std::string(postHeader) + existingJson;
    const std::string postResp = PerformLoopbackRequest(port, fullPost.c_str());
    if (postResp.find("\"status\":\"hot_reloaded\"") == std::string::npos) {
        std::cerr << "[SELF-TEST FAIL] POST /api/layout failed atomic hot-reload write.\n";
        return false;
    }

    std::cout << "[SELF-TEST PASS] GET /api/telemetry (200 OK), GET /api/layout (200 OK), "
              << "GET /api/settings (200 OK), POST /api/layout NVIDIA_HORIZONTAL + fault-tolerant hot-reload (200 OK).\n";
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    bool headless        = false;
    bool backgroundMode  = false;
    bool runSelfTest     = false;
    uint32_t durationSec = 0;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (std::strcmp(argv[i], "--background") == 0) {
            headless       = true;
            backgroundMode = true;
        } else if (std::strcmp(argv[i], "--self-test") == 0) {
            runSelfTest = true;
            headless    = true;
        } else if (std::strncmp(argv[i], "--duration=", 11) == 0) {
            const long val = std::strtol(argv[i] + 11, nullptr, 10);
            if (val > 0) {
                durationSec = static_cast<uint32_t>(val);
                headless    = true;
            }
        }
    }

    ::SetConsoleCtrlHandler(&ConsoleCtrlHandler, TRUE);

    // Attach to existing daemon/overlay Shared Memory if present, or host it as fallback.
    zetta::ipc::ZettaSharedMemoryManager ipcClient(zetta::ipc::SharedMemoryRole::Client);
    zetta::ipc::ZettaSharedMemoryManager ipcHostFallback(zetta::ipc::SharedMemoryRole::Host);
    const bool attachedExisting = ipcClient.Initialize();
    if (!attachedExisting) {
        (void)ipcHostFallback.Initialize();
    }
    zetta::ipc::ZettaSharedMemoryManager& activeIpc = attachedExisting ? ipcClient : ipcHostFallback;

    const std::wstring htmlPath     = ResolveWorkspaceFile(L"resources\\studio\\index.html");
    const std::wstring presetPath   = ResolveWorkspaceFile(L"resources\\presets\\default_layout.json");
    const std::wstring settingsPath = ResolveWorkspaceFile(L"resources\\presets\\settings.json");
    const std::wstring overlayPath  = ResolveOverlayExecutable();

    zetta::studio::ZettaStudioBridgeServer bridge(
        activeIpc,
        htmlPath,
        presetPath,
        overlayPath,
        zetta::studio::ZettaStudioBridgeServer::kDefaultPort,
        settingsPath,
        &SetAutostartRegistry
    );

    if (!bridge.Start()) {
        std::cerr << "[ERROR] Failed to bind ZettaStudioBridgeServer to 127.0.0.1:"
                  << zetta::studio::ZettaStudioBridgeServer::kDefaultPort
                  << " (WSA error: " << ::WSAGetLastError() << ").\n";
        return 1;
    }

    if (backgroundMode) {
        (void)bridge.EnsureOverlayRunning();
    }

    std::cout << "[ZettaHUD Studio v3.3] Loopback IPC, System Tray & Hot-Reload Bridge\n"
              << "  Studio URL        : http://127.0.0.1:" << bridge.GetPort() << "\n"
              << "  Shared Memory IPC : " << (attachedExisting ? "Attached to Live Daemon" : "Standalone Host Mode") << "\n"
              << "  Preset Target     : resources/presets/default_layout.json (Atomic MoveFileExW)\n"
              << "  Settings Target   : resources/presets/settings.json (HKCU Run Registry Sync)\n"
              << "  System Tray       : Active (Tooltip: ZettaHUD v3.3)\n"
              << "  Press Ctrl+C or use System Tray -> Exit to stop.\n\n";

    if (runSelfTest) {
        const bool passed = RunEndToEndBridgeSelfTest(bridge.GetPort());
        bridge.Stop();
        return passed ? 0 : 1;
    }

    // Create hidden Win32 window for System Tray callbacks (WM_APP + 1)
    const HINSTANCE hInst = ::GetModuleHandleW(nullptr);
    HICON appIcon = ::LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (appIcon == nullptr) {
        const std::wstring icoFile = ResolveWorkspaceFile(L"resources\\icons\\zetta.ico");
        appIcon = static_cast<HICON>(::LoadImageW(
            nullptr,
            icoFile.c_str(),
            IMAGE_ICON,
            32,
            32,
            LR_LOADFROMFILE
        ));
    }
    if (appIcon == nullptr) {
        appIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    }

    constexpr const wchar_t* kTrayWndClass = L"ZettaHUD_SystemTrayHost_v33";
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc   = &SystemTrayWndProc;
    wc.hInstance     = hInst;
    wc.hIcon         = appIcon;
    wc.hIconSm       = appIcon;
    wc.lpszClassName = kTrayWndClass;
    ::RegisterClassExW(&wc);

    TrayContext trayCtx{};
    trayCtx.bridge = &bridge;
    trayCtx.ipc    = &activeIpc;
    trayCtx.port   = bridge.GetPort();

    const HWND trayHwnd = ::CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kTrayWndClass,
        L"ZettaHUD v3.3 Tray Host",
        WS_POPUP,
        0,
        0,
        0,
        0,
        nullptr,
        nullptr,
        hInst,
        &trayCtx
    );

    NOTIFYICONDATAW nid{};
    bool trayAdded = false;
    if (trayHwnd != nullptr) {
        nid.cbSize           = sizeof(NOTIFYICONDATAW);
        nid.hWnd             = trayHwnd;
        nid.uID              = 1U;
        nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = kWmTrayCallback;
        nid.hIcon            = appIcon;
        std::wcsncpy(nid.szTip, L"ZettaHUD v3.3", std::size(nid.szTip) - 1U);
        trayAdded = (::Shell_NotifyIconW(NIM_ADD, &nid) == TRUE);
    }

    if (!headless) {
        OpenStudioAppWindow(bridge.GetPort());
    }

    const auto startTime = std::chrono::steady_clock::now();
    while (!g_stopRequested.load(std::memory_order_acquire)) {
        MSG msg{};
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_stopRequested.store(true, std::memory_order_release);
                break;
            }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }

        if (g_stopRequested.load(std::memory_order_acquire)) {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        if (durationSec > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - startTime
            ).count();
            if (elapsed >= static_cast<long long>(durationSec)) {
                break;
            }
        }
    }

    if (trayAdded) {
        ::Shell_NotifyIconW(NIM_DELETE, &nid);
    }
    if (trayHwnd != nullptr && ::IsWindow(trayHwnd)) {
        ::DestroyWindow(trayHwnd);
    }

    const uint64_t totalReqs = bridge.GetServedRequestCount();
    bridge.Stop();
    activeIpc.Shutdown();

    std::cout << "[ZettaHUD Studio v3.3] Bridge shut down cleanly (Served " << totalReqs << " requests).\n";
    return 0;
}
