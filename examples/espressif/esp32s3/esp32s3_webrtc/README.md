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

2. Configure product token, product key, and related options:

   ```bash
   idf.py menuconfig
   ```

   Open the **Xiaozhi Assistant** menu. Set **Key** to your product `robot_key` and **Token** to your product `robot_token`, then save.

3. Build:

   ```bash
   idf.py build
   ```

### Flash and run the demo

1. Power on the Espressif dev board.
2. Flash firmware:

   ```bash
   idf.py flash
   ```

3. Open the serial monitor:

   ```bash
   idf.py monitor
   ```

4. Wi-Fi provisioning

   1. On your phone, connect to the AP named like `Xiaozhi-XXXXXX`.
   2. In a browser, open `http://192.168.4.1` to open the Wi-Fi setup page.
   3. Enter SSID and password, then submit.

   > **Note:** To use a different Wi-Fi network, reboot the device. If it cannot reconnect to the saved network (out of range or network gone), wait about **30 seconds** to enter provisioning mode again, then repeat the three steps above.
