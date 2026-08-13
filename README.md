# Moto Guzzi CAN Diagnostic & Telemetry Toolkit (`guzzi-can-toolkit`)

A containerized ESP-IDF (v5.4.4) firmware and TypeScript Web UI project for reading live CAN bus telemetry and executing UDS service diagnostic routines on Moto Guzzi motorcycles (**V85 TT**, **V100 Mandello**, **Stelvio**, **V7/V9 Euro5**) using a **Seeed Studio XIAO ESP32-C3 / C6** + **SN65HVD230** CAN transceiver.

---

## Features

- **Built-in Dockerized Toolchain**: No local ESP-IDF or Node.js installation required—everything is built inside isolated Docker containers using `./run.sh`.
- **Wi-Fi SoftAP & Captive Portal**: Creates a local Wi-Fi Access Point (`Guzzi-CAN-Tool`, IP: `192.168.4.1`). Auto-triggers a captive portal popup on smartphones/laptops when connected.
- **Factory DBC CAN Decoder**: Passively decodes broadcast CAN frames at 500 kbps (RPM, speed, gear, oil/coolant temps, battery voltage, odometer, fuel range, traction control, ABS map, front brake pressure, and IMU G-force).
- **Active UDS Diagnostic Engine**:
  - **Service Wrench Reset (`0x720`)**: Extinguishes the maintenance spanner light on the TFT cluster via UDS session `0x10 0x03` & Routine `0x31 0x01 0x02 0x01`.
  - **Engine ECU DTC Clear (`0x7E0`)**: Clears stored engine trouble fault codes via UDS service `0x14 FF FF FF`.
- **Real-Time WebSockets Stream**: Pushes 10 Hz JSON telemetry updates directly to the embedded Web UI dashboard.

---

## Hardware Pinout & Wiring

| Component | XIAO ESP32-C3 Pin | XIAO ESP32-C6 Pin | SN65HVD230 Module | ISO 19689 6-Pin OBD Port |
| :--- | :--- | :--- | :--- | :--- |
| **CAN TX** | `GPIO 5` (D3) | `GPIO 5` (D3) | `CTX` / `TXD` | — |
| **CAN RX** | `GPIO 4` (D2) | `GPIO 4` (D2) | `CRX` / `RXD` | — |
| **VCC (3.3V)** | `3.3V` | `3.3V` | `VCC` | — |
| **GND** | `GND` | `GND` | `GND` | Pin 3 / Pin 4 (Ground) |
| **CAN High** | — | — | `CAN_H` | Pin 2 (CAN High) |
| **CAN Low** | — | — | `CAN_L` | Pin 5 (CAN Low) |
| **12V Power** | — | — | — | Pin 1 (+12V Switched/Battery) |

---

## Quick Start Guide

### 1. Build Full Project (Frontend + Firmware)

```bash
./run.sh build
```
This builds the Vite/TypeScript Web UI dashboard, compresses the web assets, generates the SPIFFS partition image, and compiles the ESP-IDF C firmware.

### 2. Flash Firmware to XIAO ESP32

Connect your XIAO ESP32-C3 / C6 board via USB-C cable and run:

```bash
./run.sh flash
```
*(Auto-detects USB serial port `/dev/ttyACM0` or `/dev/ttyUSB0`)*

### 3. Open Serial Monitor

```bash
./run.sh monitor
```

### 4. Build, Flash, and Monitor in One Command

```bash
./run.sh all
```

---

## Accessing the Web UI & Captive Portal

1. Connect your smartphone, tablet, or laptop to the Wi-Fi network:
   - **SSID**: `Guzzi-CAN-Tool`
   - **Password**: `12345678`
2. The device will automatically pop up the **Captive Portal Web Dashboard**.
3. Alternatively, open your browser and navigate to:
   ```text
   http://192.168.4.1/
   ```

---

## Bootloader Recovery Mode

If flashing fails or `/dev/ttyACM0` disappears after a crash:
1. **Hold down the BOOT button** on the XIAO ESP32 board.
2. **Re-plug** the USB-C cable.
3. **Release** the BOOT button.
4. Run `./run.sh flash`.
