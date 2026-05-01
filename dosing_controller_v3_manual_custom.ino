/*
 * ESP32 Aquarium Dosing Controller v3.1
 *
 * Correções aplicadas:
 * - Dose fracionada por porções com intervalo configurável
 * - Evita dose atrasada quando o horário já passou e a sequência ainda não começou
 * - Mudanças de configuração não forçam nova dose no mesmo dia
 * - Corrigido erro de max(long, int)
 * - Estrutura de funções revisada para compilar no Arduino IDE
 */

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>
#include <ArduinoOTA.h>

const char* WIFI_SSID = "VIVOFIBRA-34BE";
const char* WIFI_PASSWORD = "72232E34BE";
const char* OTA_HOSTNAME = "REEFDOSER32";
const char* OTA_PASSWORD = "123456789";
const char* NTP_SERVER = "pool.ntp.org";

const int RELAY_PINS[4] = {32, 33, 25, 26};
const bool RELAY_ACTIVE_LOW = false;

WebServer server(80);
Preferences preferences;

long gmtOffsetSec = -18000;
int daylightOffsetSec = 3600;

struct Channel {
  String name;
  float mlPerSecond;
  float targetMl;
  int doseHour;
  int doseMinute;
  bool doseDays[7];
  bool dosedToday;
  bool isRunning;
  unsigned long runStartTime;
  unsigned long runDuration;

  int portions;
  int intervalMinutes;
  int portionsDeliveredToday;
  bool scheduleStartedToday;
};

Channel channels[4];

bool wifiConnected = false;
bool timeSync = false;
int lastCheckedMinute = -1;
int lastCheckedDay = -1;

void setRelay(int channel, bool on);
void startPump(int channel, float mlToDose);
void startPumpTimed(int channel, unsigned long durationMs);
void stopPump(int channel);
void checkRunningPumps();
void checkScheduledDoses();
void resetDailyFlags();
void loadSettings();
void saveSettings();
void connectWiFi();
void syncTime();
void setupOTA();
void setupWebServer();
void handleRoot();
void handleStatus();
void handleDose();
void handleCalibrate();
void handleSave();
void handleStop();
void handleTimezone();

void setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== ESP32 Dosing Controller v3.1 Starting ===");

  for (int i = 0; i < 4; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    setRelay(i, false);
  }

  loadSettings();
  connectWiFi();

  if (wifiConnected) {
    setupOTA();
    syncTime();
  }

  setupWebServer();
  Serial.println("=== Setup Complete ===\n");
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  checkScheduledDoses();
  checkRunningPumps();
  resetDailyFlags();
  delay(100);
}

void setupOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
    Serial.println("OTA Start updating " + type);
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

void setRelay(int channel, bool on) {
  if (channel < 0 || channel > 3) return;
  if (RELAY_ACTIVE_LOW) {
    digitalWrite(RELAY_PINS[channel], on ? LOW : HIGH);
  } else {
    digitalWrite(RELAY_PINS[channel], on ? HIGH : LOW);
  }
}

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

  Serial.printf("Channel %d (%s): Starting pump for %.2f mL (%.1f seconds)\n",
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

void checkScheduledDoses() {
  if (!timeSync) return;

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  if (timeinfo.tm_min == lastCheckedMinute) return;
  lastCheckedMinute = timeinfo.tm_min;

  int dayOfWeek = timeinfo.tm_wday;

  for (int i = 0; i < 4; i++) {
    if (channels[i].doseHour < 0) continue;
    if (!channels[i].doseDays[dayOfWeek]) continue;
    if (channels[i].dosedToday) continue;
    if (channels[i].isRunning) continue;

    int startMin = channels[i].doseHour * 60 + channels[i].doseMinute;
    int nowMin = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    int elapsed = nowMin - startMin;

    if (elapsed < 0) continue;

    int parts = channels[i].portions;
    if (parts < 1) parts = 1;

    int intv = channels[i].intervalMinutes;
    if (intv < 1) intv = 1;

    if (elapsed > 0 && !channels[i].scheduleStartedToday && channels[i].portionsDeliveredToday == 0) {
      continue;
    }

    int portionsDue = (elapsed / intv) + 1;
    if (portionsDue > parts) portionsDue = parts;

    if (channels[i].portionsDeliveredToday < portionsDue) {
      float portionMl = channels[i].targetMl / (float)parts;

      if (channels[i].portionsDeliveredToday == 0) {
        channels[i].scheduleStartedToday = true;
      }

      startPump(i, portionMl);
      channels[i].portionsDeliveredToday++;

      Serial.printf("Channel %d (%s): dose %d/%d (%.2f mL)\n",
                    i + 1,
                    channels[i].name.c_str(),
                    channels[i].portionsDeliveredToday,
                    parts,
                    portionMl);

      if (channels[i].portionsDeliveredToday >= parts) {
        channels[i].dosedToday = true;
        Serial.printf("Channel %d (%s): Dose completa!\n", i + 1, channels[i].name.c_str());
      }

      saveSettings();
    }
  }
}

void resetDailyFlags() {
  if (!timeSync) return;

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  if (timeinfo.tm_wday != lastCheckedDay) {
    lastCheckedDay = timeinfo.tm_wday;
    for (int i = 0; i < 4; i++) {
      channels[i].dosedToday = false;
      channels[i].portionsDeliveredToday = 0;
      channels[i].scheduleStartedToday = false;
    }
    Serial.println("Daily flags reset for new day");
    saveSettings();
  }
}

void loadSettings() {
  preferences.begin("dosing", true);

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
    channels[i].portions = preferences.getInt((prefix + "parts").c_str(), 1);
    channels[i].intervalMinutes = preferences.getInt((prefix + "intv").c_str(), 10);
    channels[i].portionsDeliveredToday = preferences.getInt((prefix + "done").c_str(), 0);
    channels[i].scheduleStartedToday = preferences.getBool((prefix + "started").c_str(), false);

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
    preferences.putInt((prefix + "parts").c_str(), channels[i].portions);
    preferences.putInt((prefix + "intv").c_str(), channels[i].intervalMinutes);
    preferences.putInt((prefix + "done").c_str(), channels[i].portionsDeliveredToday);
    preferences.putBool((prefix + "started").c_str(), channels[i].scheduleStartedToday);

    for (int d = 0; d < 7; d++) {
      String dayKey = prefix + "day" + String(d);
      preferences.putBool(dayKey.c_str(), channels[i].doseDays[d]);
    }
  }

  preferences.end();
  Serial.println("Settings saved to flash");
}

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
  <link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'><text y='.9em' font-size='90'>🧪</text></svg>">
  <title>Reef Dooser DIY ESP32</title>
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
    input, select { width: 100%; padding: 10px; border: 1px solid #333; border-radius: 8px; background: #0f0f23; color: #fff; font-size: 16px; }
    input:focus { outline: none; border-color: #00d9ff; }
    button { padding: 12px 20px; border: none; border-radius: 8px; font-size: 16px; cursor: pointer; font-weight: 600; }
    .btn-primary { background: #00d9ff; color: #000; }
    .btn-danger { background: #ff4757; color: #fff; }
    .btn-secondary { background: #333; color: #fff; }
    .btn-success { background: #2ed573; color: #000; }
    .calibration { background: #1e1e3f; padding: 12px; border-radius: 8px; margin-top: 12px; }
    .calibration h3 { margin: 0 0 8px 0; font-size: 14px; color: #ffa502; }
    .cal-row { display: flex; gap: 8px; align-items: end; flex-wrap: wrap; }
    .cal-row input { width: 80px; }
    .indicator { display: inline-block; width: 10px; height: 10px; border-radius: 50%; margin-right: 8px; background: #333; }
    .indicator.on { background: #2ed573; }
    .time-inputs { display: flex; gap: 4px; align-items: center; }
    .time-inputs input { width: 60px; text-align: center; }
    .ml-rate { font-size: 12px; color: #2ed573; margin-top: 4px; }
    .days-selector { display: flex; gap: 6px; flex-wrap: wrap; margin-top: 8px; }
    .day-btn { width: 40px; height: 36px; border-radius: 6px; border: 1px solid #333; background: #0f0f23; color: #888; font-size: 12px; cursor: pointer; }
    .day-btn.active { background: #00d9ff; color: #000; border-color: #00d9ff; }
    .settings-card { background: #0f3460; }
    .name-input { background: transparent; border: 1px solid transparent; color: #00d9ff; font-size: 18px; font-weight: bold; padding: 4px 8px; width: 100%; }
    .name-input:hover { border-color: #333; }
    .name-input:focus { border-color: #00d9ff; background: #0f0f23; }
    .portion-box { background: #1a2e1e; border: 1px solid #2ed57344; border-radius: 8px; padding: 12px; margin-top: 12px; }
    .portion-box h3 { margin: 0 0 10px 0; font-size: 14px; color: #2ed573; }
    .portion-info { font-size: 11px; color: #888; margin-top: 4px; }
    .portion-progress { font-size: 13px; color: #ffa502; margin-top: 8px; background: #2a1e00; padding: 8px; border-radius: 6px; }
    input:disabled { opacity: 0.35; cursor: not-allowed; }
  </style>
</head>
<body>
  <h1>🧪 REEF Doser DIY ESP 32</h1>
  <div class="status" id="statusBar">Conectando...</div>
  <div id="channels"></div>

  <div class="card settings-card">
    <h2>⚙️ Configuracoes</h2>
    <div class="row">
      <div>
        <label>Fuso Horario (GMT Offset em horas)</label>
        <input type="number" id="tzOffset" step="0.5" min="-12" max="14">
      </div>
      <div>
        <label>Horario de Verao</label>
        <select id="dstOffset">
          <option value="0">Sem horario de verao</option>
          <option value="3600">+1 Hora DST</option>
        </select>
      </div>
      <div style="display:flex;align-items:end;">
        <button class="btn-success" onclick="saveTimezone()">Salvar Fuso</button>
      </div>
    </div>
  </div>

  <script>
    const dayNames = ['Dom','Seg','Ter','Qua','Qui','Sex','Sab'];
    let channelData = [];

    function updateStatus() {
      if (document.activeElement && (document.activeElement.tagName === 'INPUT' || document.activeElement.tagName === 'SELECT')) return;

      fetch('/status')
        .then(r => r.json())
        .then(data => {
          channelData = data.channels;
          document.getElementById('statusBar').textContent =
            (data.timeSync ? '🕐 ' + data.time : '⚠️ Hora nao sincronizada') + ' | IP: ' + data.ip;
          document.getElementById('tzOffset').value = data.gmtOffset / 3600;
          document.getElementById('dstOffset').value = data.dstOffset;

          let html = '';
          data.channels.forEach((ch, i) => {
            const calibrated = ch.mlPerSecond > 0;
            const scheduleEnabled = ch.doseHour >= 0;
            const parts = Math.max(ch.portions || 1, 1);
            const multiPortion = parts > 1;
            const portionMl = (ch.targetMl / parts).toFixed(2);
            const intv = ch.intervalMinutes || 10;

            let daysHtml = '';
            dayNames.forEach((d, di) => {
              const active = ch.doseDays[di] ? 'active' : '';
              daysHtml += `<button type="button" class="day-btn ${active}" onclick="toggleDay(${i},${di})" data-ch="${i}" data-day="${di}">${d}</button>`;
            });

            const totalMinutes = (parts - 1) * intv;
            const totalHours = (totalMinutes / 60).toFixed(1);
            const portionInfoTxt = multiPortion ? `${portionMl} mL por dose · duracao total: ~${totalHours}h` : 'Porcoes = 1: dose unica no horario';

            let progressHtml = '';
            if (multiPortion && scheduleEnabled) {
              const done = ch.portionsDeliveredToday || 0;
              const pct = Math.round((done / parts) * 100);
              progressHtml = `
                <div class="portion-progress">
                  📊 Progresso hoje: <strong>${done}/${parts}</strong> doses (${pct}%)
                </div>`;
            }

            html += `
              <div class="card">
                <h2>
                  <span class="indicator ${ch.isRunning ? 'on' : ''}"></span>
                  <input type="text" class="name-input" id="name${i}" value="${ch.name}" onchange="saveName(${i})">
                </h2>

                <div class="row">
                  <div>
                    <label>Volume total (mL)</label>
                    <input type="number" id="target${i}" value="${ch.targetMl}" step="1" min="1" oninput="recalcInfo(${i})">
                  </div>
                  <div>
                    <label>Horario de inicio</label>
                    <div class="time-inputs">
                      <input type="number" id="hour${i}" value="${scheduleEnabled ? ch.doseHour : ''}" placeholder="HH" min="0" max="23">
                      <span>:</span>
                      <input type="number" id="min${i}" value="${scheduleEnabled ? String(ch.doseMinute).padStart(2,'0') : ''}" placeholder="MM" min="0" max="59">
                    </div>
                  </div>
                </div>

                <div>
                  <label>Dias da semana</label>
                  <div class="days-selector">${daysHtml}</div>
                </div>

                <div class="portion-box">
                  <h3>⚗️ Dose Fracionada</h3>
                  <div class="row">
                    <div>
                      <label>Numero de doses</label>
                      <input type="number" id="portions${i}" value="${parts}" min="1" max="288" oninput="recalcInfo(${i})">
                    </div>
                    <div>
                      <label>Intervalo entre doses (min)</label>
                      <input type="number" id="intv${i}" value="${intv}" min="1" max="1440" ${!multiPortion ? 'disabled' : ''} oninput="recalcInfo(${i})">
                    </div>
                  </div>
                  <div class="portion-info" id="portionInfo${i}">${portionInfoTxt}</div>
                  ${progressHtml}
                </div>

                <div class="portion-box">
                  <h3>🎯 Dosagem Manual Customizada</h3>
                  <div class="row">
                    <div>
                      <label>Quantidade manual (mL)</label>
                      <input type="number" id="manualMl${i}" value="5" step="0.1" min="0.1">
                    </div>
                    <div style="display:flex;align-items:end;">
                      <button class="btn-primary" onclick="manualDose(${i})" ${!calibrated ? 'disabled' : ''}>Dosar</button>
                    </div>
                  </div>
                </div>

                <div class="row" style="margin-top:12px;">
                  <button class="btn-success" onclick="saveChannel(${i})">Salvar Programação</button>
                  ${ch.isRunning ? `<button class="btn-danger" onclick="stop(${i})">PARAR</button>` : ''}
                </div>

                ${calibrated ? `<div class="ml-rate">✓ Calibrado: ${ch.mlPerSecond.toFixed(3)} mL/seg</div>` : `<div class="ml-rate" style="color:#ff4757;">⚠ Nao calibrado</div>`}

                <div class="calibration">
                  <h3>🔧 Calibracao</h3>
                  <div class="cal-row">
                    <div>
                      <label>Segundos</label>
                      <input type="number" id="calSec${i}" value="10" min="1" max="60">
                    </div>
                    <button class="btn-secondary" onclick="calibrate(${i})">Rodar Bomba</button>
                    <div>
                      <label>mL coletados</label>
                      <input type="number" id="calMl${i}" step="0.1" placeholder="?">
                    </div>
                    <button class="btn-success" onclick="saveCalibration(${i})">Salvar Cal.</button>
                  </div>
                </div>
              </div>`;
          });

          document.getElementById('channels').innerHTML = html;
        })
        .catch(() => {
          document.getElementById('statusBar').textContent = 'Erro de conexao';
        });
    }

    function recalcInfo(ch) {
      const partsEl = document.getElementById('portions' + ch);
      const intvEl = document.getElementById('intv' + ch);
      const targetEl = document.getElementById('target' + ch);
      const infoEl = document.getElementById('portionInfo' + ch);
      if (!partsEl || !intvEl || !targetEl || !infoEl) return;

      const parts = Math.max(parseInt(partsEl.value) || 1, 1);
      const intv = Math.max(parseInt(intvEl.value) || 1, 1);
      const target = parseFloat(targetEl.value) || 0;
      intvEl.disabled = (parts <= 1);
      const portionMl = (target / parts).toFixed(2);
      const totalHours = (((parts - 1) * intv) / 60).toFixed(1);
      infoEl.textContent = parts > 1 ? `${portionMl} mL por dose · duracao total: ~${totalHours}h` : 'Porcoes = 1: dose unica no horario';
    }

    function toggleDay(ch, day) {
      channelData[ch].doseDays[day] = !channelData[ch].doseDays[day];
      const btn = document.querySelector(`[data-ch="${ch}"][data-day="${day}"]`);
      btn.classList.toggle('active');
      const days = channelData[ch].doseDays.map(d => d ? '1' : '0').join(',');
      fetch('/save?ch=' + ch + '&days=' + days);
    }

    function dose(ch) {
      const ml = document.getElementById('target' + ch).value;
      fetch('/dose?ch=' + ch + '&ml=' + ml).then(() => setTimeout(updateStatus, 500));
    }

    function manualDose(ch) {
      const ml = parseFloat(document.getElementById('manualMl' + ch).value);
      if (!ml || ml <= 0) {
        alert('Informe uma quantidade válida em mL');
        return;
      }
      fetch('/dose?ch=' + ch + '&ml=' + ml).then(() => {
        alert('Dosagem manual iniciada: ' + ml + ' mL');
        setTimeout(updateStatus, 500);
      });
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
      if (!ml || ml <= 0) { alert('Informe os mL coletados'); return; }
      const mlps = ml / sec;
      fetch('/save?ch=' + ch + '&mlps=' + mlps).then(() => {
        alert('Calibracao salva: ' + mlps.toFixed(3) + ' mL/seg');
        updateStatus();
      });
    }

    function saveChannel(ch) {
      const target = document.getElementById('target' + ch).value;
      const hour = document.getElementById('hour' + ch).value;
      const min = document.getElementById('min' + ch).value;
      const portions = document.getElementById('portions' + ch).value;
      const intv = document.getElementById('intv' + ch).value;
      const days = channelData[ch].doseDays.map(d => d ? '1' : '0').join(',');

      let url = '/save?ch=' + ch + '&target=' + target + '&days=' + days + '&portions=' + portions + '&intv=' + intv;
      if (hour !== '' && min !== '') {
        url += '&hour=' + hour + '&min=' + min;
      } else {
        url += '&hour=-1&min=0';
      }

      fetch(url).then(() => {
        alert('Programação salva!');
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
        alert('Fuso salvo!');
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
    json += "\"isRunning\":" + String(channels[i].isRunning ? "true" : "false") + ",";
    json += "\"portions\":" + String(channels[i].portions) + ",";
    json += "\"intervalMinutes\":" + String(channels[i].intervalMinutes) + ",";
    json += "\"portionsDeliveredToday\":" + String(channels[i].portionsDeliveredToday);
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
    startPumpTimed(ch, sec * 1000UL);
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

  if (server.hasArg("name")) channels[ch].name = server.arg("name");
  if (server.hasArg("mlps")) channels[ch].mlPerSecond = server.arg("mlps").toFloat();
  if (server.hasArg("target")) channels[ch].targetMl = server.arg("target").toFloat();

  if (server.hasArg("hour")) {
    channels[ch].doseHour = server.arg("hour").toInt();
  }
  if (server.hasArg("min")) {
    channels[ch].doseMinute = server.arg("min").toInt();
  }

  if (server.hasArg("portions")) {
    int p = (int)server.arg("portions").toInt();
    channels[ch].portions = (p < 1) ? 1 : p;
  }

  if (server.hasArg("intv")) {
    int intv = (int)server.arg("intv").toInt();
    channels[ch].intervalMinutes = (intv < 1) ? 1 : intv;
  }

  if (server.hasArg("days")) {
    String daysStr = server.arg("days");
    int idx = 0;
    for (int d = 0; d < 7 && idx < (int)daysStr.length(); d++) {
      channels[ch].doseDays[d] = (daysStr.charAt(idx) == '1');
      idx += 2;
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
  if (server.hasArg("gmt")) gmtOffsetSec = server.arg("gmt").toInt();
  if (server.hasArg("dst")) daylightOffsetSec = server.arg("dst").toInt();

  saveSettings();
  configTime(gmtOffsetSec, daylightOffsetSec, NTP_SERVER);
  server.send(200, "text/plain", "OK");
}
