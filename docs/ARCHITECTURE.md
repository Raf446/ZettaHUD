# ZettaHUD v1.0.0 Architecture & IPC Specification

ZettaHUD is a zero-injection hardware telemetry daemon and dynamic viewport HUD for Windows 10/11 x64. Unlike DLL-hooking overlays that inject into target swapchains and trigger anti-cheat heuristics, ZettaHUD operates entirely out-of-process using kernel/OS event hooks, ETW (`DxgkKrnl`), and zero-copy shared memory IPC.

## 1. Repository Layout

```text
ZettaHUD/
├── CMakeLists.txt                        # [PHASE 1] Root CMake configuration (C++20, x64, Unicode)
├── .gitignore                            # [PHASE 1] Ignore build/, .vs/, out/, *.user
├── resources/
│   ├── app.manifest                      # [PHASE 1] PerMonitorV2 DPI Awareness
│   └── presets/
│       └── default_layout.json           # [PHASE 1] Standard v1.0.0 layout schema
├── include/
│   ├── ipc/
│   │   ├── ZettaSharedState.h            # [PHASE 1] 1-byte packed deterministic IPC structs
│   │   └── ZettaSharedMemoryManager.hpp  # [PHASE 1] RAII wrapper for CreateFileMappingW
│   ├── tracker/
│   │   └── ZettaWindowTracker.hpp        # [PHASE 1] WinEventHook & DWM window tracker
│   ├── telemetry/                        # [PHASE 2] Hardware metrics collectors
│   ├── etw/                              # [PHASE 3] Passive DxgkKrnl Present analyzer
│   └── overlay/                          # [PHASE 4] Direct2D transparent HUD renderer
├── src/
│   ├── daemon/
│   │   └── main_phase1_test.cpp          # [PHASE 1] Diagnostic CLI test harness
│   ├── overlay/                          # [PHASE 4] Overlay implementation
│   └── studio/                           # [PHASE 5] ZettaHUD Studio CAD UI frontend
└── docs/
    └── ARCHITECTURE.md                   # [PHASE 1] IPC memory layout & build guidelines
```

## 2. Deterministic Shared Memory IPC (`Local\ZettaTelemetry_SharedMem_v1`)

All IPC structures in `include/ipc/ZettaSharedState.h` are compiled under `#pragma pack(push, 1)` and verified at compile time with `static_assert` for standard layout, trivial copyability, and exact byte offsets.

### `ZettaMetricEntry` (93 Bytes)

| Offset | Field | Type | Size (Bytes) | Description |
| :--- | :--- | :--- | :--- | :--- |
| `0x00` | `id` | `char[32]` | 32 | Null-terminated metric identifier (e.g., `"fps.avg"`) |
| `0x20` | `label` | `char[16]` | 16 | Display label (e.g., `"FPS"`) |
| `0x30` | `numericValue` | `double` | 8 | Raw numeric sensor sample |
| `0x38` | `formatted` | `char[32]` | 32 | Pre-formatted display string |
| `0x58` | `colorHex` | `uint32_t` | 4 | ARGB color (`0xAARRGGBB`) |
| `0x5C` | `isVisible` | `bool` | 1 | Visibility toggle |

### `ZettaFrameHistory` (492 Bytes)

| Offset | Field | Type | Size (Bytes) | Description |
| :--- | :--- | :--- | :--- | :--- |
| `0x000` | `samples` | `float[120]` | 480 | Ring buffer of recent frametimes in milliseconds |
| `0x1E0` | `headIndex` | `uint32_t` | 4 | Write cursor `[0, 119]` |
| `0x1E4` | `avgFrametimeMs` | `float` | 4 | Rolling average frametime (ms) |
| `0x1E8` | `onePercentLowFps` | `float` | 4 | Computed 1% Low FPS |

### `ZettaSharedState` (5,063 Bytes)

| Offset | Field | Type | Size (Bytes) | Description |
| :--- | :--- | :--- | :--- | :--- |
| `0x0000` | `magicHeader` | `uint32_t` | 4 | Constant `0x5A455454` (`'ZETT'`) |
| `0x0004` | `version` | `uint32_t` | 4 | Schema version (`100`) |
| `0x0008` | `timestampMs` | `uint64_t` | 8 | Monotonic steady-clock timestamp (ms) |
| `0x0010` | `targetPid` | `uint32_t` | 4 | Active target process ID (`0` when idle) |
| `0x0014` | `targetProcessName` | `char[64]` | 64 | UTF-8 executable name |
| `0x0054` | `isGameProcess` | `bool` | 1 | `true` when tracking a non-filtered application |
| `0x0055` | `isElevated` | `bool` | 1 | `true` if target token has Administrator elevation |
| `0x0056` | `isFullscreen` | `bool` | 1 | `true` if visible bounds cover the active monitor |
| `0x0057` | `windowX` | `int32_t` | 4 | Left coordinate of visible frame (physical px) |
| `0x005B` | `windowY` | `int32_t` | 4 | Top coordinate of visible frame (physical px) |
| `0x005F` | `windowWidth` | `int32_t` | 4 | Width of visible frame (physical px) |
| `0x0063` | `windowHeight` | `int32_t` | 4 | Height of visible frame (physical px) |
| `0x0067` | `metricCount` | `uint32_t` | 4 | Number of populated entries in `metrics` |
| `0x006B` | `metrics` | `ZettaMetricEntry[48]` | 4,464 | Fixed-capacity telemetry slot array |
| `0x11DB` | `frameGraph` | `ZettaFrameHistory` | 492 | Frametime ring buffer and percentiles |

## 3. Zero-Polling Window & Viewport Tracking

`ZettaWindowTracker` avoids timer polling (`SetTimer` / `Sleep` loops) entirely:
1. **Event-Driven Hook**: Installs out-of-context hooks (`WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS`) for `EVENT_SYSTEM_FOREGROUND`, `EVENT_SYSTEM_MINIMIZESTART..END`, and `EVENT_OBJECT_LOCATIONCHANGE`.
2. **Early Event Filtering**: `EVENT_OBJECT_LOCATIONCHANGE` notifications from cursor movement (`OBJID_CURSOR`), carets, or child controls (`idObject != OBJID_WINDOW || idChild != CHILDID_SELF`) are discarded immediately in `O(1)` before making any Win32 API calls.
3. **True Visible Geometry**: Queries `DwmGetWindowAttribute(..., DWMWA_EXTENDED_FRAME_BOUNDS, ...)` to strip invisible Windows 11 drop-shadow and resize borders, falling back to `GetWindowRect` only if DWM composition is unavailable.
4. **Per-Monitor V2 DPI Awareness**: Both the embedded `resources/app.manifest` and runtime `SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)` guarantee physical pixel coordinates across heterogeneous 100%/125%/150%/200% displays.

## 4. Build & Verification Instructions

### CMake Build (Visual Studio / Ninja)
```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
.\build\Release\zetta_phase1_test.exe
```

### Direct LLVM-MinGW / Clang++ Build (When CMake is not on PATH)
```powershell
clang++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror `
  -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 `
  -Iinclude src/daemon/main_phase1_test.cpp `
  -luser32 -lpsapi -ladvapi32 -ldwmapi -lshcore `
  -o build/zetta_phase1_test.exe
```
