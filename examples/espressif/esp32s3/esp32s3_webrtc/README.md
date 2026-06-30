<h1 align="center">ESP32-S3 Breadboard Development Guide</h1>

### Overview

This project is derived from the open-source [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32). On top of the original ESP32 AI voice interaction framework, it adds:

- Custom audio processing modules
- Optimized audio pipeline (AEC / codecs / low latency)
- Integration with proprietary AI / LLM services
- Stronger device management and communication
- Enterprise-oriented deployment support

### Espressif (ESP-IDF) environment

See the [ESP-IDF get-started guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/index.html).

> **Note:** This project targets ESP-IDF **v5.5**.

### Clone and set up the project

1. Clone this real-time conversational AI hardware example under your ESP-IDF `examples` tree.

   1. Go to the `examples` directory:

   ```bash
   cd $IDF_PATH/examples
   ```

   2. Clone the repository and switch to the `main` branch:

   ```bash
   git clone https://github.com/Bairong-Xdynamics/VoiceAI-Embedded.git
   git checkout main
   ```

### Build the firmware

1. Enter esp32s3_webrtc directory:

   ```bash
   cd VoiceAI-Embedded/examples/espressif/esp32s3/esp32s3_webrtc
   ```

2. Build:

   ```bash
   idf.py build
   ```

3. Flash firmware:

   ```bash
   idf.py flash
   ```

4. Open the serial monitor:

   ```bash
   idf.py monitor
   ```
