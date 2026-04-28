# Code Structure and Runtime Behavior

This document describes the main directories, module boundaries, and post-boot execution path of the **esp32s3_webrtc** repository (ESP32 voice-interaction firmware derived from [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)), to aid reading and further development. Compared to upstream, this repo uses an **RTC** protocol path: `RtcProtocol` in `main/protocols` joins a session and pushes media via **`components/agora_iot_sdk`** (Agora IoT RTC prebuilt libraries), not the Twilio WebSocket example.

---

## 1. Project and Build

| Item | Description |
|------|-------------|
| Build system | ESP-IDF CMake (root `CMakeLists.txt`) |
| Project name | `xiaorong` (`project(xiaorong)`) |
| Application component | `main/` (`main/CMakeLists.txt` lists all sources that participate in the build) |
| Board selection | `idf.py menuconfig` → board-related `CONFIG_BOARD_TYPE_*` selects which `boards/<name>/` implementation is compiled in |
| Product credentials | **Xiaozhi Assistant** (and similar) menus in `menuconfig`: `CONFIG_CONNECTION_CONFIG_KEY`, `CONFIG_CONNECTION_CONFIG_TOKEN`, etc., used by `RtcProtocol` for auth and sessions |

The repo root also has `scripts/` (utility scripts), `partitions/` (partition tables), and other supporting assets decoupled from core app logic.

---

## 2. Top-Level Layout (Logical View)

```
esp32s3_webrtc/
├── CMakeLists.txt          # IDF project entry
├── sdkconfig / sdkconfig.* # Kconfig-generated settings
├── components/
│   └── agora_iot_sdk/      # Agora IoT RTC: agora_rtc_api.h and prebuilt libs (e.g. librtsa / libahpl)
├── main/
│   ├── main.cc             # app_main: NVS, event loop, starts Application
│   ├── protocols/          # Protocol abstraction and RtcProtocol: protocol.{h,cc}, rtc_protocol.{h,cc}
│   ├── application.{h,cc}  # App orchestration: state machine, main event loop, protocol and audio callbacks
│   ├── audio/              # Codecs, AFE, wake word, AudioService pipeline
│   ├── display/            # Display abstraction, LVGL, LCD/OLED, etc.
│   ├── led/                # LED indication
│   ├── boards/             # Per-hardware boards: Board subclass + config.h + DECLARE_BOARD
│   ├── boards/common/      # board.h, shared network/backlight APIs
│   ├── ota.{h,cc}          # Version check, activation, firmware upgrade
│   ├── settings.{h,cc}     # NVS key-value config
│   ├── mcp_server.{h,cc}   # MCP messages and tool registration
│   ├── assets.*            # Asset partition and download
│   ├── device_state.h      # Device state enum
│   ├── device_state_event.{h,cc}  # State-change event dispatch
│   └── Kconfig.projbuild   # This component’s Kconfig options
└── scripts/                # Python and other helpers (not required at firmware runtime)
```

---

## 3. Boot and Main Thread Model

### 3.1 `app_main` (`main/main.cc`)

1. `esp_event_loop_create_default()`
2. `nvs_flash_init()` (erase and retry if needed)
3. `Application::GetInstance().Start()`

### 3.2 `Application::Start()` (`main/application.cc`) — overview

1. **Board and display**: `Board::GetInstance()`, refresh system info on the display.
2. **Audio**: `board.GetAudioCodec()` → `audio_service_.Initialize` / `Start`, register callbacks (send queue ready, wake word, VAD).
3. **Main event-loop task**: `xTaskCreate` runs `MainEventLoop()` (FreeRTOS task separate from `app_main`).
4. **Timer**: 1 s `esp_timer` sets `MAIN_EVENT_CLOCK_TICK` (status bar refresh, periodic heap stats, etc.).
5. **Network**: `board.StartNetwork()` (Wi-Fi / 4G specifics live in each Board).
6. **Optional debug**: when `CONFIG_USE_WS_DEBUG_SINK`, start WebSocket PCM debug sink.
7. **Assets**: `CheckAssetsVersion()` (may fetch assets from `download_url` in NVS and `Apply`).
8. **OTA / activation**: `Ota` + `CheckNewVersion()` (version check, upgrade, activation flow); sets `MAIN_EVENT_CHECK_NEW_VERSION_DONE` where used in the flow.
9. **MCP**: `McpServer::GetInstance().AddCommonTools()` / `AddUserOnlyTools()`.
10. **Protocol**: construct `RtcProtocol(CONFIG_CONNECTION_CONFIG_KEY, CONFIG_CONNECTION_CONFIG_TOKEN)`, register `OnConnected` / `OnNetworkError` / `OnIncomingAudio` / `OnAudioChannelOpened` / `OnAudioChannelClosed` / `OnIncomingJson`, etc.
11. **`protocol_->Start()`**: join session, open media channel, etc. — details are defined by **`RtcProtocol`** (`main/protocols/rtc_protocol.cc`, Agora RTC underneath); the app typically cooperates with the channel via `OpenAudioChannel()` in wake/listen flows.
12. Enter `kDeviceStateIdle`, play ready chime, etc.

Other tasks or ISRs should not hold the protocol or UI for long; to change state, post work with `Application::Schedule()` so it runs on the main event loop.

---

## 4. Main Event Loop (`Application::MainEventLoop`)

Waits on an `EventGroup` for these bits (see `application.h`):

| Event bit | Typical meaning |
|-----------|-----------------|
| `MAIN_EVENT_SCHEDULE` | Run `std::function` tasks from `main_tasks_` (thread-safe posting) |
| `MAIN_EVENT_SEND_AUDIO` | Dequeue from `AudioService` send queue, `protocol_->SendAudio` |
| `MAIN_EVENT_WAKE_WORD_DETECTED` | `OnWakeWordDetected()`: open channel, send wake / start listening, etc. |
| `MAIN_EVENT_VAD_CHANGE` | Update LED while in listening state |
| `MAIN_EVENT_CLOCK_TICK` | Status bar, `SystemInfo::PrintHeapStats` (every 10 s) |
| `MAIN_EVENT_ERROR` | Network error UI, fall back to idle, etc. |

`MAIN_EVENT_CHECK_NEW_VERSION_DONE` is set at the end of `CheckNewVersion` via `xEventGroupSetBits` (the main loop does not currently wait on this bit; extend if you need synchronization).

---

## 5. Device State (`device_state.h`)

Main states include: `Starting`, `WifiConfiguring`, `Idle`, `Connecting`, `Listening`, `Speaking`, `Upgrading`, `Activating`, `AudioTesting`, `FatalError`, etc.

`SetDeviceState()`:

- Logs the transition;
- Calls `DeviceStateEventManager::PostStateChangeEvent`;
- Updates display text/emojis and LED;
- Per state, toggles `AudioService` wake word, voice processing, decoder reset, etc.

**AEC mode** (`AecMode` in `application.h`) is derived from mutually exclusive Kconfig options: `CONFIG_USE_DEVICE_AEC` / `CONFIG_USE_SERVER_AEC` / neither, affecting listening mode (e.g. whether `kListeningModeRealtime` is used).

---

## 6. Audio Pipeline (`audio/audio_service.{h,cc}`)

Design summary (from header data-flow comments):

- **Uplink**: MIC → processor (AFE / debug, etc.) → encode queue → **Opus encode** → send queue → **Protocol::SendAudio** (actual payload shaped by `RtcProtocol` / Agora RTC).
- **Downlink**: Server audio packets → decode queue → **Opus decode** → play queue → speaker.

Uses **two tasks**: one for capture/playback/processing, one for Opus encode/decode, to balance memory and real-time behavior.

`Application` coordinates with `AudioService` in `OnWakeWordDetected`, `StartListening` / `StopListening`, `SetDeviceState`, etc., to switch among idle+wake, connect, listen, and speak.

---

## 7. Protocol Layer

### 7.1 Abstract `Protocol` (`main/protocols/protocol.h`)

- Defines **audio packets** `AudioStreamPacket`, binary frame descriptors, **listening modes**, **abort-speaking reasons**, etc.
- Callbacks: `OnIncomingAudio`, `OnIncomingJson`, channel open/close, network errors, connect/disconnect.
- Default text signaling: `SendStartListening` / `SendStopListening` / `SendAbortSpeaking` / `SendWakeWordDetected` / `SendMcpMessage`, etc. — build JSON then call pure virtual `SendText()`.
- Subclasses implement `Start`, `OpenAudioChannel`, `CloseAudioChannel`, `SendAudio`, `SendText`, etc.

### 7.2 `RtcProtocol` (`main/protocols/rtc_protocol.{h,cc}`)

- Subclasses `Protocol`; constructor `robot_key` / `robot_token` map to menuconfig **`CONFIG_CONNECTION_CONFIG_KEY` / `CONFIG_CONNECTION_CONFIG_TOKEN`**.
- Header exposes RTC-facing APIs: `SendVideoFrame`, `HandleStreamMessage`, `DeliverIncomingAudio`, etc.; bandwidth-estimation macros are in `rtc_protocol.h`. Before join, private helpers fetch voice session parameters (HTTP to backend — see `rtc_protocol.cc`).
- **Implementation lives in** `rtc_protocol.cc`, calling prebuilt libs in **`components/agora_iot_sdk`** via `agora_rtc_api.h` (e.g. `librtsa.a`, `libahpl.a`); compare with Agora IoT SDK and your backend contract for media/data details.
- Downlink audio via `OnIncomingAudio` typically reaches **`AudioService::PushPacketToDecodeQueue`** in this project (see `application.cc`).

**Note**: `OnIncomingJson` in `Application` is currently a **no-op**; for server-driven emoji/subtitles from JSON, add logic here per your RTC data contract.

---

## 8. Board Abstraction (`boards/common/board.h`)

- `create_board()` is injected by `DECLARE_BOARD(XXX)` in each board directory and returns a singleton `Board` subclass.
- Subclasses must implement: `GetAudioCodec()`, `GetNetwork()`, `StartNetwork()`, `SetPowerSaveMode()`, `GetBoardJson()`, etc.; optional `GetDisplay()`, `GetLed()`, `GetCamera()`, etc.
- Display, audio, network, and power-saving are accessed through `Board::GetInstance()`, decoupling `Application` from hardware so one app supports many boards.

`main/CMakeLists.txt` **adds the selected board’s sources and dependencies** from `CONFIG_BOARD_TYPE_*`, and sets font/emoji macros.

---

## 9. Other Modules (Short Table)

| Module | Role |
|--------|------|
| `Ota` | HTTP version check, firmware download, activation, mark current version valid |
| `Settings` | NVS key-value (Wi-Fi, assets URL, etc.) |
| `McpServer` | Register tools, parse server MCP payloads; `Application::SendMcpMessage` goes uplink via protocol |
| `Assets` | Asset partition verify, download by URL, apply to display/language packs |
| `DeviceStateEventManager` | Observer for state changes |
| `SystemInfo` | Version, heap, tasks, etc. |
| `debug/ws_log_pcm_sink` | Optional: PCM out over WebSocket for debug |

---

## 10. Suggested Reading Order

1. `main/main.cc` → `application.h` → `Application::Start` / `MainEventLoop` / `SetDeviceState` / `OnWakeWordDetected`
2. `main/protocols/protocol.h` → `rtc_protocol.{h,cc}` (cross-check `components/agora_iot_sdk` and backend docs as needed)
3. `audio/audio_service.h` (data-flow comments) → interaction points with `Application`
4. Your board: `main/boards/<your-board>/` and `boards/common/board.h`
5. `ota.cc`, `settings.cc` (provisioning, upgrade, activation, NVS keys)

---

## 11. Relationship to README

README sections on **Key / Token**, build/flash, and provisioning correspond to **menuconfig credentials** here, `Board::StartNetwork()`, and Wi-Fi provisioning hotspot behavior; voice session, RTC backend URL, signaling, and media formats follow **`RtcProtocol` + Agora IoT SDK and server contract** — keep deployment aligned with the backend.

---
