#include <WiFiManager.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <math.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>

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
#define HOMING_SPEED_STEPS_PER_SEC 420
#define NORMAL_SPEED_STEPS_PER_SEC 420

// Time constants
#define NTP_UPDATE_INTERVAL_MS 30000

// --- Calibration Mode Flag ---
#define CALIBRATION_MODE false

// --- Global Variables for STEPS_PER_FLAP ---
float g_minStepsPerFlap = 93.25f;
float g_hrStepsPerFlap = 93.25f;

// --- Global Variables (will be loaded from Preferences) ---
bool g_is24HourDisplay = false;
long g_timezoneBaseOffset = -21600; // Default to Mountain Time (UTC-6)
bool g_isDst = true; // Default to DST on
long g_ntpOffsetSeconds = -21600; // Calculated from base + DST

// --- Physical Clock Offset ---
#define MINUTE_HOME_OFFSET 0
#define HOUR_HOME_POSITION 0 // The physical home position for the hour flap is 0

// --- Global Objects ---
WiFiManager wifiManager;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org");
WebServer server(80);
Preferences preferences;

// --- HTML for Web Interface ---
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head>
<title>Split-Flap Clock Control</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif; margin: 0; padding: 20px; background-color: #f4f4f4; color: #333; }
.container { max-width: 600px; margin: auto; background: white; padding: 20px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }
h2, h3 { color: #0056b3; border-bottom: 2px solid #eee; padding-bottom: 10px; }
.form-group { margin-bottom: 20px; }
label { display: block; margin-bottom: 5px; font-weight: bold; }
select, .btn { padding: 12px; border-radius: 5px; border: 1px solid #ddd; width: 100%; box-sizing: border-box; font-size: 16px; }
.btn { border: none; cursor: pointer; margin-top: 10px; }
.btn-primary { background-color: #007bff; color: white; }
.btn-danger { background-color: #dc3545; color: white; }
#message { margin-top: 20px; padding: 10px; border-radius: 5px; display: none; text-align: center; }
.success { background-color: #d4edda; color: #155724; border: 1px solid #c3e6cb; }
.error { background-color: #f8d7da; color: #721c24; border: 1px solid #f5c6cb; }
#time-display { font-size: 1.5em; font-weight: bold; color: #333; text-align: center; background: #eee; padding: 15px; border-radius: 5px; }
</style>
</head><body>
<div class="container">
<h2>Clock Status</h2>
<div id="time-display">--:--:--</div>
<h2>Settings</h2>
<form id="settingsForm">
<div class="form-group">
<label for="timezone">Timezone (Standard Time)</label>
<select id="timezone" name="timezone">
<optgroup label="North America">
<option value="-36000">Hawaii (UTC-10)</option>
<option value="-32400">Alaska (UTC-9)</option>
<option value="-28800">Pacific Time (UTC-8)</option>
<option value="-25200">Mountain Time (UTC-7)</option>
<option value="-21600">Central Time (UTC-6)</option>
<option value="-18000">Eastern Time (UTC-5)</option>
<option value="-14400">Atlantic Time (UTC-4)</option>
<option value="-12600">Newfoundland (UTC-3:30)</option>
</optgroup>
<optgroup label="Europe">
<option value="0">Greenwich Mean Time (UTC+0)</option>
<option value="3600">Central European Time (UTC+1)</option>
<option value="7200">Eastern European Time (UTC+2)</option>
</optgroup>
<optgroup label="Asia & Oceania">
<option value="19800">India (UTC+5:30)</option>
<option value="20700">Nepal (UTC+5:45)</option>
<option value="28800">Western Australia (UTC+8)</option>
<option value="34200">Central Australia (UTC+9:30)</option>
<option value="36000">Eastern Australia (UTC+10)</option>
<option value="43200">New Zealand (UTC+12)</option>
</optgroup>
<optgroup label="Other">
<option value="-10800">UTC-3</option><option value="-7200">UTC-2</option><option value="-3600">UTC-1</option>
<option value="10800">UTC+3</option><option value="12600">Iran (UTC+3:30)</option>
<option value="14400">UTC+4</option><option value="16200">Afghanistan (UTC+4:30)</option>
<option value="18000">UTC+5</option><option value="21600">UTC+6</option><option value="25200">UTC+7</option>
<option value="39600">UTC+11</option>
</optgroup>
</select>
</div>
<div class="form-group">
<label>Daylight Saving</label>
<input type="checkbox" id="isDst" name="isDst" style="width:auto; vertical-align: middle;">
<label for="isDst" style="display:inline; font-weight:normal;">Daylight Saving Time is active (+1 hour)</label>
</div>
<div class="form-group">
<label>Display Format</label>
<input type="checkbox" id="is24hour" name="is24hour" style="width:auto; vertical-align: middle;">
<label for="is24hour" style="display:inline; font-weight:normal;">24-Hour Format</label>
</div>
<button type="submit" class="btn btn-primary">Save & Apply</button>
</form>
<h3>System</h3>
<button onclick="restartClock()" class="btn btn-danger">Restart Clock</button>
<div id="message"></div>
</div>
<script>
function updateTime() {
fetch('/getTime')
.then(response => response.text())
.then(data => {
document.getElementById('time-display').textContent = data;
});
}
document.addEventListener('DOMContentLoaded', function() {
fetch('/getSettings')
.then(response => response.json())
.then(data => {
document.getElementById('timezone').value = data.timezone;
document.getElementById('is24hour').checked = data.is24hour;
document.getElementById('isDst').checked = data.isDst;
});
updateTime();
setInterval(updateTime, 5000);
});
document.getElementById('settingsForm').addEventListener('submit', function(event) {
event.preventDefault();
const timezone = document.getElementById('timezone').value;
const is24hour = document.getElementById('is24hour').checked;
const isDst = document.getElementById('isDst').checked;
const messageDiv = document.getElementById('message');
fetch(`/save?timezone=${timezone}&is24hour=${is24hour}&isDst=${isDst}`)
.then(response => {
if(response.ok) {
messageDiv.textContent = 'Settings saved! Clock is re-homing...';
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
function restartClock() {
const messageDiv = document.getElementById('message');
if(confirm('Are you sure you want to restart the clock?')) {
fetch('/restart')
.then(() => {
messageDiv.textContent = 'Clock is restarting...';
messageDiv.className = 'success';
messageDiv.style.display = 'block';
});
}
}
</script>
</body></html>
)rawliteral";

// --- Stepper Motor Class ---
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

// --- Split-Flap Clock Class ---
class SplitFlapClock {
public:
    enum ClockState { IDLE, RUNNING, CALIBRATION, MINUTE_ALIGN, ERROR_STATE };

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
            handleRunning();
        } else if (_state == MINUTE_ALIGN) {
            handleMinuteAlign();
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
            Serial.println("!!! FATAL: Minute homing failed. Displaying error code.");
            if (hrHomed) {
                moveToAbsoluteFlap(_hoursMotor, _currentHourFlap, _hourStepError, g_hrStepsPerFlap, ERROR_CODE_MIN_HOME_FAIL_HR, 24);
                moveToAbsoluteFlap(_minutesMotor, _currentMinuteFlap, _minuteStepError, g_minStepsPerFlap, ERROR_CODE_MIN_HOME_FAIL_MIN, 60);
            }
            return false;
        }
        if (!hrHomed) {
            _state = ERROR_STATE;
            Serial.println("!!! FATAL: Hour homing failed. Displaying error code.");
            moveToAbsoluteFlap(_minutesMotor, _currentMinuteFlap, _minuteStepError, g_minStepsPerFlap, ERROR_CODE_HR_HOME_FAIL_MIN, 60);
            moveToAbsoluteFlap(_hoursMotor, _currentHourFlap, _hourStepError, g_hrStepsPerFlap, ERROR_CODE_HR_HOME_FAIL_HR, 24);
            return false;
        }

        #if CALIBRATION_MODE
            enterCalibrationMode();
        #else
            moveToCurrentTime();
            _state = RUNNING;
            Serial.println("--- Initialization Complete. Clock is now running. ---");
        #endif
        return true;
    }
    
    void displaySystemError(int hourFlapCode, int minuteFlapCode) {
        Serial.printf("!!! SYSTEM ERROR: Displaying H:%d M:%d\n", hourFlapCode, minuteFlapCode);
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
            Serial.println("!!! Catastrophic failure: Could not home motors to display error code.");
        }
    }

    void enterCalibrationMode() {
        _state = CALIBRATION;
        Serial.println("\n--- Entering Calibration Mode ---");
        Serial.println("Current STEPS_PER_FLAP (Minutes): " + String(g_minStepsPerFlap, 3));
        Serial.println("Current STEPS_PER_FLAP (Hours): " + String(g_hrStepsPerFlap, 3));
        Serial.println("Commands (e.g., 'sm93.5'):");
        Serial.println(" 'm'/'h': Move 1 flap | 'M'/'H': Move 10 flaps");
        Serial.println(" 'sm<value>', 'sh<value>' : Set steps per flap.");
        Serial.println(" 'se' : Show Endstop states.");
        _minutesMotor.stop();
        _hoursMotor.stop();
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

        while (motor.isMoving()) {
            motor.update();
            if (digitalRead(endstopPin) == LOW) {
                motor.stop();
                Serial.printf("%s endstop triggered. Motor homed successfully.\n", name);
                return true;
            }
        }
        
        Serial.printf("!!! HOMING FAILED: %s endstop not found. !!!\n", name);
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
                yield(); // Feeds the watchdog timer to prevent resets on long moves
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
            if (hour24 == 0) return 12;
            if (hour24 > 12) return hour24 - 12;
            return hour24;
        }
    }

    void moveToCurrentTime() {
        timeClient.update();
        int currentMinute = timeClient.getMinutes();
        int currentHour = timeClient.getHours();

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
            Serial.printf("!!! DRIFT DETECTED on minutes. Expected %d but found home. Re-homing... !!!\n", _currentMinuteFlap);
            initialize();
            return true;
        }
        if (digitalRead(_hrEndstopPin) == LOW && _currentHourFlap != HOUR_HOME_POSITION) {
            Serial.printf("!!! DRIFT DETECTED on hours. Expected flap %d but found home (flap %d). Re-homing... !!!\n", _currentHourFlap, HOUR_HOME_POSITION);
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
        int currentHour = timeClient.getHours();
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

        timeClient.update();
        int currentMinute = timeClient.getMinutes();

        if (currentMinute == _currentMinuteFlap) return;

        if (currentMinute == 0 && _currentMinuteFlap == 59) {
            int currentHour = timeClient.getHours();
            bool needsFullRehome = false;
            if (g_is24HourDisplay && currentHour == 0) {
                needsFullRehome = true;
            } else if (!g_is24HourDisplay && (currentHour == 1 || currentHour == 13)) {
                needsFullRehome = true;
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

void setupWebServer() {
    server.on("/", HTTP_GET, [](){
        server.send_P(200, "text/html", index_html);
    });

    server.on("/getTime", HTTP_GET, [](){
        server.send(200, "text/plain", timeClient.getFormattedTime());
    });

    server.on("/getSettings", HTTP_GET, [](){
        String json = "{";
        json += "\"timezone\":" + String(g_timezoneBaseOffset) + ",";
        json += "\"is24hour\":" + String(g_is24HourDisplay ? "true" : "false") + ",";
        json += "\"isDst\":" + String(g_isDst ? "true" : "false");
        json += "}";
        server.send(200, "application/json", json);
    });

    server.on("/save", HTTP_GET, [](){
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

        g_ntpOffsetSeconds = g_timezoneBaseOffset + (g_isDst ? 3600 : 0);
        timeClient.setTimeOffset(g_ntpOffsetSeconds);
        
        if (splitFlapClock.initialize()) {
            server.send(200, "text/plain", "OK");
        } else {
            server.send(500, "text/plain", "Homing failed after save. Check clock mechanism.");
        }
    });

    server.on("/restart", HTTP_GET, [](){
        server.send(200, "text/plain", "Restarting...");
        delay(1000);
        ESP.restart();
    });

    server.begin();
    Serial.println("Web server started.");
}

void loadSettings() {
    preferences.begin("clock-settings", false);
    g_timezoneBaseOffset = preferences.getLong("timezone", -21600); // Default Mountain
    g_is24HourDisplay = preferences.getBool("is24hour", false);
    g_isDst = preferences.getBool("isDst", true); // Default DST on for summer

    g_ntpOffsetSeconds = g_timezoneBaseOffset + (g_isDst ? 3600 : 0);

    Serial.printf("Loaded settings: TZ Base=%ld, DST=%s -> Final Offset=%ld, 24-Hour=%s\n",
        g_timezoneBaseOffset, g_isDst ? "Yes" : "No", g_ntpOffsetSeconds, g_is24HourDisplay ? "Yes" : "No");
}

// --- Setup Function ---
void setup() {
    Serial.begin(115200);
    Serial.println("\nStarting Split-Flap Clock...");

    loadSettings();

    wifiManager.setAPCallback([](WiFiManager* wm) { Serial.println("Entered WiFiManager AP mode."); });
    if (!wifiManager.autoConnect(WIFI_MANAGER_AP_NAME)) {
        Serial.println("Failed to connect to WiFi. Halting with error code.");
        splitFlapClock.displaySystemError(ERROR_CODE_NO_WIFI_HR, ERROR_CODE_NO_WIFI_MIN);
        while(1) { yield(); } // Halt execution but don't trigger watchdog
    }
    Serial.println("WiFi Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    if (MDNS.begin("splitflapclock")) {
        Serial.println("mDNS responder started. Access at http://splitflapclock.local");
    } else {
        Serial.println("Error setting up MDNS responder! Halting with error code.");
        splitFlapClock.displaySystemError(ERROR_CODE_WEB_FAIL_HR, ERROR_CODE_WEB_FAIL_MIN);
        while(1) { yield(); } // Halt execution
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

    setupWebServer();

    if (!splitFlapClock.initialize()) {
        Serial.println("Clock initialization failed. Check serial monitor for details. Halting.");
        while(1) { yield(); } // Halt execution
    }
}

// --- Loop Function ---
void loop() {
    server.handleClient();
    splitFlapClock.update();

    #if CALIBRATION_MODE
    if (Serial.available()) {
        String command = Serial.readStringUntil('\n');
        command.trim();
        if (command.equals("m")) {
            splitFlapClock.calibrateMoveMinute();
        } else if (command.equals("h")) {
            splitFlapClock.calibrateMoveHour();
        } else if (command.equals("M")) {
            splitFlapClock.calibrateMoveMinute10();
        } else if (command.equals("H")) {
            splitFlapClock.calibrateMoveHour10();
        } else if (command.equalsIgnoreCase("se")) {
            splitFlapClock.showEndstopStates();
        } else if (command.startsWith("sm")) {
            float newSteps = command.substring(2).toFloat();
            if (newSteps > 0) {
                g_minStepsPerFlap = newSteps;
                Serial.printf("STEPS_PER_FLAP (Minutes) set to: %.3f\n", g_minStepsPerFlap);
            }
        } else if (command.startsWith("sh")) {
            float newSteps = command.substring(2).toFloat();
            if (newSteps > 0) {
                g_hrStepsPerFlap = newSteps;
                Serial.printf("STEPS_PER_FLAP (Hours) set to: %.3f\n", g_hrStepsPerFlap);
            }
        }
    }
    #endif
}