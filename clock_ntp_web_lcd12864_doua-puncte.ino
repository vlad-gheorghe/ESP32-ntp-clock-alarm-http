/*
 * ESP32 Clock with EA DOGL128 Display, WiFi, NTP, Alarm and Timer
 * Adaptat pentru display monocrom 128x64 ST7565 SPI (U8g2)
 */

#include <U8g2lib.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include <Preferences.h>
#include <esp_timer.h>

// Web Server
WebServer server(80);

// ================= HARDWARE PINS =================
// Pinii pentru Display SPI
#define CS_PIN    5
#define DC_PIN    4
#define RST_PIN   2
// MOSI (23) și SCK (18) sunt preluați automat de interfața SPI hardware a ESP32

// Alți pini din proiectul original
#define BOOT_PIN 35
#define BUZZER_PIN 21
#define BLUE_LED_PIN 17
#define LED_ACTIVE_LOW true

// Inițializare Display
U8G2_ST7565_EA_DOGM128_F_4W_HW_SPI u8g2(U8G2_R0, CS_PIN, DC_PIN, RST_PIN);

// ================= CONFIGURATION =================
String defSSID = "Orange-2KRR-2.4G";           
String defPASS = "3Xs9chzQ";  
String defNTP = "pool.ntp.org";         
long defGMTOffset = 3 * 3600;           
long defDaylightOffset = 0;             

String wifiSSID;
String wifiPASS;
String ntpServer;
long gmtOffset_sec;
long daylightOffset_sec;

// NVS Storage
Preferences prefs;
const char* PREF_NS = "clockcfg";       
const char* ALARM_PREF_NS = "alarms";   

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
const unsigned long INFO_TIMEOUT = 10000;  
unsigned long infoStartTime = 0;

// Blinking Colon
bool colonVisible = true;
unsigned long lastBlink = 0;

// Button
const unsigned long BTN_DEBOUNCE = 300;  
unsigned long lastBtnTime = 0;

// Buzzer
bool buzzerActive = false;

// Time Strings
char hhStr[3];        
char mmStr[3];        
char dateStr[25];     
char weekdayStr[15];  

// Serial Input
String serialInput = "";
bool promptShown = false;

// Command History
const int HISTORY_SIZE = 10;
String commandHistory[HISTORY_SIZE];
int historyIndex = 0;           
int historyCount = 0;           
int historyBrowseIndex = -1;   
String tempInput = "";          

// Alarm Structure
struct Alarm {
  bool active = false;
  int year = 0;         
  int month = 0;        
  int day = 0;          
  int weekdays = 0;     
  int hour = 0;         
  int minute = 0;       
  bool repeat = false;  
  bool saved = false;   
  char text[31] = "";   
};
Alarm myAlarm;

// Timer
bool timerActive = false;
uint64_t timerStartUs = 0;
uint64_t timerDurationUs = 0;
char timerText[31] = "";  

// Trigger Flags
bool timerTriggered = false;
bool alarmTriggered = false;

// Weekdays (for display)
const char* weekdaysRU[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

// ================= DECLARĂRI FUNCȚII =================
void loadConfigFromNVS();
void loadAlarmFromNVS();
void saveConfigToNVS();
void saveAlarmToNVS();
void clearAlarmFromNVS();
void eraseNVS();
void updateAlarmIndicator();
void showSplash();
void connectWiFi();
void syncTime();
void updateClockStrings();
void setupWebServer();
void setManualTime(String s);

// ================= DISPLAY FUNCTIONS (MODIFIED FOR U8G2) =================

void drawClock() {
  u8g2.clearBuffer();
  
  // Font mare pentru ora
  u8g2.setFont(u8g2_font_logisoso32_tn);
  
  // Măsurăm elementele pentru a păstra distanța stabilă
  int hhW = u8g2.getStrWidth(hhStr);
  int colonW = u8g2.getStrWidth(":");
  int mmW = u8g2.getStrWidth(mmStr);
  
  int totalW = hhW + colonW + mmW;
  int startX = (128 - totalW) / 2;
  
  // 1. Desenăm orele
  u8g2.drawStr(startX, 38, hhStr);
  
  // 2. Animația alternantă a punctelor (sus / jos - centrate pe cifre)
  int colonX = startX + hhW;
  if (colonVisible) {
    // Punctul de sus
    u8g2.drawStr(colonX, 20, ".");
  } else {
    // Punctul de jos 
    u8g2.drawStr(colonX, 34, ".");
  }
  
  // 3. Desenăm minutele
  u8g2.drawStr(startX + hhW + colonW, 38, mmStr);

  // Date și Indicatori
  String ds = String(dateStr);
  if (myAlarm.active) ds += " *";
  if (timerActive) ds += " #";

  // Font îngroșat pentru dată
  u8g2.setFont(u8g2_font_7x13B_tr);
  int dateWidth = u8g2.getStrWidth(ds.c_str());
  int dateX = (128 - dateWidth) / 2;
  
  u8g2.drawStr(dateX, 53, ds.c_str());
  
  // ==== ADAUGAREA IP-ULUI ====
  if (WiFi.status() == WL_CONNECTED) {
    String ipStr = WiFi.localIP().toString();
    
    u8g2.setFont(u8g2_font_5x7_tr); 
    int ipWidth = u8g2.getStrWidth(ipStr.c_str());
    int ipX = 128 - ipWidth - 2; 
    
    u8g2.drawStr(ipX, 63, ipStr.c_str());
  }
  // ===========================

  u8g2.sendBuffer();
}


void drawAlarmOrTimer(const char* txt) {
  u8g2.clearBuffer();
  
  String s = (txt && txt[0]) ? String(txt) : "ALARM";
  u8g2.setFont(u8g2_font_ncenB14_tr);
  
  int textW = u8g2.getStrWidth(s.c_str());
  while (textW > 128 && s.length() > 0) {
    s = s.substring(0, s.length() - 1);
    textW = u8g2.getStrWidth(s.c_str());
  }
  
  int x = (128 - textW) / 2;
  u8g2.drawStr(x, 35, s.c_str());

  u8g2.setFont(u8g2_font_5x8_tr);
  String stopMsg = "BOOT-STOP";
  int stopW = u8g2.getStrWidth(stopMsg.c_str());
  int stopX = (128 - stopW) / 2;
  
  u8g2.drawStr(stopX, 58, stopMsg.c_str());
  
  u8g2.sendBuffer();
}

void drawInfoScreen1() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);
  
  int y = 10; 
  int x = 5;
  char buf[40];

  u8g2.drawStr(x, y, "INFO 1/2"); y += 12;

  sprintf(buf, "Day: %s", weekdayStr);
  u8g2.drawStr(x, y, buf); y += 12;

  if (myAlarm.active) {
    sprintf(buf, "Alarm: %02d:%02d", myAlarm.hour, myAlarm.minute);
  } else {
    sprintf(buf, "Alarm: OFF");
  }
  u8g2.drawStr(x, y, buf); y += 12;

  if (timerActive) {
    uint64_t elapsed = esp_timer_get_time() - timerStartUs;
    uint64_t remaining = (elapsed >= timerDurationUs) ? 0 : (timerDurationUs - elapsed);
    int secRemaining = (remaining + 500000) / 1000000;
    sprintf(buf, "Timer: %d sec", secRemaining);
  } else {
    sprintf(buf, "Timer: OFF");
  }
  u8g2.drawStr(x, y, buf); y += 12;

  sprintf(buf, "WiFi: %s", WiFi.status() == WL_CONNECTED ? "ON" : "OFF");
  u8g2.drawStr(x, y, buf);

  u8g2.sendBuffer();
}

void drawInfoScreen2() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);
  
  int y = 10;
  int x = 5;
  char buf[40];

  u8g2.drawStr(x, y, "INFO 2/2"); y += 12;

  String ssidDisplay = wifiSSID;
  if (ssidDisplay.length() > 14) {
    ssidDisplay = ssidDisplay.substring(0, 14) + "...";
  }
  sprintf(buf, "SSID: %s", ssidDisplay.c_str());
  u8g2.drawStr(x, y, buf); y += 12;

  if (WiFi.status() == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP();
    sprintf(buf, "IP: %d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  } else {
    sprintf(buf, "IP: No connection");
  }
  u8g2.drawStr(x, y, buf); y += 12;

  sprintf(buf, "Time: %s", timeValid ? "SYNC" : "NO SYNC");
  u8g2.drawStr(x, y, buf); y += 12;

  sprintf(buf, "RAM: %d KB", esp_get_free_heap_size() / 1024);
  u8g2.drawStr(x, y, buf);

  u8g2.sendBuffer();
}

void showSplash() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB10_tr);
  u8g2.drawStr(15, 25, "ESP32 CLOCK");
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(25, 45, "EA DOGL128");
  u8g2.sendBuffer();
  delay(1000); 
}

// ================= SETUP =================
void setup() {
  pinMode(BOOT_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  pinMode(BLUE_LED_PIN, OUTPUT);
  digitalWrite(BLUE_LED_PIN, HIGH);

  Serial.begin(115200);
  unsigned long start = millis();
  while (!Serial && millis() - start < 2000) delay(10);

  Serial.println("\n=== ESP32 Clock ===");
  Serial.println("Type HELP for commands");

  // Initializare U8g2 Display
  u8g2.begin();
  
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB10_tr);
  u8g2.drawStr(15, 35, "STARTING...");
  u8g2.sendBuffer();

  loadConfigFromNVS();
  loadAlarmFromNVS();
  showSplash();
  connectWiFi();
  syncTime();
  updateClockStrings();
  
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
  bool shouldBeOn = myAlarm.active && !alarmTriggered;
  if (!timerActive) {
    digitalWrite(BLUE_LED_PIN, shouldBeOn ? LOW : HIGH);
  }
}

// ================= COMMAND HISTORY FUNCTIONS =================
void addToHistory(String cmd) {
  if (cmd.length() == 0) return;
  if (historyCount > 0 && commandHistory[(historyIndex - 1 + HISTORY_SIZE) % HISTORY_SIZE] == cmd) {
    return;
  }
  commandHistory[historyIndex] = cmd;
  historyIndex = (historyIndex + 1) % HISTORY_SIZE;
  if (historyCount < HISTORY_SIZE) historyCount++;
}

String getHistoryUp() {
  if (historyCount == 0) return "";
  if (historyBrowseIndex == -1) {
    tempInput = serialInput;
    historyBrowseIndex = (historyIndex - 1 + HISTORY_SIZE) % HISTORY_SIZE;
  } else {
    int prevIndex = (historyBrowseIndex - 1 + HISTORY_SIZE) % HISTORY_SIZE;
    int oldestIndex = (historyIndex - historyCount + HISTORY_SIZE) % HISTORY_SIZE;
    if (historyBrowseIndex != oldestIndex) {
      historyBrowseIndex = prevIndex;
    }
  }
  return commandHistory[historyBrowseIndex];
}

String getHistoryDown() {
  if (historyBrowseIndex == -1) return serialInput; 
  int nextIndex = (historyBrowseIndex + 1) % HISTORY_SIZE;
  if (nextIndex == historyIndex) {
    historyBrowseIndex = -1;
    return tempInput;
  }
  historyBrowseIndex = nextIndex;
  return commandHistory[historyBrowseIndex];
}

void clearCurrentLine() {
  for (int i = 0; i < serialInput.length(); i++) {
    Serial.print("\b \b");
  }
}

// ================= WEB SERVER FUNCTIONS =================
String getStatusJSON() {
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
  
  if (timeValid) {
    char fullTime[20];
    sprintf(fullTime, "%04d-%02d-%02d %02d:%02d:%02d",
            timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
            timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    json += "\"fullTime\":\"" + String(fullTime) + "\",";
  } else {
    json += "\"fullTime\":\"\",";
  }
  
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
  <title>ESP32 Clock</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body { font-family: sans-serif; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); min-height: 100vh; padding: 20px; color: #333; }
    .container { max-width: 800px; margin: 0 auto; }
    .card { background: white; border-radius: 12px; padding: 24px; margin-bottom: 20px; box-shadow: 0 4px 6px rgba(0,0,0,0.1); }
    h1 { color: white; text-align: center; margin-bottom: 20px; }
    h2 { color: #667eea; margin-bottom: 16px; }
    .clock-display { text-align: center; font-size: 3em; font-weight: bold; color: #667eea; margin: 20px 0; }
    .date-display { text-align: center; font-size: 1.2em; color: #666; margin-bottom: 10px; }
    .status-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 12px; margin-top: 16px; }
    .status-item { padding: 12px; background: #f7f7f7; border-radius: 8px; }
    .status-label { font-size: 0.85em; color: #666; margin-bottom: 4px; }
    .status-value { font-size: 1.1em; font-weight: 600; color: #333; }
    .form-group { margin-bottom: 16px; }
    label { display: block; margin-bottom: 6px; font-weight: 500; color: #555; }
    input, select { width: 100%; padding: 10px; border: 2px solid #e0e0e0; border-radius: 6px; }
    .checkbox-group { display: flex; gap: 12px; flex-wrap: wrap; }
    .checkbox-label { display: flex; align-items: center; gap: 6px; cursor: pointer; }
    button { background: #667eea; color: white; border: none; padding: 12px 24px; border-radius: 6px; font-size: 1em; cursor: pointer; width: 100%; font-weight: 600; }
    button:hover { background: #5568d3; }
    button.danger { background: #ef4444; }
    button.danger:hover { background: #dc2626; }
    .btn-group { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; margin-top: 12px; }
    .alarm-info, .timer-info { background: #f0f9ff; border-left: 4px solid #667eea; padding: 12px; border-radius: 6px; margin-top: 12px; }
    .inactive { background: #f7f7f7; border-left-color: #ccc; }
  </style>
</head>
<body>
  <div class="container">
    <h1>⏰ ESP32 Clock</h1>
    <div class="card">
      <div class="clock-display" id="clock">--:--</div>
      <div class="date-display" id="date">Loading...</div>
      <div class="status-grid">
        <div class="status-item"><div class="status-label">WiFi SSID</div><div class="status-value" id="ssid">--</div></div>
        <div class="status-item"><div class="status-label">WiFi Status</div><div class="status-value" id="wifi">--</div></div>
        <div class="status-item"><div class="status-label">IP Address</div><div class="status-value" id="ip">--</div></div>
        <div class="status-item"><div class="status-label">Time Sync</div><div class="status-value" id="sync">--</div></div>
      </div>
    </div>
    <div class="card">
      <h2>⚙️ System Settings</h2>
      <div class="form-group"><label>NTP Server</label><input type="text" id="ntpServer"></div>
      <div class="form-group"><label>Timezone (GMT offset)</label><input type="number" id="timezone"></div>
      <div class="form-group"><label>DST Offset</label><input type="number" id="dstOffset"></div>
      <button onclick="syncTime()">🔄 Sync Time Now</button>
      <div class="form-group" style="margin-top: 16px;"><label>Manual Time (YYYY-MM-DD HH:MM:SS)</label><input type="text" id="manualTime"></div>
      <button onclick="setManualTime()">🕐 Set Manual Time</button>
      <div class="btn-group" style="margin-top: 16px;">
        <button onclick="saveSettings()">💾 Save</button>
        <button onclick="restoreSettings()">📂 Restore</button>
      </div>
      <div class="btn-group">
        <button class="danger" onclick="eraseSettings()">🗑️ Erase NVS</button>
        <button class="danger" onclick="rebootDevice()">🔄 Reboot</button>
      </div>
    </div>
    <div class="card">
      <h2>🔔 Alarm</h2>
      <div id="alarmStatus" class="alarm-info inactive">No alarm set</div>
      <div class="form-group"><label>Type</label>
        <select id="alarmType" onchange="updateAlarmFields()">
          <option value="daily">Daily</option><option value="weekdays">Weekdays</option><option value="date">Date</option>
        </select>
      </div>
      <div id="dateField" style="display:none;" class="form-group"><label>Date</label><input type="date" id="alarmDate"></div>
      <div id="weekdaysField" style="display:none;" class="form-group"><label>Select Days</label>
        <div class="checkbox-group">
          <label><input type="checkbox" value="1"> Mon</label> <label><input type="checkbox" value="2"> Tue</label>
          <label><input type="checkbox" value="4"> Wed</label> <label><input type="checkbox" value="8"> Thu</label>
          <label><input type="checkbox" value="16"> Fri</label> <label><input type="checkbox" value="32"> Sat</label>
          <label><input type="checkbox" value="64"> Sun</label>
        </div>
      </div>
      <div class="form-group"><label>Time</label><input type="time" id="alarmTime"></div>
      <div class="form-group"><label>Text</label><input type="text" id="alarmText" maxlength="30"></div>
      <div class="form-group"><label><input type="checkbox" id="alarmRepeat"> Repeat</label></div>
      <div class="form-group"><label><input type="checkbox" id="alarmSave"> Save to NVS</label></div>
      <div class="btn-group">
        <button onclick="setAlarm()">Set Alarm</button>
        <button class="danger" onclick="clearAlarm()">Clear</button>
      </div>
    </div>
    <div class="card">
      <h2>⏲️ Timer</h2>
      <div id="timerStatus" class="timer-info inactive">No timer active</div>
      <div class="form-group"><label>Duration</label><input type="time" id="timerTime" step="1" value="00:05:00"></div>
      <div class="form-group"><label>Text</label><input type="text" id="timerText" maxlength="30"></div>
      <div class="btn-group">
        <button onclick="setTimer()">Start Timer</button>
        <button class="danger" onclick="clearTimer()">Clear</button>
      </div>
    </div>
  </div>
  <script>
    function updateStatus() {
      fetch('/status').then(r => r.json()).then(data => {
          document.getElementById('clock').textContent = data.time;
          document.getElementById('date').textContent = data.date + ' • ' + data.weekday;
          document.getElementById('ssid').textContent = data.ssid;
          document.getElementById('wifi').textContent = data.wifi;
          document.getElementById('ip').textContent = data.ip;
          document.getElementById('sync').textContent = data.timeSync ? 'Synced' : 'Not synced';
          document.getElementById('ntpServer').value = data.ntpServer;
          document.getElementById('timezone').value = data.timezone;
          document.getElementById('dstOffset').value = data.dstOffset;
          if(data.fullTime) document.getElementById('manualTime').value = data.fullTime;
          
          if(data.alarm.active) {
            document.getElementById('alarmTime').value = (data.alarm.hour<10?'0':'')+data.alarm.hour+':'+(data.alarm.minute<10?'0':'')+data.alarm.minute;
            document.getElementById('alarmType').value = data.alarm.type;
            if(data.alarm.type==='date') document.getElementById('alarmDate').value = data.alarm.date;
            if(data.alarm.type==='weekdays') document.querySelectorAll('#weekdaysField input').forEach(cb => cb.checked = (data.alarm.weekdays & cb.value) !== 0);
            document.getElementById('alarmText').value = data.alarm.text || "";
            document.getElementById('alarmRepeat').checked = data.alarm.repeat;
            document.getElementById('alarmSave').checked = data.alarm.saved;
            updateAlarmFields();
          }
          
          const aDiv = document.getElementById('alarmStatus');
          if(data.alarm.active) {
            aDiv.textContent = 'Alarm Set: ' + (data.alarm.hour<10?'0':'')+data.alarm.hour+':'+(data.alarm.minute<10?'0':'')+data.alarm.minute;
            aDiv.className = 'alarm-info';
          } else {
            aDiv.textContent = 'No alarm set';
            aDiv.className = 'alarm-info inactive';
          }
          
          const tDiv = document.getElementById('timerStatus');
          if(data.timer.active) {
            tDiv.textContent = 'Timer active: ' + data.timer.remaining + 's left';
            tDiv.className = 'timer-info';
          } else {
            tDiv.textContent = 'No timer active';
            tDiv.className = 'timer-info inactive';
          }
      });
    }
    function updateAlarmFields() {
      const t = document.getElementById('alarmType').value;
      document.getElementById('dateField').style.display = t==='date'?'block':'none';
      document.getElementById('weekdaysField').style.display = t==='weekdays'?'block':'none';
    }
    function setAlarm() {
      let url = '/alarm?time=' + document.getElementById('alarmTime').value + '&type=' + document.getElementById('alarmType').value;
      if(document.getElementById('alarmType').value==='date') url += '&date=' + document.getElementById('alarmDate').value;
      if(document.getElementById('alarmType').value==='weekdays') {
        let mask=0; document.querySelectorAll('#weekdaysField input:checked').forEach(c => mask+=parseInt(c.value)); url+='&weekdays='+mask;
      }
      if(document.getElementById('alarmText').value) url+='&text='+encodeURIComponent(document.getElementById('alarmText').value);
      if(document.getElementById('alarmRepeat').checked) url+='&repeat=1';
      if(document.getElementById('alarmSave').checked) url+='&save=1';
      fetch(url).then(r=>r.text()).then(d=>{ alert(d); updateStatus(); });
    }
    function clearAlarm() { fetch('/alarm/clear').then(r=>r.text()).then(d=>{ alert(d); updateStatus(); }); }
    function setTimer() {
      const pts = document.getElementById('timerTime').value.split(':');
      const sec = parseInt(pts[0])*3600 + parseInt(pts[1])*60 + parseInt(pts[2]||0);
      let url = '/timer?duration=' + sec;
      if(document.getElementById('timerText').value) url+='&text='+encodeURIComponent(document.getElementById('timerText').value);
      fetch(url).then(r=>r.text()).then(d=>{ alert(d); updateStatus(); });
    }
    function clearTimer() { fetch('/timer/clear').then(r=>r.text()).then(d=>{ alert(d); updateStatus(); }); }
    function syncTime() {
      let url = '/sync?ntp='+encodeURIComponent(document.getElementById('ntpServer').value)+'&tz='+document.getElementById('timezone').value+'&dst='+document.getElementById('dstOffset').value;
      fetch(url).then(r=>r.text()).then(d=>{ alert(d); updateStatus(); });
    }
    function setManualTime() { fetch('/time?value='+encodeURIComponent(document.getElementById('manualTime').value)).then(r=>r.text()).then(d=>{ alert(d); updateStatus(); }); }
    function saveSettings() { fetch('/save').then(r=>r.text()).then(d=>alert(d)); }
    function restoreSettings() { fetch('/restore').then(r=>r.text()).then(d=>{alert(d); location.reload();}); }
    function eraseSettings() { if(confirm('Erase ALL?')) fetch('/erase').then(r=>r.text()).then(d=>alert(d)); }
    function rebootDevice() { if(confirm('Reboot?')) fetch('/reboot').then(r=>r.text()).then(d=>{alert('Rebooting...'); setTimeout(()=>location.reload(),10000);}); }
    updateStatus(); setInterval(updateStatus, 2000);
  </script>
</body>
</html>
)rawliteral";
  return html;
}

void setupWebServer() {
  server.on("/", HTTP_GET, []() { server.send(200, "text/html; charset=utf-8", getWebPage()); });
  server.on("/status", HTTP_GET, []() { server.send(200, "application/json; charset=utf-8", getStatusJSON()); });
  
  server.on("/alarm", HTTP_GET, []() {
    if (!timeValid) { server.send(400, "text/plain", "Wait for time sync!"); return; }
    String timeStr = server.arg("time");
    String type = server.arg("type");
    if (timeStr.length() == 0) { server.send(400, "text/plain", "Missing time"); return; }
    
    myAlarm.active = true;
    myAlarm.hour = timeStr.substring(0, 2).toInt();
    myAlarm.minute = timeStr.substring(3, 5).toInt();
    myAlarm.repeat = server.hasArg("repeat");
    alarmTriggered = false;
    
    String text = server.arg("text");
    if (text.length() > 0) {
      text.replace("+", " ");
      strncpy(myAlarm.text, text.c_str(), 30);
    } else { myAlarm.text[0] = '\0'; }
    
    if (type == "date") {
      String dateStr = server.arg("date");
      myAlarm.year = dateStr.substring(0, 4).toInt();
      myAlarm.month = dateStr.substring(5, 7).toInt();
      myAlarm.day = dateStr.substring(8, 10).toInt();
      myAlarm.weekdays = 0;
    } else if (type == "weekdays") {
      myAlarm.year = 0; myAlarm.month = 0; myAlarm.day = 0;
      myAlarm.weekdays = server.arg("weekdays").toInt();
    } else {
      myAlarm.year = 0; myAlarm.month = 0; myAlarm.day = 0; myAlarm.weekdays = 0;
    }
    
    if (server.hasArg("save")) saveAlarmToNVS();
    else { myAlarm.saved = false; updateAlarmIndicator(); }
    
    server.send(200, "text/plain", "Alarm set");
  });
  
  server.on("/alarm/clear", HTTP_GET, []() {
    memset(&myAlarm, 0, sizeof(myAlarm));
    alarmTriggered = false; buzzerActive = false; digitalWrite(BUZZER_PIN, LOW);
    clearAlarmFromNVS(); updateAlarmIndicator();
    server.send(200, "text/plain", "Alarm cleared");
  });
  
  server.on("/timer", HTTP_GET, []() {
    unsigned long totalSec = server.arg("duration").toInt();
    if (totalSec == 0 || totalSec > 86400) { server.send(400, "text/plain", "Invalid timer"); return; }
    
    String text = server.arg("text");
    if (text.length() > 0) {
      text.replace("+", " ");
      strncpy(timerText, text.c_str(), 30);
    } else strcpy(timerText, "TIMER");
    
    timerActive = true; timerTriggered = false;
    timerStartUs = esp_timer_get_time();
    timerDurationUs = (uint64_t)totalSec * 1000000;
    server.send(200, "text/plain", "Timer started");
  });
  
  server.on("/timer/clear", HTTP_GET, []() {
    timerActive = false; timerTriggered = false; buzzerActive = false; digitalWrite(BUZZER_PIN, LOW);
    server.send(200, "text/plain", "Timer cleared");
  });
  
  server.on("/sync", HTTP_GET, []() {
    if (server.hasArg("ntp")) ntpServer = server.arg("ntp");
    if (server.hasArg("tz")) gmtOffset_sec = server.arg("tz").toInt() * 3600;
    if (server.hasArg("dst")) daylightOffset_sec = server.arg("dst").toInt() * 3600;
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer.c_str());
    delay(2000); 
    if (getLocalTime(&timeinfo) && timeinfo.tm_year > 120) {
      timeValid = true; updateClockStrings();
      server.send(200, "text/plain", "Synced");
    } else server.send(500, "text/plain", "Sync failed");
  });
  
  server.on("/time", HTTP_GET, []() { setManualTime(server.arg("value")); server.send(200, "text/plain", "Time set"); });
  server.on("/save", HTTP_GET, []() { saveConfigToNVS(); server.send(200, "text/plain", "Saved"); });
  server.on("/restore", HTTP_GET, []() { loadConfigFromNVS(); connectWiFi(); syncTime(); updateClockStrings(); server.send(200, "text/plain", "Restored"); });
  server.on("/erase", HTTP_GET, []() { eraseNVS(); server.send(200, "text/plain", "Erased"); });
  server.on("/reboot", HTTP_GET, []() { server.send(200, "text/plain", "Rebooting"); delay(1000); ESP.restart(); });
}

// ================= SERIAL / SYSTEM LOGIC =================

void handleSerial() {
  if (!promptShown) { Serial.print("> "); promptShown = true; }
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 8 || c == 127) { if (serialInput.length() > 0) { serialInput.remove(serialInput.length() - 1); Serial.print("\b \b"); } continue; }
    Serial.print(c);  
    if (c == '\r' || c == '\n') {
      Serial.println(); 
      if (serialInput.length() == 0) { Serial.print("> "); return; }
      String cmd = serialInput;
      addToHistory(cmd); historyBrowseIndex = -1; serialInput = ""; cmd.trim(); cmd.toUpperCase();

      if (cmd.equals("HELP")) {
        Serial.println("Commands: TIME, WIFI, NTP, TZ, DST, SAVE, RESTORE, ERASE, STATUS, SYNC, REBOOT, ALARM, TIMER");
      } else if (cmd.startsWith("WIFI ")) {
        int sp = cmd.indexOf(' ', 5);
        if (sp > 0) { wifiSSID = cmd.substring(5, sp); wifiPASS = cmd.substring(sp + 1); connectWiFi(); }
      } else if (cmd.equals("REBOOT")) { ESP.restart(); }
      
      Serial.print("> ");
    } else { serialInput += c; }
  }
}

// Definim numele lunilor
// O singură definire a lunilor
const char* monthNames[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

void updateClockStrings() {
  if (getLocalTime(&timeinfo) && timeinfo.tm_year > 120) {
    snprintf(hhStr, sizeof(hhStr), "%02d", timeinfo.tm_hour);
    snprintf(mmStr, sizeof(mmStr), "%02d", timeinfo.tm_min);
    
    int monthIndex = timeinfo.tm_mon; 
    int wday = timeinfo.tm_wday;
    
    // Extragem ziua (Mon, Tue, Wed...) din vectorul deja existent în cod
    const char* currentDay = (wday >= 0 && wday < 7) ? weekdaysRU[wday] : "---";
    
    // Generăm textul final: "Tue 01-Sep-2026"
    snprintf(dateStr, sizeof(dateStr), "%s %02d-%s-%04d", currentDay, timeinfo.tm_mday, monthNames[monthIndex], timeinfo.tm_year + 1900);
    
    strcpy(weekdayStr, currentDay);
  }
}

void connectWiFi() {
  Serial.println("Connecting to WiFi: " + wifiSSID);
  WiFi.disconnect(true);
  WiFi.begin(wifiSSID.c_str(), wifiPASS.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > 10000) { Serial.println("WiFi timeout"); return; }
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
      timeValid = true; Serial.println("Time synced"); return;
    }
    delay(500);
  }
}

void setManualTime(String s) {
  struct tm t {};
  if (sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec) == 6) {
    t.tm_year -= 1900; t.tm_mon -= 1;
    time_t tt = mktime(&t); struct timeval now = { tt, 0 }; settimeofday(&now, nullptr);
    timeValid = true; updateClockStrings();
  }
}

void handleButton() {
  static bool btnPrev = HIGH;
  bool btnNow = digitalRead(BOOT_PIN);
  if (btnPrev == HIGH && btnNow == LOW) {
    if (millis() - lastBtnTime > BTN_DEBOUNCE) {
      if (alarmTriggered || timerTriggered) {
        if (alarmTriggered && !myAlarm.repeat) { myAlarm.active = false; if (myAlarm.saved) clearAlarmFromNVS(); }
        if (timerTriggered) timerActive = false;
        alarmTriggered = false; timerTriggered = false; buzzerActive = false; digitalWrite(BUZZER_PIN, LOW);
        currentScreen = SCREEN_CLOCK; infoScreenPage = 1; updateAlarmIndicator();
      } else if (currentScreen == SCREEN_CLOCK) {
        currentScreen = SCREEN_INFO1; infoScreenPage = 1; infoStartTime = millis();
      } else if (currentScreen == SCREEN_INFO1) {
        currentScreen = SCREEN_INFO2; infoScreenPage = 2; infoStartTime = millis();
      } else if (currentScreen == SCREEN_INFO2) {
        currentScreen = SCREEN_CLOCK; infoScreenPage = 1;
      }
      lastBtnTime = millis();
    }
  }
  btnPrev = btnNow;
}

bool checkAlarmMatch() {
  if (!myAlarm.active || alarmTriggered) return false;
  if (myAlarm.year > 0) { if (timeinfo.tm_year + 1900 != myAlarm.year || timeinfo.tm_mon + 1 != myAlarm.month || timeinfo.tm_mday != myAlarm.day) return false; }
  else if (myAlarm.weekdays > 0) {
    int wday = timeinfo.tm_wday;
    if (wday == 0) wday = 6; else wday -= 1;
    if (!(myAlarm.weekdays & (1 << wday))) return false;
  }
  if (timeinfo.tm_hour != myAlarm.hour || timeinfo.tm_min != myAlarm.minute || timeinfo.tm_sec != 0) return false;
  return true;
}

void handleAutoReturn() {
  if ((currentScreen == SCREEN_INFO1 || currentScreen == SCREEN_INFO2) && millis() - infoStartTime > INFO_TIMEOUT) {
    currentScreen = SCREEN_CLOCK; infoScreenPage = 1;
  }
}

// ================= MAIN LOOP =================
void loop() {
  server.handleClient(); 
  handleSerial();
  handleButton();
  handleAutoReturn();

  if (millis() - lastBlink >= 500) { colonVisible = !colonVisible; lastBlink = millis(); }

  static unsigned long lastLedBlink = 0;
  static bool ledState = false;
  if (timerActive && !timerTriggered) {
    if (millis() - lastLedBlink >= 500) { ledState = !ledState; digitalWrite(BLUE_LED_PIN, ledState ? LOW : HIGH); lastLedBlink = millis(); }
  } else {
    bool shouldBeOn = myAlarm.active && !alarmTriggered;
    digitalWrite(BLUE_LED_PIN, shouldBeOn ? LOW : HIGH);
  }

  if (getLocalTime(&timeinfo) && timeinfo.tm_year > 120) timeValid = true;

  static unsigned long lastSec = 0;
  if (timeValid && millis() - lastSec >= 1000) {
    lastSec = millis(); updateClockStrings();
    if (checkAlarmMatch()) {
      alarmTriggered = true; buzzerActive = true; currentScreen = SCREEN_ALARM;
      updateAlarmIndicator();
    }
  }

  if (timerActive && !timerTriggered) {
    uint64_t elapsed = esp_timer_get_time() - timerStartUs;
    if (elapsed >= timerDurationUs) { timerTriggered = true; buzzerActive = true; currentScreen = SCREEN_TIMER; }
  }

  if (buzzerActive) {
    uint64_t phase = esp_timer_get_time() % 2000000; 
    bool on = false;
    if (phase < 150000) on = true;
    else if (phase >= 300000 && phase < 450000) on = true;
    else if (phase >= 600000 && phase < 750000) on = true;
    digitalWrite(BUZZER_PIN, on ? HIGH : LOW);
  } else {
    digitalWrite(BUZZER_PIN, LOW);
  }

  // Desenare ecrane pe U8g2
  switch (currentScreen) {
    case SCREEN_INFO1: drawInfoScreen1(); delay(100); break;
    case SCREEN_INFO2: drawInfoScreen2(); delay(100); break;
    case SCREEN_ALARM:
      if (alarmTriggered) { drawAlarmOrTimer(myAlarm.text); delay(20); } 
      else { currentScreen = SCREEN_CLOCK; } break;
    case SCREEN_TIMER:
      if (timerTriggered) { drawAlarmOrTimer(timerText); delay(20); } 
      else { currentScreen = SCREEN_CLOCK; } break;
    case SCREEN_CLOCK:
    default:
      if (timeValid) {
        drawClock();
      } else {
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_ncenB10_tr);
        int w = u8g2.getStrWidth("NO TIME SYNC");
        u8g2.drawStr((128 - w) / 2, 35, "NO TIME SYNC");
        u8g2.sendBuffer();
      }
      delay(50);
      break;
  }
}