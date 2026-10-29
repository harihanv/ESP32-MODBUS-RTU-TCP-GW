ESP32 Modbus RTU to TCP Gateway

A lightweight, high-performance gateway that bridges RS-485 Modbus RTU serial devices to Modbus TCP networks using the ESP32 platform.

🚀 Key Features

RTU to TCP Bridge: Seamlessly translates Modbus TCP client requests to Modbus RTU (RS-485) slaves.

Multi-Client Access: Allows multiple Modbus TCP masters (Home Assistant, Node-RED, PLCs) to query serial devices simultaneously.

Web Management Interface: Built-in web portal for adjusting Wi-Fi, RS-485 serial parameters, and register mapping.

Captive Portal Setup: Easy initial network provisioning via auto-created Wi-Fi Access Point.

Persistent Config: Stores baud rate, unit IDs, and network parameters in ESP32 Flash memory.

<img width="721" height="290" alt="image" src="https://github.com/user-attachments/assets/f3a6d287-693a-4c7d-8386-763ec0adf558" />

🛠️ Getting Started

Flash Firmware: Compile and flash the project onto your ESP32 using Arduino IDE or PlatformIO.

Wi-Fi Setup: Connect to the ModbusGateway-Config Wi-Fi AP from your phone/PC and enter your network credentials.

Configure Gateway: Open http://<ESP32-IP> in a browser to set baud rates, slave IDs, and register settings.

Connect TCP Client: Point your TCP master (e.g., Home Assistant) to the ESP32 IP on port 502.

🙏 Acknowledgments & Credits

zivillian/esp32-modbus-gateway: Core inspiration and conceptual framework for ESP32 Modbus bridging.

Tasmota: Design patterns for web interface management and stable ESP firmware architecture.
