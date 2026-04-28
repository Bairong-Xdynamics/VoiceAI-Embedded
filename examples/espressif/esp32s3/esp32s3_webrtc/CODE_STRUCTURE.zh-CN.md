# 代码结构与运行逻辑说明

本文档描述本仓库 **esp32s3_webrtc**（基于 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 二次开发的 ESP32 语音交互固件）的主要目录、模块边界与上电后的执行路径，便于阅读与二次开发。与上游相比，本仓库协议栈为 **RTC 路径**：`main/protocols` 中的 `RtcProtocol` 通过 **`components/agora_iot_sdk`**（Agora IoT RTC 预编译库）入会推流，而非 Twilio WebSocket 示例。

---

## 1. 工程与构建

| 项 | 说明 |
|----|------|
| 构建系统 | ESP-IDF CMake（根目录 `CMakeLists.txt`） |
| 工程名 | `xiaorong`（`project(xiaorong)`） |
| 应用组件 | `main/`（`main/CMakeLists.txt` 列出全部参与编译的源文件） |
| 板型选择 | 通过 `idf.py menuconfig` → 板型相关 `CONFIG_BOARD_TYPE_*` 决定编译进哪套 `boards/<name>/` 实现 |
| 产品凭证 | `menuconfig` 中 **Xiaozhi Assistant** 等菜单：`CONFIG_CONNECTION_CONFIG_KEY`、`CONFIG_CONNECTION_CONFIG_TOKEN` 等，供 `RtcProtocol` 鉴权与会话使用 |

根目录下还有 `scripts/`（工具脚本）、`partitions/`（分区表）等辅助资源，与主应用逻辑解耦。

---

## 2. 顶层目录（逻辑视图）

```
esp32s3_webrtc/
├── CMakeLists.txt          # IDF 工程入口
├── sdkconfig / sdkconfig.* # Kconfig 生成配置
├── components/
│   └── agora_iot_sdk/      # Agora IoT RTC：`agora_rtc_api.h` 与预编译库（如 librtsa / libahpl）
├── main/
│   ├── main.cc             # app_main：NVS、事件循环、启动 Application
│   ├── protocols/          # Protocol 抽象与 RtcProtocol：protocol.{h,cc}、rtc_protocol.{h,cc}
│   ├── application.{h,cc}  # 应用编排：状态机、主事件循环、协议与音频回调
│   ├── audio/              # 编解码器、AFE、唤醒、AudioService 管线
│   ├── display/            # 显示抽象、LVGL、LCD/OLED 等
│   ├── led/                # LED 指示
│   ├── boards/             # 各硬件板：Board 子类 + config.h + DECLARE_BOARD
│   ├── boards/common/    # board.h、网络/背光等公共接口
│   ├── ota.{h,cc}        # 版本检查、激活、固件升级
│   ├── settings.{h,cc}   # NVS 键值配置
│   ├── mcp_server.{h,cc} # MCP 消息与工具注册
│   ├── assets.*            # 资源分区与下载
│   ├── device_state.h      # 设备状态枚举
│   ├── device_state_event.{h,cc}  # 状态变更事件分发
│   └── Kconfig.projbuild   # 本组件 Kconfig 选项
└── scripts/                # Python 等辅助工具（非固件运行时依赖）
```

---

## 3. 启动与主线程模型

### 3.1 `app_main`（`main/main.cc`）

1. `esp_event_loop_create_default()`
2. `nvs_flash_init()`（必要时擦除后重试）
3. `Application::GetInstance().Start()`

### 3.2 `Application::Start()`（`main/application.cc`）概要

1. **板级与显示**：`Board::GetInstance()`，更新显示上的系统信息。
2. **音频**：`board.GetAudioCodec()` → `audio_service_.Initialize` / `Start`，并注册回调（发送队列就绪、唤醒词、VAD）。
3. **主事件循环任务**：`xTaskCreate` 运行 `MainEventLoop()`（与 `app_main` 分离的 FreeRTOS 任务）。
4. **定时器**：周期 1s 的 `esp_timer`，置位 `MAIN_EVENT_CLOCK_TICK`（状态栏刷新、周期性堆信息打印等）。
5. **网络**：`board.StartNetwork()`（具体 Wi-Fi / 4G 等在对应 Board 中实现）。
6. **可选调试**：`CONFIG_USE_WS_DEBUG_SINK` 时启动 WebSocket PCM 调试输出。
7. **资源**：`CheckAssetsVersion()`（可按 NVS 中 `download_url` 拉取资源并 `Apply`）。
8. **OTA / 激活**：`Ota` + `CheckNewVersion()`（版本检查、升级、激活码流程），完成后置位 `MAIN_EVENT_CHECK_NEW_VERSION_DONE`（在流程中使用）。
9. **MCP**：`McpServer::GetInstance().AddCommonTools()` / `AddUserOnlyTools()`。
10. **协议**：构造 `RtcProtocol(CONFIG_CONNECTION_CONFIG_KEY, CONFIG_CONNECTION_CONFIG_TOKEN)`，注册 `OnConnected` / `OnNetworkError` / `OnIncomingAudio` / `OnAudioChannelOpened` / `OnAudioChannelClosed` / `OnIncomingJson` 等回调。
11. **`protocol_->Start()`**：入会、开媒体通道等细节以 **`RtcProtocol`**（`main/protocols/rtc_protocol.cc`，底层 Agora RTC）为准；应用侧通常在唤醒/监听流程中通过 `OpenAudioChannel()` 等与通道配合。
12. 进入 `kDeviceStateIdle`，播放就绪提示音等。

其他任务或 ISR 不应直接长时间占用协议或 UI；需改状态时通过 `Application::Schedule()` 把闭包投递到主事件循环执行。

---

## 4. 主事件循环（`Application::MainEventLoop`）

通过 `EventGroup` 等待以下位（定义见 `application.h`）：

| 事件位 | 典型含义 |
|--------|----------|
| `MAIN_EVENT_SCHEDULE` | 执行 `main_tasks_` 队列中的 `std::function`（线程安全投递） |
| `MAIN_EVENT_SEND_AUDIO` | 从 `AudioService` 发送队列取包，`protocol_->SendAudio` |
| `MAIN_EVENT_WAKE_WORD_DETECTED` | `OnWakeWordDetected()`：开通道、发唤醒/开始监听等 |
| `MAIN_EVENT_VAD_CHANGE` | 监听态下更新 LED 等 |
| `MAIN_EVENT_CLOCK_TICK` | 状态栏、`SystemInfo::PrintHeapStats`（每 10 秒） |
| `MAIN_EVENT_ERROR` | 网络错误提示，回退空闲等 |

`MAIN_EVENT_CHECK_NEW_VERSION_DONE` 在 `CheckNewVersion` 流程末尾通过 `xEventGroupSetBits` 置位（主循环当前未等待该位，可按需扩展同步）。

---

## 5. 设备状态（`device_state.h`）

主要状态包括：`Starting`、`WifiConfiguring`、`Idle`、`Connecting`、`Listening`、`Speaking`、`Upgrading`、`Activating`、`AudioTesting`、`FatalError` 等。

`SetDeviceState()` 会：

- 打印状态日志；
- `DeviceStateEventManager::PostStateChangeEvent`；
- 更新 Display 文案/表情、LED；
- 按状态开关 `AudioService` 的唤醒、语音处理、解码器重置等。

**AEC 模式**（`application.h` 中 `AecMode`）由 Kconfig 互斥项推导：`CONFIG_USE_DEVICE_AEC` / `CONFIG_USE_SERVER_AEC` / 全无，影响监听模式（如是否使用 `kListeningModeRealtime`）。

---

## 6. 音频管线（`audio/audio_service.{h,cc}`）

设计要点（头文件注释中的数据流）：

- **上行**：MIC → 处理器（含 AFE/调试等）→ 编码队列 → **Opus 编码** → 发送队列 → **Protocol::SendAudio**（具体载荷由 `RtcProtocol` / Agora RTC 封装决定）。
- **下行**：服务端音频包 → 解码队列 → **Opus 解码** → 播放队列 → 扬声器。

使用 **两个任务** 分工：一路负责采集/播放/处理，一路负责 Opus 编解码，以控制内存与实时性。

`Application` 在 `OnWakeWordDetected`、`StartListening` / `StopListening`、`SetDeviceState` 等与 `AudioService` 协同，完成「空闲听唤醒 ↔ 连接 ↔ 监听 ↔ 播报」的切换。

---

## 7. 协议层

### 7.1 抽象类 `Protocol`（`main/protocols/protocol.h`）

- 定义 **音频包** `AudioStreamPacket`、二进制帧描述结构、**监听模式**、**中止播报原因** 等。
- 对外回调：`OnIncomingAudio`、`OnIncomingJson`、通道开闭、网络错误、连接/断开。
- 默认文本信令：`SendStartListening` / `SendStopListening` / `SendAbortSpeaking` / `SendWakeWordDetected` / `SendMcpMessage` 等，内部拼 JSON 后走纯虚函数 `SendText()`。
- 子类实现 `Start`、`OpenAudioChannel`、`CloseAudioChannel`、`SendAudio`、`SendText` 等。

### 7.2 `RtcProtocol`（`main/protocols/rtc_protocol.{h,cc}`）

- 继承 `Protocol`；构造传入的 `robot_key` / `robot_token` 对应 menuconfig 的 **`CONFIG_CONNECTION_CONFIG_KEY` / `CONFIG_CONNECTION_CONFIG_TOKEN`**。
- 头文件可见 RTC 侧对外能力：`SendVideoFrame`、`HandleStreamMessage`、`DeliverIncomingAudio` 等；带宽估计相关宏见 `rtc_protocol.h`。入会前通过私有方法拉取语音会话参数（与后端 HTTP 交互，见 `rtc_protocol.cc`）。
- **实现位于工程源码** `rtc_protocol.cc`，通过 `agora_rtc_api.h` 调用 **`components/agora_iot_sdk`** 中的预编译库（如 `librtsa.a`、`libahpl.a`）；媒体与数据流细节可对照 Agora IoT SDK 与当前后端约定。
- 下行音频经 `OnIncomingAudio` 进入应用后，当前工程里典型路径为 **`AudioService::PushPacketToDecodeQueue`**（见 `application.cc`）。

**说明**：`Application` 里 `OnIncomingJson` 回调当前为**空实现**；若需服务端 JSON 驱动表情与字幕，需在此按 RTC 数据流约定补充逻辑。

---

## 8. 板级抽象（`boards/common/board.h`）

- `create_board()` 由具体板目录中 `DECLARE_BOARD(XXX)` 注入，返回单例 `Board` 子类。
- 子类必须实现：`GetAudioCodec()`、`GetNetwork()`、`StartNetwork()`、`SetPowerSaveMode()`、`GetBoardJson()` 等；可选 `GetDisplay()`、`GetLed()`、`GetCamera()` 等。
- 显示、音频、网络、省电策略均通过 `Board::GetInstance()` 与 `Application` 解耦，便于同一套应用代码支持多硬件。

`main/CMakeLists.txt` 根据 `CONFIG_BOARD_TYPE_*` **追加对应板目录的源文件与依赖**，并设置字体、emoji 等宏。

---

## 9. 其他模块职责（简表）

| 模块 | 职责 |
|------|------|
| `Ota` | HTTP 检查版本、下载固件、激活流程、标记当前版本有效 |
| `Settings` | 基于 NVS 的键值读写（如 Wi-Fi、assets URL 等） |
| `McpServer` | 注册工具、解析服务端下发的 MCP payload；`Application::SendMcpMessage` 经协议上行 |
| `Assets` | 资源分区校验、按 URL 下载、应用到显示/语言包等 |
| `DeviceStateEventManager` | 状态变更观察者模式，供其他组件订阅 |
| `SystemInfo` | 版本、堆、任务等系统信息 |
| `debug/ws_log_pcm_sink` | 可选：PCM 经 WebSocket 输出调试 |

---

## 10. 阅读代码的推荐顺序

1. `main/main.cc` → `application.h` → `Application::Start` / `MainEventLoop` / `SetDeviceState` / `OnWakeWordDetected`
2. `main/protocols/protocol.h` → `rtc_protocol.{h,cc}`（需要时再对照 `components/agora_iot_sdk` 与后端文档）
3. `audio/audio_service.h`（数据流注释）→ 与 `Application` 的交互处
4. 你所使用的板型：`main/boards/<your-board>/` 与 `boards/common/board.h`
5. `ota.cc`、`settings.cc`（配网、升级、激活与 NVS 键名）

---

## 11. 与 README 的对应关系

README 中的 **Key / Tokey**、编译烧录、配网流程，对应本文中的 **menuconfig 凭证**、`Board::StartNetwork()` 与 Wi-Fi 配网热点行为；语音会话与 RTC 后端 URL、信令与媒体格式以 **`RtcProtocol` + Agora IoT SDK 及服务器约定** 为准，部署时需与后端一致。

---
