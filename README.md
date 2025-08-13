# Split-Flap Clock

A WiFi-enabled split-flap clock using ESP32, supporting both minutes and hours mechanisms with non-blocking operation for accurate and drift-free timekeeping.

---

## ✨ Features

- **WiFi Setup** via captive portal using WiFiManager.
- **Automatic Time Sync** from NTP servers with timezone selection.
- **Non-Blocking Operation** using `millis()` for precise flap control.
- **Dual Motor Control** for minutes and hours with independent state machines.
- **Homing Function** for both motors at startup using Hall effect sensors.
- **Web Interface** for:
  - Time zone selection
  - 12-hour or 24-hour mode
  - Manual reset
- **Error Codes Displayed on Flaps**:
  - `01` → No WiFi connection
  - `02` → Web interface setup failure
  - `03` → Minutes homing error
  - `04` → Hours homing error

---

## ⚙️ Hardware Requirements

- **ESP32** microcontroller
- **12v Stepper Motor 28BYJ-48 with ULN2003 Driver**
- **2x optical endstop** 
- **12v to 5v USB Power Supply**

---

## 🔧 How It Works

1. **Startup & Homing**  
   - Both minutes and hours drums are homed using Hall effect sensors.
2. **NTP Time Sync**  
   - Time is fetched via WiFi and adjusted for the selected time zone.
3. **Precise Step Control**  
   - Minutes advance 1 flap every 60 seconds.
   - Hours advance 1 flap every 60 minutes.
4. **Error Handling**  
   - If a fault occurs, a specific error code is displayed on the flaps.


