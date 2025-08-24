#include <WiFiManager.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <math.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <Update.h>

// --- Constants ---
#define WIFI_MANAGER_AP_NAME "SplitFlapClockSetup"

// --- Error Codes ---
// These values define which flap number to show for each error.
#define ERROR_CODE_NO_WIFI_HR     1  // Hour flap shows '1'
#define ERROR_CODE_NO_WIFI_MIN    1  // Minute flap shows '1'
#define ERROR_CODE_WEB_FAIL_HR    2  // Hour flap shows '2'
#define ERROR_CODE_WEB_FAIL_MIN   2  // Minute flap shows '2'
#define ERROR_CODE_HR_HOME_FAIL_HR 0 // Hour flap shows '0' (blank)
#define ERROR_CODE_HR_HOME_FAIL_MIN 3  // Minute flap shows '3'
#define ERROR_CODE_MIN_HOME_FAIL_HR 4  // Hour flap shows '4'
#define ERROR_CODE_MIN_HOME_FAIL_MIN 0 // Minute flap shows '0' (blank)

// Motor and Endstop Pins
#define MIN_MOTOR_IN1_PIN 16
#define MIN_MOTOR_IN2_PIN 17
#define MIN_MOTOR_IN3_PIN 18
#define MIN_MOTOR_IN4_PIN 19
#define HR_MOTOR_IN1_PIN 21
#define HR_MOTOR_IN2_PIN 22
#define HR_MOTOR_IN3_PIN 23
#define HR_MOTOR_IN4_PIN 25
#define MIN_ENDSTOP_PIN 26
#define HR_ENDSTOP_PIN 27

// Motor Characteristics
#define MOTOR_STEPS_PER_REVOLUTION 4096

// Speeds (steps per second)
#define HOMING_SPEED_STEPS_PER_SEC 650
#define NORMAL_SPEED_STEPS_PER_SEC 650

// Time constants
#define NTP_UPDATE_INTERVAL_MS 30000

// --- Calibration Mode Flag ---
#define CALIBRATION_MODE false

// --- Global Variables for STEPS_PER_FLAP ---
float g_minStepsPerFlap = 93.0f;
float g_hrStepsPerFlap = 93.0f;

// --- Global Variables (will be loaded from Preferences) ---
bool g_is24HourDisplay = false;
long g_timezoneBaseOffset = -21600; // Default to Mountain Time (UTC-6)
bool g_isDst = true; // Default to DST on
long g_ntpOffsetSeconds = -21600; // Calculated from base + DST

// --- Quiet Hours Settings (Revised for multiple schedules) ---
bool g_quietHoursEnabled = false; // Master enable switch

// Schedule A (e.g., Weekdays)
int g_quietStartA = 21; // Default 9 PM
int g_quietEndA = 7;    // Default 7 AM
uint8_t g_quietDaysA = 0b00011110; // Default Mon, Tue, Wed, Thu (Bit order: SMTWTFS)

// Schedule B (e.g., Weekends)
int g_quietStartB = 23; // Default 11 PM
int g_quietEndB = 10;   // Default 10 AM
uint8_t g_quietDaysB = 0b01100001; // Default Fri, Sat, Sun

// --- Manual Time Settings ---
bool g_isManualTimeMode = false;
int g_manualHour = 10;
int g_manualMinute = 10;
unsigned long g_lastMinuteTick = 0;

// --- Physical Clock Offset ---
#define MINUTE_HOME_OFFSET 0
#define HOUR_HOME_POSITION 0 // The physical home position for the hour flap is 0

// --- Global Objects ---
WiFiManager wifiManager;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org");
WebServer server(80);
Preferences preferences;

// --- Forward Declaration to fix compilation error ---
String getFormattedTime();

// --- Error Logger ---
namespace Logger {
    const int MAX_LOG_ENTRIES = 20;
    const char* PREFS_NAMESPACE = "clock-logs";
    
    void add(String message) {
        preferences.begin(PREFS_NAMESPACE, false);
        int nextIndex = preferences.getInt("log_idx", 0);
        int count = preferences.getInt("log_count", 0);

        // Add timestamp to log message
        String timestampedMessage = getFormattedTime() + " - " + message;

        String key = "log_" + String(nextIndex);
        preferences.putString(key.c_str(), timestampedMessage);
        
        nextIndex = (nextIndex + 1) % MAX_LOG_ENTRIES;
        preferences.putInt("log_idx", nextIndex);
        
        if (count < MAX_LOG_ENTRIES) {
            preferences.putInt("log_count", count + 1);
        }

        preferences.end();
        Serial.println("LOG: " + timestampedMessage);
    }

    String getAll() {
        preferences.begin(PREFS_NAMESPACE, true); // Read-only
        String allLogs = "";
        int logStart = preferences.getInt("log_idx", 0);
        int count = preferences.getInt("log_count", 0);
        
        if (count == 0) {
            preferences.end();
            return "No log entries found.";
        }
        
        // Read logs in chronological order
        for (int i = 0; i < count; ++i) {
            int entryIndex;
             if (count < MAX_LOG_ENTRIES) {
                 entryIndex = i;
            } else {
                 entryIndex = (logStart + i) % MAX_LOG_ENTRIES;
            }
            String key = "log_" + String(entryIndex);
            if (preferences.isKey(key.c_str())) {
                allLogs += preferences.getString(key.c_str(), "") + "\n";
            }
        }
        preferences.end();
        return allLogs;
    }

    void clear() {
        preferences.begin(PREFS_NAMESPACE, false);
        preferences.clear();
        preferences.putInt("log_count", 0); // Reset the count
        preferences.putInt("log_idx", 0);
        preferences.end();
        Logger::add("Logs cleared.");
    }
}

// --- Time Source Abstraction ---
// This allows the clock to seamlessly switch between NTP and Manual time.
int getCurrentHour() {
    if (g_isManualTimeMode) {
        return g_manualHour;
    }
    return timeClient.getHours();
}

int getCurrentMinute() {
    if (g_isManualTimeMode) {
        return g_manualMinute;
    }
    return timeClient.getMinutes();
}

String getFormattedTime() {
    if (g_isManualTimeMode) {
        char buf[9];
        sprintf(buf, "%02d:%02d:--", g_manualHour, g_manualMinute);
        return String(buf);
    }
    return timeClient.getFormattedTime();
}

// --- Helper to check if NTP is running ---
// The day-specific Quiet Hours function relies on the day of the week, which is only available from NTP.
bool isNtpActive() {
    return !g_isManualTimeMode && WiFi.status() == WL_CONNECTED;
}

// --- Quiet Hours Check (Revised for day-specific schedules) ---
bool isInQuietHours() {
    // Quiet hours are disabled if the master switch is off OR if we are in manual mode (since we don't know the day of the week).
    if (!g_quietHoursEnabled || !isNtpActive()) return false;
    
    int currentHour = getCurrentHour();
    int dayOfWeek = timeClient.getDay(); // 0 = Sunday, 1 = Monday, ..., 6 = Saturday

    // Check Schedule A: Is today's bit set in the bitmask for Schedule A?
    if ((g_quietDaysA >> dayOfWeek) & 1) { 
        if (g_quietStartA > g_quietEndA) { // Overnight schedule (e.g., 22:00 - 07:00)
            if (currentHour >= g_quietStartA || currentHour < g_quietEndA) return true;
        } else { // Same-day schedule (e.g., 09:00 - 17:00)
            if (currentHour >= g_quietStartA && currentHour < g_quietEndA) return true;
        }
    }
    
    // Check Schedule B: Is today's bit set in the bitmask for Schedule B?
    if ((g_quietDaysB >> dayOfWeek) & 1) { 
        if (g_quietStartB > g_quietEndB) { // Overnight schedule
            if (currentHour >= g_quietStartB || currentHour < g_quietEndB) return true;
        } else { // Same-day schedule
            if (currentHour >= g_quietStartB && currentHour < g_quietEndB) return true;
        }
    }
    
    return false; // Not in quiet hours for any active schedule today
}


// --- HTML for Web Interface (Updated for day-specific Quiet Hours) ---
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head>
<title>Split-Flap Clock Control</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif; margin: 0; padding: 20px; background-color: #f4f4f4; color: #333; }
.container { max-width: 600px; margin: auto; background: white; padding: 20px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }
h2, h3 { color: #0056b3; border-bottom: 2px solid #eee; padding-bottom: 10px; }
.form-group { margin-bottom: 20px; }
.form-group-inline { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
label { display: block; margin-bottom: 5px; font-weight: bold; }
input[type=number], select, .btn { padding: 12px; border-radius: 5px; border: 1px solid #ddd; width: 100%; box-sizing: border-box; font-size: 16px; }
.form-group-inline select { width: auto; flex-grow: 1; }
.form-group-inline input[type=number] { width: 80px; }
.btn { border: none; cursor: pointer; margin-top: 10px; }
.btn-primary { background-color: #007bff; color: white; }
.btn-secondary { background-color: #6c757d; color: white; }
.btn-danger { background-color: #dc3545; color: white; }
#message { margin-top: 20px; padding: 10px; border-radius: 5px; display: none; text-align: center; }
.success { background-color: #d4edda; color: #155724; border: 1px solid #c3e6cb; }
.error { background-color: #f8d7da; color: #721c24; border: 1px solid #f5c6cb; }
.info { background-color: #d1ecf1; color: #0c5460; border: 1px solid #bee5eb; padding: 10px; border-radius: 5px; margin-bottom: 15px; }
#time-display { font-size: 1.5em; font-weight: bold; color: #333; text-align: center; background: #eee; padding: 15px; border-radius: 5px; }
#log-viewer { display: none; margin-top: 15px; }
#log-content { background: #222; color: #0f0; font-family: monospace; padding: 15px; border-radius: 5px; white-space: pre-wrap; word-wrap: break-word; max-height: 300px; overflow-y: auto; }
.schedule-box { border: 1px solid #eee; padding: 15px; border-radius: 5px; margin-top: 10px; }
.schedule-title { font-weight: bold; color: #555; margin-bottom: 10px; }
.day-selector { display: flex; justify-content: space-between; margin-top: 10px; }
.day-button { padding: 8px 12px; border: 1px solid #ddd; border-radius: 20px; cursor: pointer; user-select: none; transition: background-color 0.2s, color 0.2s; }
.day-button.active { background-color: #007bff; color: white; border-color: #007bff; }
</style>
</head><body>
<div class="container">
<h2>Clock Status</h2>
<div id="time-display">--:--:--</div>
<h2>Settings</h2>
<form id="settingsForm">
<h3>Time & Display</h3>
<div class="form-group">
<label for="timezone">Timezone (Standard Time)</label>
<select id="timezone" name="timezone">
<optgroup label="North America">
<option value="-36000">Hawaii (UTC-10)</option><option value="-32400">Alaska (UTC-9)</option><option value="-28800">Pacific Time (UTC-8)</option><option value="-25200">Mountain Time (UTC-7)</option><option value="-21600">Central Time (UTC-6)</option><option value="-18000">Eastern Time (UTC-5)</option><option value="-14400">Atlantic Time (UTC-4)</option><option value="-12600">Newfoundland (UTC-3:30)</option>
</optgroup>
<optgroup label="Europe">
<option value="0">Greenwich Mean Time (UTC+0)</option><option value="3600">Central European Time (UTC+1)</option><option value="7200">Eastern European Time (UTC+2)</option>
</optgroup>
<optgroup label="Asia & Oceania">
<option value="19800">India (UTC+5:30)</option><option value="20700">Nepal (UTC+5:45)</option><option value="28800">Western Australia (UTC+8)</option><option value="34200">Central Australia (UTC+9:30)</option><option value="36000">Eastern Australia (UTC+10)</option><option value="43200">New Zealand (UTC+12)</option>
</optgroup>
</select>
</div>
<div class="form-group">
<input type="checkbox" id="isDst" name="isDst" style="width:auto; vertical-align: middle;">
<label for="isDst" style="display:inline; font-weight:normal;">Daylight Saving Time is active (+1 hour)</label>
</div>
<div class="form-group">
<input type="checkbox" id="is24hour" name="is24hour" style="width:auto; vertical-align: middle;">
<label for="is24hour" style="display:inline; font-weight:normal;">24-Hour Format</label>
</div>

<h3>Quiet Hours</h3>
<div class="form-group">
  <input type="checkbox" id="quietEnabled" name="quietEnabled" style="width:auto; vertical-align: middle;">
  <label for="quietEnabled" style="display:inline; font-weight:normal;">Enable Quiet Hours (no flap movement)</label>
  <div class="info" style="font-size:0.9em; margin-top:5px;">Note: Quiet hours requires an active NTP connection and will not function in manual time mode.</div>
</div>

<div class="schedule-box">
  <div class="schedule-title">Schedule A</div>
  <div class="day-selector" id="day-selector-A">
    <span class="day-button" data-day="0">S</span>
    <span class="day-button" data-day="1">M</span>
    <span class="day-button" data-day="2">T</span>
    <span class="day-button" data-day="3">W</span>
    <span class="day-button" data-day="4">T</span>
    <span class="day-button" data-day="5">F</span>
    <span class="day-button" data-day="6">S</span>
  </div>
  <div class="form-group-inline qh-24" style="margin-top: 15px;">
    <label style="display:inline; font-weight:normal; margin-bottom:0;">Start (0-23):</label>
    <input type="number" id="quietStartA24" min="0" max="23">
    <label style="display:inline; font-weight:normal; margin-bottom:0;">End (0-23):</label>
    <input type="number" id="quietEndA24" min="0" max="23">
  </div>
  <div class="form-group-inline qh-12" style="display: none; margin-top: 15px;">
    <label style="display:inline; font-weight:normal; margin-bottom:0;">Start:</label>
    <select id="quietStartAHour12"><option>1</option><option>2</option><option>3</option><option>4</option><option>5</option><option>6</option><option>7</option><option>8</option><option>9</option><option>10</option><option>11</option><option>12</option></select>
    <select id="quietStartAAmPm"><option>AM</option><option>PM</option></select>
    <label style="display:inline; font-weight:normal; margin-bottom:0;">End:</label>
    <select id="quietEndAHour12"><option>1</option><option>2</option><option>3</option><option>4</option><option>5</option><option>6</option><option>7</option><option>8</option><option>9</option><option>10</option><option>11</option><option>12</option></select>
    <select id="quietEndAAmPm"><option>AM</option><option>PM</option></select>
  </div>
</div>

<div class="schedule-box">
  <div class="schedule-title">Schedule B</div>
  <div class="day-selector" id="day-selector-B">
    <span class="day-button" data-day="0">S</span>
    <span class="day-button" data-day="1">M</span>
    <span class="day-button" data-day="2">T</span>
    <span class="day-button" data-day="3">W</span>
    <span class="day-button" data-day="4">T</span>
    <span class="day-button" data-day="5">F</span>
    <span class="day-button" data-day="6">S</span>
  </div>
  <div class="form-group-inline qh-24" style="margin-top: 15px;">
    <label style="display:inline; font-weight:normal; margin-bottom:0;">Start (0-23):</label>
    <input type="number" id="quietStartB24" min="0" max="23">
    <label style="display:inline; font-weight:normal; margin-bottom:0;">End (0-23):</label>
    <input type="number" id="quietEndB24" min="0" max="23">
  </div>
  <div class="form-group-inline qh-12" style="display: none; margin-top: 15px;">
    <label style="display:inline; font-weight:normal; margin-bottom:0;">Start:</label>
    <select id="quietStartBHour12"><option>1</option><option>2</option><option>3</option><option>4</option><option>5</option><option>6</option><option>7</option><option>8</option><option>9</option><option>10</option><option>11</option><option>12</option></select>
    <select id="quietStartBAmPm"><option>AM</option><option>PM</option></select>
    <label style="display:inline; font-weight:normal; margin-bottom:0;">End:</label>
    <select id="quietEndBHour12"><option>1</option><option>2</option><option>3</option><option>4</option><option>5</option><option>6</option><option>7</option><option>8</option><option>9</option><option>10</option><option>11</option><option>12</option></select>
    <select id="quietEndBAmPm"><option>AM</option><option>PM</option></select>
  </div>
</div>

<button type="submit" class="btn btn-primary" style="margin-top:20px;">Save All Settings & Use NTP</button>
</form>

<h3>Manual Time Set</h3>
<form id="manualTimeForm">
<div class="form-group-inline">
<label for="manualHour" style="display:inline; font-weight:normal; margin-bottom:0;">Hour (0-23):</label>
<input type="number" id="manualHour" name="manualHour" min="0" max="23">
<label for="manualMinute" style="display:inline; font-weight:normal; margin-bottom:0;">Minute (0-59):</label>
<input type="number" id="manualMinute" name="manualMinute" min="0" max="59">
</div>
<button type="submit" class="btn btn-secondary">Set Time Manually & Use It</button>
</form>

<h3>System</h3>
<div class="info">OTA Updates are enabled. Upload new firmware from the Arduino IDE via the network port (splitflapclock.local).</div>
<button onclick="toggleLogViewer()" class="btn btn-secondary">View Error Log</button>
<button onclick="clearLogs()" class="btn btn-danger">Clear Error Log</button>
<button onclick="restartClock()" class="btn btn-danger">Restart Clock</button>
<div id="log-viewer"><pre id="log-content">Loading logs...</pre></div>
<div id="message"></div>
</div>
<script>
function updateTime() {
    fetch('/getTime')
    .then(response => response.text())
    .then(data => { document.getElementById('time-display').textContent = data; });
}

function from24to12(hour24) {
    const ampm = hour24 >= 12 ? 'PM' : 'AM';
    let hour12 = hour24 % 12;
    if (hour12 === 0) { hour12 = 12; }
    return { hour12, ampm };
}

function from12to24(hour12, ampm) {
    hour12 = parseInt(hour12, 10);
    if (ampm === 'AM' && hour12 === 12) { return 0; } // Midnight case
    if (ampm === 'PM' && hour12 < 12) { return hour12 + 12; }
    return hour12;
}

function setQuietHoursUI(is24HourMode) {
    document.querySelectorAll('.qh-24').forEach(el => el.style.display = is24HourMode ? 'flex' : 'none');
    document.querySelectorAll('.qh-12').forEach(el => el.style.display = is24HourMode ? 'none' : 'flex');
}

function loadSettings() {
    fetch('/getSettings')
    .then(response => response.json())
    .then(data => {
        document.getElementById('timezone').value = data.timezone;
        document.getElementById('is24hour').checked = data.is24hour;
        document.getElementById('isDst').checked = data.isDst;
        document.getElementById('quietEnabled').checked = data.quietEnabled;

        // --- Schedule A Population ---
        document.getElementById('quietStartA24').value = data.quietStartA;
        document.getElementById('quietEndA24').value = data.quietEndA;
        const startA12 = from24to12(data.quietStartA);
        document.getElementById('quietStartAHour12').value = startA12.hour12;
        document.getElementById('quietStartAAmPm').value = startA12.ampm;
        const endA12 = from24to12(data.quietEndA);
        document.getElementById('quietEndAHour12').value = endA12.hour12;
        document.getElementById('quietEndAAmPm').value = endA12.ampm;
        
        document.querySelectorAll('#day-selector-A .day-button').forEach(button => {
            const day = parseInt(button.dataset.day, 10);
            if ((data.quietDaysA >> day) & 1) {
                button.classList.add('active');
            } else {
                button.classList.remove('active');
            }
        });

        // --- Schedule B Population ---
        document.getElementById('quietStartB24').value = data.quietStartB;
        document.getElementById('quietEndB24').value = data.quietEndB;
        const startB12 = from24to12(data.quietStartB);
        document.getElementById('quietStartBHour12').value = startB12.hour12;
        document.getElementById('quietStartBAmPm').value = startB12.ampm;
        const endB12 = from24to12(data.quietEndB);
        document.getElementById('quietEndBHour12').value = endB12.hour12;
        document.getElementById('quietEndBAmPm').value = endB12.ampm;

        document.querySelectorAll('#day-selector-B .day-button').forEach(button => {
            const day = parseInt(button.dataset.day, 10);
            if ((data.quietDaysB >> day) & 1) {
                button.classList.add('active');
            } else {
                button.classList.remove('active');
            }
        });

        // Set UI visibility
        setQuietHoursUI(data.is24hour);

        const now = new Date();
        document.getElementById('manualHour').value = data.manualHour || now.getHours();
        document.getElementById('manualMinute').value = data.manualMinute || now.getMinutes();
    });
}

function toggleLogViewer() {
    const viewer = document.getElementById('log-viewer');
    const content = document.getElementById('log-content');
    if (viewer.style.display === 'block') {
        viewer.style.display = 'none';
    } else {
        viewer.style.display = 'block';
        content.textContent = 'Loading logs...';
        fetch('/getLogs')
        .then(response => response.text())
        .then(data => { content.textContent = data; });
    }
}

function clearLogs() {
    const messageDiv = document.getElementById('message');
    if (confirm('Are you sure you want to clear all log entries? This cannot be undone.')) {
        fetch('/clearLogs').then(response => {
            if (response.ok) {
                messageDiv.textContent = 'Logs cleared successfully.';
                messageDiv.className = 'success';
                if (document.getElementById('log-viewer').style.display === 'block') {
                    document.getElementById('log-content').textContent = 'Logs have been cleared.';
                }
            } else {
                messageDiv.textContent = 'Failed to clear logs.';
                messageDiv.className = 'error';
            }
            messageDiv.style.display = 'block';
        });
    }
}

function restartClock() {
    const messageDiv = document.getElementById('message');
    if (confirm('Are you sure you want to restart the clock?')) {
        fetch('/restart').then(() => {
            messageDiv.textContent = 'Clock is restarting...';
            messageDiv.className = 'success';
            messageDiv.style.display = 'block';
        });
    }
}

function setupDaySelector(selectorId) {
    document.querySelectorAll(`#${selectorId} .day-button`).forEach(button => {
        button.addEventListener('click', () => {
            button.classList.toggle('active');
        });
    });
}

document.addEventListener('DOMContentLoaded', function() {
    loadSettings();
    updateTime();
    setInterval(updateTime, 5000);
    setupDaySelector('day-selector-A');
    setupDaySelector('day-selector-B');

    // Event listener to toggle Quiet Hours UI
    document.getElementById('is24hour').addEventListener('change', function(event) {
        setQuietHoursUI(event.target.checked);
    });
});

document.getElementById('settingsForm').addEventListener('submit', function(event) {
    event.preventDefault();
    const is24hour = document.getElementById('is24hour').checked;
    
    // --- Schedule A data gathering ---
    let quietStartA, quietEndA;
    if (is24hour) {
        quietStartA = document.getElementById('quietStartA24').value;
        quietEndA = document.getElementById('quietEndA24').value;
    } else {
        quietStartA = from12to24(document.getElementById('quietStartAHour12').value, document.getElementById('quietStartAAmPm').value);
        quietEndA = from12to24(document.getElementById('quietEndAHour12').value, document.getElementById('quietEndAAmPm').value);
    }
    let quietDaysA = 0;
    document.querySelectorAll('#day-selector-A .day-button.active').forEach(button => {
        quietDaysA |= (1 << parseInt(button.dataset.day, 10));
    });

    // --- Schedule B data gathering ---
    let quietStartB, quietEndB;
    if (is24hour) {
        quietStartB = document.getElementById('quietStartB24').value;
        quietEndB = document.getElementById('quietEndB24').value;
    } else {
        quietStartB = from12to24(document.getElementById('quietStartBHour12').value, document.getElementById('quietStartBAmPm').value);
        quietEndB = from12to24(document.getElementById('quietEndBHour12').value, document.getElementById('quietEndBAmPm').value);
    }
    let quietDaysB = 0;
    document.querySelectorAll('#day-selector-B .day-button.active').forEach(button => {
        quietDaysB |= (1 << parseInt(button.dataset.day, 10));
    });
    
    const timezone = document.getElementById('timezone').value;
    const isDst = document.getElementById('isDst').checked;
    const quietEnabled = document.getElementById('quietEnabled').checked;
    const messageDiv = document.getElementById('message');
    
    const url = `/save?timezone=${timezone}&is24hour=${is24hour}&isDst=${isDst}&quietEnabled=${quietEnabled}` +
                `&quietStartA=${quietStartA}&quietEndA=${quietEndA}&quietDaysA=${quietDaysA}` +
                `&quietStartB=${quietStartB}&quietEndB=${quietEndB}&quietDaysB=${quietDaysB}`;

    fetch(url).then(response => {
        if(response.ok) {
            messageDiv.textContent = 'Settings saved! Clock is re-homing and switching to NTP time...';
            messageDiv.className = 'success';
        } else {
            response.text().then(text => {
                messageDiv.textContent = 'Failed to save settings: ' + text;
                messageDiv.className = 'error';
            });
        }
        messageDiv.style.display = 'block';
    });
});

document.getElementById('manualTimeForm').addEventListener('submit', function(event) {
    event.preventDefault();
    const hour = document.getElementById('manualHour').value;
    const minute = document.getElementById('manualMinute').value;
    const messageDiv = document.getElementById('message');
    
    fetch(`/setManualTime?hour=${hour}&minute=${minute}`).then(response => {
        if(response.ok) {
            messageDiv.textContent = 'Manual time set! Clock is re-homing...';
            messageDiv.className = 'success';
        } else {
            response.text().then(text => {
                messageDiv.textContent = 'Failed to set time: ' + text;
                messageDiv.className = 'error';
            });
        }
        messageDiv.style.display = 'block';
    });
});
</script>
</body></html>
)rawliteral";

// --- Stepper Motor Class (Unchanged) ---
class StepperMotor {
public:
    StepperMotor(int in1, int in2, int in3, int in4, int stepsPerRev, bool reverseDirection = false) {
        _pins[0] = in1; _pins[1] = in2; _pins[2] = in3; _pins[3] = in4;
        _stepsPerRevolution = stepsPerRev;
        _reverseDirection = reverseDirection;
        _isMoving = false;
        for (int i = 0; i < 4; i++) {
            pinMode(_pins[i], OUTPUT);
            digitalWrite(_pins[i], LOW);
        }
    }

    void setSpeed(int stepsPerSecond) {
        if (stepsPerSecond > 0) _stepInterval = 1000000L / stepsPerSecond;
        else _stepInterval = 0;
    }

    void moveSteps(long numSteps) {
        _targetSteps = abs(numSteps);
        _stepsMoved = 0;
        if (_targetSteps > 0) {
            _isMoving = true;
            _direction = (numSteps > 0) ? 1 : -1;
            if (_reverseDirection) _direction *= -1;
            _lastStepTime = micros();
        }
    }

    void stop() {
        _isMoving = false;
        for (int i = 0; i < 4; i++) digitalWrite(_pins[i], LOW);
    }

    bool update() {
        if (!_isMoving) return false;
        if (micros() - _lastStepTime >= _stepInterval) {
            _lastStepTime += _stepInterval;
            _currentStepInSequence += _direction;
            if (_currentStepInSequence >= 8) _currentStepInSequence = 0;
            else if (_currentStepInSequence < 0) _currentStepInSequence = 7;
            setMotorPins(_currentStepInSequence);
            _stepsMoved++;
            if (_stepsMoved >= _targetSteps) {
                stop();
                return false;
            }
        }
        return true;
    }

    bool isMoving() { return _isMoving; }

private:
    int _pins[4];
    int _stepsPerRevolution;
    int _currentStepInSequence = 0;
    long _targetSteps = 0;
    long _stepsMoved = 0;
    int _direction = 1;
    bool _isMoving;
    unsigned long _lastStepTime = 0;
    unsigned long _stepInterval = 0;
    bool _reverseDirection;
    const int _stepSequence[8][4] = {
        {1, 0, 0, 0}, {1, 1, 0, 0}, {0, 1, 0, 0}, {0, 1, 1, 0},
        {0, 0, 1, 0}, {0, 0, 1, 1}, {0, 0, 0, 1}, {1, 0, 0, 1}
    };
    void setMotorPins(int seqIdx) {
        for (int i = 0; i < 4; i++) digitalWrite(_pins[i], _stepSequence[seqIdx][i]);
    }
};

// --- Split-Flap Clock Class (Unchanged) ---
class SplitFlapClock {
public:
    enum ClockState { IDLE, RUNNING, CALIBRATION, MINUTE_ALIGN, QUIET_MODE, ERROR_STATE };

    SplitFlapClock(StepperMotor& minMotor, StepperMotor& hrMotor, int minEndstop, int hrEndstop)
    : _minutesMotor(minMotor), _hoursMotor(hrMotor) {
        _minEndstopPin = minEndstop;
        _hrEndstopPin = hrEndstop;
        pinMode(_minEndstopPin, INPUT_PULLUP);
        pinMode(_hrEndstopPin, INPUT_PULLUP);
    }

    void update() {
        if (_state != ERROR_STATE) {
            _minutesMotor.update();
            _hoursMotor.update();
        }
        
        if (_state == RUNNING) {
            if (isInQuietHours()) {
                Serial.println("Entering Quiet Hours.");
                Logger::add("Entering Quiet Hours.");
                _state = QUIET_MODE;
                return;
            }
            handleRunning();
        } else if (_state == MINUTE_ALIGN) {
            handleMinuteAlign();
        } else if (_state == QUIET_MODE) {
            if (!isInQuietHours()) {
                Serial.println("Exiting Quiet Hours. Catching up...");
                Logger::add("Exiting Quiet Hours. Catching up.");
                _state = IDLE;
                moveToCurrentTime();
                _state = RUNNING;
                Serial.println("Catch up complete. Resuming normal operation.");
            }
        }
    }

    bool initialize() {
        _state = IDLE;
        Serial.println("--- Starting Clock Initialization ---");

        bool minHomed = homeMotor(_minutesMotor, _minEndstopPin, "Minutes");
        if(minHomed) {
            _currentMinuteFlap = MINUTE_HOME_OFFSET;
            _minuteStepError = 0.0f;
        }

        bool hrHomed = homeMotor(_hoursMotor, _hrEndstopPin, "Hours");
        if(hrHomed) {
            _currentHourFlap = HOUR_HOME_POSITION;
            _hourStepError = 0.0f;
        }

        if (!minHomed) {
            _state = ERROR_STATE;
            String errorMsg = "FATAL: Minute homing failed.";
            Serial.println("!!! " + errorMsg + " Displaying error code.");
            Logger::add(errorMsg);
            if (hrHomed) {
                moveToAbsoluteFlap(_hoursMotor, _currentHourFlap, _hourStepError, g_hrStepsPerFlap, ERROR_CODE_MIN_HOME_FAIL_HR, 24);
                moveToAbsoluteFlap(_minutesMotor, _currentMinuteFlap, _minuteStepError, g_minStepsPerFlap, ERROR_CODE_MIN_HOME_FAIL_MIN, 60);
            }
            return false;
        }
        if (!hrHomed) {
            _state = ERROR_STATE;
            String errorMsg = "FATAL: Hour homing failed.";
            Serial.println("!!! " + errorMsg + " Displaying error code.");
            Logger::add(errorMsg);
            moveToAbsoluteFlap(_minutesMotor, _currentMinuteFlap, _minuteStepError, g_minStepsPerFlap, ERROR_CODE_HR_HOME_FAIL_MIN, 60);
            moveToAbsoluteFlap(_hoursMotor, _currentHourFlap, _hourStepError, g_hrStepsPerFlap, ERROR_CODE_HR_HOME_FAIL_HR, 24);
            return false;
        }

        #if CALIBRATION_MODE
            enterCalibrationMode();
        #else
            if (isInQuietHours()) {
                Serial.println("Initializing into Quiet Hours. Flaps will not move until quiet hours end.");
                Logger::add("Initialized into Quiet Hours.");
                _state = QUIET_MODE;
            } else {
                moveToCurrentTime();
                _state = RUNNING;
                Serial.println("--- Initialization Complete. Clock is now running. ---");
            }
        #endif
        return true;
    }
    
    void displaySystemError(int hourFlapCode, int minuteFlapCode) {
        String errorMsg = "SYSTEM ERROR: Displaying H:" + String(hourFlapCode) + " M:" + String(minuteFlapCode);
        Serial.println("!!! " + errorMsg);
        Logger::add(errorMsg);
        _state = IDLE;
        
        bool minHomed = homeMotor(_minutesMotor, _minEndstopPin, "Minutes (Error recovery)");
        bool hrHomed = homeMotor(_hoursMotor, _hrEndstopPin, "Hours (Error recovery)");

        _state = ERROR_STATE;

        if (minHomed && hrHomed) {
            _currentMinuteFlap = MINUTE_HOME_OFFSET;
            _minuteStepError = 0.0f;
            _currentHourFlap = HOUR_HOME_POSITION;
            _hourStepError = 0.0f;
            moveToAbsoluteFlap(_hoursMotor, _currentHourFlap, _hourStepError, g_hrStepsPerFlap, hourFlapCode, 24);
            moveToAbsoluteFlap(_minutesMotor, _currentMinuteFlap, _minuteStepError, g_minStepsPerFlap, minuteFlapCode, 60);
        } else {
            String fatalMsg = "Catastrophic failure: Could not home motors to display error code.";
            Serial.println("!!! " + fatalMsg);
            Logger::add(fatalMsg);
        }
    }

    void enterCalibrationMode() {
        _state = CALIBRATION;
        Serial.println("\n--- Entering Calibration Mode ---");
        // ... (calibration code unchanged)
    }
    
    void calibrateMoveMinute() { if (_state == CALIBRATION) moveFlaps(_minutesMotor, _minuteStepError, g_minStepsPerFlap, 1); }
    void calibrateMoveHour() { if (_state == CALIBRATION) moveFlaps(_hoursMotor, _hourStepError, g_hrStepsPerFlap, 1); }
    void calibrateMoveMinute10() { if (_state == CALIBRATION) moveFlaps(_minutesMotor, _minuteStepError, g_minStepsPerFlap, 10); }
    void calibrateMoveHour10() { if (_state == CALIBRATION) moveFlaps(_hoursMotor, _hourStepError, g_hrStepsPerFlap, 10); }
    void showEndstopStates() {
        Serial.printf("Minutes Endstop: %s\n", digitalRead(_minEndstopPin) == LOW ? "TRIGGERED" : "Open");
        Serial.printf("Hours Endstop: %s\n", digitalRead(_hrEndstopPin) == LOW ? "TRIGGERED" : "Open");
    }

private:
    StepperMotor& _minutesMotor;
    StepperMotor& _hoursMotor;
    int _minEndstopPin, _hrEndstopPin;
    int _currentMinuteFlap = 0, _currentHourFlap = 0;
    ClockState _state = IDLE;
    float _minuteStepError = 0.0f;
    float _hourStepError = 0.0f;

    bool homeMotor(StepperMotor& motor, int endstopPin, const char* name) {
        Serial.printf("Homing %s motor...\n", name);
        motor.setSpeed(HOMING_SPEED_STEPS_PER_SEC);
        motor.moveSteps(MOTOR_STEPS_PER_REVOLUTION * 3);

        unsigned long startTime = millis();
        while (motor.isMoving()) {
            motor.update();
            if (digitalRead(endstopPin) == LOW) {
                motor.stop();
                Serial.printf("%s endstop triggered. Motor homed successfully.\n", name);
                return true;
            }
            if (millis() - startTime > 15000) { // 15 second timeout
                motor.stop();
                String errorMsg = "HOMING FAILED: " + String(name) + " endstop not found within timeout.";
                Serial.println("!!! " + errorMsg + " !!!");
                Logger::add(errorMsg);
                return false;
            }
        }
        
        String errorMsg = "HOMING FAILED: " + String(name) + " endstop not found.";
        Serial.println("!!! " + errorMsg + " !!!");
        Logger::add(errorMsg);
        return false;
    }

    void moveFlaps(StepperMotor& motor, float& errorAccumulator, float stepsPerFlap, int flapsToMove) {
        if (flapsToMove <= 0) return;

        for (int i = 0; i < flapsToMove; ++i) {
            float idealSteps = stepsPerFlap + errorAccumulator;
            long actualSteps = round(idealSteps);
            errorAccumulator = idealSteps - actualSteps;

            motor.moveSteps(actualSteps);

            while(motor.isMoving()) {
                motor.update();
                ArduinoOTA.handle(); 
                yield(); 
            }
        }
    }
    
    void moveToAbsoluteFlap(StepperMotor& motor, int& currentFlap, float& errorAccumulator, float stepsPerFlap, int targetFlap, int totalFlapsOnWheel) {
        int flapsToMove = targetFlap - currentFlap;
        if (flapsToMove < 0) {
            flapsToMove += totalFlapsOnWheel;
        }
        moveFlaps(motor, errorAccumulator, stepsPerFlap, flapsToMove);
        currentFlap = targetFlap;
    }

    int getTargetHourFlap(int hour24) {
        if (g_is24HourDisplay) {
            return hour24;
        } else {
            if (hour24 == 0 || hour24 == 12) return 12; // Midnight and Noon are 12
            if (hour24 > 12) return hour24 - 12;
            return hour24;
        }
    }

    void moveToCurrentTime() {
        if (!g_isManualTimeMode) {
            timeClient.update();
        }
        int currentMinute = getCurrentMinute();
        int currentHour = getCurrentHour();

        int targetHourFlap = getTargetHourFlap(currentHour);
        int targetMinuteFlap = currentMinute;

        Serial.printf("Target: %02d:%02d. Moving to M:%d, H:%d\n", currentHour, currentMinute, targetMinuteFlap, targetHourFlap);

        _minutesMotor.setSpeed(NORMAL_SPEED_STEPS_PER_SEC);
        _hoursMotor.setSpeed(NORMAL_SPEED_STEPS_PER_SEC);
        
        moveToAbsoluteFlap(_minutesMotor, _currentMinuteFlap, _minuteStepError, g_minStepsPerFlap, targetMinuteFlap, 60);
        moveToAbsoluteFlap(_hoursMotor, _currentHourFlap, _hourStepError, g_hrStepsPerFlap, targetHourFlap, 24);
    }
    
    bool checkForDrift() {
        if (_minutesMotor.isMoving() || _hoursMotor.isMoving()) return false;
        if (digitalRead(_minEndstopPin) == LOW && _currentMinuteFlap != MINUTE_HOME_OFFSET) {
            String errorMsg = "DRIFT DETECTED on minutes. Expected " + String(_currentMinuteFlap) + " but found home. Re-homing...";
            Serial.println("!!! " + errorMsg + " !!!");
            Logger::add(errorMsg);
            initialize();
            return true;
        }
        if (digitalRead(_hrEndstopPin) == LOW && _currentHourFlap != HOUR_HOME_POSITION) {
            String errorMsg = "DRIFT DETECTED on hours. Expected flap " + String(_currentHourFlap) + " but found home. Re-homing...";
            Serial.println("!!! " + errorMsg + " !!!");
            Logger::add(errorMsg);
            initialize();
            return true;
        }
        return false;
    }

    void handleMinuteAlign() {
        if (digitalRead(_minEndstopPin) == LOW) {
            _minutesMotor.stop();
            _currentMinuteFlap = MINUTE_HOME_OFFSET;
            _minuteStepError = 0.0f;
            Serial.println("59->00 alignment complete.");
            _state = RUNNING;
            handleHourChange();
        } else if (!_minutesMotor.isMoving()) {
            _minutesMotor.moveSteps(MOTOR_STEPS_PER_REVOLUTION * 2);
        }
    }

    void handleHourChange() {
        int currentHour = getCurrentHour();
        int targetHourFlap = getTargetHourFlap(currentHour);

        if (targetHourFlap != _currentHourFlap) {
            int hoursToMove = targetHourFlap - _currentHourFlap;
            if (hoursToMove < 0) hoursToMove += 24;

            Serial.printf("Hour change: Moving %d flaps from %d to %d\n", hoursToMove, _currentHourFlap, targetHourFlap);
            moveFlaps(_hoursMotor, _hourStepError, g_hrStepsPerFlap, hoursToMove);
            _currentHourFlap = targetHourFlap;
        }
    }

    void handleRunning() {
        if (checkForDrift()) return;

        if (!g_isManualTimeMode) {
            timeClient.update();
        }
        int currentMinute = getCurrentMinute();

        if (currentMinute == _currentMinuteFlap) return;

        if (currentMinute == 0 && _currentMinuteFlap == 59) {
            int currentHour = getCurrentHour();
            bool needsFullRehome = false;

            if (g_is24HourDisplay && currentHour == 0) {
                needsFullRehome = true;
                Logger::add("Midnight re-home triggered.");
            } 
            else if (!g_is24HourDisplay && (currentHour == 1 || currentHour == 13)) {
                needsFullRehome = true;
                Logger::add("12->1 transition re-home triggered.");
            }

            if (needsFullRehome) {
                Serial.printf("Scheduled re-homing at %02d:00. Re-initializing clock...\n", currentHour);
                initialize();
                return;
            }

            Serial.println("Top of the hour: Performing minute re-homing alignment.");
            _state = MINUTE_ALIGN;
            _minutesMotor.setSpeed(HOMING_SPEED_STEPS_PER_SEC);
            _minutesMotor.moveSteps(MOTOR_STEPS_PER_REVOLUTION * 2);
            return;
        }

        moveFlaps(_minutesMotor, _minuteStepError, g_minStepsPerFlap, 1);
        _currentMinuteFlap = currentMinute;
    }
};

// --- Global Instances ---
StepperMotor minutesMotor(MIN_MOTOR_IN1_PIN, MIN_MOTOR_IN2_PIN, MIN_MOTOR_IN3_PIN, MIN_MOTOR_IN4_PIN, MOTOR_STEPS_PER_REVOLUTION, true);
StepperMotor hoursMotor(HR_MOTOR_IN1_PIN, HR_MOTOR_IN2_PIN, HR_MOTOR_IN3_PIN, HR_MOTOR_IN4_PIN, MOTOR_STEPS_PER_REVOLUTION, false);
SplitFlapClock splitFlapClock(minutesMotor, hoursMotor, MIN_ENDSTOP_PIN, HR_ENDSTOP_PIN);

// Variable to track scheduled re-homing to prevent multiple triggers in the same minute
int lastRehomeCheckMinute = -1;


// --- Web Server & System Setup Functions ---
void setupWebServer() {
    server.on("/", HTTP_GET, [](){
        server.send_P(200, "text/html", index_html);
    });

    server.on("/getTime", HTTP_GET, [](){
        server.send(200, "text/plain", getFormattedTime());
    });

    server.on("/getSettings", HTTP_GET, [](){
        String json = "{";
        json += "\"timezone\":" + String(g_timezoneBaseOffset) + ",";
        json += "\"is24hour\":" + String(g_is24HourDisplay ? "true" : "false") + ",";
        json += "\"isDst\":" + String(g_isDst ? "true" : "false") + ",";
        json += "\"quietEnabled\":" + String(g_quietHoursEnabled ? "true" : "false") + ",";
        json += "\"quietStartA\":" + String(g_quietStartA) + ",";
        json += "\"quietEndA\":" + String(g_quietEndA) + ",";
        json += "\"quietDaysA\":" + String(g_quietDaysA) + ",";
        json += "\"quietStartB\":" + String(g_quietStartB) + ",";
        json += "\"quietEndB\":" + String(g_quietEndB) + ",";
        json += "\"quietDaysB\":" + String(g_quietDaysB) + ",";
        json += "\"manualHour\":" + String(g_manualHour) + ",";
        json += "\"manualMinute\":" + String(g_manualMinute);
        json += "}";
        server.send(200, "application/json", json);
    });

    server.on("/save", HTTP_GET, [](){
        g_isManualTimeMode = false; 
        
        if(server.hasArg("timezone")) {
            g_timezoneBaseOffset = server.arg("timezone").toInt();
            preferences.putLong("timezone", g_timezoneBaseOffset);
        }
        if(server.hasArg("is24hour")) {
            g_is24HourDisplay = server.arg("is24hour") == "true";
            preferences.putBool("is24hour", g_is24HourDisplay);
        }
        if(server.hasArg("isDst")) {
            g_isDst = server.arg("isDst") == "true";
            preferences.putBool("isDst", g_isDst);
        }
        // New Quiet Hours Args
        if(server.hasArg("quietEnabled")) {
            g_quietHoursEnabled = server.arg("quietEnabled") == "true";
            preferences.putBool("quietEn", g_quietHoursEnabled);
        }
        if(server.hasArg("quietStartA")) {
            g_quietStartA = server.arg("quietStartA").toInt();
            preferences.putInt("qStartA", g_quietStartA);
        }
        if(server.hasArg("quietEndA")) {
            g_quietEndA = server.arg("quietEndA").toInt();
            preferences.putInt("qEndA", g_quietEndA);
        }
        if(server.hasArg("quietDaysA")) {
            g_quietDaysA = (uint8_t)server.arg("quietDaysA").toInt();
            preferences.putUChar("qDaysA", g_quietDaysA);
        }
        if(server.hasArg("quietStartB")) {
            g_quietStartB = server.arg("quietStartB").toInt();
            preferences.putInt("qStartB", g_quietStartB);
        }
        if(server.hasArg("quietEndB")) {
            g_quietEndB = server.arg("quietEndB").toInt();
            preferences.putInt("qEndB", g_quietEndB);
        }
        if(server.hasArg("quietDaysB")) {
            g_quietDaysB = (uint8_t)server.arg("quietDaysB").toInt();
            preferences.putUChar("qDaysB", g_quietDaysB);
        }

        g_ntpOffsetSeconds = g_timezoneBaseOffset + (g_isDst ? 3600 : 0);
        timeClient.setTimeOffset(g_ntpOffsetSeconds);
        
        if (splitFlapClock.initialize()) {
            server.send(200, "text/plain", "OK");
        } else {
            server.send(500, "text/plain", "Homing failed after save. Check clock mechanism.");
        }
    });

    server.on("/setManualTime", HTTP_GET, [](){
        g_isManualTimeMode = true;
        if(server.hasArg("hour")) {
            g_manualHour = server.arg("hour").toInt();
        }
        if(server.hasArg("minute")) {
            g_manualMinute = server.arg("minute").toInt();
        }
        g_lastMinuteTick = millis(); 

        Logger::add("Switched to Manual Time: " + String(g_manualHour) + ":" + String(g_manualMinute));

        if (splitFlapClock.initialize()) {
            server.send(200, "text/plain", "OK");
        } else {
            server.send(500, "text/plain", "Homing failed after setting manual time.");
        }
    });

    server.on("/getLogs", HTTP_GET, [](){
        server.send(200, "text/plain", Logger::getAll());
    });

    server.on("/clearLogs", HTTP_GET, [](){
        Logger::clear();
        server.send(200, "text/plain", "Logs cleared.");
    });

    server.on("/restart", HTTP_GET, [](){
        server.send(200, "text/plain", "Restarting...");
        delay(1000);
        ESP.restart();
    });

    server.begin();
    Serial.println("Web server started.");
}

void setupOTA() {
    ArduinoOTA.setHostname("splitflapclock");
    ArduinoOTA.onStart([]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH) { type = "sketch"; } else { type = "filesystem"; }
        Serial.println("Start updating " + type);
        Logger::add("OTA Update Started...");
    }).onEnd([]() {
        Serial.println("\nEnd");
        Logger::add("OTA Update Finished.");
    }).onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    }).onError([](ota_error_t error) {
        Serial.printf("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR) Serial.println("End Failed");
        Logger::add("OTA Error: " + String(error));
    });
    ArduinoOTA.begin();
    Serial.println("OTA Ready.");
}

void loadSettings() {
    preferences.begin("clock-settings", false);
    g_timezoneBaseOffset = preferences.getLong("timezone", -21600);
    g_is24HourDisplay = preferences.getBool("is24hour", false);
    g_isDst = preferences.getBool("isDst", true);

    // Load new quiet hours settings
    g_quietHoursEnabled = preferences.getBool("quietEn", false);
    g_quietStartA = preferences.getInt("qStartA", 21);
    g_quietEndA = preferences.getInt("qEndA", 7);
    g_quietDaysA = preferences.getUChar("qDaysA", 0b00011110); // Mon-Thu
    g_quietStartB = preferences.getInt("qStartB", 23);
    g_quietEndB = preferences.getInt("qEndB", 10);
    g_quietDaysB = preferences.getUChar("qDaysB", 0b01100001); // Fri, Sat, Sun
    
    preferences.end();

    g_ntpOffsetSeconds = g_timezoneBaseOffset + (g_isDst ? 3600 : 0);

    Serial.printf("Loaded settings: TZ Base=%ld, DST=%s -> Final Offset=%ld, 24-Hour=%s\n",
        g_timezoneBaseOffset, g_isDst ? "Yes" : "No", g_ntpOffsetSeconds, g_is24HourDisplay ? "Yes" : "No");
    Serial.printf("Quiet Hours: Enabled=%s\n", g_quietHoursEnabled ? "Yes" : "No");
}

void setup() {
    Serial.begin(115200);
    Serial.println("\nStarting Split-Flap Clock v2.3...");
    
    loadSettings();

    wifiManager.setAPCallback([](WiFiManager* wm) { Serial.println("Entered WiFiManager AP mode."); });
    if (!wifiManager.autoConnect(WIFI_MANAGER_AP_NAME)) {
        Serial.println("Failed to connect to WiFi. Halting with error code.");
        Logger::add("FATAL: WiFi connection failed.");
        splitFlapClock.displaySystemError(ERROR_CODE_NO_WIFI_HR, ERROR_CODE_NO_WIFI_MIN);
        while(1) { yield(); }
    }
    Serial.println("WiFi Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    if (MDNS.begin("splitflapclock")) {
        Serial.println("mDNS responder started. Access at http://splitflapclock.local");
    } else {
        Serial.println("Error setting up MDNS responder! Halting with error code.");
        Logger::add("FATAL: mDNS setup failed.");
        splitFlapClock.displaySystemError(ERROR_CODE_WEB_FAIL_HR, ERROR_CODE_WEB_FAIL_MIN);
        while(1) { yield(); }
    }

    timeClient.setTimeOffset(g_ntpOffsetSeconds);
    timeClient.begin();

    Serial.print("Syncing NTP time...");
    while (!timeClient.update()) {
        timeClient.forceUpdate();
        Serial.print(".");
        delay(500);
    }
    Serial.print("\nNTP Time Synced: ");
    Serial.println(timeClient.getFormattedTime());
    Logger::add("System Started. NTP time synced.");

    setupWebServer();
    setupOTA();

    if (!splitFlapClock.initialize()) {
        Serial.println("Clock initialization failed. Check serial monitor for details. Halting.");
        while(1) { ArduinoOTA.handle(); yield(); }
    }
}

void loop() {
    ArduinoOTA.handle();
    server.handleClient();
    splitFlapClock.update();

    // Scheduled Re-homing Logic
    // This block checks the time and triggers a full re-homing sequence at
    // 8:30 AM (08:30) and 8:30 PM (20:30).
    int currentHour = getCurrentHour();
    int currentMinute = getCurrentMinute();

    // We only perform the check when the minute changes to ensure the action
    // is triggered only once, not repeatedly for 60 seconds.
    if (currentMinute != lastRehomeCheckMinute) {
        
        // Check if the current time matches either of the scheduled re-homing times.
        if ((currentHour == 8 && currentMinute == 30) || (currentHour == 20 && currentMinute == 30)) {
            Serial.printf("Scheduled re-homing triggered at %02d:%02d. Re-initializing clock...\n", currentHour, currentMinute);
            Logger::add("Scheduled re-home triggered.");
            
            // Calling initialize() performs a full re-home of both motors and
            // then moves them to the correct current time.
            splitFlapClock.initialize();
        }
        
        // Update the tracking variable to the current minute so this logic block
        // won't execute again until the next minute.
        lastRehomeCheckMinute = currentMinute;
    }


    if (g_isManualTimeMode) {
        if (millis() - g_lastMinuteTick >= 60000) {
            g_lastMinuteTick += 60000;
            g_manualMinute++;
            if (g_manualMinute >= 60) {
                g_manualMinute = 0;
                g_manualHour++;
                if (g_manualHour >= 24) {
                    g_manualHour = 0;
                }
            }
        }
    }

    #if CALIBRATION_MODE
    if (Serial.available()) {
        // ... (calibration code unchanged)
    }
    #endif
}
