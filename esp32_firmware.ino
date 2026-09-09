/*
  ESP32-C3 Mini — BME280 + SSD1306 climate station
  =================================================
  - Reads temperature / humidity / pressure from a BME280 (I2C)
  - Shows time, temperature and status on a 128x32 SSD1306 OLED (shared I2C bus)
  - Sends temperature + threshold "level" (GREEN/YELLOW/RED) to the Orpheus
    Pico over UART, so the Pico can blink the matching LED
  - Hosts a web dashboard over Wi-Fi: live readings + threshold editor

  ================== WIRING — matches Tenstar ESP32-C3 Super Mini build ==================
  I2C bus (BME280 + SSD1306 share it — different addresses, one bus):
    SDA -> GPIO6
    SCL -> GPIO7

  UART link to the Orpheus Pico:
    ESP32 TX  (GPIO21) -> Pico IO1 / GP1 (UART0 RX)
    ESP32 RX  (GPIO20) <- Pico IO0 / GP0 (UART0 TX)
    ESP32 G           -- Pico GND (3rd pin)  <-- REQUIRED. Even though both
                                                   boards are powered
                                                   separately, the UART
                                                   signal needs a shared
                                                   ground reference.

  IMPORTANT — check before powering on:
  1) BME280 VCC must go to the ESP32's "3.3" pin. This wasn't mentioned in
     the soldering notes for this build — if it's not connected, that alone
     explains a "BME280 not found" error, and if it's floating or on 5V it
     can damage a 3.3V-only BME280 module.
  2) The OLED's VCC is wired to "5V" here. If your OLED module works fine
     there, you can leave it — but moving it to the "3.3" pin instead is
     safer: it keeps the whole shared I2C bus at 3.3V logic, which is what
     both the ESP32-C3's GPIOs and a 3.3V-only BME280 expect. The ESP32-C3
     is NOT 5V tolerant on its GPIOs, and this bus is shared with the BME280.
  ==========================================================================================
*/

#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>

// ---------------- USER CONFIG ----------------
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// The ESP32 ALSO broadcasts its own hotspot at the same time as connecting
// to your home Wi-Fi above. Connect to this network directly and the
// dashboard is always at http://192.168.4.1 — no IP lookup needed.
const char* AP_SSID = "ClimateStation";
const char* AP_PASS = "climate123";   // must be 8+ characters, or use "" for an open network

// Timezone offset for the clock. Default = Central Europe (CET/CEST).
const long GMT_OFFSET_SEC = 3600;
const int  DST_OFFSET_SEC = 3600;

// Default LED thresholds in °C. Editable later from the web dashboard
// (persisted to flash, survives reboots).
float threshold_yellow = 25.0;
float threshold_red    = 30.0;

// ---------------- PIN CONFIG ----------------
#define I2C_SDA        6
#define I2C_SCL        7
#define PICO_UART_TX   21   // -> Pico IO1/GP1
#define PICO_UART_RX   20   // <- Pico IO0/GP0
#define PICO_UART_BAUD 115200

#define OLED_WIDTH  128
#define OLED_HEIGHT 32
#define OLED_ADDR   0x3C   // some modules use 0x3D — change if display stays blank
#define BME_ADDR    0x76   // some modules use 0x77 — code falls back automatically

// ---------------- GLOBAL OBJECTS ----------------
Adafruit_BME280 bme;
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
WebServer server(80);
Preferences prefs;

bool bmeOk = false;
float g_temp = NAN, g_hum = NAN, g_pres = NAN;
String g_level = "UNKNOWN";
unsigned long lastSensorRead = 0;
unsigned long lastPicoSend = 0;
unsigned long lastDisplayUpdate = 0;

// ---------------- HELPERS ----------------
String computeLevel(float t) {
  if (isnan(t)) return "UNKNOWN";
  if (t >= threshold_red) return "RED";
  if (t >= threshold_yellow) return "YELLOW";
  return "GREEN";
}

String timeString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 200)) return "--:--:--";
  char buf[9];
  strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo);
  return String(buf);
}

String uptimeString() {
  unsigned long s = millis() / 1000;
  unsigned int h = s / 3600;
  unsigned int m = (s % 3600) / 60;
  unsigned int sec = s % 60;
  char buf[16];
  snprintf(buf, sizeof(buf), "%02u:%02u:%02u", h, m, sec);
  return String(buf);
}

void sendToPico() {
  // Line protocol the Pico expects: LEVEL:<GREEN|YELLOW|RED>,TEMP:<float>\n
  Serial1.printf("LEVEL:%s,TEMP:%.2f\n", g_level.c_str(), g_temp);
}

void updateDisplay() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print(timeString());
  display.setCursor(70, 0);
  display.printf("%ddBm", WiFi.RSSI());

  display.setCursor(0, 10);
  if (bmeOk) display.printf("%.1fC  %.0f%%RH", g_temp, g_hum);
  else display.print("BME280 error");

  display.setCursor(0, 20);
  if (bmeOk) display.printf("%.0fhPa  %s", g_pres, g_level.c_str());
  else display.printf("up %s", uptimeString().c_str());

  display.display();
}

// ---------------- WEB DASHBOARD ----------------
const char INDEX_HTML[] = R"HTMLPAGE(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Climate Station</title>
<style>
  :root{
    --bg:#0b0f1a; --panel:#131a2a; --panel2:#0f1522;
    --line:#24304a; --text:#e8edf7; --muted:#7c8aa8;
    --blue:#4f8fff; --green:#33d17a; --yellow:#f5c242; --red:#ff5c5c;
  }
  *{box-sizing:border-box;}
  body{
    margin:0; background:var(--bg); color:var(--text);
    font-family:-apple-system,Segoe UI,Helvetica,Arial,sans-serif;
    padding:20px 16px 60px; max-width:480px; margin:0 auto;
  }
  header{display:flex; align-items:center; gap:10px; margin-bottom:18px;}
  .dot{width:9px;height:9px;border-radius:50%;background:var(--blue);
    animation:pulse 2s infinite;}
  @keyframes pulse{
    0%{box-shadow:0 0 0 0 rgba(79,143,255,.5);}
    70%{box-shadow:0 0 0 8px rgba(79,143,255,0);}
    100%{box-shadow:0 0 0 0 rgba(79,143,255,0);}
  }
  h1{font-size:15px; font-weight:600; letter-spacing:.02em; margin:0; color:var(--muted); text-transform:uppercase;}
  .card{background:var(--panel); border:1px solid var(--line); border-radius:14px; padding:18px; margin-bottom:14px;}
  .hero{text-align:center; padding:28px 18px;}
  .hero .temp{font-size:56px; font-weight:700; line-height:1; letter-spacing:-.02em;}
  .hero .sub{font-family:ui-monospace,SFMono-Regular,Consolas,monospace; color:var(--muted); margin-top:8px; font-size:13px;}
  .gauge{position:relative; height:10px; border-radius:6px; margin:22px 4px 8px;
    background:linear-gradient(90deg, var(--green) 0%, var(--green) 40%, var(--yellow) 55%, var(--red) 100%);}
  .marker{position:absolute; top:-6px; width:2px; height:22px; background:var(--text); border-radius:2px; transition:left .4s ease;}
  .gauge-labels{display:flex; justify-content:space-between; font-family:ui-monospace,monospace; font-size:11px; color:var(--muted); margin:0 4px;}
  .grid{display:grid; grid-template-columns:1fr 1fr; gap:10px;}
  .stat{background:var(--panel2); border:1px solid var(--line); border-radius:10px; padding:12px;}
  .stat .label{font-size:11px; color:var(--muted); text-transform:uppercase; letter-spacing:.04em;}
  .stat .value{font-family:ui-monospace,SFMono-Regular,Consolas,monospace; font-size:18px; margin-top:4px;}
  .level-tag{display:inline-block; padding:3px 10px; border-radius:20px; font-size:12px; font-weight:600; margin-top:8px;}
  .level-GREEN{background:rgba(51,209,122,.15); color:var(--green);}
  .level-YELLOW{background:rgba(245,194,66,.15); color:var(--yellow);}
  .level-RED{background:rgba(255,92,92,.15); color:var(--red);}
  .level-UNKNOWN{background:rgba(124,138,168,.15); color:var(--muted);}
  h2{font-size:13px; color:var(--muted); text-transform:uppercase; letter-spacing:.04em; margin:0 0 14px;}
  label{display:block; font-size:12px; color:var(--muted); margin-bottom:6px;}
  input[type=number]{
    width:100%; background:var(--panel2); border:1px solid var(--line); color:var(--text);
    padding:10px; border-radius:8px; font-family:ui-monospace,monospace; font-size:15px; margin-bottom:14px;
  }
  button{
    width:100%; padding:12px; border:none; border-radius:8px; background:var(--blue);
    color:#fff; font-weight:600; font-size:14px; cursor:pointer;
  }
  button:active{opacity:.85;}
  footer{text-align:center; color:var(--muted); font-size:11px; margin-top:10px;}
</style>
</head>
<body>
  <header>
    <div class="dot"></div>
    <h1>Climate Station</h1>
  </header>

  <div class="card hero">
    <div class="temp" id="temp">--.-&deg;</div>
    <div class="sub" id="clock">--:--:--</div>
    <div class="gauge">
      <div class="marker" id="marker" style="left:0%"></div>
    </div>
    <div class="gauge-labels"><span>0&deg;</span><span id="lblY">y</span><span id="lblR">r</span><span>40&deg;</span></div>
    <span class="level-tag" id="levelTag">--</span>
  </div>

  <div class="card">
    <div class="grid">
      <div class="stat"><div class="label">Humidity</div><div class="value" id="hum">--%</div></div>
      <div class="stat"><div class="label">Pressure</div><div class="value" id="pres">-- hPa</div></div>
      <div class="stat"><div class="label">Wi-Fi</div><div class="value" id="rssi">-- dBm</div></div>
      <div class="stat"><div class="label">Uptime</div><div class="value" id="uptime">--:--:--</div></div>
    </div>
  </div>

  <div class="card">
    <h2>LED Thresholds (&deg;C)</h2>
    <form id="thForm">
      <label for="yellowInput">Yellow turns on at</label>
      <input type="number" step="0.1" name="yellow" id="yellowInput" required>
      <label for="redInput">Red turns on at</label>
      <input type="number" step="0.1" name="red" id="redInput" required>
      <button type="submit">Save thresholds</button>
    </form>
  </div>

  <footer>Green is default below the yellow threshold &middot; blue blinks whenever the Pico is alive</footer>

<script>
const RANGE_MIN = 0, RANGE_MAX = 40;
function pct(v){ return Math.min(100, Math.max(0, (v - RANGE_MIN) / (RANGE_MAX - RANGE_MIN) * 100)); }

async function refresh(){
  try{
    const r = await fetch('/data');
    const d = await r.json();
    document.getElementById('temp').innerHTML = d.temp.toFixed(1) + '&deg;';
    document.getElementById('clock').textContent = d.time;
    document.getElementById('hum').textContent = d.humidity.toFixed(0) + '%';
    document.getElementById('pres').textContent = d.pressure.toFixed(0) + ' hPa';
    document.getElementById('rssi').textContent = d.rssi + ' dBm';
    document.getElementById('uptime').textContent = d.uptime;
    document.getElementById('marker').style.left = pct(d.temp) + '%';
    document.getElementById('lblY').textContent = d.threshold_yellow + '\u00b0';
    document.getElementById('lblR').textContent = d.threshold_red + '\u00b0';
    const tag = document.getElementById('levelTag');
    tag.textContent = d.level;
    tag.className = 'level-tag level-' + d.level;
    if(!document.activeElement || document.activeElement.tagName !== 'INPUT'){
      document.getElementById('yellowInput').value = d.threshold_yellow;
      document.getElementById('redInput').value = d.threshold_red;
    }
  }catch(e){ /* offline this tick, ignore */ }
}

document.getElementById('thForm').addEventListener('submit', async (e) => {
  e.preventDefault();
  const yellow = document.getElementById('yellowInput').value;
  const red = document.getElementById('redInput').value;
  await fetch('/set-thresholds', {
    method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'yellow=' + encodeURIComponent(yellow) + '&red=' + encodeURIComponent(red)
  });
  refresh();
});

refresh();
setInterval(refresh, 2000);
</script>
</body>
</html>
)HTMLPAGE";

void handleRoot() {
  server.send(200, "text/html", INDEX_HTML);
}

void handleData() {
  String json = "{";
  json += "\"temp\":" + String(bmeOk ? g_temp : 0, 2) + ",";
  json += "\"humidity\":" + String(bmeOk ? g_hum : 0, 2) + ",";
  json += "\"pressure\":" + String(bmeOk ? g_pres : 0, 2) + ",";
  json += "\"time\":\"" + timeString() + "\",";
  json += "\"uptime\":\"" + uptimeString() + "\",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"level\":\"" + g_level + "\",";
  json += "\"threshold_yellow\":" + String(threshold_yellow, 1) + ",";
  json += "\"threshold_red\":" + String(threshold_red, 1);
  json += "}";
  server.send(200, "application/json", json);
}

void handleSetThresholds() {
  if (server.hasArg("yellow")) threshold_yellow = server.arg("yellow").toFloat();
  if (server.hasArg("red"))    threshold_red    = server.arg("red").toFloat();
  prefs.putFloat("yellow", threshold_yellow);
  prefs.putFloat("red", threshold_red);
  server.send(200, "text/plain", "OK");
}

// ---------------- SETUP / LOOP ----------------
void setup() {
  Serial.begin(115200);
  Serial1.begin(PICO_UART_BAUD, SERIAL_8N1, PICO_UART_RX, PICO_UART_TX);

  Wire.begin(I2C_SDA, I2C_SCL);

  prefs.begin("thermcfg", false);
  threshold_yellow = prefs.getFloat("yellow", threshold_yellow);
  threshold_red    = prefs.getFloat("red", threshold_red);

  bmeOk = bme.begin(BME_ADDR, &Wire);
  if (!bmeOk) bmeOk = bme.begin(0x77, &Wire);
  if (!bmeOk) Serial.println("BME280 not found - check wiring/address");

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("SSD1306 not found - check wiring/address");
  }
  display.setRotation(2);  // 0=normal, 1=90°, 2=180°, 3=270° — adjust if this isn't right
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Connecting WiFi...");
  display.display();

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("Hotspot IP: ");
  Serial.println(WiFi.softAPIP());

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 20000) {
    delay(300);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected. IP: ");
    Serial.println(WiFi.localIP());
    configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");
  } else {
    Serial.println("WiFi not connected - dashboard/NTP unavailable");
  }

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/set-thresholds", HTTP_POST, handleSetThresholds);
  server.begin();
}

void loop() {
  server.handleClient();

  unsigned long now = millis();

  if (now - lastSensorRead >= 2000) {
    lastSensorRead = now;
    if (bmeOk) {
      g_temp = bme.readTemperature();
      g_hum  = bme.readHumidity();
      g_pres = bme.readPressure() / 100.0F;
      g_level = computeLevel(g_temp);
    }
  }

  if (now - lastPicoSend >= 1000) {
    lastPicoSend = now;
    if (bmeOk) sendToPico();
  }

  if (now - lastDisplayUpdate >= 1000) {
    lastDisplayUpdate = now;
    updateDisplay();
  }
}
