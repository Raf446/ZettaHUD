#pragma once

#include "ipc/ZettaSharedState.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>

namespace zetta::overlay {

enum class LayoutMode : uint8_t {
    CardVertical     = 0,
    NvidiaHorizontal = 1
};

struct MetricStyleOverride {
    char     id[32]{};
    char     label[16]{};
    uint32_t colorHex{0xFFFFFFFFU};
    bool     isVisible{true};
};

struct LayoutConfigData {
    LayoutMode layoutMode{LayoutMode::CardVertical};
    char       layoutModeName[24]{"CARD_VERTICAL"};
    int32_t    offsetX{20};
    int32_t    offsetY{20};
    float      bgOpacity{0.88f};
    float      cornerRadius{4.0f};
    wchar_t    fontFamily[64]{L"JetBrains Mono"};
    float      fontSize{12.0f};
    bool       showGraph{true};
    float      graphHeight{38.0f};
    uint32_t   graphColorHex{0xFF4ADE80U};
    bool       hideWhenIdle{false};

    uint32_t   styleCount{0};
    std::array<MetricStyleOverride, zetta::ipc::kMaxMetrics> styles{};
};

class ZettaLayoutConfig {
public:
    ZettaLayoutConfig() noexcept {
        ResetToDefaults();
    }

    ~ZettaLayoutConfig() noexcept = default;

    void ResetToDefaults() noexcept {
        data_ = LayoutConfigData{};
        std::wcsncpy(data_.fontFamily, L"JetBrains Mono", std::size(data_.fontFamily) - 1);
        zetta::ipc::CopyBoundedString(data_.layoutModeName, "CARD_VERTICAL");

        struct DefaultSlot {
            const char* id;
            const char* label;
            uint32_t    color;
            bool        visible;
        };

        static constexpr std::array<DefaultSlot, 13> kDefaults = {{
            {"fps_engine",  "FPS",      0xFFFB7185U, true},
            {"fps_1low",    "1% LOW",   0xFF34D399U, true},
            {"gpu_load",    "GPU",      0xFF22C55EU, true},
            {"gpu_temp",    "GPU_TMP",  0xFF34D399U, true},
            {"cpu_load",    "CPU",      0xFF38BDF8U, true},
            {"cpu_temp",    "CPU_TMP",  0xFFFB923CU, true},
            {"vram_ded",    "VRAM",     0xFFC084FCU, true},
            {"vram_shared", "SH_VRAM",  0xFFA855F7U, true},
            {"ram_sys",     "RAM",      0xFFF472B6U, true},
            {"proc_ram",    "PROC_MEM", 0xFFE879F9U, true},
            {"disk_io",     "NVME_IO",  0xFFFB923CU, true},
            {"net_rate",    "NET_IO",   0xFF2DD4BFU, true},
            {"pwr_batt",    "BATT_PWR", 0xFFFACC15U, true}
        }};

        data_.styleCount = static_cast<uint32_t>(kDefaults.size());
        for (size_t i = 0; i < kDefaults.size(); ++i) {
            zetta::ipc::CopyBoundedString(data_.styles[i].id, kDefaults[i].id);
            zetta::ipc::CopyBoundedString(data_.styles[i].label, kDefaults[i].label);
            data_.styles[i].colorHex  = kDefaults[i].color;
            data_.styles[i].isVisible = kDefaults[i].visible;
        }
    }

    bool LoadFromFile(const std::wstring& filePath) noexcept {
        configFilePath_ = filePath;
        WIN32_FILE_ATTRIBUTE_DATA attrData{};
        if (::GetFileAttributesExW(filePath.c_str(), GetFileExInfoStandard, &attrData)) {
            lastWriteTime_ = attrData.ftLastWriteTime;
            lastFileSize_  = attrData.nFileSizeLow;
        }
        return ParseFileInternal(filePath);
    }

    bool ForceReload() noexcept {
        if (configFilePath_.empty()) {
            return false;
        }
        WIN32_FILE_ATTRIBUTE_DATA attrData{};
        if (::GetFileAttributesExW(configFilePath_.c_str(), GetFileExInfoStandard, &attrData)) {
            lastWriteTime_ = attrData.ftLastWriteTime;
            lastFileSize_  = attrData.nFileSizeLow;
        }
        return ParseFileInternal(configFilePath_);
    }

    [[nodiscard]] bool CheckAndReloadIfModified() noexcept {
        if (configFilePath_.empty()) {
            return false;
        }

        WIN32_FILE_ATTRIBUTE_DATA attrData{};
        if (!::GetFileAttributesExW(configFilePath_.c_str(), GetFileExInfoStandard, &attrData)) {
            return false;
        }

        if (::CompareFileTime(&attrData.ftLastWriteTime, &lastWriteTime_) == 0 &&
            attrData.nFileSizeLow == lastFileSize_) {
            return false;
        }

        lastWriteTime_ = attrData.ftLastWriteTime;
        lastFileSize_  = attrData.nFileSizeLow;
        return ParseFileInternal(configFilePath_);
    }

    [[nodiscard]] const LayoutConfigData& Get() const noexcept {
        return data_;
    }

    [[nodiscard]] LayoutMode GetLayoutMode() const noexcept {
        return data_.layoutMode;
    }

    [[nodiscard]] const MetricStyleOverride* FindStyle(std::string_view metricId) const noexcept {
        for (uint32_t i = 0; i < data_.styleCount; ++i) {
            if (metricId == data_.styles[i].id) {
                return &data_.styles[i];
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool ParseJson(std::string_view content) noexcept {
        while (!content.empty() &&
               (content.front() == ' ' || content.front() == '\t' ||
                content.front() == '\r' || content.front() == '\n' ||
                static_cast<unsigned char>(content.front()) >= 0x80U)) {
            content.remove_prefix(1);
        }
        if (content.empty() || content.find('{') == std::string_view::npos) {
            return false;
        }

        ResetToDefaults();
        (void)ParseTopLevelFields(content);
        ParseMetricEntries(content);
        return true;
    }

private:
    bool ParseFileInternal(const std::wstring& filePath) noexcept {
        const int utf8Len = ::WideCharToMultiByte(
            CP_UTF8, 0, filePath.c_str(), static_cast<int>(filePath.size()),
            nullptr, 0, nullptr, nullptr
        );
        if (utf8Len <= 0) {
            return false;
        }
        std::string narrowPath(static_cast<size_t>(utf8Len), '\0');
        ::WideCharToMultiByte(
            CP_UTF8, 0, filePath.c_str(), static_cast<int>(filePath.size()),
            narrowPath.data(), utf8Len, nullptr, nullptr
        );

        std::ifstream ifs(narrowPath, std::ios::in | std::ios::binary);
        if (!ifs.is_open()) {
            return false;
        }

        std::string content(
            (std::istreambuf_iterator<char>(ifs)),
            std::istreambuf_iterator<char>()
        );
        return ParseJson(content);
    }

    static bool ExtractNumberAfterKey(std::string_view json, std::string_view key, double& outVal) noexcept {
        const size_t keyPos = json.find(key);
        if (keyPos == std::string_view::npos) {
            return false;
        }
        const size_t colonPos = json.find(':', keyPos + key.size());
        if (colonPos == std::string_view::npos) {
            return false;
        }
        size_t valPos = colonPos + 1;
        while (valPos < json.size() && (json[valPos] == ' ' || json[valPos] == '\t' ||
                                        json[valPos] == '\r' || json[valPos] == '\n')) {
            ++valPos;
        }
        if (valPos >= json.size()) {
            return false;
        }
        char* endPtr = nullptr;
        const std::string temp(json.substr(valPos, 32));
        outVal = std::strtod(temp.c_str(), &endPtr);
        return endPtr != temp.c_str();
    }

    static bool ExtractBoolAfterKey(std::string_view json, std::string_view key, bool& outVal) noexcept {
        const size_t keyPos = json.find(key);
        if (keyPos == std::string_view::npos) {
            return false;
        }
        const size_t colonPos = json.find(':', keyPos + key.size());
        if (colonPos == std::string_view::npos) {
            return false;
        }
        size_t valPos = colonPos + 1;
        while (valPos < json.size() && (json[valPos] == ' ' || json[valPos] == '\t' ||
                                        json[valPos] == '\r' || json[valPos] == '\n')) {
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

    static bool ExtractStringField(
        std::string_view json,
        std::string_view key,
        std::string& outStr
    ) noexcept {
        size_t keyPos = json.find(key);
        if (keyPos == std::string_view::npos && key.size() > 2 && key.front() == '"' && key.back() == '"') {
            keyPos = json.find(key.substr(1, key.size() - 2));
        }
        if (keyPos == std::string_view::npos) {
            return false;
        }
        const size_t colonPos = json.find(':', keyPos);
        if (colonPos == std::string_view::npos) {
            return false;
        }
        const size_t quoteStart = json.find('"', colonPos + 1);
        if (quoteStart == std::string_view::npos) {
            return false;
        }
        const size_t quoteEnd = json.find('"', quoteStart + 1);
        if (quoteEnd == std::string_view::npos) {
            return false;
        }
        outStr = TrimQuotesAndWhitespace(json.substr(quoteStart + 1, quoteEnd - quoteStart - 1));
        return true;
    }

    static bool ExtractStringAfterKey(
        std::string_view json,
        std::string_view key,
        std::string& outStr
    ) noexcept {
        return ExtractStringField(json, key, outStr);
    }

    static uint32_t ParseHexColorString(std::string_view str, uint32_t fallback) noexcept {
        if (str.empty()) {
            return fallback;
        }
        if (str.starts_with('#')) {
            str.remove_prefix(1);
        } else if (str.starts_with("0x") || str.starts_with("0X")) {
            str.remove_prefix(2);
        }

        std::string temp(str.substr(0, 8));
        char* endPtr = nullptr;
        const unsigned long parsed = std::strtoul(temp.c_str(), &endPtr, 16);
        if (endPtr == temp.c_str()) {
            return fallback;
        }
        if (temp.size() <= 6) {
            return 0xFF000000U | static_cast<uint32_t>(parsed);
        }
        return static_cast<uint32_t>(parsed);
    }

    static std::string TrimQuotesAndWhitespace(std::string_view sv) noexcept {
        if (sv.empty()) {
            return {};
        }
        size_t first = 0;
        while (first < sv.size()) {
            const char c = sv[first];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"' || c == '\'') {
                ++first;
            } else {
                break;
            }
        }
        if (first >= sv.size()) {
            return {};
        }
        size_t last = sv.size();
        while (last > first) {
            const char c = sv[last - 1U];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"' || c == '\'') {
                --last;
            } else {
                break;
            }
        }
        return std::string(sv.substr(first, last - first));
    }

    bool ParseTopLevelFields(std::string_view json) noexcept {
        // Non-fatal layoutMode extraction: default to CardVertical if missing or unrecognized,
        // and never return false so the rest of the payload (colors, sensors) is always applied.
        std::string rawModeStr;
        if (ExtractStringField(json, "\"layoutMode\"", rawModeStr)) {
            const std::string modeStr = TrimQuotesAndWhitespace(rawModeStr);
            if (modeStr == "NVIDIA_HORIZONTAL") {
                data_.layoutMode = LayoutMode::NvidiaHorizontal;
                zetta::ipc::CopyBoundedString(data_.layoutModeName, "NVIDIA_HORIZONTAL");
            } else if (modeStr == "CARD_VERTICAL") {
                data_.layoutMode = LayoutMode::CardVertical;
                zetta::ipc::CopyBoundedString(data_.layoutModeName, "CARD_VERTICAL");
            } else {
                data_.layoutMode = LayoutMode::CardVertical;
                zetta::ipc::CopyBoundedString(data_.layoutModeName, "CARD_VERTICAL");
            }
        } else {
            data_.layoutMode = LayoutMode::CardVertical;
            zetta::ipc::CopyBoundedString(data_.layoutModeName, "CARD_VERTICAL");
        }

        double numVal = 0.0;
        if (ExtractNumberAfterKey(json, "\"offsetX\"", numVal)) {
            data_.offsetX = static_cast<int32_t>(numVal);
        }
        if (ExtractNumberAfterKey(json, "\"offsetY\"", numVal)) {
            data_.offsetY = static_cast<int32_t>(numVal);
        }
        if (ExtractNumberAfterKey(json, "\"bgOpacity\"", numVal) ||
            ExtractNumberAfterKey(json, "\"opacity\"", numVal)) {
            data_.bgOpacity = std::clamp(static_cast<float>(numVal), 0.1f, 1.0f);
        }
        if (ExtractNumberAfterKey(json, "\"cornerRadius\"", numVal) ||
            ExtractNumberAfterKey(json, "\"cornerRadiusPx\"", numVal)) {
            data_.cornerRadius = std::clamp(static_cast<float>(numVal), 0.0f, 24.0f);
        }
        if (ExtractNumberAfterKey(json, "\"fontSize\"", numVal) ||
            ExtractNumberAfterKey(json, "\"fontSizePt\"", numVal)) {
            data_.fontSize = std::clamp(static_cast<float>(numVal), 8.0f, 28.0f);
        }
        if (ExtractNumberAfterKey(json, "\"graphHeight\"", numVal) ||
            ExtractNumberAfterKey(json, "\"heightPx\"", numVal)) {
            data_.graphHeight = std::clamp(static_cast<float>(numVal), 20.0f, 120.0f);
        }

        bool boolVal = false;
        if (ExtractBoolAfterKey(json, "\"showGraph\"", boolVal) ||
            ExtractBoolAfterKey(json, "\"enabled\"", boolVal)) {
            data_.showGraph = boolVal;
        }
        if (ExtractBoolAfterKey(json, "\"hideWhenIdle\"", boolVal)) {
            data_.hideWhenIdle = boolVal;
        }

        std::string strVal;
        if (ExtractStringAfterKey(json, "\"graphColorHex\"", strVal) ||
            ExtractStringAfterKey(json, "\"strokeColorHex\"", strVal)) {
            data_.graphColorHex = ParseHexColorString(strVal, data_.graphColorHex);
        }

        if (ExtractStringAfterKey(json, "\"fontFamily\"", strVal) && !strVal.empty()) {
            std::memset(data_.fontFamily, 0, sizeof(data_.fontFamily));
            ::MultiByteToWideChar(
                CP_UTF8, 0, strVal.c_str(), static_cast<int>(strVal.size()),
                data_.fontFamily, static_cast<int>(std::size(data_.fontFamily) - 1)
            );
        }
        return true;
    }

    void ParseMetricEntries(std::string_view json) noexcept {
        const size_t metricsPos = json.find("\"metrics\"");
        if (metricsPos == std::string_view::npos) {
            return;
        }
        const size_t arrayStart = json.find('[', metricsPos);
        const size_t arrayEnd   = json.find(']', arrayStart);
        if (arrayStart == std::string_view::npos || arrayEnd == std::string_view::npos) {
            return;
        }

        std::string_view body = json.substr(arrayStart + 1, arrayEnd - arrayStart - 1);
        size_t pos = 0;
        while (pos < body.size()) {
            const size_t objStart = body.find('{', pos);
            if (objStart == std::string_view::npos) {
                break;
            }
            const size_t objEnd = body.find('}', objStart);
            if (objEnd == std::string_view::npos) {
                break;
            }

            std::string_view obj = body.substr(objStart, objEnd - objStart + 1);
            std::string idStr;
            if (ExtractStringAfterKey(obj, "\"id\"", idStr) && !idStr.empty()) {
                for (uint32_t i = 0; i < data_.styleCount; ++i) {
                    if (idStr == data_.styles[i].id) {
                        std::string labelStr;
                        if (ExtractStringAfterKey(obj, "\"label\"", labelStr) && !labelStr.empty()) {
                            zetta::ipc::CopyBoundedString(data_.styles[i].label, labelStr.c_str());
                        }
                        std::string colorStr;
                        if (ExtractStringAfterKey(obj, "\"colorHex\"", colorStr)) {
                            data_.styles[i].colorHex = ParseHexColorString(colorStr, data_.styles[i].colorHex);
                        }
                        bool vis = true;
                        if (ExtractBoolAfterKey(obj, "\"isVisible\"", vis)) {
                            data_.styles[i].isVisible = vis;
                        }
                        break;
                    }
                }
            }

            pos = objEnd + 1;
        }
    }

    LayoutConfigData data_{};
    std::wstring     configFilePath_{};
    FILETIME         lastWriteTime_{};
    DWORD            lastFileSize_{0};
};

} // namespace zetta::overlay
