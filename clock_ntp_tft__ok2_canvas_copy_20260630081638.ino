/*
 * ESP32-C3 Clock with OLED Display, WiFi, NTP, Alarm and Timer
 * https://www.instructables.com/ESP32-C3-042-OLED-Clock-With-WiFi-NTP-Alarm-and-Ti/
 * Features:
 * - 0.42" OLED display (72x40 pixels)
 * - WiFi connectivity with NTP time sync
 * - Flexible alarm (daily, weekdays, specific date)
 * - Countdown timer (1 second to 24 hours)
 * - Web interface for remote control
 * - Non-volatile storage (NVS) for settings
 * - Command history in serial terminal
 * - UTF-8 support (Cyrillic and Latin)
 * 
 * Hardware:
 * - ESP32-C3 SuperMini
 * - Built-in 0.42" OLED (SDA=GPIO5, SCL=GPIO6)
 * - Built-in BOOT button (GPIO9)
 * - Built-in blue LED (GPIO8, active low)
 * - HCM1203X piezo buzzer (GPIO10)
 * 
 * Author: CheshirCa
 * License: MIT
 */

//#include <U8g2lib.h>
#include <Adafruit_GFX.h>    // Include biblioteca grafică Adafruit
#include <Adafruit_ST7789.h> // Include driverul specific pentru ecranul tău
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include <Wire.h>
#include <Preferences.h>
#include <esp_timer.h>

//#include <Fonts/FreeSansBold18pt7b.h>
//#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h> // Fontul nou, mai mare pentru ceas
#include <Fonts/FreeSans9pt7b.h>

// Web Server
WebServer server(80);

#define TFT_CS   5
#define TFT_DC    16
#define TFT_RST   23
#define TFT_MOSI 19  // Data out
#define TFT_SCLK 18  // Clock out
#define bl 4

// OLED Display (72x40 visible area in 128x64 buffer)
//U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, 6, 5);
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
GFXcanvas16 canvas(240, 135); 
//const int DISP_W = 72;          // Display width
//const int DISP_H = 40;          // Display height
//const int X_OFF = 30;           // X offset in buffer
//const int Y_OFF = 20;           // Y offset in buffer

const int DISP_W = 240;          
const int DISP_H = 135;          
const int X_OFF = 10;           
const int Y_OFF = 20;           
const int INFO_Y_OFFSET = 20;  

//const int INFO_Y_OFFSET = 10;  // Info screen Y offset


// Default Configuration
//String defSSID = "your_SSID";           // Default WiFi SSID
//String defPASS = "your_PASSWORD";       // Default WiFi password
String defSSID = "your_SSID";           
String defPASS = "your_PASSWORD";  
String defNTP = "pool.ntp.org";         // Default NTP server
long defGMTOffset = 3 * 3600;           // Default GMT offset (seconds)
long defDaylightOffset = 0;             // Default DST offset (seconds)

// Active Configuration
String wifiSSID;
String wifiPASS;
String ntpServer;
long gmtOffset_sec;
long daylightOffset_sec;

// NVS Storage
Preferences prefs;
const char* PREF_NS = "clockcfg";       // NVS namespace for config
const char* ALARM_PREF_NS = "alarms";   // NVS namespace for alarms

// Time
struct tm timeinfo;
bool timeValid = false;

// UI States
enum ScreenMode {
  SCREEN_CLOCK,
  SCREEN_INFO1,
  SCREEN_INFO2,
  SCREEN_ALARM,
  SCREEN_TIMER
};
ScreenMode currentScreen = SCREEN_CLOCK;
int infoScreenPage = 1;

// Auto Return
const unsigned long INFO_TIMEOUT = 10000;  // Auto return timeout (ms)
unsigned long infoStartTime = 0;

// Blinking Colon
bool colonVisible = true;
unsigned long lastBlink = 0;

// Button
#define BOOT_PIN 27
const unsigned long BTN_DEBOUNCE = 300;  // Button debounce time (ms)
unsigned long lastBtnTime = 0;

// Buzzer
#define BUZZER_PIN 21
bool buzzerActive = false;

// Time Strings
char hhStr[3];        // Hours string
char mmStr[3];        // Minutes string
char dateStr[11];     // Date string
char weekdayStr[15];  // Weekday string

// Serial Input
String serialInput = "";
bool promptShown = false;

// Command History
const int HISTORY_SIZE = 10;
String commandHistory[HISTORY_SIZE];
int historyIndex = 0;           // Current position in history
int historyCount = 0;           // Total commands in history
int historyBrowseIndex = -1;   // Index while browsing (-1 = not browsing)
String tempInput = "";          // Temporary storage while browsing

// Alarm Structure
struct Alarm {
  bool active = false;
  int year = 0;         // 0 = daily, specific year if set
  int month = 0;        // 0 = daily, 1-12 if specific date
  int day = 0;          // 0 = daily, 1-31 if specific date
  int weekdays = 0;     // Bitmask: 0x01=Mon, 0x02=Tue, 0x04=Wed, 0x08=Thu, 0x10=Fri, 0x20=Sat, 0x40=Sun
  int hour = 0;         // 0-23
  int minute = 0;       // 0-59
  bool repeat = false;  // Repeat after trigger
  bool saved = false;   // Save to NVS
  char text[31] = "";   // Alarm text (max 30 bytes for UTF-8, ~10-15 cyrillic chars)
};
Alarm myAlarm;

// Timer
bool timerActive = false;
uint64_t timerStartUs = 0;
uint64_t timerDurationUs = 0;
char timerText[31] = "";  // Timer text (max 30 bytes for UTF-8)

// Trigger Flags
bool timerTriggered = false;
bool alarmTriggered = false;

// Weekdays (for display)
const char* weekdaysRU[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

// Blue LED for Alarm Indicator
#define BLUE_LED_PIN 17
#define LED_ACTIVE_LOW true  // Active low logic

// =================================================
// ================= SETUP =================
void setup() {
  // Initialize GPIO
  pinMode(BOOT_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(bl, OUTPUT);
   digitalWrite(bl, HIGH);
  // Initialize blue LED
  pinMode(BLUE_LED_PIN, OUTPUT);
  digitalWrite(BLUE_LED_PIN, HIGH);

  // Initialize serial
  Serial.begin(115200);
  unsigned long start = millis();
  while (!Serial && millis() - start < 2000) delay(10);

  Serial.println("\n=== ESP32- Clock ===");
  Serial.println("Type HELP for commands");

  // Initialize OLED
 // u8g2.begin();
  //u8g2.setContrast(255);
  //u8g2.setBusClock(400000);
  //u8g2.enableUTF8Print();

  // Display test
  /*u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_7x14_tf);
  u8g2.setCursor(30, 30);
  u8g2.print("TEST OK");
  u8g2.sendBuffer();
  delay(500);
*/
tft.init(135, 240);           // Inițializează ecranul ST7789 240x135
  tft.setRotation(1);           // Pune ecranul în mod Landscape (orizontal)
  tft.fillScreen(ST77XX_BLACK); // Fundal negru complet
  
  tft.setTextColor(ST77XX_WHITE); // Text de culoare Cyan luminos!
  tft.setTextSize(2);           // Mărime text vizibilă
  tft.setCursor(40, 50);
  tft.print("STARTING...");
  //delay(100); 
 //tft.fillScreen(ST77XX_BLACK); 
  // Load configuration and connect
  loadConfigFromNVS();
  loadAlarmFromNVS();
  showSplash();
  connectWiFi();
  syncTime();
  updateClockStrings();
  
  // Start web server
  setupWebServer();
  server.begin();
  Serial.println("Web server started");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Open browser at: http://");
    Serial.println(WiFi.localIP());
  }
}

// ================= NVS FUNCTIONS =================
void loadConfigFromNVS() {
  prefs.begin(PREF_NS, true);

  if (prefs.isKey("ssid") && !prefs.getString("ssid").isEmpty()) {
    wifiSSID = prefs.getString("ssid");
    wifiPASS = prefs.getString("pass");
    ntpServer = prefs.getString("ntp");
    gmtOffset_sec = prefs.getInt("gmtOffset", defGMTOffset);
    daylightOffset_sec = prefs.getInt("daylightOffset", defDaylightOffset);
    Serial.println("Config loaded from NVS");
  } else {
    wifiSSID = defSSID;
    wifiPASS = defPASS;
    ntpServer = defNTP;
    gmtOffset_sec = defGMTOffset;
    daylightOffset_sec = defDaylightOffset;
    Serial.println("Using default config");
  }

  prefs.end();
}

void saveConfigToNVS() {
  prefs.begin(PREF_NS, false);
  prefs.putString("ssid", wifiSSID);
  prefs.putString("pass", wifiPASS);
  prefs.putString("ntp", ntpServer);
  prefs.putInt("gmtOffset", gmtOffset_sec);
  prefs.putInt("daylightOffset", daylightOffset_sec);
  prefs.end();
  Serial.println("Config saved");
}

void loadAlarmFromNVS() {
  prefs.begin(ALARM_PREF_NS, true);

  if (prefs.isKey("active")) {
    myAlarm.active = prefs.getBool("active", false);
    myAlarm.year = prefs.getInt("year", 0);
    myAlarm.month = prefs.getInt("month", 0);
    myAlarm.day = prefs.getInt("day", 0);
    myAlarm.weekdays = prefs.getInt("weekdays", 0);
    myAlarm.hour = prefs.getInt("hour", 0);
    myAlarm.minute = prefs.getInt("minute", 0);
    myAlarm.repeat = prefs.getBool("repeat", false);
    myAlarm.saved = true;

    String text = prefs.getString("text", "");
    text.toCharArray(myAlarm.text, sizeof(myAlarm.text));

    Serial.println("Alarm loaded from NVS");
  } else {
    memset(&myAlarm, 0, sizeof(myAlarm));
  }

  prefs.end();
  updateAlarmIndicator();
}

void saveAlarmToNVS() {
  prefs.begin(ALARM_PREF_NS, false);
  prefs.putBool("active", myAlarm.active);
  prefs.putInt("year", myAlarm.year);
  prefs.putInt("month", myAlarm.month);
  prefs.putInt("day", myAlarm.day);
  prefs.putInt("weekdays", myAlarm.weekdays);
  prefs.putInt("hour", myAlarm.hour);
  prefs.putInt("minute", myAlarm.minute);
  prefs.putBool("repeat", myAlarm.repeat);
  prefs.putString("text", String(myAlarm.text));
  prefs.end();

  myAlarm.saved = true;
  Serial.println("Alarm saved to NVS");
  updateAlarmIndicator();
}

void clearAlarmFromNVS() {
  prefs.begin(ALARM_PREF_NS, false);
  prefs.clear();
  prefs.end();

  myAlarm.saved = false;
  Serial.println("Alarm cleared from NVS");
  updateAlarmIndicator();
}

void eraseNVS() {
  prefs.begin(PREF_NS, false);
  prefs.clear();
  prefs.end();

  prefs.begin(ALARM_PREF_NS, false);
  prefs.clear();
  prefs.end();

  Serial.println("NVS erased");
}

void updateAlarmIndicator() {
  // For active LOW: LOW = ON, HIGH = OFF
  // Note: This function now only updates for alarm, timer indication handled in loop
  bool shouldBeOn = myAlarm.active && !alarmTriggered;
  
  // If timer is active, LED will blink in main loop, so we don't set it here
  if (!timerActive) {
    digitalWrite(BLUE_LED_PIN, shouldBeOn ? LOW : HIGH);
  }
}

// ================= COMMAND HISTORY FUNCTIONS =================
void addToHistory(String cmd) {
  if (cmd.length() == 0) return;
  
  // Don't add if same as last command
  if (historyCount > 0 && commandHistory[(historyIndex - 1 + HISTORY_SIZE) % HISTORY_SIZE] == cmd) {
    return;
  }
  
  commandHistory[historyIndex] = cmd;
  historyIndex = (historyIndex + 1) % HISTORY_SIZE;
  if (historyCount < HISTORY_SIZE) historyCount++;
}

String getHistoryUp() {
  if (historyCount == 0) return "";
  
  // First time pressing up - save current input
  if (historyBrowseIndex == -1) {
    tempInput = serialInput;
    historyBrowseIndex = (historyIndex - 1 + HISTORY_SIZE) % HISTORY_SIZE;
  } else {
    // Move back in history
    int prevIndex = (historyBrowseIndex - 1 + HISTORY_SIZE) % HISTORY_SIZE;
    // Check if we've reached the oldest command
    int oldestIndex = (historyIndex - historyCount + HISTORY_SIZE) % HISTORY_SIZE;
    if (historyBrowseIndex != oldestIndex) {
      historyBrowseIndex = prevIndex;
    }
  }
  
  return commandHistory[historyBrowseIndex];
}

String getHistoryDown() {
  if (historyBrowseIndex == -1) return serialInput; // Not browsing
  
  int nextIndex = (historyBrowseIndex + 1) % HISTORY_SIZE;
  
  // If moving forward to current position, restore temp input
  if (nextIndex == historyIndex) {
    historyBrowseIndex = -1;
    return tempInput;
  }
  
  historyBrowseIndex = nextIndex;
  return commandHistory[historyBrowseIndex];
}

void clearCurrentLine() {
  // Move cursor to start of input and clear it
  for (int i = 0; i < serialInput.length(); i++) {
    Serial.print("\b \b");
  }
}

// ================= WEB SERVER FUNCTIONS =================
String getStatusJSON() {
  // Ensure we have current time data
  if (getLocalTime(&timeinfo) && timeinfo.tm_year > 120) {
    timeValid = true;
    updateClockStrings();
  }
  
  String json = "{";
  json += "\"time\":\"" + String(hhStr) + ":" + String(mmStr) + "\",";
  json += "\"date\":\"" + String(dateStr) + "\",";
  json += "\"weekday\":\"" + String(weekdayStr) + "\",";
  json += "\"ssid\":\"" + wifiSSID + "\",";
  json += "\"wifi\":\"" + String(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected") + "\",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"timeSync\":" + String(timeValid ? "true" : "false") + ",";
  json += "\"ntpServer\":\"" + ntpServer + "\",";
  json += "\"timezone\":" + String(gmtOffset_sec / 3600) + ",";
  json += "\"dstOffset\":" + String(daylightOffset_sec / 3600) + ",";
  
  // Current full time for manual setting
  if (timeValid) {
    char fullTime[20];
    sprintf(fullTime, "%04d-%02d-%02d %02d:%02d:%02d",
            timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
            timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    json += "\"fullTime\":\"" + String(fullTime) + "\",";
  } else {
    json += "\"fullTime\":\"\",";
  }
  
  // Alarm info
  json += "\"alarm\":{";
  json += "\"active\":" + String(myAlarm.active ? "true" : "false");
  if (myAlarm.active) {
    json += ",\"hour\":" + String(myAlarm.hour);
    json += ",\"minute\":" + String(myAlarm.minute);
    json += ",\"text\":\"" + String(myAlarm.text) + "\"";
    json += ",\"repeat\":" + String(myAlarm.repeat ? "true" : "false");
    json += ",\"saved\":" + String(myAlarm.saved ? "true" : "false");
    
    if (myAlarm.year > 0) {
      json += ",\"type\":\"date\"";
      json += ",\"date\":\"" + String(myAlarm.year) + "-";
      if (myAlarm.month < 10) json += "0";
      json += String(myAlarm.month) + "-";
      if (myAlarm.day < 10) json += "0";
      json += String(myAlarm.day) + "\"";
    } else if (myAlarm.weekdays > 0) {
      json += ",\"type\":\"weekdays\"";
      json += ",\"weekdays\":" + String(myAlarm.weekdays);
    } else {
      json += ",\"type\":\"daily\"";
    }
  }
  json += "},";
  
  // Timer info
  json += "\"timer\":{";
  json += "\"active\":" + String(timerActive ? "true" : "false");
  if (timerActive) {
    uint64_t elapsed = esp_timer_get_time() - timerStartUs;
    uint64_t remaining = (elapsed >= timerDurationUs) ? 0 : (timerDurationUs - elapsed);
    int secRemaining = (remaining + 500000) / 1000000;
    json += ",\"remaining\":" + String(secRemaining);
    json += ",\"text\":\"" + String(timerText) + "\"";
  }
  json += "}";
  
  json += "}";
  return json;
}

String getWebPage() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ESP32-dev Clock</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Arial, sans-serif;
      background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
      min-height: 100vh;
      padding: 20px;
      color: #333;
    }
    .container {
      max-width: 800px;
      margin: 0 auto;
    }
    .card {
      background: white;
      border-radius: 12px;
      padding: 24px;
      margin-bottom: 20px;
      box-shadow: 0 4px 6px rgba(0,0,0,0.1);
    }
    h1 {
      color: white;
      text-align: center;
      margin-bottom: 20px;
      font-size: 2em;
    }
    h2 {
      color: #667eea;
      margin-bottom: 16px;
      font-size: 1.5em;
    }
    .clock-display {
      text-align: center;
      font-size: 3em;
      font-weight: bold;
      color: #667eea;
      margin: 20px 0;
    }
    .date-display {
      text-align: center;
      font-size: 1.2em;
      color: #666;
      margin-bottom: 10px;
    }
    .status-grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));
      gap: 12px;
      margin-top: 16px;
    }
    .status-item {
      padding: 12px;
      background: #f7f7f7;
      border-radius: 8px;
    }
    .status-label {
      font-size: 0.85em;
      color: #666;
      margin-bottom: 4px;
    }
    .status-value {
      font-size: 1.1em;
      font-weight: 600;
      color: #333;
    }
    .form-group {
      margin-bottom: 16px;
    }
    label {
      display: block;
      margin-bottom: 6px;
      font-weight: 500;
      color: #555;
    }
    input, select {
      width: 100%;
      padding: 10px;
      border: 2px solid #e0e0e0;
      border-radius: 6px;
      font-size: 1em;
      transition: border-color 0.3s;
    }
    input:focus, select:focus {
      outline: none;
      border-color: #667eea;
    }
    .checkbox-group {
      display: flex;
      gap: 12px;
      flex-wrap: wrap;
    }
    .checkbox-label {
      display: flex;
      align-items: center;
      gap: 6px;
      cursor: pointer;
    }
    .checkbox-label input {
      width: auto;
    }
    button {
      background: #667eea;
      color: white;
      border: none;
      padding: 12px 24px;
      border-radius: 6px;
      font-size: 1em;
      cursor: pointer;
      transition: background 0.3s;
      width: 100%;
      font-weight: 600;
    }
    button:hover {
      background: #5568d3;
    }
    button.danger {
      background: #ef4444;
    }
    button.danger:hover {
      background: #dc2626;
    }
    .btn-group {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 12px;
      margin-top: 12px;
    }
    .alarm-info, .timer-info {
      background: #f0f9ff;
      border-left: 4px solid #667eea;
      padding: 12px;
      border-radius: 6px;
      margin-top: 12px;
    }
    .inactive {
      background: #f7f7f7;
      border-left-color: #ccc;
    }
    @media (max-width: 600px) {
      .clock-display { font-size: 2em; }
      h1 { font-size: 1.5em; }
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>⏰ ESP32-dev Clock</h1>
    
    <div class="card">
      <div class="clock-display" id="clock">--:--</div>
      <div class="date-display" id="date">Loading...</div>
      <div class="status-grid">
        <div class="status-item">
          <div class="status-label">WiFi SSID</div>
          <div class="status-value" id="ssid">--</div>
        </div>
        <div class="status-item">
          <div class="status-label">WiFi Status</div>
          <div class="status-value" id="wifi">--</div>
        </div>
        <div class="status-item">
          <div class="status-label">IP Address</div>
          <div class="status-value" id="ip">--</div>
        </div>
        <div class="status-item">
          <div class="status-label">Time Sync</div>
          <div class="status-value" id="sync">--</div>
        </div>
      </div>
    </div>

    <div class="card">
      <h2>⚙️ System Settings</h2>
      
      <div class="form-group">
        <label>NTP Server</label>
        <input type="text" id="ntpServer" placeholder="pool.ntp.org">
      </div>
      
      <div class="form-group">
        <label>Timezone (GMT offset in hours)</label>
        <input type="number" id="timezone" min="-12" max="14" step="1" placeholder="3">
      </div>
      
      <div class="form-group">
        <label>DST Offset (hours)</label>
        <input type="number" id="dstOffset" min="-2" max="2" step="1" placeholder="0">
      </div>
      
      <button onclick="syncTime()">🔄 Sync Time Now</button>
      
      <div class="form-group" style="margin-top: 16px;">
        <label>Manual Time (YYYY-MM-DD HH:MM:SS)</label>
        <input type="text" id="manualTime" placeholder="2025-01-15 14:30:00">
      </div>
      
      <button onclick="setManualTime()">🕐 Set Manual Time</button>
      
      <div class="btn-group" style="margin-top: 16px;">
        <button onclick="saveSettings()">💾 Save Settings</button>
        <button onclick="restoreSettings()">📂 Restore Settings</button>
      </div>
      
      <div class="btn-group">
        <button class="danger" onclick="eraseSettings()">🗑️ Erase NVS</button>
        <button class="danger" onclick="rebootDevice()">🔄 Reboot</button>
      </div>
    </div>

    <div class="card">
      <h2>🔔 Alarm</h2>
      <div id="alarmStatus" class="alarm-info inactive">No alarm set</div>
      
      <div class="form-group">
        <label>Alarm Type</label>
        <select id="alarmType" onchange="updateAlarmFields()">
          <option value="daily">Daily</option>
          <option value="weekdays">Weekdays</option>
          <option value="date">Specific Date</option>
        </select>
      </div>
      
      <div id="dateField" style="display:none;" class="form-group">
        <label>Date</label>
        <input type="date" id="alarmDate">
      </div>
      
      <div id="weekdaysField" style="display:none;" class="form-group">
        <label>Select Days</label>
        <div class="checkbox-group">
          <label class="checkbox-label"><input type="checkbox" value="1"> Mon</label>
          <label class="checkbox-label"><input type="checkbox" value="2"> Tue</label>
          <label class="checkbox-label"><input type="checkbox" value="4"> Wed</label>
          <label class="checkbox-label"><input type="checkbox" value="8"> Thu</label>
          <label class="checkbox-label"><input type="checkbox" value="16"> Fri</label>
          <label class="checkbox-label"><input type="checkbox" value="32"> Sat</label>
          <label class="checkbox-label"><input type="checkbox" value="64"> Sun</label>
        </div>
      </div>
      
      <div class="form-group">
        <label>Time</label>
        <input type="time" id="alarmTime">
      </div>
      
      <div class="form-group">
        <label>Text (optional, max 10 chars)</label>
        <input type="text" id="alarmText" maxlength="30" placeholder="Wake up / Подъём">
      </div>
      
      <div class="form-group">
        <label class="checkbox-label">
          <input type="checkbox" id="alarmRepeat"> Repeat after trigger
        </label>
      </div>
      
      <div class="form-group">
        <label class="checkbox-label">
          <input type="checkbox" id="alarmSave"> Save to NVS
        </label>
      </div>
      
      <div class="btn-group">
        <button onclick="setAlarm()">Set Alarm</button>
        <button class="danger" onclick="clearAlarm()">Clear Alarm</button>
      </div>
    </div>

    <div class="card">
      <h2>⏲️ Timer</h2>
      <div id="timerStatus" class="timer-info inactive">No timer active</div>
      
      <div class="form-group">
        <label>Duration</label>
        <input type="time" id="timerTime" step="1" value="00:05:00">
      </div>
      
      <div class="form-group">
        <label>Text (optional, max 10 chars)</label>
        <input type="text" id="timerText" maxlength="30" placeholder="Timer / Таймер">
      </div>
      
      <div class="btn-group">
        <button onclick="setTimer()">Start Timer</button>
        <button class="danger" onclick="clearTimer()">Clear Timer</button>
      </div>
    </div>
  </div>

  <script>
    function updateStatus() {
      fetch('/status')
        .then(r => r.json())
        .then(data => {
          document.getElementById('clock').textContent = data.time;
          document.getElementById('date').textContent = data.date + ' • ' + data.weekday;
          document.getElementById('ssid').textContent = data.ssid;
          document.getElementById('wifi').textContent = data.wifi;
          document.getElementById('ip').textContent = data.ip;
          document.getElementById('sync').textContent = data.timeSync ? 'Synced' : 'Not synced';
          
          // Update settings fields
          document.getElementById('ntpServer').value = data.ntpServer;
          document.getElementById('timezone').value = data.timezone;
          document.getElementById('dstOffset').value = data.dstOffset;
          if (data.fullTime) {
            document.getElementById('manualTime').value = data.fullTime;
          }
          
          // Update alarm form from current alarm settings
          if (data.alarm.active) {
            // Set time
            const hourStr = (data.alarm.hour < 10 ? '0' : '') + data.alarm.hour;
            const minStr = (data.alarm.minute < 10 ? '0' : '') + data.alarm.minute;
            document.getElementById('alarmTime').value = hourStr + ':' + minStr;
            
            // Set type and related fields
            if (data.alarm.type === 'date') {
              document.getElementById('alarmType').value = 'date';
              document.getElementById('alarmDate').value = data.alarm.date;
            } else if (data.alarm.type === 'weekdays') {
              document.getElementById('alarmType').value = 'weekdays';
              // Set weekday checkboxes
              const weekdayMask = data.alarm.weekdays;
              const checkboxes = document.querySelectorAll('#weekdaysField input[type="checkbox"]');
              checkboxes.forEach(cb => {
                const value = parseInt(cb.value);
                cb.checked = (weekdayMask & value) !== 0;
              });
            } else {
              document.getElementById('alarmType').value = 'daily';
            }
            
            // Set text
            if (data.alarm.text) {
              document.getElementById('alarmText').value = data.alarm.text;
            }
            
            // Set flags
            document.getElementById('alarmRepeat').checked = data.alarm.repeat;
            document.getElementById('alarmSave').checked = data.alarm.saved;
            
            // Update fields visibility
            updateAlarmFields();
          }
          
          // Alarm status
          const alarmDiv = document.getElementById('alarmStatus');
          if (data.alarm.active) {
            let alarmText = 'Alarm: ';
            if (data.alarm.type === 'date') alarmText += data.alarm.date + ' ';
            else if (data.alarm.type === 'weekdays') alarmText += 'Weekdays ';
            else alarmText += 'Daily ';
            alarmText += (data.alarm.hour < 10 ? '0' : '') + data.alarm.hour + ':';
            alarmText += (data.alarm.minute < 10 ? '0' : '') + data.alarm.minute;
            if (data.alarm.text) alarmText += ' "' + data.alarm.text + '"';
            if (data.alarm.repeat) alarmText += ' [R]';
            if (data.alarm.saved) alarmText += ' [S]';
            alarmDiv.textContent = alarmText;
            alarmDiv.className = 'alarm-info';
          } else {
            alarmDiv.textContent = 'No alarm set';
            alarmDiv.className = 'alarm-info inactive';
          }
          
          // Timer status
          const timerDiv = document.getElementById('timerStatus');
          if (data.timer.active) {
            const min = Math.floor(data.timer.remaining / 60);
            const sec = data.timer.remaining % 60;
            let timerText = 'Timer: ' + min + ':' + (sec < 10 ? '0' : '') + sec + ' remaining';
            if (data.timer.text && data.timer.text !== 'TIMER') timerText += ' "' + data.timer.text + '"';
            timerDiv.textContent = timerText;
            timerDiv.className = 'timer-info';
            
            // Update timer form with remaining time
            const hours = Math.floor(data.timer.remaining / 3600);
            const minutes = Math.floor((data.timer.remaining % 3600) / 60);
            const seconds = data.timer.remaining % 60;
            const hourStr = (hours < 10 ? '0' : '') + hours;
            const minStr = (minutes < 10 ? '0' : '') + minutes;
            const secStr = (seconds < 10 ? '0' : '') + seconds;
            document.getElementById('timerTime').value = hourStr + ':' + minStr + ':' + secStr;
            
            // Update timer text
            if (data.timer.text && data.timer.text !== 'TIMER') {
              document.getElementById('timerText').value = data.timer.text;
            }
          } else {
            timerDiv.textContent = 'No timer active';
            timerDiv.className = 'timer-info inactive';
          }
        });
    }
    
    function updateAlarmFields() {
      const type = document.getElementById('alarmType').value;
      document.getElementById('dateField').style.display = type === 'date' ? 'block' : 'none';
      document.getElementById('weekdaysField').style.display = type === 'weekdays' ? 'block' : 'none';
    }
    
    function setAlarm() {
      const type = document.getElementById('alarmType').value;
      const time = document.getElementById('alarmTime').value;
      const text = document.getElementById('alarmText').value;
      const repeat = document.getElementById('alarmRepeat').checked;
      const save = document.getElementById('alarmSave').checked;
      
      if (!time) {
        alert('Please set alarm time');
        return;
      }
      
      let url = '/alarm?time=' + time;
      url += '&type=' + type;
      
      if (type === 'date') {
        const date = document.getElementById('alarmDate').value;
        if (!date) {
          alert('Please select a date');
          return;
        }
        url += '&date=' + date;
      } else if (type === 'weekdays') {
        const checks = document.querySelectorAll('#weekdaysField input:checked');
        if (checks.length === 0) {
          alert('Please select at least one day');
          return;
        }
        let mask = 0;
        checks.forEach(c => mask += parseInt(c.value));
        url += '&weekdays=' + mask;
      }
      
      if (text) url += '&text=' + encodeURIComponent(text);
      if (repeat) url += '&repeat=1';
      if (save) url += '&save=1';
      
      fetch(url)
        .then(r => r.text())
        .then(data => {
          alert(data);
          updateStatus();
        });
    }
    
    function clearAlarm() {
      fetch('/alarm/clear')
        .then(r => r.text())
        .then(data => {
          alert(data);
          updateStatus();
        });
    }
    
    function setTimer() {
      const time = document.getElementById('timerTime').value;
      const text = document.getElementById('timerText').value;
      
      if (!time) {
        alert('Please set timer duration');
        return;
      }
      
      const parts = time.split(':');
      const hours = parseInt(parts[0]);
      const minutes = parseInt(parts[1]);
      const seconds = parseInt(parts[2] || 0);
      const totalSec = hours * 3600 + minutes * 60 + seconds;
      
      if (totalSec === 0) {
        alert('Timer must be at least 1 second');
        return;
      }
      
      let url = '/timer?duration=' + totalSec;
      if (text) url += '&text=' + encodeURIComponent(text);
      
      fetch(url)
        .then(r => r.text())
        .then(data => {
          alert(data);
          updateStatus();
        });
    }
    
    function clearTimer() {
      fetch('/timer/clear')
        .then(r => r.text())
        .then(data => {
          alert(data);
          updateStatus();
        });
    }
    
    function syncTime() {
      const ntp = document.getElementById('ntpServer').value;
      const tz = document.getElementById('timezone').value;
      const dst = document.getElementById('dstOffset').value;
      
      let url = '/sync';
      if (ntp) url += '?ntp=' + encodeURIComponent(ntp);
      if (tz) url += (url.includes('?') ? '&' : '?') + 'tz=' + tz;
      if (dst) url += '&dst=' + dst;
      
      fetch(url)
        .then(r => r.text())
        .then(data => {
          alert(data);
          updateStatus();
        });
    }
    
    function setManualTime() {
      const time = document.getElementById('manualTime').value;
      if (!time) {
        alert('Please enter time in format: YYYY-MM-DD HH:MM:SS');
        return;
      }
      
      fetch('/time?value=' + encodeURIComponent(time))
        .then(r => r.text())
        .then(data => {
          alert(data);
          updateStatus();
        });
    }
    
    function saveSettings() {
      if (confirm('Save current settings to NVS?')) {
        fetch('/save')
          .then(r => r.text())
          .then(data => {
            alert(data);
          });
      }
    }
    
    function restoreSettings() {
      if (confirm('Restore settings from NVS? This will reload the page.')) {
        fetch('/restore')
          .then(r => r.text())
          .then(data => {
            alert(data);
            setTimeout(() => location.reload(), 1000);
          });
      }
    }
    
    function eraseSettings() {
      if (confirm('WARNING: This will erase ALL settings from NVS! Continue?')) {
        fetch('/erase')
          .then(r => r.text())
          .then(data => {
            alert(data);
          });
      }
    }
    
    function rebootDevice() {
      if (confirm('Reboot the device?')) {
        fetch('/reboot')
          .then(r => r.text())
          .then(data => {
            alert('Device is rebooting... Please wait 10 seconds.');
            setTimeout(() => location.reload(), 10000);
          });
      }
    }
    
    updateStatus();
    setInterval(updateStatus, 2000);
  </script>
</body>
</html>
)rawliteral";
  return html;
}

void setupWebServer() {
  // Main page
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html; charset=utf-8", getWebPage());
  });
  
  // Status API
  server.on("/status", HTTP_GET, []() {
    server.send(200, "application/json; charset=utf-8", getStatusJSON());
  });
  
  // Set alarm
  server.on("/alarm", HTTP_GET, []() {
    if (!timeValid) {
      server.send(400, "text/plain; charset=utf-8", "Wait for time sync!");
      return;
    }
    
    String timeStr = server.arg("time");
    String type = server.arg("type");
    
    if (timeStr.length() == 0) {
      server.send(400, "text/plain; charset=utf-8", "Missing time parameter");
      return;
    }
    
    int hour = timeStr.substring(0, 2).toInt();
    int minute = timeStr.substring(3, 5).toInt();
    
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
      server.send(400, "text/plain; charset=utf-8", "Invalid time");
      return;
    }
    
    myAlarm.active = true;
    myAlarm.hour = hour;
    myAlarm.minute = minute;
    myAlarm.repeat = server.hasArg("repeat");
    alarmTriggered = false;
    
    // Decode URL-encoded text (supports UTF-8)
    String text = server.arg("text");
    if (text.length() > 0) {
      // URL decode
      text.replace("+", " ");
      String decoded = "";
      for (int i = 0; i < text.length(); i++) {
        if (text[i] == '%' && i + 2 < text.length()) {
          String hex = text.substring(i + 1, i + 3);
          char c = strtol(hex.c_str(), NULL, 16);
          decoded += c;
          i += 2;
        } else {
          decoded += text[i];
        }
      }
      // Limit to 30 bytes (for UTF-8 strings)
      int byteCount = 0;
      for (int i = 0; i < decoded.length() && byteCount < 30; i++) {
        myAlarm.text[byteCount++] = decoded[i];
      }
      myAlarm.text[byteCount] = '\0';
    } else {
      myAlarm.text[0] = '\0';
    }
    
    if (type == "date") {
      String dateStr = server.arg("date");
      myAlarm.year = dateStr.substring(0, 4).toInt();
      myAlarm.month = dateStr.substring(5, 7).toInt();
      myAlarm.day = dateStr.substring(8, 10).toInt();
      myAlarm.weekdays = 0;
    } else if (type == "weekdays") {
      myAlarm.year = 0;
      myAlarm.month = 0;
      myAlarm.day = 0;
      myAlarm.weekdays = server.arg("weekdays").toInt();
    } else {
      myAlarm.year = 0;
      myAlarm.month = 0;
      myAlarm.day = 0;
      myAlarm.weekdays = 0;
    }
    
    if (server.hasArg("save")) {
      saveAlarmToNVS();
    } else {
      myAlarm.saved = false;
      updateAlarmIndicator();
    }
    
    server.send(200, "text/plain; charset=utf-8", "Alarm set successfully");
  });
  
  // Clear alarm
  server.on("/alarm/clear", HTTP_GET, []() {
    memset(&myAlarm, 0, sizeof(myAlarm));
    alarmTriggered = false;
    buzzerActive = false;
    digitalWrite(BUZZER_PIN, LOW);
    clearAlarmFromNVS();
    updateAlarmIndicator();
    server.send(200, "text/plain; charset=utf-8", "Alarm cleared");
  });
  
  // Set timer
  server.on("/timer", HTTP_GET, []() {
    String durationStr = server.arg("duration");
    
    if (durationStr.length() == 0) {
      server.send(400, "text/plain; charset=utf-8", "Missing duration parameter");
      return;
    }
    
    unsigned long totalSec = durationStr.toInt();
    
    if (totalSec == 0 || totalSec > 86400) {
      server.send(400, "text/plain; charset=utf-8", "Timer must be 1-86400 seconds");
      return;
    }
    
    // Decode URL-encoded text (supports UTF-8)
    String text = server.arg("text");
    if (text.length() > 0) {
      text.replace("+", " ");
      String decoded = "";
      for (int i = 0; i < text.length(); i++) {
        if (text[i] == '%' && i + 2 < text.length()) {
          String hex = text.substring(i + 1, i + 3);
          char c = strtol(hex.c_str(), NULL, 16);
          decoded += c;
          i += 2;
        } else {
          decoded += text[i];
        }
      }
      // Limit to 30 bytes (for UTF-8 strings)
      int byteCount = 0;
      for (int i = 0; i < decoded.length() && byteCount < 30; i++) {
        timerText[byteCount++] = decoded[i];
      }
      timerText[byteCount] = '\0';
    } else {
      strcpy(timerText, "TIMER");
    }
    
    timerActive = true;
    timerTriggered = false;
    timerStartUs = esp_timer_get_time();
    timerDurationUs = (uint64_t)totalSec * 1000000;
    
    server.send(200, "text/plain; charset=utf-8", "Timer started");
  });
  
  // Clear timer
  server.on("/timer/clear", HTTP_GET, []() {
    timerActive = false;
    timerTriggered = false;
    buzzerActive = false;
    digitalWrite(BUZZER_PIN, LOW);
    server.send(200, "text/plain; charset=utf-8", "Timer cleared");
  });
  
  // Sync time
  server.on("/sync", HTTP_GET, []() {
    if (server.hasArg("ntp")) {
      ntpServer = server.arg("ntp");
    }
    if (server.hasArg("tz")) {
      gmtOffset_sec = server.arg("tz").toInt() * 3600;
    }
    if (server.hasArg("dst")) {
      daylightOffset_sec = server.arg("dst").toInt() * 3600;
    }
    
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer.c_str());
    delay(2000); // Wait for sync
    
    if (getLocalTime(&timeinfo) && timeinfo.tm_year > 120) {
      timeValid = true;
      updateClockStrings();
      server.send(200, "text/plain; charset=utf-8", "Time synchronized successfully");
    } else {
      server.send(500, "text/plain; charset=utf-8", "Time synchronization failed");
    }
  });
  
  // Set manual time
  server.on("/time", HTTP_GET, []() {
    String timeStr = server.arg("value");
    if (timeStr.length() == 0) {
      server.send(400, "text/plain; charset=utf-8", "Missing time parameter");
      return;
    }
    
    struct tm t {};
    if (sscanf(timeStr.c_str(), "%d-%d-%d %d:%d:%d",
               &t.tm_year, &t.tm_mon, &t.tm_mday,
               &t.tm_hour, &t.tm_min, &t.tm_sec) == 6) {
      t.tm_year -= 1900;
      t.tm_mon -= 1;
      
      time_t tt = mktime(&t);
      struct timeval now = { tt, 0 };
      settimeofday(&now, nullptr);
      timeValid = true;
      updateClockStrings();
      server.send(200, "text/plain; charset=utf-8", "Manual time set successfully");
    } else {
      server.send(400, "text/plain; charset=utf-8", "Invalid time format. Use: YYYY-MM-DD HH:MM:SS");
    }
  });
  
  // Save settings
  server.on("/save", HTTP_GET, []() {
    saveConfigToNVS();
    server.send(200, "text/plain; charset=utf-8", "Settings saved to NVS");
  });
  
  // Restore settings
  server.on("/restore", HTTP_GET, []() {
    loadConfigFromNVS();
    connectWiFi();
    syncTime();
    updateClockStrings();
    server.send(200, "text/plain; charset=utf-8", "Settings restored from NVS");
  });
  
  // Erase NVS
  server.on("/erase", HTTP_GET, []() {
    eraseNVS();
    server.send(200, "text/plain; charset=utf-8", "NVS erased successfully");
  });
  
  // Reboot
  server.on("/reboot", HTTP_GET, []() {
    server.send(200, "text/plain; charset=utf-8", "Rebooting...");
    delay(1000);
    ESP.restart();
  });
}

// ================= SERIAL COMMAND HANDLER =================
void handleSerial() {
  if (!promptShown) {
    Serial.print("> ");
    promptShown = true;
  }

  while (Serial.available()) {
    char c = Serial.read();

    // Handle ANSI escape sequences (arrow keys)
    static bool escapeMode = false;
    static bool bracketMode = false;
    
    if (c == 27) { // ESC
      escapeMode = true;
      continue;
    }
    
    if (escapeMode) {
      if (c == '[') {
        bracketMode = true;
        continue;
      }
      
      if (bracketMode) {
        if (c == 'A') { // Up arrow
          String histCmd = getHistoryUp();
          if (histCmd.length() > 0) {
            clearCurrentLine();
            serialInput = histCmd;
            Serial.print(serialInput);
          }
          escapeMode = false;
          bracketMode = false;
          continue;
        } else if (c == 'B') { // Down arrow
          String histCmd = getHistoryDown();
          clearCurrentLine();
          serialInput = histCmd;
          Serial.print(serialInput);
          escapeMode = false;
          bracketMode = false;
          continue;
        } else if (c == 'C' || c == 'D') { // Right/Left arrow - ignore for now
          escapeMode = false;
          bracketMode = false;
          continue;
        }
      }
      escapeMode = false;
      bracketMode = false;
      continue;
    }

    // Handle Backspace
    if (c == 8 || c == 127) {
      if (serialInput.length() > 0) {
        serialInput.remove(serialInput.length() - 1);
        Serial.print("\b \b");
      }
      continue;  // Skip further processing for backspace
    }

    Serial.print(c);  // echo

    if (c == '\r' || c == '\n') {
      Serial.println(); // Перевод строки после команды
      
      if (serialInput.length() == 0) {
        Serial.print("> ");
        return;
      }

      String cmd = serialInput;
      addToHistory(cmd); // Add to history
      historyBrowseIndex = -1; // Reset browse mode
      serialInput = "";
      cmd.trim();
      cmd.toUpperCase();

      // HELP command
      if (cmd.equals("HELP")) {
        Serial.println("=== Clock Commands ===");
        Serial.println("TIME YYYY-MM-DD HH:MM:SS");
        Serial.println("WIFI <ssid> <pass>");
        Serial.println("NTP <server>");
        Serial.println("TZ <+/-offset_hours>");
        Serial.println("DST <+/-offset_hours>");
        Serial.println("SAVE | RESTORE | ERASE");
        Serial.println("STATUS | SYNC | REBOOT");
        Serial.println("=== Alarm Commands ===");
        Serial.println("ALARM [YYYY-MM-DD|1234567] HH:MM [TEXT] [R] [S]");
        Serial.println("  YYYY-MM-DD = specific date");
        Serial.println("  1234567 = weekdays (1=Mon,7=Sun)");
        Serial.println("  R = repeat after trigger");
        Serial.println("  S = save to NVS");
        Serial.println("ALARM CLEAR");
        Serial.println("=== Timer Commands ===");
        Serial.println("TIMER HH:MM:SS [TEXT]");
        Serial.println("TIMER MM:SS [TEXT]");
        Serial.println("TIMER SS [TEXT]");
        Serial.println("TIMER CLEAR");
      }
      // TIME command
      else if (cmd.startsWith("TIME ")) {
        setManualTime(cmd.substring(5));
        updateClockStrings();
      }
      // WIFI command
      else if (cmd.startsWith("WIFI ")) {
        int sp = cmd.indexOf(' ', 5);
        if (sp > 0) {
          wifiSSID = cmd.substring(5, sp);
          wifiPASS = cmd.substring(sp + 1);
          connectWiFi();
        }
      }
      // NTP command
      else if (cmd.startsWith("NTP ")) {
        ntpServer = cmd.substring(4);
        syncTime();
        updateClockStrings();
      }
      // TZ command
      else if (cmd.startsWith("TZ ")) {
        String tzStr = cmd.substring(3);
        tzStr.trim();
        if (tzStr.length() > 0) {
          bool negative = false;
          int startIndex = 0;

          if (tzStr[0] == '+') {
            startIndex = 1;
          } else if (tzStr[0] == '-') {
            negative = true;
            startIndex = 1;
          }

          String numStr = tzStr.substring(startIndex);
          long value = numStr.toInt() * 3600;

          if (negative) value = -value;
          gmtOffset_sec = value;

          Serial.print("GMT offset set to ");
          Serial.print(value >= 0 ? "+" : "-");
          Serial.print(abs(value / 3600));
          Serial.println(" hours");
          configTime(gmtOffset_sec, daylightOffset_sec, ntpServer.c_str());
        }
      }
      // DST command
      else if (cmd.startsWith("DST ")) {
        String dstStr = cmd.substring(4);
        dstStr.trim();
        if (dstStr.length() > 0) {
          bool negative = false;
          int startIndex = 0;

          if (dstStr[0] == '+') {
            startIndex = 1;
          } else if (dstStr[0] == '-') {
            negative = true;
            startIndex = 1;
          }

          String numStr = dstStr.substring(startIndex);
          long value = numStr.toInt() * 3600;

          if (negative) value = -value;
          daylightOffset_sec = value;

          Serial.print("Daylight offset set to ");
          Serial.print(value >= 0 ? "+" : "-");
          Serial.print(abs(value / 3600));
          Serial.println(" hours");
          configTime(gmtOffset_sec, daylightOffset_sec, ntpServer.c_str());
        }
      }
      // SAVE command
      else if (cmd.equals("SAVE")) {
        saveConfigToNVS();
      }
      // RESTORE command
      else if (cmd.equals("RESTORE")) {
        loadConfigFromNVS();
        connectWiFi();
        syncTime();
        updateClockStrings();
      }
      // ERASE command
      else if (cmd.equals("ERASE")) {
        eraseNVS();
      }
      // STATUS command
      else if (cmd.equals("STATUS")) {
        Serial.println("=== SYSTEM STATUS ===");
        Serial.println("SSID: " + wifiSSID);
        Serial.println("WiFi: " + String(WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED"));
        if (WiFi.status() == WL_CONNECTED) {
          Serial.println("IP: " + WiFi.localIP().toString());
        }
        Serial.println("NTP Server: " + ntpServer);
        Serial.println("NTP Status: " + String(timeValid ? "SYNCED" : "NOT SYNCED"));

        // Current time
        if (getLocalTime(&timeinfo) && timeinfo.tm_year > 120) {
          char timeStr[50];
          sprintf(timeStr, "Current Time: %02d:%02d:%02d",
                  timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
          Serial.println(timeStr);

          sprintf(timeStr, "Current Date: %02d.%02d.%04d",
                  timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900);
          Serial.println(timeStr);
        } else {
          Serial.println("Current Time: NOT AVAILABLE");
        }

        // Alarm status
        if (myAlarm.active) {
          Serial.print("Alarm: ");
          if (myAlarm.year > 0) {
            Serial.printf("%04d-%02d-%02d ", myAlarm.year, myAlarm.month, myAlarm.day);
          } else if (myAlarm.weekdays > 0) {
            Serial.print("Weekdays: ");
            if (myAlarm.weekdays & 0x01) Serial.print("Mon");
            if (myAlarm.weekdays & 0x02) Serial.print("Tue");
            if (myAlarm.weekdays & 0x04) Serial.print("Wed");
            if (myAlarm.weekdays & 0x08) Serial.print("Thu");
            if (myAlarm.weekdays & 0x10) Serial.print("Fri");
            if (myAlarm.weekdays & 0x20) Serial.print("Sat");
            if (myAlarm.weekdays & 0x40) Serial.print("Sun");
            Serial.print(" ");
          } else {
            Serial.print("Daily ");
          }
          Serial.printf("%02d:%02d", myAlarm.hour, myAlarm.minute);
          if (myAlarm.repeat) Serial.print(" [R]");
          if (myAlarm.saved) Serial.print(" [S]");
          if (myAlarm.text[0]) Serial.printf(" '%s'", myAlarm.text);
          Serial.println();
        } else {
          Serial.println("Alarm: OFF");
        }

        // Timer status
        if (timerActive) {
          uint64_t elapsed = esp_timer_get_time() - timerStartUs;
          uint64_t remaining = (elapsed >= timerDurationUs) ? 0 : (timerDurationUs - elapsed);
          int secRemaining = (remaining + 500000) / 1000000;
          int minRemaining = secRemaining / 60;
          secRemaining = secRemaining % 60;
          int hourRemaining = minRemaining / 60;
          minRemaining = minRemaining % 60;

          char timerStr[60];
          sprintf(timerStr, "Timer: %02d:%02d:%02d remaining",
                  hourRemaining, minRemaining, secRemaining);
          if (strcmp(timerText, "TIMER") != 0) {
            sprintf(timerStr + strlen(timerStr), ", text='%s'", timerText);
          }
          Serial.println(timerStr);
        } else {
          Serial.println("Timer: OFF");
        }

        Serial.println("Free RAM: " + String(esp_get_free_heap_size() / 1024) + " KB");
      }
      // SYNC command
      else if (cmd.equals("SYNC")) {
        Serial.println("Forcing NTP synchronization...");
        syncTime();
        if (timeValid) {
          Serial.println("Time synchronized successfully");
          updateClockStrings();
        } else {
          Serial.println("NTP synchronization failed");
        }
      }
      // REBOOT command
      else if (cmd.equals("REBOOT")) {
        ESP.restart();
      }
      // ALARM CLEAR command
      else if (cmd.equals("ALARM CLEAR")) {
        memset(&myAlarm, 0, sizeof(myAlarm));
        alarmTriggered = false;
        buzzerActive = false;
        digitalWrite(BUZZER_PIN, LOW);
        clearAlarmFromNVS();
        updateAlarmIndicator();
        Serial.println("Alarm cleared");
      }
      // SET ALARM command (improved format)
      else if (cmd.startsWith("ALARM ")) {
        String s = cmd.substring(6);
        s.trim();

        // Parse date/weekdays
        int dateType = 0;  // 0=daily, 1=specific date, 2=weekdays
        int year = 0, month = 0, day = 0;
        int weekdaysMask = 0;

        // Check for specific date (YYYY-MM-DD) - must have exactly 2 dashes at positions 4 and 7
        if (s.length() >= 10 && s.charAt(4) == '-' && s.charAt(7) == '-') {
          dateType = 1;
          year = s.substring(0, 4).toInt();
          month = s.substring(5, 7).toInt();
          day = s.substring(8, 10).toInt();
          s = s.substring(11);  // Remove date part
          s.trim();
        }
        // Check for weekdays (only digits 1-7 without colons)
        else if (s.length() > 0 && s[0] >= '1' && s[0] <= '7') {
          // Check if this is weekdays or time by looking for colon
          int firstColon = s.indexOf(':');
          int firstSpace = s.indexOf(' ');
          
          // If colon comes before any weekday digits end, it's time not weekdays
          // Example: "17:00" - colon at position 2, so it's time
          // Example: "12345 17:00" - space at position 5, colon at 8, so 12345 are weekdays
          
          bool isWeekdays = false;
          if (firstColon == -1) {
            // No colon at all - can't be valid, but treat first part as weekdays
            isWeekdays = true;
          } else if (firstSpace != -1 && firstSpace < firstColon) {
            // Space before colon - digits before space are weekdays
            isWeekdays = true;
          } else {
            // Colon comes first - this is time, not weekdays
            isWeekdays = false;
          }
          
          if (isWeekdays) {
            dateType = 2;
            String digits = "";
            int i = 0;
            while (i < s.length() && s[i] >= '1' && s[i] <= '7') {
              digits += s[i];
              i++;
            }
            s = s.substring(i);
            s.trim();

            // Convert digits to bitmask
            for (int j = 0; j < digits.length(); j++) {
              char d = digits[j];
              if (d >= '1' && d <= '7') {
                int dayNum = d - '1';  // 0=Mon, 6=Sun
                weekdaysMask |= (1 << dayNum);
              }
            }
          }
        }

        // Parse time (HH:MM)
        int colonPos = s.indexOf(':');
        if (colonPos == -1 || colonPos >= 3) {
          Serial.println("Format: ALARM [YYYY-MM-DD|1234567] HH:MM [TEXT] [R] [S]");
          Serial.print("> ");
          return;
        }

        int spacePos = s.indexOf(' ', colonPos + 1);
        String timePart = spacePos == -1 ? s : s.substring(0, spacePos);
        String rest = spacePos == -1 ? "" : s.substring(spacePos + 1);

        int hour = timePart.substring(0, colonPos).toInt();
        int minute = timePart.substring(colonPos + 1).toInt();

        if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
          Serial.println("Hours: 0-23, Minutes: 0-59");
          Serial.print("> ");
          return;
        }

        if (!timeValid) {
          Serial.println("Wait for time sync!");
          Serial.print("> ");
          return;
        }

        // Parse options (R, S, TEXT)
        // Strategy: first collect all single-letter flags (R, S), then rest is TEXT
        bool repeat = false;
        bool save = false;
        String textContent = "";
        
        rest.trim();
        
        while (rest.length() > 0) {
          // Check if starts with R or S followed by space or end
          if ((rest[0] == 'R' || rest[0] == 'r') && 
              (rest.length() == 1 || rest[1] == ' ')) {
            repeat = true;
            rest = rest.substring(1);
            rest.trim();
          } 
          else if ((rest[0] == 'S' || rest[0] == 's') && 
                   (rest.length() == 1 || rest[1] == ' ')) {
            save = true;
            rest = rest.substring(1);
            rest.trim();
          } 
          else {
            // Everything else is TEXT
            textContent = rest;
            break;
          }
        }
        
        // Convert text to char array (max 30 bytes for UTF-8)
        char text[31] = "";
        if (textContent.length() > 0) {
          int len = min((int)textContent.length(), 30);
          textContent.substring(0, len).toCharArray(text, sizeof(text));
        }

        // Set alarm
        myAlarm.active = true;
        myAlarm.hour = hour;
        myAlarm.minute = minute;
        myAlarm.repeat = repeat;
        alarmTriggered = false;
        strcpy(myAlarm.text, text);

        if (dateType == 1) {
          myAlarm.year = year;
          myAlarm.month = month;
          myAlarm.day = day;
          myAlarm.weekdays = 0;
        } else if (dateType == 2) {
          myAlarm.year = 0;
          myAlarm.month = 0;
          myAlarm.day = 0;
          myAlarm.weekdays = weekdaysMask;
        } else {
          myAlarm.year = 0;
          myAlarm.month = 0;
          myAlarm.day = 0;
          myAlarm.weekdays = 0;
        }

        if (save) {
          saveAlarmToNVS();
        } else {
          myAlarm.saved = false;
          updateAlarmIndicator();
        }

        // Print confirmation
        Serial.print("Alarm set for ");
        if (dateType == 1) {
          Serial.printf("%04d-%02d-%02d ", year, month, day);
        } else if (dateType == 2) {
          Serial.print("Weekdays: ");
          if (weekdaysMask & 0x01) Serial.print("Mon ");
          if (weekdaysMask & 0x02) Serial.print("Tue ");
          if (weekdaysMask & 0x04) Serial.print("Wed ");
          if (weekdaysMask & 0x08) Serial.print("Thu ");
          if (weekdaysMask & 0x10) Serial.print("Fri ");
          if (weekdaysMask & 0x20) Serial.print("Sat ");
          if (weekdaysMask & 0x40) Serial.print("Sun ");
        } else {
          Serial.print("Daily ");
        }
        Serial.printf("%02d:%02d", hour, minute);
        if (repeat) Serial.print(" [R]");
        if (save) Serial.print(" [S]");
        if (text[0]) Serial.printf(" '%s'", text);
        Serial.println();
      }
      // TIMER CLEAR command
      else if (cmd.equals("TIMER CLEAR")) {
        timerActive = false;
        timerTriggered = false;
        buzzerActive = false;
        digitalWrite(BUZZER_PIN, LOW);
        Serial.println("Timer cleared");
      }
      // SET TIMER command (improved format - removed 1 hour limit)
      else if (cmd.startsWith("TIMER ")) {
        String s = cmd.substring(6);
        s.trim();

        // Find where time part ends
        int firstSpace = s.indexOf(' ');
        String timePart;
        String rest = "";

        if (firstSpace != -1) {
          timePart = s.substring(0, firstSpace);
          rest = s.substring(firstSpace + 1);
          rest.trim();
        } else {
          timePart = s;
        }

        // Parse time in different formats
        int hour = 0, minute = 0, second = 0;
        int colonCount = 0;

        for (int i = 0; i < timePart.length(); i++) {
          if (timePart[i] == ':') colonCount++;
        }

        if (colonCount == 2) {
          // HH:MM:SS format
          int c1 = timePart.indexOf(':');
          int c2 = timePart.indexOf(':', c1 + 1);
          hour = timePart.substring(0, c1).toInt();
          minute = timePart.substring(c1 + 1, c2).toInt();
          second = timePart.substring(c2 + 1).toInt();
        } else if (colonCount == 1) {
          // MM:SS format
          int c1 = timePart.indexOf(':');
          minute = timePart.substring(0, c1).toInt();
          second = timePart.substring(c1 + 1).toInt();
        } else if (colonCount == 0) {
          // SS format
          second = timePart.toInt();
        } else {
          Serial.println("Format: TIMER [HH:]MM:SS [TEXT] or TIMER SS [TEXT]");
          Serial.print("> ");
          return;
        }

        // Validate
        if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
          Serial.println("Invalid time values (Hours: 0-23, Minutes: 0-59, Seconds: 0-59)");
          Serial.print("> ");
          return;
        }

        unsigned long totalSec = (unsigned long)hour * 3600 + (unsigned long)minute * 60 + (unsigned long)second;

        if (totalSec == 0) {
          Serial.println("Timer must be at least 1 second");
          Serial.print("> ");
          return;
        }

        // Maximum ~24 hours (86400 seconds)
        if (totalSec > 86400) {
          Serial.println("Timer maximum is 24 hours");
          Serial.print("> ");
          return;
        }

        // Default text
        strcpy(timerText, "TIMER");

        // Parse TEXT (max 30 bytes for UTF-8)
        if (rest.length() > 0) {
          int len = min((int)rest.length(), 30);
          rest.substring(0, len).toCharArray(timerText, sizeof(timerText));
        }

        timerActive = true;
        timerTriggered = false;
        timerStartUs = esp_timer_get_time();
        timerDurationUs = (uint64_t)totalSec * 1000000;

        Serial.print("Timer set for ");
        if (hour > 0) Serial.printf("%02d:", hour);
        if (minute > 0 || hour > 0) Serial.printf("%02d:", minute);
        Serial.printf("%02d", second);
        Serial.print(" seconds");

        if (strcmp(timerText, "TIMER") != 0) {
          Serial.print(", text='");
          Serial.print(timerText);
          Serial.print("'");
        }
        Serial.println();
      }
      // Hidden debug commands (not shown in HELP)
      else if (cmd.equals("BUZZER ON")) {
        buzzerActive = true;
        Serial.println("Buzzer ON");
      } else if (cmd.equals("BUZZER OFF")) {
        buzzerActive = false;
        digitalWrite(BUZZER_PIN, LOW);
        Serial.println("Buzzer OFF");
      } else {
        Serial.println("Unknown command. Type HELP for commands.");
      }

      Serial.print("> ");
    } else {
      serialInput += c;
    }
  }
}

// ================= TIME FUNCTIONS =================
void updateClockStrings() {
  if (getLocalTime(&timeinfo) && timeinfo.tm_year > 120) {
    snprintf(hhStr, sizeof(hhStr), "%02d", timeinfo.tm_hour);
    snprintf(mmStr, sizeof(mmStr), "%02d", timeinfo.tm_min);
    snprintf(dateStr, sizeof(dateStr), "%02d.%02d.%04d",
             timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900);

    int wday = timeinfo.tm_wday;
    if (wday >= 0 && wday < 7) {
      strcpy(weekdayStr, weekdaysRU[wday]);
    } else {
      strcpy(weekdayStr, "---");
    }
  }
}

/*void drawClock() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_logisoso24_tn);

  // Center "88:88"
  const char* ref = "88:88";
  int refW = u8g2.getStrWidth(ref);
  int refX = X_OFF + (DISP_W - refW) / 2;
  int refY = Y_OFF + 30;

  int colonOffset = u8g2.getStrWidth("88");
  int hhX = refX;
  int colonX = refX + colonOffset;
  int mmX = colonX + u8g2.getStrWidth(":");

  u8g2.setCursor(hhX, refY);
  u8g2.print(hhStr);

  u8g2.setCursor(colonX, refY);
  u8g2.print(colonVisible ? ":" : " ");

  u8g2.setCursor(mmX, refY);
  u8g2.print(mmStr);

  // Date and indicators
  String ds = String(dateStr);
  if (myAlarm.active) ds += " *";
  if (timerActive) ds += " #";

  u8g2.setFont(u8g2_font_5x7_t_cyrillic);
  int dw = u8g2.getStrWidth(ds.c_str());
  int dx = X_OFF + (DISP_W - dw) / 2;
  u8g2.setCursor(dx, Y_OFF + DISP_H - 1);
  u8g2.print(ds);

  u8g2.sendBuffer();
}
*/
        /*void drawClock() {
  tft.setTextSize(5);
  
  // CORECTARE: x1 și y1 rămân cu semn (int16_t), dar refW și refH trebuie să fie fără semn (uint16_t)
  int16_t x1, y1;
  uint16_t refW, refH; 
  
  const char* ref = "88:88";
  tft.getTextBounds(ref, 0, 0, &x1, &y1, &refW, &refH);
  
  int refX = (240 - refW) / 2;
  int refY = 20; 

  // CORECTARE: Toate variabilele de lățime/înălțime transmise cu ampersand (&) trebuie să fie uint16_t
  uint16_t w88, h88, wColon, hColon;
  tft.getTextBounds("88", 0, 0, &x1, &y1, &w88, &h88);
  tft.getTextBounds(":", 0, 0, &x1, &y1, &wColon, &hColon);

  int hhX = refX;
  int colonX = refX + w88;
  int mmX = colonX + wColon;

  tft.setTextColor(ST77XX_CYAN, ST77XX_BLACK);

  tft.setCursor(hhX, refY);
  tft.print(hhStr);

  tft.setCursor(colonX, refY);
  tft.print(colonVisible ? ":" : " ");

  tft.setCursor(mmX, refY);
  tft.print(mmStr);

  String ds = String(dateStr);
  if (myAlarm.active) ds += " *";
  if (timerActive) ds += " #";

  tft.setTextSize(2);
  uint16_t dw, dh; // CORECTARE: Definit corect ca uint16_t
  tft.getTextBounds(ds.c_str(), 0, 0, &x1, &y1, &dw, &dh);
  
  int dx = (240 - dw) / 2;
  int dy = 90; 

  tft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
  tft.setCursor(dx, dy);
  tft.print(ds);
}
        */
 void drawClock() {
  // 1. Curățăm ecranul virtual din RAM cu negru (se întâmplă instantaneu, fără pâlpâire)
  canvas.fillScreen(ST77XX_BLACK);
  
  // 2. Configurare font ceas uriaș pe canvas
  canvas.setFont(&FreeSansBold24pt7b);
  canvas.setTextSize(2); 
  
  int16_t x1, y1;
  uint16_t refW, refH; 
  
  String fullClockStr = String(hhStr) + (colonVisible ? ":" : " ") + String(mmStr);
  canvas.getTextBounds(fullClockStr.c_str(), 0, 0, &x1, &y1, &refW, &refH);
  
  // Centrarea pe axa X calculată direct în buffer
  int refX = (240 - refW) / 2;
  int refY = 75; 

  // Desenăm textul ceasului în memoria virtuală (Cyan)
  canvas.setTextColor(ST77XX_CYAN);
  canvas.setCursor(refX, refY);
  canvas.print(fullClockStr);

  // 3. Pregătim linia de text pentru Dată și Indicatori
  String ds = String(dateStr);
  if (myAlarm.active) ds += " *";
  if (timerActive) ds += " #";

  // Schimbăm fontul pe canvas pentru linia de dată
  canvas.setFont(&FreeSans9pt7b);
  canvas.setTextSize(1); 
  
  uint16_t dw, dh;
  canvas.getTextBounds(ds.c_str(), 0, 0, &x1, &y1, &dw, &dh);
  
  int dx = (240 - dw) / 2;
  int dy = 115; 

  // Desenăm data în memoria virtuală (Galbenă)
  canvas.setTextColor(ST77XX_YELLOW);
  canvas.setCursor(dx, dy);
  canvas.print(ds);
  
  // 4. IMPULSUL HARDWARE FIX: Împingem toată imaginea procesată din RAM direct pe ecran
  // Această funcție trimite pixelii direct prin SPI și oferă o fluiditate perfectă, de 0% flicker
  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), 240, 135);
  
  // Resetăm fontul implicit al ecranului fizic pentru celelalte meniuri
  tft.setFont(); 
}
/*void drawAlarmOrTimer(const char* txt) {
  u8g2.clearBuffer();

  String s = (txt && txt[0]) ? String(txt) : "ALARM";

  // Use smaller font with Cyrillic support for better fit
  u8g2.setFont(u8g2_font_5x8_t_cyrillic);
  
  // For Cyrillic, use getUTF8Width()
  int w = u8g2.getUTF8Width(s.c_str());
  
  // Debug output
  Serial.print("Text: '");
  Serial.print(s);
  Serial.print("', UTF8Width: ");
  Serial.print(w);
  
  // If text is too wide, truncate
  while (w > DISP_W && s.length() > 0) {
    // Remove last character (UTF-8 aware)
    int lastPos = s.length() - 1;
    // Go back to find UTF-8 character start
    while (lastPos > 0 && (s[lastPos] & 0xC0) == 0x80) {
      lastPos--;
    }
    s = s.substring(0, lastPos);
    w = u8g2.getUTF8Width(s.c_str());
  }
  
  // Center within the visible window
  int x = X_OFF + (DISP_W - w) / 2;
  int y = Y_OFF + 18;
  
  Serial.print(", X: ");
  Serial.print(x);
  Serial.print(", Y: ");
  Serial.print(y);
  Serial.print(", Width: ");
  Serial.println(w);

  // Use drawUTF8 instead of setCursor + print for proper UTF-8 rendering
  u8g2.drawUTF8(x, y, s.c_str());

  // Bottom text with smaller font
  u8g2.setFont(u8g2_font_4x6_t_cyrillic);
  String stopMsg = "BOOT-STOP";
  int stopW = u8g2.getUTF8Width(stopMsg.c_str());
  int stopX = X_OFF + (DISP_W - stopW) / 2;
  u8g2.drawUTF8(stopX, Y_OFF + DISP_H - 2, stopMsg.c_str());

  u8g2.sendBuffer();
}
*/
void drawAlarmOrTimer(const char* txt) {
  // Ștergem ecranul
  tft.fillScreen(ST77XX_BLACK);

  String s = (txt && txt[0]) ? String(txt) : "ALARM";

  tft.setTextSize(2); // Folosim mărimea 2 pentru vizibilitate clară pe TFT
  
  int16_t x1, y1;
  uint16_t w, h;
  
  // Măsurăm lățimea textului folosind motorul Adafruit GFX
  tft.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  
  // Debug serial (păstrat din logica ta)
  Serial.print("Text: '");
  Serial.print(s);
  Serial.print("', Width: ");
  Serial.print(w);
  
  // ALGORITMUL TĂU: Dacă textul e mai lat decât ecranul (240px), îl scurtăm caracter cu caracter
  while (w > 240 && s.length() > 0) {
    int lastPos = s.length() - 1;
    // Gestionare biți pentru compatibilitate caractere speciale UTF-8
    while (lastPos > 0 && (s[lastPos] & 0xC0) == 0x80) {
      lastPos--;
    }
    s = s.substring(0, lastPos);
    
    // Recalculăm lățimea noului șir scurtat
    tft.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  }
  
  Serial.print(", Final X Centered Width: ");
  Serial.println(w);

  // Centrare pe ecran (Lățime ecran = 240)
  int x = (240 - w) / 2;
  int y = 45; // Poziționat central pe înălțime
  
  // Desenăm textul alarmei sau timerului cu Roșu Aprins (Alertă)
  tft.setCursor(x, y);
  tft.setTextColor(ST77XX_RED);
  tft.print(s);

  // Mesajul de oprire din subsol (BOOT-STOP) - afișat mai mic și centrat
  tft.setTextSize(1);
  String stopMsg = "BOOT-STOP";
  tft.getTextBounds(stopMsg, 0, 0, &x1, &y1, &w, &h);
  
  int stopX = (240 - w) / 2;
  int stopY = 115; // Aproape de marginea de jos
  
  tft.setCursor(stopX, stopY);
  tft.setTextColor(ST77XX_WHITE);
  tft.print(stopMsg);
}
/*void drawInfoScreen1() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_t_cyrillic);

  int y = Y_OFF + INFO_Y_OFFSET;

  u8g2.setCursor(X_OFF, y);
  u8g2.print("INFO 1/2");
  y += 8;

  u8g2.setCursor(X_OFF, y);
  u8g2.printf("Day: %s", weekdayStr);
  y += 8;

  u8g2.setCursor(X_OFF, y);
  if (myAlarm.active) {
    u8g2.printf("Alarm: %02d:%02d", myAlarm.hour, myAlarm.minute);
  } else {
    u8g2.print("Alarm: OFF");
  }
  y += 8;

  u8g2.setCursor(X_OFF, y);
  if (timerActive) {
    uint64_t elapsed = esp_timer_get_time() - timerStartUs;
    uint64_t remaining = (elapsed >= timerDurationUs) ? 0 : (timerDurationUs - elapsed);
    int secRemaining = (remaining + 500000) / 1000000;
    u8g2.printf("Timer: %02d sec", secRemaining);
  } else {
    u8g2.print("Timer: OFF");
  }
  y += 8;

  u8g2.setCursor(X_OFF, y);
  u8g2.printf("WiFi: %s", WiFi.status() == WL_CONNECTED ? "ON" : "OFF");

  u8g2.sendBuffer();
}
*/
void drawInfoScreen1() {
  // Curățăm ecranul cu fundal negru
  tft.fillScreen(ST77XX_BLACK);
  
  // Setăm text de mărime 2 (foarte lizibil)
  tft.setTextSize(1);
  
  // Începem scrierea mai de sus, profitând de ecranul mai înalt
  int y = 15; 
  int x = 15; // Marginea din stânga

  // Titlu pagină (Portocaliu)
  tft.setCursor(x, y);
  tft.setTextColor(0xFA60); // Portocaliu aprins
  tft.print("INFO 1/2");
  y += 22; // Spațiere generoasă între rânduri

  // Ziua curentă (Alb)
  tft.setCursor(x, y);
  tft.setTextColor(ST77XX_WHITE);
  tft.print("Day: ");
  tft.print(weekdayStr);
  y += 22;

  // Stare Alarmă (Dacă e activă, afișăm ora cu Roșu, altfel text normal)
  tft.setCursor(x, y);
  tft.print("Alarm: ");
  if (myAlarm.active) {
    tft.setTextColor(ST77XX_RED);
    if (myAlarm.hour < 10) tft.print("0");
    tft.print(myAlarm.hour);
    tft.print(":");
    if (myAlarm.minute < 10) tft.print("0");
    tft.print(myAlarm.minute);
  } else {
    tft.setTextColor(0x7BEF); // Gri închis/stins pentru "OFF"
    tft.print("OFF");
  }
  y += 22;

  // Stare Timer (Cyan dacă rulează)
  tft.setCursor(x, y);
  tft.setTextColor(ST77XX_WHITE);
  tft.print("Timer: ");
  if (timerActive) {
    tft.setTextColor(0x07FF); // Cyan/Albastru deschis
    uint64_t elapsed = esp_timer_get_time() - timerStartUs;
    uint64_t remaining = (elapsed >= timerDurationUs) ? 0 : (timerDurationUs - elapsed);
    int secRemaining = (remaining + 500000) / 1000000;
    tft.print(secRemaining);
    tft.print(" sec");
  } else {
    tft.setTextColor(0x7BEF); // Gri
    tft.print("OFF");
  }
  y += 22;

  // Stare WiFi (Verde dacă e conectat, Roșu dacă e deconectat)
  tft.setCursor(x, y);
  tft.setTextColor(ST77XX_WHITE);
  tft.print("WiFi: ");
  if (WiFi.status() == WL_CONNECTED) {
    tft.setTextColor(ST77XX_GREEN);
    tft.print("ON");
  } else {
    tft.setTextColor(ST77XX_RED);
    tft.print("OFF");
  }
}

/*void drawInfoScreen2() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_4x6_t_cyrillic);

  int y = Y_OFF + INFO_Y_OFFSET;

  u8g2.setCursor(X_OFF, y);
  u8g2.print("INFO 2/2");
  y += 7;

  u8g2.setCursor(X_OFF, y);
  String ssidDisplay = wifiSSID;
  if (ssidDisplay.length() > 14) {
    ssidDisplay = ssidDisplay.substring(0, 14) + "...";
  }
  u8g2.printf("SSID: %s", ssidDisplay.c_str());
  y += 7;

  u8g2.setCursor(X_OFF, y);
  if (WiFi.status() == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP();
    u8g2.printf("IP: %d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  } else {
    u8g2.print("IP: No connection");
  }
  y += 7;

  u8g2.setCursor(X_OFF, y);
  u8g2.printf("Time: %s", timeValid ? "SYNC" : "NO SYNC");
  y += 7;

  u8g2.setCursor(X_OFF, y);
  u8g2.printf("RAM: %d KB", esp_get_free_heap_size() / 1024);

  u8g2.sendBuffer();
}
*/
void drawInfoScreen2() {
  // Curățăm ecranul
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  
  int y = 15;
  int x = 15;

  // Titlu pagină (Portocaliu)
  tft.setCursor(x, y);
  tft.setTextColor(0xFA60);
  tft.print("INFO 2/2");
  y += 22;

  // Nume rețea WiFi (SSID) - Ecranul fiind mai lat, putem lăsa lungimea până la 18 caractere!
  tft.setCursor(x, y);
  tft.setTextColor(ST77XX_WHITE);
  String ssidDisplay = wifiSSID;
  if (ssidDisplay.length() > 18) {
    ssidDisplay = ssidDisplay.substring(0, 18) + "...";
  }
  tft.print("SSID: ");
  tft.print(ssidDisplay);
  y += 22;

  // Afișare Adresă IP (Verde dacă avem conexiune)
  tft.setCursor(x, y);
  tft.print("IP: ");
  if (WiFi.status() == WL_CONNECTED) {
    tft.setTextColor(ST77XX_GREEN);
    tft.print(WiFi.localIP());
  } else {
    tft.setTextColor(ST77XX_RED);
    tft.print("No connection");
  }
  y += 22;

  // Sincronizare timp (NTP Status)
  tft.setCursor(x, y);
  tft.setTextColor(ST77XX_WHITE);
  tft.print("Time: ");
  if (timeValid) {
    tft.setTextColor(0x07FF); // Cyan pentru SYNC
    tft.print("SYNC");
  } else {
    tft.setTextColor(ST77XX_YELLOW);
    tft.print("NO SYNC");
  }
  y += 22;

  // Memorie RAM disponibilă
  tft.setCursor(x, y);
  tft.setTextColor(ST77XX_WHITE);
  tft.print("RAM: ");
  tft.print(esp_get_free_heap_size() / 1024);
  tft.print(" KB");
}
// ================= SYSTEM FUNCTIONS =================
void showSplash() {
  // Ștergem ecranul cu fundal negru
  tft.fillScreen(ST77XX_BLACK);
  
  // Setăm mărimea 3 (mare și de impact pentru boot)
  tft.setTextSize(3);
  
  // Afișăm primul rând cu text Albastru/Cyan deschis
  tft.setTextColor(0x07FF); 
  tft.setCursor(45, 40); // Poziționare centrată pe noul ecran
  tft.print("ESP32-DEV");
  
  // Afișăm al doilea rând cu Portocaliu aprins
  tft.setTextColor(0xFA60); 
  tft.setCursor(70, 80);
  tft.print("CLOCK");
  
  // Pauză pentru a lăsa logo-ul vizibil la pornire
  delay(100);
   tft.fillScreen(ST77XX_BLACK);
}
// ================= SYSTEM FUNCTIONS =================
/*void showSplash() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_unifont_t_cyrillic);
  u8g2.setCursor(X_OFF + 6, Y_OFF + 20);
  u8g2.print("ESP32-C3");
  u8g2.setCursor(X_OFF + 20, Y_OFF + 36);
  u8g2.print("CLOCK");
  u8g2.sendBuffer();
  delay(1500);
}
*/
/*void showSplash() {
  // Ștergem ecranul cu fundal negru
  tft.fillScreen(ST77XX_BLACK);
  
  // Setăm mărimea 3 (mare și de impact pentru boot)
  tft.setTextSize(3);
  
  // Afișăm primul rând cu text Albastru/Cyan deschis
  tft.setTextColor(0x07FF); 
  tft.setCursor(45, 40); // Poziționare centrată pe noul ecran
  tft.print("ESP32-C3");
  
  // Afișăm al doilea rând cu Portocaliu aprins
  tft.setTextColor(0xFA60); 
  tft.setCursor(70, 80);
  tft.print("CLOCK");
  
  // Pauză pentru a lăsa logo-ul vizibil la pornire
  delay(1500);
}
*/
void connectWiFi() {
  Serial.println("Connecting to WiFi...");
  Serial.println("SSID: " + wifiSSID);

  WiFi.disconnect(true);
  WiFi.begin(wifiSSID.c_str(), wifiPASS.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > 10000) {
      Serial.println("WiFi timeout");
      return;
    }
    delay(200);
  }

  Serial.println("WiFi connected");
  Serial.println(WiFi.localIP());
}

void syncTime() {
  timeValid = false;
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer.c_str());
  for (int i = 0; i < 30; i++) {
    if (getLocalTime(&timeinfo) && timeinfo.tm_year > 120) {
      timeValid = true;
      Serial.println("Time synced");
      return;
    }
    delay(500);
  }
  Serial.println("Time sync failed");
}

void setManualTime(String s) {
  struct tm t {};
  sscanf(s.c_str(), "%d-%d-%d %d:%d:%d",
         &t.tm_year, &t.tm_mon, &t.tm_mday,
         &t.tm_hour, &t.tm_min, &t.tm_sec);
  t.tm_year -= 1900;
  t.tm_mon -= 1;

  time_t tt = mktime(&t);
  struct timeval now = { tt, 0 };
  settimeofday(&now, nullptr);
  timeValid = true;
  updateClockStrings();
  Serial.println("Manual time set");
}

// ================= BUTTON HANDLER =================
void handleButton() {
  static bool btnPrev = HIGH;
  bool btnNow = digitalRead(BOOT_PIN);

  if (btnPrev == HIGH && btnNow == LOW) {
    if (millis() - lastBtnTime > BTN_DEBOUNCE) {
      if (alarmTriggered || timerTriggered) {
        // Stop alarm/timer
        if (alarmTriggered && !myAlarm.repeat) {
          myAlarm.active = false;
          if (myAlarm.saved) clearAlarmFromNVS();
        }
        if (timerTriggered) timerActive = false;

        alarmTriggered = false;
        timerTriggered = false;
        buzzerActive = false;
        digitalWrite(BUZZER_PIN, LOW);
        currentScreen = SCREEN_CLOCK;
        infoScreenPage = 1;
        updateAlarmIndicator();
        Serial.println("Signal stopped");
      } else if (currentScreen == SCREEN_CLOCK) {
        currentScreen = SCREEN_INFO1;
        infoScreenPage = 1;
        infoStartTime = millis();
        Serial.println("Showing Info Screen 1");
      } else if (currentScreen == SCREEN_INFO1) {
        currentScreen = SCREEN_INFO2;
        infoScreenPage = 2;
        infoStartTime = millis();
        Serial.println("Showing Info Screen 2");
      } else if (currentScreen == SCREEN_INFO2) {
        currentScreen = SCREEN_CLOCK;
        infoScreenPage = 1;
        Serial.println("Returning to Clock");
      }
      lastBtnTime = millis();
    }
  }
  btnPrev = btnNow;
}

// ================= ALARM CHECK =================
bool checkAlarmMatch() {
  if (!myAlarm.active || alarmTriggered) return false;

  // Check specific date
  if (myAlarm.year > 0) {
    if (timeinfo.tm_year + 1900 != myAlarm.year || timeinfo.tm_mon + 1 != myAlarm.month || timeinfo.tm_mday != myAlarm.day) {
      return false;
    }
  }
  // Check weekdays
  else if (myAlarm.weekdays > 0) {
    // Convert tm_wday (0=Sun,6=Sat) to our bitmask (0=Mon,6=Sun)
    int wday = timeinfo.tm_wday;
    if (wday == 0) wday = 6;  // Sunday becomes 6
    else wday -= 1;           // Mon=0, Tue=1, etc.

    if (!(myAlarm.weekdays & (1 << wday))) {
      return false;
    }
  }
  // Daily alarm - always match date-wise

  // Check time
  if (timeinfo.tm_hour != myAlarm.hour || timeinfo.tm_min != myAlarm.minute || timeinfo.tm_sec != 0) {
    return false;
  }

  return true;
}

void handleAutoReturn() {
  if ((currentScreen == SCREEN_INFO1 || currentScreen == SCREEN_INFO2) && millis() - infoStartTime > INFO_TIMEOUT) {
    currentScreen = SCREEN_CLOCK;
    infoScreenPage = 1;
  }
}

// ================= MAIN LOOP =================
void loop() {
  server.handleClient(); // Handle web requests
  
  handleSerial();
  handleButton();
  handleAutoReturn();

  // Blinking colon
  if (millis() - lastBlink >= 500) {
    colonVisible = !colonVisible;
    lastBlink = millis();
  }

  // Blue LED control for timer indication
  static unsigned long lastLedBlink = 0;
  static bool ledState = false;
  
  if (timerActive && !timerTriggered) {
    // Blink LED for timer (500ms on, 500ms off)
    if (millis() - lastLedBlink >= 500) {
      ledState = !ledState;
      digitalWrite(BLUE_LED_PIN, ledState ? LOW : HIGH);
      lastLedBlink = millis();
    }
  } else {
    // Normal alarm indication (solid on/off)
    bool shouldBeOn = myAlarm.active && !alarmTriggered;
    digitalWrite(BLUE_LED_PIN, shouldBeOn ? LOW : HIGH);
  }

  // Update time
  if (getLocalTime(&timeinfo) && timeinfo.tm_year > 120) {
    timeValid = true;
  }

  // Alarm check (once per second)
  static unsigned long lastSec = 0;
  if (timeValid && millis() - lastSec >= 1000) {
    lastSec = millis();
    updateClockStrings();

    if (checkAlarmMatch()) {
      alarmTriggered = true;
      buzzerActive = true;
      currentScreen = SCREEN_ALARM;
      updateAlarmIndicator();
      Serial.println("ALARM TRIGGERED!");
    }
  }

  // Timer check
  if (timerActive && !timerTriggered) {
    uint64_t elapsed = esp_timer_get_time() - timerStartUs;
    if (elapsed >= timerDurationUs) {
      timerTriggered = true;
      buzzerActive = true;
      currentScreen = SCREEN_TIMER;
      Serial.println("TIMER TRIGGERED!");
    }
  }

  // Buzzer control - alarm pattern: beep-pause-beep-pause-beep-long pause
  if (buzzerActive) {
    uint64_t phase = esp_timer_get_time() % 2000000;  // 2 second cycle
    bool on = false;
    
    // First beep: 0-150ms
    if (phase < 150000) {
      on = true;
    }
    // Pause: 150-300ms
    // Second beep: 300-450ms
    else if (phase >= 300000 && phase < 450000) {
      on = true;
    }
    // Pause: 450-600ms
    // Third beep: 600-750ms
    else if (phase >= 600000 && phase < 750000) {
      on = true;
    }
    // Long pause: 750-2000ms
    
    digitalWrite(BUZZER_PIN, on ? HIGH : LOW);
  } else {
    digitalWrite(BUZZER_PIN, LOW);
  }

  // Display
  switch (currentScreen) {
    case SCREEN_INFO1:
      drawInfoScreen1();
      delay(100);
      break;

    case SCREEN_INFO2:
      drawInfoScreen2();
      delay(100);
      break;

    case SCREEN_ALARM:
      if (alarmTriggered) {
        drawAlarmOrTimer(myAlarm.text);
        delay(20);
      } else {
        currentScreen = SCREEN_CLOCK;
      }
      break;

    case SCREEN_TIMER:
      if (timerTriggered) {
        drawAlarmOrTimer(timerText);
        delay(20);
      } else {
        currentScreen = SCREEN_CLOCK;
      }
      break;

    case SCREEN_CLOCK:
    default:
     /* if (timeValid) {
        drawClock();
      } else {
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_5x7_t_cyrillic);
        u8g2.setCursor(X_OFF, Y_OFF + 20);
        u8g2.print("NO TIME SYNC");
        u8g2.sendBuffer();
      }
      delay(50);
      break;
  }
  */
 if (timeValid) {
    drawClock();
} else {
    // Curățăm ecranul cu fundal negru
    tft.fillScreen(ST77XX_BLACK);
    
    // Setăm text de mărime 2, culoare Roșie pentru avertizare
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_RED);
    
    // Poziționăm textul în centrul ecranului TFT (240x135)
    // "NO TIME SYNC" are 12 caractere. La mărimea 2, un caracter are ~12 pixeli lățime (12 * 12 = 144px lățime totală)
    tft.setCursor((240 - 144) / 2, 55); 
    tft.print("NO TIME SYNC");
}
 delay(50);
      break;
  }
}

