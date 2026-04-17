# Omni-Node — ESP12-F IoT Gateway

**Omni-Node** is a powerful, self-contained IoT gateway built on the ESP8266 (ESP12-F) platform. It serves as a bridge between physical sensors/actuators and MQTT brokers, featuring a modern, on-device web dashboard with a "Minimalist Neo-Brutalist" aesthetic.

---

## Key Features

- **Dual-Mode Connectivity**: Seamlessly handles Station (STA) mode for primary use and Access Point (AP) mode for fail-safe configuration.
- **Industrial-Grade Networking**: Supports DHCP and Static IP with manual DNS configuration (Primary & Secondary) for restrictive intranet environments.
- **MQTT Engine**: Full MQTT support with Last Will and Testament (LWT) and customizable topic prefixes.
- **IO Polling Engine**: Dynamic pin mapping with configurable labels, data types (float32, int16, etc.), and multipliers.
- **Real-time Streaming**: Uses **WebSockets** for zero-latency sensor monitoring on the web dashboard.
- **System Management**:
  - **OTA Updates**: Remote firmware updates via web interface.
  - **Authentication**: Protected dashboard access.
  - **Failsafe Button**: Hardware-based reset and factory reset (long press 10s).
  - **Persistence**: Configuration stored in **LittleFS** (JSON).

## UI/UX: Neo-Brutalist Aesthetic

The dashboard follows a unique **Minimalist Neo-Brutalist** design:
- **Bold Borders**: Consistent 2.5px solid black borders.
- **Monospace Typography**: Technical "terminal-style" feel.
- **Hard Shadows**: High-contrast, no-blur shadows for a retro-modern look.
- **Resource Intelligent**: Extremely lightweight (Vanilla JS/CSS), optimized for the ESP8266's limited memory.

## Technical Stack

- **Firmware**: Arduino / C++
- **Frontend**: HTML5, CSS3 (Vanilla), JavaScript (ES6+)
- **Communication**: HTTP, WebSockets, MQTT
- **Core Libraries**:
  - `ESPAsyncWebServer` & `ESPAsyncTCP`
  - `ArduinoJson` (v7+)
  - `PubSubClient` (MQTT)
  - `LittleFS`
  - `NTPClient`

## Project Structure

```text
omninode_gateway/
├── omninode_gateway.ino     # Main firmware source
└── data/                    # Filesystem data (stored in LittleFS)
    └── www/
        ├── index.html       # Web dashboard
        ├── style.css        # Neo-Brutalist design system
        └── app.js           # Frontend logic & WebSockets
```

## Quick Start

1. **Hardware**: Use an ESP12-F or NodeMCU board.
2. **Library Setup**: Install the required libraries as detailed in the [Project Guide](project_guide.md).
3. **Filesystem Upload**: Use the **ESP8266 LittleFS Data Upload** tool in Arduino IDE to upload the `data/` folder content.
4. **Flash**: Upload the `.ino` sketch to your board.
5. **Configure**: Connect to the `OmniNode-Setup` WiFi and head to `http://192.168.4.1` to configure your node.

---

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

---
*Built by Ichsan*
