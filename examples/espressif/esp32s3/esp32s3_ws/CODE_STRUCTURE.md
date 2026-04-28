# Code Structure and Runtime Behavior

This document describes the main directories, module boundaries, and post-boot execution path of this repository: ESP32 voice-interaction firmware derived from [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32). It is intended to help reading and further development.

---

## 1. Project and Build

| Item | Description |
|------|-------------|
| Build system | ESP-IDF CMake (root `CMakeLists.txt`) |
| Project name | `xiaorong` (`project(xiaorong)`) |
| Application component | `main/` (`main/CMakeLists.txt` lists all sources that participate in the build) |
| Board selection | `idf.py menuconfig` → board-related `CONFIG_BOARD_TYPE_*` selects which `boards/<name>/` implementation is compiled in |
| Product credentials | **Xiaozhi Assistant** and related menus in `menuconfig`: `CONFIG_CONNECTION_CONFIG_KEY`, `CONFIG_CONNECTION_CONFIG_TOKEN`, etc., used by `TwilioProtocol` to reach the backend |

The root also contains `scripts/` (utility scripts), `partitions/` (partition tables), and other supporting assets decoupled from core application logic.

---

## 2. Top-Level Directory (Logical View)

```
esp32s3_ws/
├── CMakeLists.txt          # IDF project entry
├── sdkconfig / sdkconfig.* # Kconfig-generated settings
├── main/
│   ├── main.cc             # app_main: NVS, event loop, starts Application
│   ├── application.{h,cc}  # App orchestration: state machine, main event loop, protocol & audio callbacks
│   ├── protocols/          # Transport abstraction and Twilio-style WebSocket implementation
│   ├── audio/              # Codecs, AFE, wake word, AudioService pipeline
│   ├── display/            # Display abstraction, LVGL, LCD/OLED, etc.
│   ├── led/                # LED indication
│   ├── boards/             # Per-hardware boards: Board subclass + config.h + DECLARE_BOARD
│   ├── boards/common/      # board.h, network/backlight and other shared interfaces
│   ├── ota.{h,cc}          # Version check, activation, firmware upgrade
│   ├── settings.{h,cc}     # NVS key-value configuration
│   ├── mcp_server.{h,cc}   # MCP messages and tool registration
│   ├── assets.*            # Asset partition and download
│   ├── device_state*.h/cc  # Device state enum and event dispatch
│   └── Kconfig.projbuild   # This component’s Kconfig options
└── scripts/                # Python and other helpers (not required at firmware runtime)
```

---

## 3. Startup and Main Thread Model

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
8. **OTA / activation**: `Ota` + `CheckNewVersion()` (version check, upgrade, activation flow); completion sets `MAIN_EVENT_CHECK_NEW_VERSION_DONE` (used inside that flow).
9. **MCP**: `McpServer::GetInstance().AddCommonTools()` / `AddUserOnlyTools()`.
10. **Protocol**: construct `TwilioProtocol(CONFIG_CONNECTION_CONFIG_KEY, CONFIG_CONNECTION_CONFIG_TOKEN)`, register `OnConnected` / `OnNetworkError` / `OnIncomingAudio` / `OnAudioChannelOpened` / `OnAudioChannelClosed` / `OnIncomingJson`, etc.
11. **`protocol_->Start()`**: today `TwilioProtocol::Start()` only returns success; **the WebSocket is actually opened in `OpenAudioChannel()`**.
12. Enter `kDeviceStateIdle`, play ready chime, etc.

Other tasks or ISRs must not hold the protocol or UI for long; to change state, use `Application::Schedule()` to post a closure onto the main event loop.

---

## 4. Main Event Loop (`Application::MainEventLoop`)

Waits on an `EventGroup` for the bits defined in `application.h`:

| Event bit | Typical meaning |
|-----------|-----------------|
| `MAIN_EVENT_SCHEDULE` | Run `std::function` tasks from `main_tasks_` (thread-safe posting) |
| `MAIN_EVENT_SEND_AUDIO` | Dequeue from `AudioService` send queue, `protocol_->SendAudio` |
| `MAIN_EVENT_WAKE_WORD_DETECTED` | `OnWakeWordDetected()`: open channel, send wake / start listening, etc. |
| `MAIN_EVENT_VAD_CHANGE` | In listening state, update LED, etc. |
| `MAIN_EVENT_CLOCK_TICK` | Status bar, `SystemInfo::PrintHeapStats` (every 10 s) |
| `MAIN_EVENT_ERROR` | Network error UI, fall back to idle, etc. |

`MAIN_EVENT_CHECK_NEW_VERSION_DONE` and others coordinate with `xEventGroupWaitBits` inside the version-check path (see `CheckNewVersion` call chain).

---

## 5. Device State (`device_state.h`)

Main states include: `Starting`, `WifiConfiguring`, `Idle`, `Connecting`, `Listening`, `Speaking`, `Upgrading`, `Activating`, `AudioTesting`, `FatalError`, etc.

`SetDeviceState()`:

- Logs the transition;
- Calls `DeviceStateEventManager::PostStateChangeEvent`;
- Updates display text/emojis and LED;
- Per state, toggles `AudioService` wake word, voice processing, decoder reset, etc.

**AEC mode** (`AecMode` in `application.h`) is derived from mutually exclusive Kconfig: `CONFIG_USE_DEVICE_AEC` / `CONFIG_USE_SERVER_AEC` / neither, affecting listening mode (e.g. whether `kListeningModeRealtime` is used).

---

## 6. Audio Pipeline (`audio/audio_service.{h,cc}`)

Design summary (from header comments on data flow):

- **Uplink**: MIC → processor (AFE / debug, etc.) → send queue → **Protocol::SendAudio** (payload shape is protocol-specific; Twilio path wraps into media events).
- **Downlink**: Server audio packets → audio queue → play queue → speaker.

`Application` coordinates with `AudioService` in `OnWakeWordDetected`, `StartListening` / `StopListening`, `SetDeviceState`, etc., to switch among idle wake ↔ connect ↔ listen ↔ speak.

---

## 7. Protocol Layer

### 7.1 Abstract `Protocol` (`protocols/protocol.h`)

- Defines **audio packet** `AudioStreamPacket`, binary frame descriptors, **listening mode**, **abort-speaking reason**, etc.
- Callbacks: `OnIncomingAudio`, `OnIncomingJson`, channel open/close, network errors, connect/disconnect.
- Default text signaling: `SendStartListening` / `SendStopListening` / `SendAbortSpeaking` / `SendWakeWordDetected` / `SendMcpMessage`, etc.; builds JSON then uses pure virtual `SendText()`.
- Subclasses implement `Start`, `OpenAudioChannel`, `CloseAudioChannel`, `SendAudio`, `SendText`, etc.

### 7.2 `TwilioProtocol` (`protocols/twilio_protocol.{h,cc}`)

- Uses `EspNetwork` + `WebSocket` toward **`TWILIO_WEBSOCKET_URL`** (macro in source, often intranet or gateway); query carries `robotKey`, `robotToken` (from menuconfig).
- `OpenAudioChannel()`: after connect, sends **Twilio-style `event: start`** (with `mediaFormat`: PCM16, sample rate, etc.), then enters bidirectional media.
- **Uplink**: `SendAudio` Base64-encodes PCM payload and sends **`event: media`** JSON; very small packets may be dropped (length check in implementation).
- **Downlink**: parse WebSocket text JSON; when `event == "media"`, `ParseTwilioMedia` decodes and triggers `OnIncomingAudio`, eventually `AudioService::PushPacketToPcmQueue`.
- `SendText` parses the JSON `event` field before sending; when mixed with base-class “Xiaozhi-style” listen/abort JSON, backend compatibility must be agreed.

**Note**: Large blocks in `OnIncomingJson` for `tts` / `stt` / `llm` / `mcp` are **commented out** in current sources; to drive expressions and subtitles from server JSON, restore or rewrite against the Twilio event model.

---

## 8. Board Abstraction (`boards/common/board.h`)

- `create_board()` is injected by `DECLARE_BOARD(XXX)` in each board directory and returns a singleton `Board` subclass.
- Subclasses must implement: `GetAudioCodec()`, `GetNetwork()`, `StartNetwork()`, `SetPowerSaveMode()`, `GetBoardJson()`, etc.; optional `GetDisplay()`, `GetLed()`, `GetCamera()`, etc.
- Display, audio, network, and power-saving policy are decoupled from `Application` via `Board::GetInstance()`, so one application codebase can support multiple boards.

`main/CMakeLists.txt` **adds the selected board’s sources and dependencies** from `CONFIG_BOARD_TYPE_*` and sets font/emoji macros.

---

## 9. Other Modules (Short Table)

| Module | Role |
|--------|------|
| `Ota` | HTTP version check, firmware download, activation, mark current version valid |
| `Settings` | NVS key-value (Wi-Fi, assets URL, etc.) |
| `McpServer` | Register tools, parse MCP payloads from server; `Application::SendMcpMessage` goes uplink via protocol |
| `Assets` | Validate asset partition, download by URL, apply to display/language packs |
| `DeviceStateEventManager` | Observer for state changes |
| `SystemInfo` | Version, heap, tasks |
| `debug/ws_log_pcm_sink` | Optional: PCM out over WebSocket for debug |

---

## 10. Suggested Reading Order

1. `main/main.cc` → `application.h` → `Application::Start` / `MainEventLoop` / `SetDeviceState` / `OnWakeWordDetected`
2. `protocols/protocol.h` → `twilio_protocol.cc` (`OpenAudioChannel`, `SendTwilioMedia`, `ParseTwilioMedia`)
3. `audio/audio_service.h` (data-flow comments) → interaction sites with `Application`
4. Your board: `main/boards/<your-board>/` and `boards/common/board.h`
5. `ota.cc`, `settings.cc` (provisioning, upgrade, activation, NVS keys)

---

## 11. Relation to README

README **Key / Token**, build/flash, and provisioning flow map to **menuconfig credentials** here, `Board::StartNetwork()`, and Wi-Fi provisioning AP behavior; business backend URL and protocol details follow **`TWILIO_WEBSOCKET_URL` and query parameters** in `twilio_protocol.cc`—keep them aligned with the server deployment.

---
