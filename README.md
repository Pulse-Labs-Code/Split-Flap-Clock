# Split-Flap Clock

> Wi-Fi-enabled split-flap clock using ESP32, NTP time sync, and non-blocking stepper control with homing sequence and error handling.

A compact, fully automated split-flap clock powered by an ESP32.  
Keeps time with NTP, uses a non-blocking state machine for smooth flap movement, and features error codes for easy troubleshooting.  
Designed to work with a custom-built split-flap display mechanism.


---

## ✨ Features

- **Wi-Fi setup via captive portal** (WiFiManager)
- **Accurate time sync** from NTP servers
- **Non-blocking operation** using `millis()` — no delays
- **Homing sequence** for hours and minutes flaps at startup
- **Reverse direction** support for hours mechanism
- **Error codes** displayed for:
  - `1` – No Wi-Fi connection
  - `2` – Captive portal setup failure (404)
  - `3` – Minutes homing error
  - `4` – Hours homing error

---

## 🛠 Hardware Requirements

- **ESP32** development board
- **2 × DC12V 28BYJ-48** with ULN2003 driver boards
- **2 × Optical Endstop**
- **12v to 5v 5A Converter Step-Down Power Supply**
- **female to female jumper cables**
---

## 🔌 Wiring

| Component             | ESP32 Pin |
|-----------------------|-----------|
| Minutes Stepper IN1   | GPIO 16       |
| Minutes Stepper IN2   | GPIO 17       |
| Minutes Stepper IN3   | GPIO 18       |
| Minutes Stepper IN4   | GPIO 19       |
| Minutes Hall Sensor   | GPIO 26       |
| Hours Stepper IN1     | GPIO 21       |
| Hours Stepper IN2     | GPIO 22       |
| Hours Stepper IN3     | GPIO 23       |
| Hours Stepper IN4     | GPIO 25       |
| Hours Hall Sensor     | GPIO 27       |


---

## 📦 Installation

1. **Install Arduino IDE** (https://www.arduino.cc/en/software)
2. **Add ESP32 board support** via Boards Manager
3. **Install libraries** via Library Manager:
   - `WiFiManager`
   - `NTPClient`
   - `AccelStepper`
4. Clone or download this repository
5. Open `.ino` file in Arduino IDE
6. Select **ESP32 Dev Module** in Tools → Board
7. Upload to your ESP32

---

## 🚀 How It Works

1. **Startup**  
   - ESP32 boots and tries to connect to Wi-Fi  
   - If no Wi-Fi, captive portal opens for setup  
   - If setup fails, error code `2` is shown

2. **Homing**  
   - Each motor runs until its Hall sensor detects the "zero" flap position  
   - If sensor not detected within set time, error code `3` or `4` is shown

3. **Time Sync**  
   - ESP32 gets current time from NTP  
   - Motors move to correct hour and minute positions

4. **Normal Operation**  
   - Every minute: minutes motor moves one flap  
   - Every hour: hours motor moves one flap  
   - Fully non-blocking so no drift between time and position

---

## ❗ Error Codes

| Code | Description                  |
|------|------------------------------|
| 1    | No Wi-Fi connection          |
| 2    | Captive portal setup failure |
| 3    | Minutes homing error         |
| 4    | Hours homing error           |

---


