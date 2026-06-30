<h1 align="center">ESP32-S3 Breadboard Development Guide</h1>

### Overview

This project extends the open-source [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) project.
On top of the original ESP32 AI voice interaction framework, it adds:

- Custom audio processing modules
- Optimized audio path (AEC / codecs / low latency)
- Integration with in-house AI / LLM services
- Stronger device management and communication
- Enterprise-oriented deployment

### Espressif environment setup

See the [ESP-IDF getting started guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/index.html).

> **Note:** This project targets ESP-IDF `v5.5`.

### Clone and configure the project

1. Clone this real-time conversational AI hardware example into your ESP-IDF `examples` directory.

   1. Go to the `examples` folder under ESP-IDF.

   ```bash
   cd $IDF_PATH/examples
   ```

   2. Clone the example repository and check out the `main` branch.

   ```bash
   git clone https://github.com/Bairong-Xdynamics/VoiceAI-Embedded.git
   git checkout main
   ```

### Build the firmware

1. Enter esp32s3_ws directory.

   ```bash
   cd VoiceAI-Embedded/examples/espressif/esp32s3/esp32s3_ws
   ```

2. Build.

   ```bash
   idf.py build
   ```

3. Flash the firmware.

   ```bash
   idf.py flash
   ```

4. Run the monitor to view serial logs.

   ```bash
   idf.py monitor
   ```
