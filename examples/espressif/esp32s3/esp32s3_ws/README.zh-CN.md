<h1 align="center"> esp32s3面包板开发说明</h1>

### 项目简介

本项目基于开源项目 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 进行二次开发，
在原有 ESP32 AI 语音交互框架基础上，扩展了以下能力：

- ✅ 自定义音处理模块
- ✅ 优化音频链路（AEC / 编解码 / 低延迟）
- ✅ 集成自研 AI / LLM 服务
- ✅ 增强设备管理与通信能力
- ✅ 支持企业级部署

### 配置乐鑫环境

详见[开发环境配置文档](https://docs.espressif.com/projects/esp-idf/zh_CN/stable/esp32s3/get-started/index.html)。
   > **注意**：工程使用的 IDF 版本为 `v5.5`
   >

### 下载并配置工程

1. 将实时对话式 AI 硬件示例工程克隆到 乐鑫 IDF examples 目录下。

   1. 进入 esp-df/examples 目录。

   ```bash
   cd $IDF_PATH/examples
   ```

   2. 克隆实时对话式 AI 硬件示例工程，切换到main分支。

   ```bash
   git clone https://github.com/Bairong-Xdynamics/VoiceAI-Embedded.git
   git checkout main
   ```

### 编译固件

1. 进入esp32s3_ws目录。

   ```bash
   cd VoiceAI-Embedded/examples/espressif/esp32s3/esp32s3_ws
   ```

2. 编译固件。

   ```bash
   idf.py build
   ```

3. 烧录固件。

   ```bash
   idf.py flash
   ```

4. 运行示例 Demo 并查看串口日志输出。

   ```bash
   idf.py monitor
   ```
