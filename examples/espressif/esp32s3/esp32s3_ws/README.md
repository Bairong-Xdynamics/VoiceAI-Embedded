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

2. Set product token, product key, and related options.

   ```bash
   idf.py menuconfig
   ```

   Open the **Xiaozhi Assistant** menu. Set **Key** to your product `robot_key`, and **Token** (the menu may label this as **Tokey**) to your product `robot_tokey`, then save.

3. Build.

   ```bash
   idf.py build
   ```

### Flash and run the demo

1. Power on the Espressif development board.
2. Flash the firmware.

   ```bash
   idf.py flash
   ```

3. Run the monitor to view serial logs.

   ```bash
   idf.py monitor
   ```

4. Wi-Fi provisioning.

   1. On your phone, connect to a hotspot named like `Xiaozhi-XXXXXX`.
   2. Open a browser and go to `http://192.168.4.1` to open the Wi-Fi setup page.
   3. Enter the Wi-Fi SSID and password, then submit.

   > **Note:** To switch to another Wi-Fi network, reboot the device. If after reboot it cannot reach the previously saved network (out of range or network gone), wait about 30 seconds for provisioning mode, then repeat the three Wi-Fi steps above.
