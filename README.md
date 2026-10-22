ESP32 Modbus RTU to TCP Gateway
A professional-grade, lightweight solution built on the ESP32 to bridge RS-485 Modbus RTU serial devices to Ethernet/Wi-Fi Modbus TCP networks.
This gateway acts as a protocol translator and polling manager. It polls downstream Modbus RTU devices over RS-485 and serves data to multiple Modbus TCP clients (SCADA systems, PLCs, Home Assistant, Node-RED, or custom software dashboards) concurrently.
---
📸 Overview & Key Features
Bi-directional Protocol Translation: Converts Modbus TCP requests to Modbus RTU frame formats and back.
Multi-Client Handling: Allows multiple network TCP clients to request data simultaneously without causing serial bus contention or timeouts.
Device Profiles & Presets: Native support for standard industrial and renewable energy devices (e.g., Eastron SDM meters, Growatt/SolarEdge inverters, generic Modbus slaves).
Web-Based Management Portal: Interactive browser-based GUI for adjusting Wi-Fi, serial parameters, slave client profiles, and register polling configurations.
Real-time Diagnostics & Monitoring: Live view of holding/input register states, communication success/error counters, and signal strength (RSSI).
Captive Portal Provisioning: Automatic Access Point (AP) mode on first boot for easy network credential configuration.
Non-Volatile Storage: All settings (baud rates, slave IDs, register maps) persist across power cycles using EEPROM/Preferences.
---
🔌 Hardware Setup & Wiring
Requirements
ESP32 Development Board (ESP32-WROOM-32 or similar variant)
TTL to RS-485 Module (MAX485, MAX3485, or modules with hardware automatic flow control)
Modbus RTU Slave Device(s) (energy meters, solar inverters, sensors, PLCs)
5V Power Supply (via USB or external regulated supply)
Pin Mapping
ESP32 Pin	RS-485 Transceiver	Description
5V / 3.3V	VCC	Module power supply
GND	GND	Common ground connection
GPIO 16	RO / RX	ESP32 Hardware Serial RX2
GPIO 17	DI / TX	ESP32 Hardware Serial TX2
GPIO 4 (Optional)	DE / RE	Direction control pin (for non-auto flow control modules)
—	A (+)	RS-485 Data Positive Line
—	B (-)	RS-485 Data Negative Line
> 💡 **Wiring Note:** For long physical cable runs, place a **120Ω termination resistor** between the `A` and `B` terminals at both physical ends of the RS-485 bus line to prevent signal reflections.
---
🚀 Quick Start Guide
1. Flashing Firmware
Open the project folder in Arduino IDE or PlatformIO.
Install the necessary ESP32 core board definitions and required libraries (`ModbusIP_ESP8266`, `WiFiManager` or project dependencies).
Select your target ESP32 Dev Module board and the correct serial COM port.
Compile and upload the sketch to your board.
2. Wi-Fi Provisioning
On initial power-up, the ESP32 enters Access Point mode and broadcasts a Wi-Fi SSID named `ModbusGateway-Config` (or similar).
Connect to this Wi-Fi network using a smartphone or computer (Default Password: `modbus123` if enabled).
The captive web portal will automatically pop up (or navigate to `192.168.4.1` in your browser).
Select your local Wi-Fi network, enter the network key, set your preferred hostname (e.g., `modbus.local`), and click Save.
The device will restart and connect to your local network.
---
🌐 Web Interface & Configuration
Once connected to your local network, access the web control panel by navigating to `http://<ESP32-IP-Address>` or `http://modbus.local`.
⚙️ Modbus Serial Settings
Baud Rate: Configurable from `1200` to `115200` baud (Default: `9600`).
Data Bits / Parity / Stop Bits: Configurable serial mode (e.g., `8N1`, `8E1`).
Serial RX/TX Pins: Reconfigurable GPIO assignments via settings.
📋 Slave Device Management
Navigate to Manage Clients / Devices.
Click Add Device and enter:
Slave ID: Target RTU device unit ID (1–247).
Device Profile: Select from built-in presets (SDM630, Growatt, Generic) or create custom maps.
Start Register: Register address offset.
Register Count: Number of 16-bit registers to query.
Polling Interval: Refresh period (recommended: `1000ms`).
Click Save to apply the configuration.
---
💻 Interfacing with Modbus TCP Masters
To query the gateway from an external TCP master (e.g., Home Assistant Modbus integration, Modbus Poll, Node-RED):
Protocol: Modbus TCP
IP Address: ESP32 assigned local IP address (or `modbus.local`)
Port: `502` (standard Modbus TCP port)
Slave ID (Unit Identifier): Match the target RTU Slave ID configured in the gateway.
---
🛠️ Troubleshooting & FAQ
Symptom	Probable Cause	Action
Cannot open web interface	Device lost Wi-Fi connection or changed IP	Check router DHCP client list or check Serial Monitor output at 115200 baud for current IP address.
Modbus Read Timeout	Reversed RS-485 polarity or mismatched baud rate	Swap the `A` and `B` wires. Ensure the slave device baud rate and parity match the gateway configuration.
Garbled / Inverted Values	Byte order / Word order mismatch	Enable word/byte swapping or float32 conversion options in the web interface device profile settings.
Frequent Communication Errors	Polling rate too fast for serial bus	Increase the polling interval (e.g., from `200ms` to `1000ms` or higher).
---
🙏 Acknowledgments & Credits
This project builds upon and draws inspiration from several open-source works in the IoT and hardware ecosystem:
zivillian/esp32-modbus-gateway: Core reference for bridging Modbus RTU (RS-485) to Modbus TCP on ESP32 microcontrollers.
Tasmota: Design inspiration for robust firmware management, web configuration user experience, and IoT device integration standards.
