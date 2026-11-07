#pragma once

#include "ipc/ZettaSharedMemoryManager.hpp"
#include "ipc/ZettaSharedState.h"
#include "tracker/ZettaWindowTracker.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>

#include "overlay/ZettaLayoutConfig.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

namespace zetta::studio {

[[nodiscard]] inline bool SetAutostartRegistryKey(bool enable) noexcept {
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

[[nodiscard]] inline bool QueryAutostartRegistryKey() noexcept {
    constexpr const wchar_t* kRunSubKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr const wchar_t* kValueName = L"ZettaHUD";

    HKEY hKey = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunSubKey, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS ||
        hKey == nullptr) {
        return false;
    }

    DWORD type = 0;
    DWORD dataSize = 0;
    const LSTATUS status = ::RegQueryValueExW(hKey, kValueName, nullptr, &type, nullptr, &dataSize);
    ::RegCloseKey(hKey);
    return (status == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && dataSize > 0);
}

class ZettaStudioBridgeServer {
public:
    static constexpr uint16_t kDefaultPort = 19450U;
    using AutostartHandler = std::function<bool(bool)>;

    ZettaStudioBridgeServer(
        zetta::ipc::ZettaSharedMemoryManager& ipcManager,
        std::wstring htmlFilePath,
        std::wstring presetFilePath,
        std::wstring overlayExePath,
        uint16_t port = kDefaultPort,
        std::wstring settingsFilePath = L"",
        AutostartHandler autostartHandler = nullptr
    )
        : ipcManager_(ipcManager),
          htmlFilePath_(std::move(htmlFilePath)),
          presetFilePath_(std::move(presetFilePath)),
          overlayExePath_(std::move(overlayExePath)),
          settingsFilePath_(std::move(settingsFilePath)),
          port_(port),
          autostartHandler_(std::move(autostartHandler)) {
        if (settingsFilePath_.empty()) {
            const size_t slashPos = presetFilePath_.find_last_of(L"\\/");
            if (slashPos != std::wstring::npos) {
                settingsFilePath_ = presetFilePath_.substr(0, slashPos + 1) + L"settings.json";
            } else {
                settingsFilePath_ = L"resources\\presets\\settings.json";
            }
        }
    }

    ~ZettaStudioBridgeServer() noexcept {
        Stop();
    }

    ZettaStudioBridgeServer(const ZettaStudioBridgeServer&) = delete;
    ZettaStudioBridgeServer& operator=(const ZettaStudioBridgeServer&) = delete;

    [[nodiscard]] bool Start() noexcept {
        if (listenSocket_ != INVALID_SOCKET) {
            return true;
        }

        WSADATA wsaData{};
        if (::WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            return false;
        }
        wsaInitialized_ = true;

        listenSocket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSocket_ == INVALID_SOCKET) {
            ShutdownWsa();
            return false;
        }

        const char reuseOpt = 1;
        ::setsockopt(listenSocket_, SOL_SOCKET, SO_REUSEADDR, &reuseOpt, sizeof(reuseOpt));

        sockaddr_in bindAddr{};
        bindAddr.sin_family      = AF_INET;
        bindAddr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK); // Strictly 127.0.0.1 loopback
        bindAddr.sin_port        = ::htons(port_);

        if (::bind(listenSocket_, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR) {
            CloseListenSocket();
            ShutdownWsa();
            return false;
        }

        if (::listen(listenSocket_, SOMAXCONN) == SOCKET_ERROR) {
            CloseListenSocket();
            ShutdownWsa();
            return false;
        }

        serverThread_ = std::jthread([this](std::stop_token stopToken) {
            AcceptLoop(stopToken);
        });
        return true;
    }

    void Stop() noexcept {
        if (serverThread_.joinable()) {
            serverThread_.request_stop();
        }
        CloseListenSocket();
        if (serverThread_.joinable()) {
            serverThread_.join();
        }
        ShutdownWsa();
    }

    [[nodiscard]] uint16_t GetPort() const noexcept { return port_; }
    [[nodiscard]] uint64_t GetServedRequestCount() const noexcept {
        return requestCount_.load(std::memory_order_acquire);
    }

    bool EnsureOverlayRunning() const noexcept {
        return EnsureOverlayProcessRunning();
    }

    bool StopOverlayRunning() const noexcept {
        return TerminateOverlayProcesses();
    }

private:
    void CloseListenSocket() noexcept {
        if (listenSocket_ != INVALID_SOCKET) {
            ::closesocket(listenSocket_);
            listenSocket_ = INVALID_SOCKET;
        }
    }

    void ShutdownWsa() noexcept {
        if (wsaInitialized_) {
            ::WSACleanup();
            wsaInitialized_ = false;
        }
    }

    void AcceptLoop(std::stop_token stopToken) noexcept {
        while (!stopToken.stop_requested()) {
            fd_set readSet{};
            FD_ZERO(&readSet);
            FD_SET(listenSocket_, &readSet);

            timeval timeout{};
            timeout.tv_sec  = 0;
            timeout.tv_usec = 100000; // 100ms poll for responsive stop_token checks

            const int ready = ::select(0, &readSet, nullptr, nullptr, &timeout);
            if (ready <= 0 || stopToken.stop_requested()) {
                continue;
            }

            sockaddr_in clientAddr{};
            int addrLen = sizeof(clientAddr);
            SOCKET clientSock = ::accept(
                listenSocket_,
                reinterpret_cast<sockaddr*>(&clientAddr),
                &addrLen
            );
            if (clientSock == INVALID_SOCKET) {
                if (stopToken.stop_requested()) {
                    break;
                }
                continue;
            }

            const DWORD recvTimeoutMs = 2000;
            ::setsockopt(
                clientSock,
                SOL_SOCKET,
                SO_RCVTIMEO,
                reinterpret_cast<const char*>(&recvTimeoutMs),
                sizeof(recvTimeoutMs)
            );

            HandleClientConnection(clientSock);
            ::closesocket(clientSock);
        }
    }

    void HandleClientConnection(SOCKET clientSock) noexcept {
        std::string requestBuf;
        requestBuf.reserve(8192);

        char chunk[4096]{};
        size_t headerEndPos  = std::string::npos;
        size_t contentLength = 0;

        while (requestBuf.size() < 73728U) {
            const int bytesRead = ::recv(clientSock, chunk, sizeof(chunk), 0);
            if (bytesRead <= 0) {
                break;
            }
            requestBuf.append(chunk, static_cast<size_t>(bytesRead));

            if (headerEndPos == std::string::npos) {
                headerEndPos = requestBuf.find("\r\n\r\n");
                if (headerEndPos != std::string::npos) {
                    contentLength = ParseContentLength(std::string_view(requestBuf.data(), headerEndPos));
                    if (contentLength > 65536U) {
                        SendHttpResponse(clientSock, "413 Payload Too Large", "application/json", "{\"error\":\"payload_too_large\"}");
                        return;
                    }
                }
            }

            if (headerEndPos != std::string::npos) {
                const size_t bodyAvailable = requestBuf.size() - (headerEndPos + 4U);
                if (bodyAvailable >= contentLength) {
                    break;
                }
            }
        }

        if (headerEndPos == std::string::npos) {
            return;
        }

        ++requestCount_;

        const size_t firstLineEnd = requestBuf.find("\r\n");
        const std::string_view requestLine(requestBuf.data(), firstLineEnd);
        const std::string_view bodyView(
            requestBuf.data() + headerEndPos + 4U,
            requestBuf.size() - (headerEndPos + 4U)
        );

        if (requestLine.starts_with("GET / ") ||
            requestLine.starts_with("GET /?") ||
            requestLine.starts_with("GET /index.html")) {
            std::string htmlContent = ReadBinaryFileUtf8(htmlFilePath_);
            if (htmlContent.empty()) {
                SendHttpResponse(clientSock, "404 Not Found", "text/plain", "Studio UI file not found.");
                return;
            }
            SendHttpResponse(clientSock, "200 OK", "text/html; charset=utf-8", htmlContent);
            return;
        }

        if (requestLine.starts_with("GET /api/telemetry")) {
            const std::string json = BuildTelemetryJson();
            SendHttpResponse(clientSock, "200 OK", "application/json; charset=utf-8", json);
            return;
        }

        if (requestLine.starts_with("GET /api/layout")) {
            std::string presetJson = ReadBinaryFileUtf8(presetFilePath_);
            if (presetJson.empty()) {
                presetJson = "{\"error\":\"preset_missing\"}";
            }
            SendHttpResponse(clientSock, "200 OK", "application/json; charset=utf-8", presetJson);
            return;
        }

        if (requestLine.starts_with("POST /api/layout")) {
            zetta::overlay::ZettaLayoutConfig layoutValidator;
            if (!layoutValidator.ParseJson(bodyView)) {
                const std::string_view preview = bodyView.substr(0, std::min<size_t>(bodyView.size(), 200U));
                std::cerr << "[ERROR] POST /api/layout: ParseJson failed on payload ("
                          << bodyView.size() << " bytes). First 200 chars:\n"
                          << preview << "\n";
                SendHttpResponse(clientSock, "400 Bad Request", "application/json", "{\"ok\":false,\"error\":\"invalid_json\"}");
                return;
            }
            if (!WriteFileAtomically(presetFilePath_, bodyView)) {
                const std::string_view preview = bodyView.substr(0, std::min<size_t>(bodyView.size(), 200U));
                std::cerr << "[ERROR] POST /api/layout: WriteFileAtomically failed (Win32 error="
                          << ::GetLastError() << "). First 200 chars:\n"
                          << preview << "\n";
                SendHttpResponse(clientSock, "400 Bad Request", "application/json", "{\"ok\":false,\"error\":\"write_failed\"}");
                return;
            }
            const HWND overlayHwnd = ::FindWindowW(L"ZettaHUD_OverlayWindow_v1", nullptr);
            if (overlayHwnd != nullptr) {
                ::PostMessageW(overlayHwnd, WM_APP + 2U, 0, 0);
            }
            SendHttpResponse(clientSock, "200 OK", "application/json", "{\"ok\":true,\"status\":\"hot_reloaded\"}");
            return;
        }

        if (requestLine.starts_with("GET /api/settings")) {
            const std::string settingsJson = BuildSettingsJson();
            SendHttpResponse(clientSock, "200 OK", "application/json; charset=utf-8", settingsJson);
            return;
        }

        if (requestLine.starts_with("POST /api/settings/autostart")) {
            bool enabled = false;
            if (!ExtractJsonBool(bodyView, "\"enabled\"", enabled)) {
                SendHttpResponse(clientSock, "400 Bad Request", "application/json", "{\"ok\":false,\"error\":\"missing_enabled_flag\"}");
                return;
            }
            const bool regOk = autostartHandler_ ? autostartHandler_(enabled) : SetAutostartRegistryKey(enabled);
            UpdateSettingsFileAutostart(enabled);

            std::ostringstream oss;
            oss << "{\"ok\":" << (regOk ? "true" : "false")
                << ",\"autostart\":" << (enabled ? "true" : "false") << "}";
            SendHttpResponse(clientSock, "200 OK", "application/json", oss.str());
            return;
        }

        if (requestLine.starts_with("POST /api/settings")) {
            bool enabled = false;
            if (ExtractJsonBool(bodyView, "\"autostart\"", enabled)) {
                (void)(autostartHandler_ ? autostartHandler_(enabled) : SetAutostartRegistryKey(enabled));
            }
            if (!WriteFileAtomically(settingsFilePath_, bodyView)) {
                SendHttpResponse(clientSock, "400 Bad Request", "application/json", "{\"ok\":false,\"error\":\"invalid_settings_json\"}");
                return;
            }
            SendHttpResponse(clientSock, "200 OK", "application/json", "{\"ok\":true,\"status\":\"settings_saved\"}");
            return;
        }

        if (requestLine.starts_with("POST /api/overlay/toggle")) {
            std::string action;
            (void)ExtractJsonString(bodyView, "\"action\"", action);
            const bool currentlyRunning = IsProcessRunningByName(L"zetta_hud_live.exe");
            bool nowRunning = currentlyRunning;

            if (action == "stop" || (action != "start" && currentlyRunning)) {
                (void)TerminateOverlayProcesses();
                nowRunning = IsProcessRunningByName(L"zetta_hud_live.exe");
            } else {
                nowRunning = EnsureOverlayProcessRunning();
            }

            SendHttpResponse(
                clientSock,
                "200 OK",
                "application/json",
                nowRunning ? "{\"ok\":true,\"overlayRunning\":true}" : "{\"ok\":true,\"overlayRunning\":false}"
            );
            return;
        }

        if (requestLine.starts_with("POST /api/spawn-overlay")) {
            const bool running = EnsureOverlayProcessRunning();
            SendHttpResponse(
                clientSock,
                "200 OK",
                "application/json",
                running ? "{\"ok\":true,\"overlayRunning\":true}" : "{\"ok\":false,\"overlayRunning\":false}"
            );
            return;
        }

        SendHttpResponse(clientSock, "404 Not Found", "application/json", "{\"error\":\"not_found\"}");
    }

    static bool ExtractJsonBool(std::string_view json, std::string_view key, bool& outVal) noexcept {
        const size_t keyPos = json.find(key);
        if (keyPos == std::string_view::npos) {
            return false;
        }
        const size_t colonPos = json.find(':', keyPos + key.size());
        if (colonPos == std::string_view::npos) {
            return false;
        }
        size_t valPos = colonPos + 1U;
        while (valPos < json.size() &&
               (json[valPos] == ' ' || json[valPos] == '\t' || json[valPos] == '\r' || json[valPos] == '\n')) {
            ++valPos;
        }
        if (json.substr(valPos).starts_with("true")) {
            outVal = true;
            return true;
        }
        if (json.substr(valPos).starts_with("false")) {
            outVal = false;
            return true;
        }
        return false;
    }

    static bool ExtractJsonString(std::string_view json, std::string_view key, std::string& outVal) noexcept {
        const size_t keyPos = json.find(key);
        if (keyPos == std::string_view::npos) {
            return false;
        }
        const size_t colonPos = json.find(':', keyPos + key.size());
        if (colonPos == std::string_view::npos) {
            return false;
        }
        const size_t qStart = json.find('"', colonPos + 1U);
        if (qStart == std::string_view::npos) {
            return false;
        }
        const size_t qEnd = json.find('"', qStart + 1U);
        if (qEnd == std::string_view::npos) {
            return false;
        }
        outVal.assign(json.substr(qStart + 1U, qEnd - qStart - 1U));
        return true;
    }

    std::string BuildSettingsJson() const {
        const std::string raw = ReadBinaryFileUtf8(settingsFilePath_);
        std::string toggleHotkey  = "Ctrl+Shift+F12";
        std::string compactHotkey = "Ctrl+Shift+F11";
        bool autostart = QueryAutostartRegistryKey();

        if (!raw.empty()) {
            (void)ExtractJsonString(raw, "\"toggleHotkey\"", toggleHotkey);
            (void)ExtractJsonString(raw, "\"compactHotkey\"", compactHotkey);
            bool fileAutostart = false;
            if (!autostart && ExtractJsonBool(raw, "\"autostart\"", fileAutostart)) {
                autostart = fileAutostart;
            }
        }

        const bool overlayRunning = IsProcessRunningByName(L"zetta_hud_live.exe");

        std::ostringstream oss;
        oss << "{"
            << "\"version\":\"3.3.0\","
            << "\"autostart\":" << (autostart ? "true" : "false") << ","
            << "\"toggleHotkey\":\"" << EscapeJsonString(toggleHotkey) << "\","
            << "\"compactHotkey\":\"" << EscapeJsonString(compactHotkey) << "\","
            << "\"overlayRunning\":" << (overlayRunning ? "true" : "false")
            << "}";
        return oss.str();
    }

    void UpdateSettingsFileAutostart(bool enabled) const noexcept {
        const std::string raw = ReadBinaryFileUtf8(settingsFilePath_);
        std::string toggleHotkey  = "Ctrl+Shift+F12";
        std::string compactHotkey = "Ctrl+Shift+F11";
        if (!raw.empty()) {
            (void)ExtractJsonString(raw, "\"toggleHotkey\"", toggleHotkey);
            (void)ExtractJsonString(raw, "\"compactHotkey\"", compactHotkey);
        }

        std::ostringstream oss;
        oss << "{\n"
            << "  \"version\": \"3.3.0\",\n"
            << "  \"autostart\": " << (enabled ? "true" : "false") << ",\n"
            << "  \"toggleHotkey\": \"" << EscapeJsonString(toggleHotkey) << "\",\n"
            << "  \"compactHotkey\": \"" << EscapeJsonString(compactHotkey) << "\"\n"
            << "}\n";
        const std::string payload = oss.str();
        (void)WriteFileAtomically(settingsFilePath_, payload);
    }

    static size_t ParseContentLength(std::string_view headers) noexcept {
        constexpr std::string_view kPrefix = "Content-Length:";
        size_t pos = 0;
        while (pos < headers.size()) {
            const size_t lineEnd = headers.find("\r\n", pos);
            const std::string_view line = headers.substr(
                pos,
                lineEnd == std::string_view::npos ? std::string_view::npos : (lineEnd - pos)
            );
            if (line.size() >= kPrefix.size()) {
                bool match = true;
                for (size_t i = 0; i < kPrefix.size(); ++i) {
                    const char a = (line[i] >= 'A' && line[i] <= 'Z') ? static_cast<char>(line[i] + 32) : line[i];
                    const char b = (kPrefix[i] >= 'A' && kPrefix[i] <= 'Z') ? static_cast<char>(kPrefix[i] + 32) : kPrefix[i];
                    if (a != b) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    const std::string valStr(line.substr(kPrefix.size()));
                    return static_cast<size_t>(std::strtoul(valStr.c_str(), nullptr, 10));
                }
            }
            if (lineEnd == std::string_view::npos) {
                break;
            }
            pos = lineEnd + 2U;
        }
        return 0;
    }

    static void SendHttpResponse(
        SOCKET sock,
        const char* statusText,
        const char* contentType,
        std::string_view body
    ) noexcept {
        char header[512]{};
        const int headerLen = std::snprintf(
            header,
            sizeof(header),
            "HTTP/1.1 %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Cache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n"
            "Pragma: no-cache\r\n"
            "Expires: 0\r\n"
            "Connection: close\r\n\r\n",
            statusText,
            contentType,
            body.size()
        );
        if (headerLen > 0) {
            ::send(sock, header, headerLen, 0);
        }
        if (!body.empty()) {
            ::send(sock, body.data(), static_cast<int>(body.size()), 0);
        }
    }

    static std::string EscapeJsonString(std::string_view input) {
        std::string out;
        out.reserve(input.size() + 8U);
        for (const char ch : input) {
            if (ch == '"') {
                out += "\\\"";
            } else if (ch == '\\') {
                out += "\\\\";
            } else if (static_cast<unsigned char>(ch) >= 0x20U) {
                out += ch;
            }
        }
        return out;
    }

    std::string BuildTelemetryJson() const {
        zetta::ipc::ZettaSharedState state{};
        const bool ok = ipcManager_.ReadSnapshot(state);

        std::ostringstream oss;
        oss << "{";
        oss << "\"connected\":" << (ok ? "true" : "false") << ",";
        oss << "\"overlayRunning\":" << (IsProcessRunningByName(L"zetta_hud_live.exe") ? "true" : "false") << ",";
        oss << "\"timestampMs\":" << state.timestampMs << ",";
        oss << "\"targetPid\":" << state.targetPid << ",";
        oss << "\"targetProcessName\":\"" << EscapeJsonString(state.targetProcessName) << "\",";
        oss << "\"isGameProcess\":" << (state.isGameProcess ? "true" : "false") << ",";
        oss << "\"isElevated\":" << (state.isElevated ? "true" : "false") << ",";
        oss << "\"isFullscreen\":" << (state.isFullscreen ? "true" : "false") << ",";
        oss << "\"windowX\":" << state.windowX << ",";
        oss << "\"windowY\":" << state.windowY << ",";
        oss << "\"windowWidth\":" << state.windowWidth << ",";
        oss << "\"windowHeight\":" << state.windowHeight << ",";

        const float avgFt = std::isfinite(state.frameGraph.avgFrametimeMs) ? state.frameGraph.avgFrametimeMs : 0.0f;
        const float low1  = std::isfinite(state.frameGraph.onePercentLowFps) ? state.frameGraph.onePercentLowFps : 0.0f;
        oss << "\"avgFrametimeMs\":" << avgFt << ",";
        oss << "\"onePercentLowFps\":" << low1 << ",";

        oss << "\"metrics\":[";
        const uint32_t count = (state.metricCount <= zetta::ipc::kMaxMetrics) ? state.metricCount : 0U;
        bool firstMetric = true;
        for (uint32_t i = 0; i < count; ++i) {
            const auto& m = state.metrics[i];
            if (m.id[0] == '\0') {
                continue;
            }
            if (!firstMetric) {
                oss << ",";
            }
            firstMetric = false;

            char hexBuf[16]{};
            std::snprintf(hexBuf, sizeof(hexBuf), "#%06X", m.colorHex & 0xFFFFFFU);

            const double safeVal = std::isfinite(m.numericValue) ? m.numericValue : 0.0;
            oss << "{"
                << "\"id\":\"" << EscapeJsonString(m.id) << "\","
                << "\"label\":\"" << EscapeJsonString(m.label) << "\","
                << "\"numericValue\":" << safeVal << ","
                << "\"formatted\":\"" << EscapeJsonString(m.formatted) << "\","
                << "\"colorHex\":\"" << hexBuf << "\","
                << "\"isVisible\":" << (m.isVisible ? "true" : "false")
                << "}";
        }
        oss << "],";

        oss << "\"frameSamples\":[";
        for (size_t i = 0; i < zetta::ipc::kFrameHistorySize; ++i) {
            if (i > 0) {
                oss << ",";
            }
            const size_t ringIdx = (state.frameGraph.headIndex + i) % zetta::ipc::kFrameHistorySize;
            const float val = std::isfinite(state.frameGraph.samples[ringIdx])
                                  ? state.frameGraph.samples[ringIdx]
                                  : 0.0f;
            oss << val;
        }
        oss << "]}";
        return oss.str();
    }

    static std::string WideToUtf8Path(const std::wstring& widePath) noexcept {
        if (widePath.empty()) {
            return {};
        }
        const int len = ::WideCharToMultiByte(
            CP_UTF8, 0, widePath.c_str(), static_cast<int>(widePath.size()),
            nullptr, 0, nullptr, nullptr
        );
        if (len <= 0) {
            return {};
        }
        std::string out(static_cast<size_t>(len), '\0');
        ::WideCharToMultiByte(
            CP_UTF8, 0, widePath.c_str(), static_cast<int>(widePath.size()),
            out.data(), len, nullptr, nullptr
        );
        return out;
    }

    static std::string ReadBinaryFileUtf8(const std::wstring& widePath) noexcept {
        const std::string path = WideToUtf8Path(widePath);
        if (path.empty()) {
            return {};
        }
        std::ifstream ifs(path, std::ios::in | std::ios::binary);
        if (!ifs.is_open()) {
            return {};
        }
        return std::string(
            (std::istreambuf_iterator<char>(ifs)),
            std::istreambuf_iterator<char>()
        );
    }

    static bool WriteFileAtomically(const std::wstring& targetPath, std::string_view jsonPayload) noexcept {
        const size_t openBracePos = jsonPayload.find('{');
        if (openBracePos == std::string_view::npos) {
            return false;
        }
        jsonPayload.remove_prefix(openBracePos);
        if (jsonPayload.size() < 2U || jsonPayload.size() > 65536U) {
            return false;
        }

        const std::wstring tmpPath  = targetPath + L".tmp";
        const std::string narrowTmp = WideToUtf8Path(tmpPath);
        if (!narrowTmp.empty()) {
            std::ofstream ofs(narrowTmp, std::ios::out | std::ios::binary | std::ios::trunc);
            if (ofs.is_open()) {
                ofs.write(jsonPayload.data(), static_cast<std::streamsize>(jsonPayload.size()));
                ofs.flush();
                const bool tmpOk = ofs.good();
                ofs.close();

                if (tmpOk) {
                    for (int attempt = 0; attempt < 5; ++attempt) {
                        if (::MoveFileExW(
                                tmpPath.c_str(),
                                targetPath.c_str(),
                                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
                            ) != FALSE) {
                            return true;
                        }
                        ::Sleep(5);
                    }
                }
            }
        }

        // Direct write fallback if atomic MoveFileExW encountered a sharing lock
        const std::string narrowTarget = WideToUtf8Path(targetPath);
        if (narrowTarget.empty()) {
            return false;
        }
        std::ofstream directOfs(narrowTarget, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!directOfs.is_open()) {
            return false;
        }
        directOfs.write(jsonPayload.data(), static_cast<std::streamsize>(jsonPayload.size()));
        directOfs.flush();
        return directOfs.good();
    }

    [[nodiscard]] static bool EqualsCaseInsensitiveWide(std::wstring_view a, std::wstring_view b) noexcept {
        if (a.size() != b.size()) {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::towlower(static_cast<wint_t>(a[i])) != std::towlower(static_cast<wint_t>(b[i]))) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] static bool IsProcessRunningByName(std::wstring_view targetExe) noexcept {
        zetta::tracker::ScopedKernelHandle snap(
            ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
        );
        if (!snap.IsValid()) {
            return false;
        }

        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(PROCESSENTRY32W);
        if (!::Process32FirstW(snap.Get(), &pe)) {
            return false;
        }

        do {
            if (EqualsCaseInsensitiveWide(pe.szExeFile, targetExe)) {
                return true;
            }
        } while (::Process32NextW(snap.Get(), &pe));

        return false;
    }

    static bool TerminateOverlayProcesses() noexcept {
        zetta::tracker::ScopedKernelHandle snap(
            ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
        );
        if (!snap.IsValid()) {
            return false;
        }

        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(PROCESSENTRY32W);
        if (!::Process32FirstW(snap.Get(), &pe)) {
            return false;
        }

        bool terminatedAny = false;
        do {
            if (EqualsCaseInsensitiveWide(pe.szExeFile, L"zetta_hud_live.exe")) {
                zetta::tracker::ScopedKernelHandle proc(
                    ::OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID)
                );
                if (proc.IsValid()) {
                    if (::TerminateProcess(proc.Get(), 0)) {
                        ::WaitForSingleObject(proc.Get(), 500);
                        terminatedAny = true;
                    }
                }
            }
        } while (::Process32NextW(snap.Get(), &pe));

        return terminatedAny;
    }

    bool EnsureOverlayProcessRunning() const noexcept {
        if (IsProcessRunningByName(L"zetta_hud_live.exe")) {
            return true;
        }
        if (overlayExePath_.empty() ||
            ::GetFileAttributesW(overlayExePath_.c_str()) == INVALID_FILE_ATTRIBUTES) {
            return false;
        }

        STARTUPINFOW si{};
        si.cb = sizeof(STARTUPINFOW);
        PROCESS_INFORMATION pi{};

        std::wstring cmdLine = L"\"" + overlayExePath_ + L"\"";
        const BOOL created = ::CreateProcessW(
            overlayExePath_.c_str(),
            cmdLine.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi
        );

        if (created) {
            ::CloseHandle(pi.hThread);
            ::CloseHandle(pi.hProcess);
            return true;
        }
        return false;
    }

    zetta::ipc::ZettaSharedMemoryManager& ipcManager_;
    std::wstring                          htmlFilePath_;
    std::wstring                          presetFilePath_;
    std::wstring                          overlayExePath_;
    std::wstring                          settingsFilePath_;
    uint16_t                              port_{kDefaultPort};
    AutostartHandler                      autostartHandler_{};

    SOCKET                                listenSocket_{INVALID_SOCKET};
    bool                                  wsaInitialized_{false};
    std::atomic<uint64_t>                 requestCount_{0};
    std::jthread                          serverThread_{};
};

} // namespace zetta::studio
