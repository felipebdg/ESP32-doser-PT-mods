/*
 * ESP32 Aquarium Dosing Controller v2.0
 * 4-Channel Peristaltic Pump Controller with Web Interface
 * 
 * Features:
 * - WiFi connectivity with web-based configuration
 * - OTA (Over-The-Air) firmware updates
 * - NTP time synchronization with configurable timezone
 * - Per-channel calibration (mL per second)
 * - Weekly scheduling (select specific days)
 * - Custom pump names
 * - Manual dosing via web interface
 * - Persistent settings (survives reboot)
 * 
 * Hardware:
 * - ESP32-WROOM-32E with 4-channel relay board
 * - GPIO32: Relay 1, GPIO33: Relay 2, GPIO25: Relay 3, GPIO26: Relay 4
 * - 12V peristaltic pumps connected to relay NO/COM terminals
 */

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>
#include <ArduinoOTA.h>

// ============== CONFIGURATION ==============
// Update these with your WiFi credentials
const char* WIFI_SSID = "Sampey";
const char* WIFI_PASSWORD = "Liam1506!";

// OTA Configuration
const char* OTA_HOSTNAME = "dosing-controller";
const char* OTA_PASSWORD = "doser123";  // Change this!

// NTP Configuration
const char* NTP_SERVER = "pool.ntp.org";

// Relay GPIO pins
const int RELAY_PINS[4] = {32, 33, 25, 26};

// Relay active state
const bool RELAY_ACTIVE_LOW = false;

// ============== GLOBAL OBJECTS ==============
WebServer server(80);
Preferences preferences;

// Timezone offset (stored in flash, configurable via web)
long gmtOffsetSec = -18000;      // Default: EST (-5 hours)
int daylightOffsetSec = 3600;   // Default: 1 hour DST

// ============== CHANNEL DATA STRUCTURE ==============
struct Channel {
  String name;
  float mlPerSecond;      // Calibration: mL output per second
  float targetMl;         // How many mL to dose
  int doseHour;           // Hour to dose (0-23), -1 = disabled
  int doseMinute;         // Minute to dose (0-59)
  bool doseDays[7];       // Which days to dose: [Sun, Mon, Tue, Wed, Thu, Fri, Sat]
  bool dosedToday;        // Flag to prevent double-dosing
  bool isRunning;         // Currently pumping
  unsigned long runStartTime;
  unsigned long runDuration;
};

Channel channels[4];

// ============== STATE VARIABLES ==============
bool wifiConnected = false;
bool timeSync = false;
int lastCheckedMinute = -1;
int lastCheckedDay = -1;

// ============== SETUP ==============
void setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== ESP32 Dosing Controller v2.0 Starting ===");
  
  // Initialize relay pins
  for (int i = 0; i < 4; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    setRelay(i, false);  // Ensure all relays are OFF
  }
  
  // Load saved settings
  loadSettings();
  
  // Connect to WiFi
  connectWiFi();
  
  // Setup OTA
  if (wifiConnected) {
    setupOTA();
    syncTime();
  }
  
  // Setup web server routes
  setupWebServer();
  
  Serial.println("=== Setup Complete ===\n");
}

// ============== MAIN LOOP ==============
void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  
  // Check for scheduled doses every minute
  checkScheduledDoses();
  
  // Check for running pumps that need to stop
  checkRunningPumps();
  
  // Reset dosedToday flags at midnight
  resetDailyFlags();
  
  delay(100);
}

// ============== OTA SETUP ==============
void setupOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  
  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
    Serial.println("OTA Start updating " + type);
    // Turn off all pumps during update
    for (int i = 0; i < 4; i++) {
      setRelay(i, false);
      channels[i].isRunning = false;
    }
  });
  
  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA End");
  });
  
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA Progress: %u%%\r", (progress / (total / 100)));
  });
  
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  
  ArduinoOTA.begin();
  Serial.println("OTA Ready");
  Serial.printf("Hostname: %s\n", OTA_HOSTNAME);
}

// ============== WIFI CONNECTION ==============
void connectWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("\nWiFi Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi Connection Failed!");
  }
}

// ============== NTP TIME SYNC ==============
void syncTime() {
  Serial.println("Syncing time via NTP...");
  configTime(gmtOffsetSec, daylightOffsetSec, NTP_SERVER);
  
  struct tm timeinfo;
  int attempts = 0;
  while (!getLocalTime(&timeinfo) && attempts < 10) {
    delay(500);
    attempts++;
  }
  
  if (getLocalTime(&timeinfo)) {
    timeSync = true;
    Serial.println("Time synchronized!");
    Serial.println(&timeinfo, "Current time: %Y-%m-%d %H:%M:%S");
  } else {
    Serial.println("Failed to sync time!");
  }
}

// ============== RELAY CONTROL ==============
void setRelay(int channel, bool on) {
  if (channel < 0 || channel > 3) return;
  
  if (RELAY_ACTIVE_LOW) {
    digitalWrite(RELAY_PINS[channel], on ? LOW : HIGH);
  } else {
    digitalWrite(RELAY_PINS[channel], on ? HIGH : LOW);
  }
}

// ============== PUMP CONTROL ==============
void startPump(int channel, float mlToDose) {
  if (channel < 0 || channel > 3) return;
  if (channels[channel].mlPerSecond <= 0) {
    Serial.println("Channel not calibrated!");
    return;
  }
  
  float seconds = mlToDose / channels[channel].mlPerSecond;
  unsigned long duration = (unsigned long)(seconds * 1000);
  
  channels[channel].isRunning = true;
  channels[channel].runStartTime = millis();
  channels[channel].runDuration = duration;
  
  setRelay(channel, true);
  
  Serial.printf("Channel %d (%s): Starting pump for %.1f mL (%.1f seconds)\n", 
                channel + 1, channels[channel].name.c_str(), mlToDose, seconds);
}

void startPumpTimed(int channel, unsigned long durationMs) {
  if (channel < 0 || channel > 3) return;
  
  channels[channel].isRunning = true;
  channels[channel].runStartTime = millis();
  channels[channel].runDuration = durationMs;
  
  setRelay(channel, true);
  
  Serial.printf("Channel %d (%s): Starting pump for %lu ms (calibration)\n", 
                channel + 1, channels[channel].name.c_str(), durationMs);
}

void stopPump(int channel) {
  if (channel < 0 || channel > 3) return;
  
  channels[channel].isRunning = false;
  setRelay(channel, false);
  
  Serial.printf("Channel %d (%s): Pump stopped\n", channel + 1, channels[channel].name.c_str());
}

void checkRunningPumps() {
  for (int i = 0; i < 4; i++) {
    if (channels[i].isRunning) {
      if (millis() - channels[i].runStartTime >= channels[i].runDuration) {
        stopPump(i);
      }
    }
  }
}

// ============== SCHEDULING ==============
void checkScheduledDoses() {
  if (!timeSync) return;
  
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;
  
  // Only check once per minute
  if (timeinfo.tm_min == lastCheckedMinute) return;
  lastCheckedMinute = timeinfo.tm_min;
  
  int dayOfWeek = timeinfo.tm_wday;  // 0 = Sunday
  
  for (int i = 0; i < 4; i++) {
    if (channels[i].doseHour >= 0 && 
        channels[i].doseHour == timeinfo.tm_hour &&
        channels[i].doseMinute == timeinfo.tm_min &&
        channels[i].doseDays[dayOfWeek] &&  // Check if today is a dose day
        !channels[i].dosedToday &&
        !channels[i].isRunning) {
      
      Serial.printf("Scheduled dose triggered for %s\n", channels[i].name.c_str());
      startPump(i, channels[i].targetMl);
      channels[i].dosedToday = true;
      saveSettings();
    }
  }
}

void resetDailyFlags() {
  if (!timeSync) return;
  
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;
  
  // Reset at midnight (when day changes)
  if (timeinfo.tm_wday != lastCheckedDay) {
    lastCheckedDay = timeinfo.tm_wday;
    for (int i = 0; i < 4; i++) {
      channels[i].dosedToday = false;
    }
    Serial.println("Daily flags reset for new day");
    saveSettings();
  }
}

// ============== SETTINGS PERSISTENCE ==============
void loadSettings() {
  preferences.begin("dosing", true);
  
  // Load timezone
  gmtOffsetSec = preferences.getLong("gmtOffset", -18000);
  daylightOffsetSec = preferences.getInt("dstOffset", 3600);
  
  for (int i = 0; i < 4; i++) {
    String prefix = "ch" + String(i) + "_";
    channels[i].name = preferences.getString((prefix + "name").c_str(), "Pump " + String(i + 1));
    channels[i].mlPerSecond = preferences.getFloat((prefix + "mlps").c_str(), 0.0);
    channels[i].targetMl = preferences.getFloat((prefix + "target").c_str(), 100.0);
    channels[i].doseHour = preferences.getInt((prefix + "hour").c_str(), -1);
    channels[i].doseMinute = preferences.getInt((prefix + "min").c_str(), 0);
    channels[i].dosedToday = preferences.getBool((prefix + "dosed").c_str(), false);
    
    // Load dose days (default to all days)
    for (int d = 0; d < 7; d++) {
      String dayKey = prefix + "day" + String(d);
      channels[i].doseDays[d] = preferences.getBool(dayKey.c_str(), true);
    }
    
    channels[i].isRunning = false;
  }
  
  preferences.end();
  Serial.println("Settings loaded from flash");
}

void saveSettings() {
  preferences.begin("dosing", false);
  
  // Save timezone
  preferences.putLong("gmtOffset", gmtOffsetSec);
  preferences.putInt("dstOffset", daylightOffsetSec);
  
  for (int i = 0; i < 4; i++) {
    String prefix = "ch" + String(i) + "_";
    preferences.putString((prefix + "name").c_str(), channels[i].name);
    preferences.putFloat((prefix + "mlps").c_str(), channels[i].mlPerSecond);
    preferences.putFloat((prefix + "target").c_str(), channels[i].targetMl);
    preferences.putInt((prefix + "hour").c_str(), channels[i].doseHour);
    preferences.putInt((prefix + "min").c_str(), channels[i].doseMinute);
    preferences.putBool((prefix + "dosed").c_str(), channels[i].dosedToday);
    
    // Save dose days
    for (int d = 0; d < 7; d++) {
      String dayKey = prefix + "day" + String(d);
      preferences.putBool(dayKey.c_str(), channels[i].doseDays[d]);
    }
  }
  
  preferences.end();
  Serial.println("Settings saved to flash");
}

// ============== WEB SERVER ==============
void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/dose", handleDose);
  server.on("/calibrate", handleCalibrate);
  server.on("/save", handleSave);
  server.on("/stop", handleStop);
  server.on("/timezone", handleTimezone);
  
  server.begin();
  Serial.println("Web server started on port 80");
}

void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Dosing Controller</title>
  <style>
    * { box-sizing: border-box; font-family: -apple-system, sans-serif; }
    body { margin: 0; padding: 16px; background: #1a1a2e; color: #eee; }
    h1 { color: #00d9ff; margin-bottom: 8px; font-size: 24px; }
    .status { color: #888; font-size: 14px; margin-bottom: 20px; }
    .card { background: #16213e; border-radius: 12px; padding: 16px; margin-bottom: 16px; }
    .card h2 { margin: 0 0 12px 0; font-size: 18px; color: #00d9ff; }
    .row { display: flex; gap: 12px; margin-bottom: 12px; flex-wrap: wrap; }
    .row > * { flex: 1; min-width: 100px; }
    label { display: block; font-size: 12px; color: #888; margin-bottom: 4px; }
    input, select { width: 100%; padding: 10px; border: 1px solid #333; border-radius: 8px; 
                    background: #0f0f23; color: #fff; font-size: 16px; }
    input:focus { outline: none; border-color: #00d9ff; }
    button { padding: 12px 20px; border: none; border-radius: 8px; font-size: 16px; 
             cursor: pointer; font-weight: 600; }
    .btn-primary { background: #00d9ff; color: #000; }
    .btn-danger { background: #ff4757; color: #fff; }
    .btn-secondary { background: #333; color: #fff; }
    .btn-success { background: #2ed573; color: #000; }
    .calibration { background: #1e1e3f; padding: 12px; border-radius: 8px; margin-top: 12px; }
    .calibration h3 { margin: 0 0 8px 0; font-size: 14px; color: #ffa502; }
    .cal-row { display: flex; gap: 8px; align-items: end; flex-wrap: wrap; }
    .cal-row input { width: 80px; }
    .cal-row button { white-space: nowrap; }
    .running { animation: pulse 1s infinite; }
    @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.5; } }
    .indicator { display: inline-block; width: 10px; height: 10px; border-radius: 50%; 
                 margin-right: 8px; background: #333; }
    .indicator.on { background: #2ed573; }
    .time-inputs { display: flex; gap: 4px; align-items: center; }
    .time-inputs input { width: 60px; text-align: center; }
    .ml-rate { font-size: 12px; color: #2ed573; margin-top: 4px; }
    .disabled { opacity: 0.5; }
    .days-selector { display: flex; gap: 6px; flex-wrap: wrap; margin-top: 8px; }
    .day-btn { width: 40px; height: 36px; border-radius: 6px; border: 1px solid #333;
               background: #0f0f23; color: #888; font-size: 12px; cursor: pointer; }
    .day-btn.active { background: #00d9ff; color: #000; border-color: #00d9ff; }
    .settings-card { background: #0f3460; }
    .name-input { background: transparent; border: 1px solid transparent; color: #00d9ff; 
                  font-size: 18px; font-weight: bold; padding: 4px 8px; width: 100%; }
    .name-input:hover { border-color: #333; }
    .name-input:focus { border-color: #00d9ff; background: #0f0f23; }
  </style>
</head>
<body>
  <h1>🧪 Dosing Controller</h1>
  <div class="status" id="statusBar">Connecting...</div>
  
  <div id="channels"></div>
  
  <div class="card settings-card">
    <h2>⚙️ Settings</h2>
    <div class="row">
      <div>
        <label>Timezone (GMT Offset Hours)</label>
        <input type="number" id="tzOffset" step="0.5" min="-12" max="14">
      </div>
      <div>
        <label>Daylight Saving</label>
        <select id="dstOffset">
          <option value="0">No DST</option>
          <option value="3600">+1 Hour DST</option>
        </select>
      </div>
      <div style="display:flex;align-items:end;">
        <button class="btn-success" onclick="saveTimezone()">Save Timezone</button>
      </div>
    </div>
    <div style="font-size:12px;color:#888;margin-top:8px;">
      OTA Updates enabled. Hostname: <strong>dosing-controller</strong>
    </div>
  </div>

  <script>
    const dayNames = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
    let channelData = [];
    
    function updateStatus() {
      // Skip refresh if user is typing in an input field
      if (document.activeElement && (document.activeElement.tagName === 'INPUT' || document.activeElement.tagName === 'SELECT')) {
        return;
      }
      
      fetch('/status')
        .then(r => r.json())
        .then(data => {
          channelData = data.channels;
          
          document.getElementById('statusBar').textContent = 
            (data.timeSync ? '🕐 ' + data.time : '⚠️ Time not synced') + 
            ' | IP: ' + data.ip;
          
          document.getElementById('tzOffset').value = data.gmtOffset / 3600;
          document.getElementById('dstOffset').value = data.dstOffset;
          
          let html = '';
          data.channels.forEach((ch, i) => {
            const calibrated = ch.mlPerSecond > 0;
            const scheduleEnabled = ch.doseHour >= 0;
            
            let daysHtml = '';
            dayNames.forEach((d, di) => {
              const active = ch.doseDays[di] ? 'active' : '';
              daysHtml += `<button type="button" class="day-btn ${active}" onclick="toggleDay(${i},${di})" data-ch="${i}" data-day="${di}">${d}</button>`;
            });
            
            html += `
              <div class="card">
                <h2>
                  <span class="indicator ${ch.isRunning ? 'on' : ''}"></span>
                  <input type="text" class="name-input" id="name${i}" value="${ch.name}" 
                         onchange="saveName(${i})" placeholder="Pump name">
                </h2>
                
                <div class="row">
                  <div>
                    <label>Dose Amount (mL)</label>
                    <input type="number" id="target${i}" value="${ch.targetMl}" step="1" min="1">
                  </div>
                  <div>
                    <label>Dose Time</label>
                    <div class="time-inputs">
                      <input type="number" id="hour${i}" value="${scheduleEnabled ? ch.doseHour : ''}" 
                             placeholder="HH" min="0" max="23">
                      <span>:</span>
                      <input type="number" id="min${i}" value="${scheduleEnabled ? String(ch.doseMinute).padStart(2,'0') : ''}" 
                             placeholder="MM" min="0" max="59">
                    </div>
                  </div>
                </div>
                
                <div>
                  <label>Dose Days</label>
                  <div class="days-selector" id="days${i}">
                    ${daysHtml}
                  </div>
                </div>
                
                <div class="row" style="margin-top:12px;">
                  <button class="btn-primary ${!calibrated ? 'disabled' : ''}" 
                          onclick="dose(${i})" ${!calibrated ? 'disabled' : ''}>
                    Dose Now
                  </button>
                  <button class="btn-success" onclick="saveChannel(${i})">Save Schedule</button>
                  ${ch.isRunning ? `<button class="btn-danger" onclick="stop(${i})">STOP</button>` : ''}
                </div>
                
                ${calibrated ? `<div class="ml-rate">✓ Calibrated: ${ch.mlPerSecond.toFixed(3)} mL/sec</div>` : 
                              `<div class="ml-rate" style="color:#ff4757;">⚠ Not calibrated</div>`}
                
                <div class="calibration">
                  <h3>🔧 Calibration</h3>
                  <p style="font-size:12px;color:#888;margin:0 0 8px 0;">
                    Run pump → measure output → enter mL dispensed
                  </p>
                  <div class="cal-row">
                    <div>
                      <label>Seconds</label>
                      <input type="number" id="calSec${i}" value="10" min="1" max="60">
                    </div>
                    <button class="btn-secondary" onclick="calibrate(${i})">Run Pump</button>
                    <div>
                      <label>mL Output</label>
                      <input type="number" id="calMl${i}" value="" step="0.1" placeholder="?">
                    </div>
                    <button class="btn-success" onclick="saveCalibration(${i})">Save Cal</button>
                  </div>
                </div>
              </div>
            `;
          });
          document.getElementById('channels').innerHTML = html;
        })
        .catch(err => {
          document.getElementById('statusBar').textContent = '❌ Connection error';
        });
    }
    
    function toggleDay(ch, day) {
      channelData[ch].doseDays[day] = !channelData[ch].doseDays[day];
      const btn = document.querySelector(`[data-ch="${ch}"][data-day="${day}"]`);
      btn.classList.toggle('active');
      
      // Save immediately so refresh doesn't reset it
      const days = channelData[ch].doseDays.map((d, i) => d ? '1' : '0').join(',');
      fetch('/save?ch=' + ch + '&days=' + days);
    }
    
    function dose(ch) {
      const ml = document.getElementById('target' + ch).value;
      fetch('/dose?ch=' + ch + '&ml=' + ml).then(() => setTimeout(updateStatus, 500));
    }
    
    function stop(ch) {
      fetch('/stop?ch=' + ch).then(() => setTimeout(updateStatus, 500));
    }
    
    function calibrate(ch) {
      const sec = document.getElementById('calSec' + ch).value;
      fetch('/calibrate?ch=' + ch + '&sec=' + sec).then(() => setTimeout(updateStatus, 500));
    }
    
    function saveCalibration(ch) {
      const sec = document.getElementById('calSec' + ch).value;
      const ml = document.getElementById('calMl' + ch).value;
      if (!ml || ml <= 0) { alert('Enter the mL dispensed'); return; }
      const mlps = ml / sec;
      fetch('/save?ch=' + ch + '&mlps=' + mlps).then(() => {
        alert('Calibration saved: ' + mlps.toFixed(3) + ' mL/sec');
        updateStatus();
      });
    }
    
    function saveChannel(ch) {
      const target = document.getElementById('target' + ch).value;
      const hour = document.getElementById('hour' + ch).value;
      const min = document.getElementById('min' + ch).value;
      const days = channelData[ch].doseDays.map((d, i) => d ? '1' : '0').join(',');
      
      let url = '/save?ch=' + ch + '&target=' + target + '&days=' + days;
      if (hour !== '' && min !== '') {
        url += '&hour=' + hour + '&min=' + min;
      } else {
        url += '&hour=-1&min=0';
      }
      fetch(url).then(() => {
        alert('Schedule saved!');
        updateStatus();
      });
    }
    
    function saveName(ch) {
      const name = document.getElementById('name' + ch).value;
      fetch('/save?ch=' + ch + '&name=' + encodeURIComponent(name));
    }
    
    function saveTimezone() {
      const offset = parseFloat(document.getElementById('tzOffset').value) * 3600;
      const dst = document.getElementById('dstOffset').value;
      fetch('/timezone?gmt=' + offset + '&dst=' + dst).then(() => {
        alert('Timezone saved! Time will resync.');
        updateStatus();
      });
    }
    
    updateStatus();
    setInterval(updateStatus, 5000);
  </script>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}

void handleStatus() {
  struct tm timeinfo;
  char timeStr[20] = "Not synced";
  if (getLocalTime(&timeinfo)) {
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
  }
  
  String json = "{";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"timeSync\":" + String(timeSync ? "true" : "false") + ",";
  json += "\"time\":\"" + String(timeStr) + "\",";
  json += "\"gmtOffset\":" + String(gmtOffsetSec) + ",";
  json += "\"dstOffset\":" + String(daylightOffsetSec) + ",";
  json += "\"channels\":[";
  
  for (int i = 0; i < 4; i++) {
    if (i > 0) json += ",";
    json += "{";
    json += "\"name\":\"" + channels[i].name + "\",";
    json += "\"mlPerSecond\":" + String(channels[i].mlPerSecond, 4) + ",";
    json += "\"targetMl\":" + String(channels[i].targetMl, 1) + ",";
    json += "\"doseHour\":" + String(channels[i].doseHour) + ",";
    json += "\"doseMinute\":" + String(channels[i].doseMinute) + ",";
    json += "\"doseDays\":[";
    for (int d = 0; d < 7; d++) {
      if (d > 0) json += ",";
      json += channels[i].doseDays[d] ? "true" : "false";
    }
    json += "],";
    json += "\"dosedToday\":" + String(channels[i].dosedToday ? "true" : "false") + ",";
    json += "\"isRunning\":" + String(channels[i].isRunning ? "true" : "false");
    json += "}";
  }
  
  json += "]}";
  server.send(200, "application/json", json);
}

void handleDose() {
  int ch = server.arg("ch").toInt();
  float ml = server.arg("ml").toFloat();
  
  if (ch >= 0 && ch < 4 && ml > 0) {
    startPump(ch, ml);
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Invalid parameters");
  }
}

void handleCalibrate() {
  int ch = server.arg("ch").toInt();
  int sec = server.arg("sec").toInt();
  
  if (ch >= 0 && ch < 4 && sec > 0) {
    startPumpTimed(ch, sec * 1000);
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Invalid parameters");
  }
}

void handleSave() {
  int ch = server.arg("ch").toInt();
  if (ch < 0 || ch > 3) {
    server.send(400, "text/plain", "Invalid channel");
    return;
  }
  
  if (server.hasArg("name")) {
    channels[ch].name = server.arg("name");
  }
  if (server.hasArg("mlps")) {
    channels[ch].mlPerSecond = server.arg("mlps").toFloat();
  }
  if (server.hasArg("target")) {
    channels[ch].targetMl = server.arg("target").toFloat();
  }
  if (server.hasArg("hour")) {
    channels[ch].doseHour = server.arg("hour").toInt();
    channels[ch].dosedToday = false;
  }
  if (server.hasArg("min")) {
    channels[ch].doseMinute = server.arg("min").toInt();
  }
  if (server.hasArg("days")) {
    String daysStr = server.arg("days");
    int idx = 0;
    for (int d = 0; d < 7 && idx < daysStr.length(); d++) {
      channels[ch].doseDays[d] = (daysStr.charAt(idx) == '1');
      idx += 2;  // Skip comma
    }
  }
  
  saveSettings();
  server.send(200, "text/plain", "OK");
}

void handleStop() {
  int ch = server.arg("ch").toInt();
  if (ch >= 0 && ch < 4) {
    stopPump(ch);
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Invalid channel");
  }
}

void handleTimezone() {
  if (server.hasArg("gmt")) {
    gmtOffsetSec = server.arg("gmt").toInt();
  }
  if (server.hasArg("dst")) {
    daylightOffsetSec = server.arg("dst").toInt();
  }
  
  saveSettings();
  
  // Resync time with new timezone
  configTime(gmtOffsetSec, daylightOffsetSec, NTP_SERVER);
  
  server.send(200, "text/plain", "OK");
}
