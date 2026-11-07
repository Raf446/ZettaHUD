#pragma once

#include "ipc/ZettaSharedState.h"
#include "overlay/ZettaLayoutConfig.hpp"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace zetta::overlay {

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

class ZettaD2DRenderer {
public:
    ZettaD2DRenderer() noexcept = default;

    ~ZettaD2DRenderer() noexcept {
        Shutdown();
    }

    ZettaD2DRenderer(const ZettaD2DRenderer&) = delete;
    ZettaD2DRenderer& operator=(const ZettaD2DRenderer&) = delete;

    [[nodiscard]] bool Initialize(HWND hwnd, const LayoutConfigData& config) noexcept {
        hwnd_ = hwnd;

        const HRESULT d2dHr = ::D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory),
            nullptr,
            d2dFactory_.VoidAddressOf()
        );
        if (FAILED(d2dHr)) {
            return false;
        }

        const HRESULT dwHr = ::DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwriteFactory_.AddressOf())
        );
        if (FAILED(dwHr)) {
            return false;
        }

        if (!RebuildTextFormats(config)) {
            return false;
        }

        return EnsureDeviceResources(256, 320);
    }

    void Shutdown() noexcept {
        DiscardDeviceResources();
        headerFormat_.Reset();
        labelFormat_.Reset();
        valueFormat_.Reset();
        inlineValueFormat_.Reset();
        dwriteFactory_.Reset();
        d2dFactory_.Reset();
        hwnd_ = nullptr;
    }

    bool RebuildTextFormats(const LayoutConfigData& config) noexcept {
        if (!dwriteFactory_.IsValid()) {
            return false;
        }

        const wchar_t* resolvedFont = ResolveInstalledFont(config.fontFamily);
        const float baseSize        = std::clamp(config.fontSize, 8.0f, 28.0f);
        const float headerSize      = std::max(8.0f, baseSize - 1.5f);

        headerFormat_.Reset();
        labelFormat_.Reset();
        valueFormat_.Reset();
        inlineValueFormat_.Reset();

        if (FAILED(dwriteFactory_->CreateTextFormat(
                resolvedFont,
                nullptr,
                DWRITE_FONT_WEIGHT_BOLD,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                headerSize,
                L"en-us",
                headerFormat_.AddressOf()))) {
            return false;
        }

        if (FAILED(dwriteFactory_->CreateTextFormat(
                resolvedFont,
                nullptr,
                DWRITE_FONT_WEIGHT_BOLD,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                baseSize,
                L"en-us",
                labelFormat_.AddressOf()))) {
            return false;
        }

        if (FAILED(dwriteFactory_->CreateTextFormat(
                resolvedFont,
                nullptr,
                DWRITE_FONT_WEIGHT_MEDIUM,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                baseSize,
                L"en-us",
                valueFormat_.AddressOf()))) {
            return false;
        }

        if (FAILED(dwriteFactory_->CreateTextFormat(
                resolvedFont,
                nullptr,
                DWRITE_FONT_WEIGHT_SEMI_BOLD,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                baseSize,
                L"en-us",
                inlineValueFormat_.AddressOf()))) {
            return false;
        }

        headerFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        labelFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        valueFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        inlineValueFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

        headerFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        labelFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        valueFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        valueFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        inlineValueFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        inlineValueFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

        return true;
    }

    void ComputeRequiredCanvasSize(
        const zetta::ipc::ZettaSharedState& state,
        const ZettaLayoutConfig& layoutConfig,
        bool compactMode,
        uint32_t& outWidth,
        uint32_t& outHeight
    ) const noexcept {
        const LayoutConfigData& cfg = layoutConfig.Get();
        const uint32_t visibleRows  = CountVisibleRows(state, layoutConfig, compactMode);

        if (cfg.layoutMode == LayoutMode::NvidiaHorizontal) {
            const float fontScale   = std::clamp(cfg.fontSize, 8.0f, 28.0f);
            const float perSlotW    = fontScale * 12.5f;
            const float inlineWidth = std::clamp(
                (static_cast<float>( std::max(1U, visibleRows) ) * perSlotW) + 36.0f,
                340.0f,
                1680.0f
            );
            const float inlineHeight = std::max(38.0f, fontScale * 2.6f);
            outWidth  = static_cast<uint32_t>(std::ceil(inlineWidth));
            outHeight = static_cast<uint32_t>(std::ceil(inlineHeight));
            return;
        }

        constexpr float kCardWidth     = 248.0f;
        constexpr float kHeaderHeight  = 26.0f;
        constexpr float kRowHeight     = 20.0f;
        constexpr float kPaddingTop    = 10.0f;
        constexpr float kPaddingBottom = 10.0f;

        float totalHeight = kPaddingTop + kHeaderHeight +
                            (static_cast<float>(visibleRows) * kRowHeight) +
                            kPaddingBottom;

        if (cfg.showGraph && !compactMode) {
            totalHeight += cfg.graphHeight + 10.0f;
        }

        outWidth  = static_cast<uint32_t>(std::ceil(kCardWidth));
        outHeight = static_cast<uint32_t>(std::ceil(std::max(64.0f, totalHeight)));
    }

    bool Render(
        const zetta::ipc::ZettaSharedState& state,
        const ZettaLayoutConfig& config
    ) noexcept {
        uint32_t canvasW = 248;
        uint32_t canvasH = 310;
        ComputeRequiredCanvasSize(state, config, false, canvasW, canvasH);
        return RenderFrame(state, config, false, "ETW", canvasW, canvasH);
    }

    bool RenderFrame(
        const zetta::ipc::ZettaSharedState& state,
        const ZettaLayoutConfig& config,
        bool compactMode,
        const char* captureModeBadge,
        uint32_t canvasWidth,
        uint32_t canvasHeight
    ) noexcept {
        const LayoutMode currentMode = config.GetLayoutMode();
        if (currentMode != lastRenderedMode_) {
            DiscardDeviceResources();
            lastRenderedMode_ = currentMode;
        }

        if (!EnsureDeviceResources(canvasWidth, canvasHeight)) {
            return false;
        }

        activeWidthF_       = static_cast<float>(canvasWidth);
        activeHeightF_      = static_cast<float>(canvasHeight);
        activeCompactMode_  = compactMode;
        activeCaptureBadge_ = captureModeBadge;

        renderTarget_->BeginDraw();
        renderTarget_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

        if (config.GetLayoutMode() == LayoutMode::NvidiaHorizontal) {
            RenderNvidiaHorizontalStrip(state, config);
        } else {
            RenderCardVertical(state, config);
        }

        const HRESULT endHr = renderTarget_->EndDraw();
        if (endHr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) {
            DiscardDeviceResources();
            ++recreateCount_;
            return false;
        }

        if (SUCCEEDED(endHr)) {
            ++renderedFrameCount_;
            return true;
        }
        return false;
    }

    [[nodiscard]] uint64_t GetRenderedFrameCount() const noexcept {
        return renderedFrameCount_;
    }

    [[nodiscard]] uint32_t GetDeviceRecreateCount() const noexcept {
        return recreateCount_;
    }

private:
    void RenderNvidiaHorizontalStrip(
        const zetta::ipc::ZettaSharedState& state,
        const ZettaLayoutConfig& layoutConfig
    ) noexcept {
        const float widthF    = activeWidthF_;
        const float heightF   = activeHeightF_;
        const bool compactMode = activeCompactMode_;

        // Ignore bgOpacity and cornerRadius; do not draw any background container or borders.
        valueBrush_->SetColor(ColorFromArgb(0xFFF8FAFCU, 1.0f));
        headerBrush_->SetColor(ColorFromArgb(0xFF94A3B8U, 0.92f));

        const uint32_t count = std::min(state.metricCount, static_cast<uint32_t>(zetta::ipc::kMaxMetrics));
        float cursorX = 8.0f;
        uint32_t renderedItems = 0;

        wchar_t wideBuf[128]{};

        for (uint32_t i = 0; i < count; ++i) {
            const zetta::ipc::ZettaMetricEntry& entry = state.metrics[i];
            if (!entry.isVisible || entry.id[0] == '\0') {
                continue;
            }

            const MetricStyleOverride* style = layoutConfig.FindStyle(entry.id);
            if (style != nullptr && !style->isVisible) {
                continue;
            }

            if (compactMode && renderedItems >= 5U) {
                break;
            }

            if (renderedItems > 0U) {
                cursorX += DrawShadowedInlineSegment(
                    L"  |  ",
                    headerFormat_.Get(),
                    headerBrush_.Get(),
                    cursorX,
                    heightF,
                    widthF
                );
            }

            const uint32_t labelColor = (style != nullptr) ? style->colorHex : entry.colorHex;
            const char* labelText     = (style != nullptr && style->label[0] != '\0')
                                            ? style->label
                                            : entry.label;

            dynamicLabelBrush_->SetColor(ColorFromArgb(labelColor, 1.0f));
            Utf8ToWideBounded(labelText, wideBuf);
            cursorX += DrawShadowedInlineSegment(
                wideBuf,
                labelFormat_.Get(),
                dynamicLabelBrush_.Get(),
                cursorX,
                heightF,
                widthF
            );

            cursorX += 6.0f;

            Utf8ToWideBounded(entry.formatted, wideBuf);
            cursorX += DrawShadowedInlineSegment(
                wideBuf,
                inlineValueFormat_.Get(),
                valueBrush_.Get(),
                cursorX,
                heightF,
                widthF
            );

            ++renderedItems;
            if (cursorX >= widthF - 24.0f) {
                break;
            }
        }

        if (renderedItems == 0U) {
            dynamicLabelBrush_->SetColor(ColorFromArgb(0xFF22C55EU, 1.0f));
            (void)DrawShadowedInlineSegment(
                L"FPS --  |  GPU --%  |  CPU --%  |  LAT -- ms",
                labelFormat_.Get(),
                dynamicLabelBrush_.Get(),
                cursorX,
                heightF,
                widthF
            );
        }
    }

    float DrawShadowedInlineSegment(
        const wchar_t* text,
        IDWriteTextFormat* format,
        ID2D1SolidColorBrush* foregroundBrush,
        float x,
        float heightF,
        float maxRight
    ) noexcept {
        if (text == nullptr || text[0] == L'\0' || format == nullptr || !dwriteFactory_.IsValid()) {
            return 0.0f;
        }

        const UINT32 textLen = static_cast<UINT32>(std::wcslen(text));
        const float availW   = std::max(16.0f, maxRight - x);

        ScopedComPtr<IDWriteTextLayout> textLayout;
        if (FAILED(dwriteFactory_->CreateTextLayout(
                text,
                textLen,
                format,
                availW,
                heightF,
                textLayout.AddressOf()))) {
            return 0.0f;
        }

        DWRITE_TEXT_METRICS metrics{};
        if (FAILED(textLayout->GetMetrics(&metrics))) {
            return 0.0f;
        }

        // Draw 1px offset shadow (#000000 at 0.8 opacity) followed by foreground text.
        shadowBrush_->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.80f));
        renderTarget_->DrawTextLayout(D2D1::Point2F(x + 1.0f, 1.0f), textLayout.Get(), shadowBrush_.Get());
        renderTarget_->DrawTextLayout(D2D1::Point2F(x, 0.0f), textLayout.Get(), foregroundBrush);

        return metrics.widthIncludingTrailingWhitespace;
    }

    void RenderCardVertical(
        const zetta::ipc::ZettaSharedState& state,
        const ZettaLayoutConfig& layoutConfig
    ) noexcept {
        const float widthF             = activeWidthF_;
        const float heightF            = activeHeightF_;
        const bool compactMode         = activeCompactMode_;
        const char* captureModeBadge   = activeCaptureBadge_;
        const LayoutConfigData& cfg    = layoutConfig.Get();

        // 1. Industrial CAD HUD container backdrop (#0A0B0E) and 1px border (#262B3A)
        bgBrush_->SetColor(ColorFromArgb(0xFF0A0B0EU, cfg.bgOpacity));
        borderBrush_->SetColor(ColorFromArgb(0xFF262B3AU, 0.95f));

        const D2D1_ROUNDED_RECT cardRect = D2D1::RoundedRect(
            D2D1::RectF(0.5f, 0.5f, widthF - 0.5f, heightF - 0.5f),
            cfg.cornerRadius,
            cfg.cornerRadius
        );
        renderTarget_->FillRoundedRectangle(cardRect, bgBrush_.Get());
        renderTarget_->DrawRoundedRectangle(cardRect, borderBrush_.Get(), 1.0f);

        // 2. Process & Capture Engine Header Row
        constexpr float kPadX = 12.0f;
        float cursorY = 8.0f;

        char headerText[96]{};
        const char* procName = (state.targetProcessName[0] != '\0') ? state.targetProcessName : "Desktop";
        std::snprintf(
            headerText,
            sizeof(headerText),
            "[%s] %s%s",
            captureModeBadge != nullptr ? captureModeBadge : "HUD",
            procName,
            state.isElevated ? " [ADM]" : ""
        );

        wchar_t wideBuf[128]{};
        Utf8ToWideBounded(headerText, wideBuf);

        headerBrush_->SetColor(ColorFromArgb(0xFF94A3B8U, 1.0f));
        const D2D1_RECT_F headerRect = D2D1::RectF(kPadX, cursorY, widthF - kPadX, cursorY + 20.0f);
        renderTarget_->DrawText(
            wideBuf,
            static_cast<UINT32>(std::wcslen(wideBuf)),
            headerFormat_.Get(),
            headerRect,
            headerBrush_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP
        );

        cursorY += 22.0f;
        graphGridBrush_->SetColor(ColorFromArgb(0xFF1E2430U, 1.0f));
        renderTarget_->DrawLine(
            D2D1::Point2F(kPadX, cursorY),
            D2D1::Point2F(widthF - kPadX, cursorY),
            graphGridBrush_.Get(),
            1.0f
        );
        cursorY += 4.0f;

        // 3. Two-Column Tabular Telemetry Rows
        valueBrush_->SetColor(ColorFromArgb(0xFFF8FAFCU, 1.0f));
        constexpr float kRowHeight = 20.0f;
        const uint32_t count = std::min(state.metricCount, static_cast<uint32_t>(zetta::ipc::kMaxMetrics));

        uint32_t renderedRows = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const zetta::ipc::ZettaMetricEntry& entry = state.metrics[i];
            if (!entry.isVisible || entry.id[0] == '\0') {
                continue;
            }

            const MetricStyleOverride* style = layoutConfig.FindStyle(entry.id);
            if (style != nullptr && !style->isVisible) {
                continue;
            }

            if (compactMode && renderedRows >= 5U) {
                break;
            }

            const uint32_t labelColor = (style != nullptr) ? style->colorHex : entry.colorHex;
            const char* labelText     = (style != nullptr && style->label[0] != '\0')
                                            ? style->label
                                            : entry.label;

            dynamicLabelBrush_->SetColor(ColorFromArgb(labelColor, 1.0f));

            const D2D1_RECT_F rowRect = D2D1::RectF(kPadX, cursorY, widthF - kPadX, cursorY + kRowHeight);

            Utf8ToWideBounded(labelText, wideBuf);
            renderTarget_->DrawText(
                wideBuf,
                static_cast<UINT32>(std::wcslen(wideBuf)),
                labelFormat_.Get(),
                rowRect,
                dynamicLabelBrush_.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP
            );

            Utf8ToWideBounded(entry.formatted, wideBuf);
            renderTarget_->DrawText(
                wideBuf,
                static_cast<UINT32>(std::wcslen(wideBuf)),
                valueFormat_.Get(),
                rowRect,
                valueBrush_.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP
            );

            cursorY += kRowHeight;
            ++renderedRows;
        }

        // 4. Real-Time 120-Sample Frametime Pulse Graph
        if (cfg.showGraph && !compactMode) {
            cursorY += 4.0f;
            const D2D1_RECT_F graphRect = D2D1::RectF(
                kPadX,
                cursorY,
                widthF - kPadX,
                cursorY + cfg.graphHeight
            );
            RenderFrameTimeGraph(state.frameGraph, graphRect, cfg.graphColorHex);
        }
    }

    [[nodiscard]] static D2D1_COLOR_F ColorFromArgb(uint32_t argb, float alphaMultiplier) noexcept {
        const float a = static_cast<float>((argb >> 24U) & 0xFFU) / 255.0f;
        const float r = static_cast<float>((argb >> 16U) & 0xFFU) / 255.0f;
        const float g = static_cast<float>((argb >> 8U) & 0xFFU) / 255.0f;
        const float b = static_cast<float>(argb & 0xFFU) / 255.0f;
        const float finalAlpha = (a > 0.0f ? a : 1.0f) * alphaMultiplier;
        return D2D1::ColorF(r, g, b, std::clamp(finalAlpha, 0.0f, 1.0f));
    }

    static void Utf8ToWideBounded(const char* src, wchar_t (&dest)[128]) noexcept {
        std::memset(dest, 0, sizeof(dest));
        if (src == nullptr || src[0] == '\0') {
            return;
        }
        ::MultiByteToWideChar(
            CP_UTF8,
            0,
            src,
            -1,
            dest,
            static_cast<int>(std::size(dest) - 1)
        );
    }

    [[nodiscard]] const wchar_t* ResolveInstalledFont(const wchar_t* preferredFont) noexcept {
        if (!dwriteFactory_.IsValid() || preferredFont == nullptr || preferredFont[0] == L'\0') {
            return L"Consolas";
        }

        ScopedComPtr<IDWriteFontCollection> fontCollection;
        if (FAILED(dwriteFactory_->GetSystemFontCollection(fontCollection.AddressOf(), FALSE))) {
            return L"Consolas";
        }

        UINT32 familyIndex = 0;
        BOOL exists        = FALSE;
        if (SUCCEEDED(fontCollection->FindFamilyName(preferredFont, &familyIndex, &exists)) && exists) {
            return preferredFont;
        }
        return L"Consolas";
    }

    [[nodiscard]] static uint32_t CountVisibleRows(
        const zetta::ipc::ZettaSharedState& state,
        const ZettaLayoutConfig& layoutConfig,
        bool compactMode
    ) noexcept {
        const uint32_t count = std::min(state.metricCount, static_cast<uint32_t>(zetta::ipc::kMaxMetrics));
        uint32_t visible = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const zetta::ipc::ZettaMetricEntry& entry = state.metrics[i];
            if (!entry.isVisible || entry.id[0] == '\0') {
                continue;
            }
            const MetricStyleOverride* style = layoutConfig.FindStyle(entry.id);
            if (style != nullptr && !style->isVisible) {
                continue;
            }
            ++visible;
        }
        if (visible == 0) {
            visible = 6U;
        }
        return compactMode ? std::min(visible, 5U) : visible;
    }

    void RenderFrameTimeGraph(
        const zetta::ipc::ZettaFrameHistory& fg,
        const D2D1_RECT_F& rect,
        uint32_t normalStrokeHex
    ) noexcept {
        graphBgBrush_->SetColor(ColorFromArgb(0xFF11141DU, 0.90f));
        graphGridBrush_->SetColor(ColorFromArgb(0xFF1E2430U, 1.0f));
        graphLineBrush_->SetColor(ColorFromArgb(normalStrokeHex, 1.0f));
        graphSpikeBrush_->SetColor(ColorFromArgb(0xFFFB7185U, 1.0f));

        renderTarget_->FillRectangle(rect, graphBgBrush_.Get());
        renderTarget_->DrawRectangle(rect, graphGridBrush_.Get(), 1.0f);

        const float graphW = rect.right - rect.left;
        const float graphH = rect.bottom - rect.top;
        if (graphW <= 4.0f || graphH <= 4.0f) {
            return;
        }

        // 16.67 ms (60 FPS) reference line within [0..50 ms] vertical scale
        constexpr float kMaxScaleMs = 50.0f;
        const float refY = rect.bottom - ((16.67f / kMaxScaleMs) * graphH);
        renderTarget_->DrawLine(
            D2D1::Point2F(rect.left + 1.0f, refY),
            D2D1::Point2F(rect.right - 1.0f, refY),
            graphGridBrush_.Get(),
            1.0f
        );

        constexpr size_t kSamples = zetta::ipc::kFrameHistorySize;
        const float stepX = (graphW - 2.0f) / static_cast<float>(kSamples - 1U);

        D2D1_POINT_2F prevPt{};
        bool hasPrev = false;

        for (size_t i = 0; i < kSamples; ++i) {
            const size_t ringIdx = (fg.headIndex + i) % kSamples;
            const float sampleMs = std::clamp(fg.samples[ringIdx], 0.0f, kMaxScaleMs);
            const float x = (rect.left + 1.0f) + (static_cast<float>(i) * stepX);
            const float y = (rect.bottom - 1.0f) - ((sampleMs / kMaxScaleMs) * (graphH - 2.0f));

            const D2D1_POINT_2F currPt = D2D1::Point2F(x, y);
            if (hasPrev) {
                ID2D1SolidColorBrush* strokeBrush = (sampleMs > 25.0f)
                                                        ? graphSpikeBrush_.Get()
                                                        : graphLineBrush_.Get();
                renderTarget_->DrawLine(prevPt, currPt, strokeBrush, 1.4f);
            }
            prevPt  = currPt;
            hasPrev = true;
        }
    }

    bool EnsureDeviceResources(uint32_t width, uint32_t height) noexcept {
        if (!d2dFactory_.IsValid() || hwnd_ == nullptr) {
            return false;
        }

        if (renderTarget_.IsValid()) {
            if (width != targetWidth_ || height != targetHeight_) {
                if (SUCCEEDED(renderTarget_->Resize(D2D1::SizeU(width, height)))) {
                    targetWidth_  = width;
                    targetHeight_ = height;
                }
            }
            return true;
        }

        const D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f,
            96.0f
        );

        const D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps = D2D1::HwndRenderTargetProperties(
            hwnd_,
            D2D1::SizeU(width, height),
            D2D1_PRESENT_OPTIONS_IMMEDIATELY
        );

        if (FAILED(d2dFactory_->CreateHwndRenderTarget(rtProps, hwndProps, renderTarget_.AddressOf()))) {
            return false;
        }

        renderTarget_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

        targetWidth_  = width;
        targetHeight_ = height;

        const D2D1_COLOR_F white = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
        if (FAILED(renderTarget_->CreateSolidColorBrush(white, bgBrush_.AddressOf())) ||
            FAILED(renderTarget_->CreateSolidColorBrush(white, borderBrush_.AddressOf())) ||
            FAILED(renderTarget_->CreateSolidColorBrush(white, headerBrush_.AddressOf())) ||
            FAILED(renderTarget_->CreateSolidColorBrush(white, valueBrush_.AddressOf())) ||
            FAILED(renderTarget_->CreateSolidColorBrush(white, dynamicLabelBrush_.AddressOf())) ||
            FAILED(renderTarget_->CreateSolidColorBrush(white, shadowBrush_.AddressOf())) ||
            FAILED(renderTarget_->CreateSolidColorBrush(white, graphBgBrush_.AddressOf())) ||
            FAILED(renderTarget_->CreateSolidColorBrush(white, graphGridBrush_.AddressOf())) ||
            FAILED(renderTarget_->CreateSolidColorBrush(white, graphLineBrush_.AddressOf())) ||
            FAILED(renderTarget_->CreateSolidColorBrush(white, graphSpikeBrush_.AddressOf()))) {
            DiscardDeviceResources();
            return false;
        }

        return true;
    }

    void DiscardDeviceResources() noexcept {
        graphSpikeBrush_.Reset();
        graphLineBrush_.Reset();
        graphGridBrush_.Reset();
        graphBgBrush_.Reset();
        shadowBrush_.Reset();
        dynamicLabelBrush_.Reset();
        valueBrush_.Reset();
        headerBrush_.Reset();
        borderBrush_.Reset();
        bgBrush_.Reset();
        renderTarget_.Reset();
        targetWidth_  = 0;
        targetHeight_ = 0;
    }

    HWND                                hwnd_{nullptr};
    uint32_t                            targetWidth_{0};
    uint32_t                            targetHeight_{0};
    uint64_t                            renderedFrameCount_{0};
    uint32_t                            recreateCount_{0};
    LayoutMode                          lastRenderedMode_{LayoutMode::CardVertical};
    float                               activeWidthF_{248.0f};
    float                               activeHeightF_{310.0f};
    bool                                activeCompactMode_{false};
    const char*                         activeCaptureBadge_{"ETW"};

    ScopedComPtr<ID2D1Factory>          d2dFactory_{};
    ScopedComPtr<IDWriteFactory>        dwriteFactory_{};
    ScopedComPtr<IDWriteTextFormat>     headerFormat_{};
    ScopedComPtr<IDWriteTextFormat>     labelFormat_{};
    ScopedComPtr<IDWriteTextFormat>     valueFormat_{};
    ScopedComPtr<IDWriteTextFormat>     inlineValueFormat_{};

    ScopedComPtr<ID2D1HwndRenderTarget> renderTarget_{};
    ScopedComPtr<ID2D1SolidColorBrush>  bgBrush_{};
    ScopedComPtr<ID2D1SolidColorBrush>  borderBrush_{};
    ScopedComPtr<ID2D1SolidColorBrush>  headerBrush_{};
    ScopedComPtr<ID2D1SolidColorBrush>  valueBrush_{};
    ScopedComPtr<ID2D1SolidColorBrush>  dynamicLabelBrush_{};
    ScopedComPtr<ID2D1SolidColorBrush>  shadowBrush_{};
    ScopedComPtr<ID2D1SolidColorBrush>  graphBgBrush_{};
    ScopedComPtr<ID2D1SolidColorBrush>  graphGridBrush_{};
    ScopedComPtr<ID2D1SolidColorBrush>  graphLineBrush_{};
    ScopedComPtr<ID2D1SolidColorBrush>  graphSpikeBrush_{};
};

} // namespace zetta::overlay
