#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <Adafruit_MAX31865.h>
#include <ArduinoJson.h>

// Software SPI: CS, DI, DO, CLK
Adafruit_MAX31865 thermo = Adafruit_MAX31865(4, 7, 8, 9);

const char* ssid = "m_unifi_main";
const char* password = "shoal-mUpU@S";

// 72x40 mapped into SSD1306 128x64 buffer
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, 6, 5);
int width = 72;
int height = 40;
int xOffset = 30;
int yOffset = 12;

#define RREF      4300.0
#define RNOMINAL  1000.0

// Global temperature variable
float temperature = -100.0;

// -------- Ring buffer config --------
static const uint16_t HISTORY_LEN = 240; // 2 hours, every 30 seconds.
static float tempHistory[HISTORY_LEN];
static uint16_t histWrite = 0;
static uint16_t histCount = 0;

// timing
static uint32_t lastSampleMs = 0;
static uint32_t lastHistoryMs = 0;

float tempAverage = 0.0;
static uint32_t nrOfMeasurement = 0;

void addTempSample(float t) {
  tempHistory[histWrite] = t;
  histWrite = (histWrite + 1) % HISTORY_LEN;
  if (histCount < HISTORY_LEN) histCount++;
}

// HTML page (uses fetch to poll /temp every 1s)
const char index_html[] PROGMEM = R"rawliteral(
<!doctype html>
<html>
  <head>
    <meta charset="utf-8">
    <title>ESP32-C3 Temperature</title>
    <meta name="viewport" content="width=device-width,initial-scale=1">
    <style>
      :root{
        --bg1:#f6fff8;
        --bg2:#ffffff;
        --card:#ffffff;
        --text:#0f172a;
        --muted:#475569;
        --green1:#0ea75a;
        --green2:#21c46b;
      }

      body{
        font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,"Helvetica Neue",Arial;
        margin:0;
        min-height:100vh;
        background: radial-gradient(1200px 600px at 20% 10%, #d8ffe7, transparent 60%),
                    radial-gradient(900px 500px at 90% 30%, #d2fbe2, transparent 55%),
                    linear-gradient(180deg, var(--bg1), var(--bg2));
        display:flex;
        align-items:center;
        justify-content:center;
        padding:20px;
        color:var(--text);
      }

      .wrap{
        width:min(1100px, 98vw);
      }

      .header{
        display:flex;
        align-items:flex-end;
        justify-content:space-between;
        gap:16px;
        margin-bottom:14px;
      }

      .title{
        font-size:14px;
        letter-spacing:.08em;
        text-transform:uppercase;
        color:var(--muted);
      }

      .readout{
        display:flex;
        align-items:baseline;
        gap:10px;
      }

      #temp{
        font-size:84px;
        font-weight:800;
        line-height:1;
        letter-spacing:.5px;
      }

      .unit{
        font-size:26px;
        font-weight:700;
        color:var(--muted);
      }

      .sub{
        margin-top:4px;
        font-size:13px;
        color:var(--muted);
      }

      .panel{
        border-radius:22px;
        background:rgba(255,255,255,0.75);
        backdrop-filter: blur(10px);
        box-shadow: 0 18px 60px rgba(2, 44, 20, 0.14);
        padding:18px;
      }

      /* Big chart dominates */
      canvas{
        width:100%;
        height:520px; /* BIG */
        display:block;
        border-radius:18px;
        background:
          radial-gradient(900px 400px at 20% 20%, rgba(34,197,94,0.35), transparent 55%),
          radial-gradient(700px 380px at 80% 30%, rgba(16,185,129,0.28), transparent 60%),
          linear-gradient(180deg, rgba(6,95,70,0.35), rgba(34,197,94,0.18));
        box-shadow: inset 0 0 0 1px rgba(255,255,255,0.18);
      }

      @media (max-width: 720px){
        #temp{ font-size:68px; }
        canvas{ height:420px; }
      }
    </style>
  </head>

  <body>
    <div class="wrap">
      <div class="header">
        <div>
          <div class="title">Temperature monitor</div>
          <div class="sub">Updates every second • history up to 2 hours</div>
        </div>

        <div class="readout">
          <div id="temp">--.-</div>
          <div class="unit">°C</div>
        </div>
      </div>

      <div class="panel">
        <canvas id="tempChart" width="1200" height="520"></canvas>
      </div>
    </div>

    <script>
      const maxSeconds = 240;
      const dataPoints = [];

      const canvas = document.getElementById("tempChart");
      const ctx = canvas.getContext("2d");

      function roundDownTo5(x){ return Math.floor(x / 5) * 5; }
      function roundUpTo5(x){ return Math.ceil(x / 5) * 5; }

      async function fetchCurrentTemp() {
        const res = await fetch('/temp', { cache: 'no-store' });
        if (!res.ok) throw new Error('Network response not OK');
        const j = await res.json();
        return Number(j.temp);
      }

      async function loadHistory() {
        try {
          const res = await fetch('/history', { cache: 'no-store' });
          if (!res.ok) throw new Error('History endpoint not OK');
          const j = await res.json();
          const arr = Array.isArray(j) ? j : (Array.isArray(j.temps) ? j.temps : []);
          console.log('Loaded history points:', arr);
          console.log('datapoints before update:', dataPoints);
          for(let i = 0; i < arr.length; i++) dataPoints[i] = arr[i]; // update in-place to avoid reallocating array and breaking references
          drawChart();
        } catch (e) {
          console.warn('loadHistory error', e);
        }
      }

      async function tick() {
        if (tick.counter == undefined) tick.counter = 0;
        try {
          const value = await fetchCurrentTemp();
          document.getElementById('temp').textContent = value.toFixed(2);
          if (tick.counter++ % 30 == 0) { // every 30 ticks, add to chart for smoother animation
            loadHistory(); // reload full history every 30s to correct any drift or missed points
          }
        } catch (e) {
          document.getElementById('temp').textContent = '--.-';
          console.warn('tick error', e);
        }
      }

      function addDataPoint(value) {
        dataPoints.push(value);
        if (dataPoints.length > maxSeconds) dataPoints.shift();
      }

      function drawGridAndLabels(min5, max5, padL, padR, padT, padB, innerW, innerH){
        const w = canvas.width, h = canvas.height;

        const MAGENTA_LINE = "rgba(255, 0, 255, 0.35)";
        const MAGENTA_TEXT = "rgba(255, 0, 255, 0.95)";

        // outer frame
        ctx.save();
        ctx.strokeStyle = MAGENTA_LINE;
        ctx.lineWidth = 1;
        ctx.strokeRect(0.5, 0.5, w - 1, h - 1);
        ctx.restore();

        // ----- Horizontal grid every 5°C -----
        ctx.save();
        ctx.font = "14px -apple-system, BlinkMacSystemFont, Segoe UI, Roboto, Arial";
        ctx.lineWidth = 1;

        for (let t = min5; t <= max5; t += 5){
          const norm = (t - min5) / (max5 - min5 || 1);
          const y = padT + (1 - norm) * innerH;
          const yy = Math.round(y) + 0.5;

          // grid line
          ctx.strokeStyle = MAGENTA_LINE;
          ctx.beginPath();
          ctx.moveTo(padL, yy);
          ctx.lineTo(padL + innerW, yy);
          ctx.stroke();

          // label
          ctx.fillStyle = MAGENTA_TEXT;
          ctx.fillText(`${t}°C`, padL + 8, yy - 18);
        }
        ctx.restore();


        // ----- Vertical time markers -----
        const markers = [
          { secondsAgo: 240, label: "2h" },
          { secondsAgo: 120, label: "1h" },
          { secondsAgo: 60,  label: "30m" }
        ];

        ctx.save();
        ctx.font = "14px -apple-system, BlinkMacSystemFont, Segoe UI, Roboto, Arial";
        ctx.lineWidth = 1;

        markers.forEach(m => {
          const x = padL + innerW - (m.secondsAgo / maxSeconds) * innerW;
          const xx = Math.round(x) + 0.5;

          // vertical line
          ctx.strokeStyle = MAGENTA_LINE;
          ctx.beginPath();
          ctx.moveTo(xx, padT);
          ctx.lineTo(xx, padT + innerH);
          ctx.stroke();

          // label
          ctx.fillStyle = MAGENTA_TEXT;
          ctx.fillText(
            m.label,
            Math.min(Math.max(xx + 6, padL + 6), padL + innerW - 30),
            padT + innerH + 10
          );
        });

        ctx.restore();
      }

      function drawSmoothedPath(xs, ys) {
        const n = xs.length;
        if (n < 2) return;

        ctx.beginPath();
        ctx.moveTo(xs[0], ys[0]);

        for (let i = 0; i < n - 1; i++) {
          const x0 = xs[Math.max(0, i - 1)];
          const y0 = ys[Math.max(0, i - 1)];
          const x1 = xs[i];
          const y1 = ys[i];
          const x2 = xs[i + 1];
          const y2 = ys[i + 1];
          const x3 = xs[Math.min(n - 1, i + 2)];
          const y3 = ys[Math.min(n - 1, i + 2)];

          const t = 0.22;
          const cp1x = x1 + (x2 - x0) * t;
          const cp1y = y1 + (y2 - y0) * t;
          const cp2x = x2 - (x3 - x1) * t;
          const cp2y = y2 - (y3 - y1) * t;

          ctx.bezierCurveTo(cp1x, cp1y, cp2x, cp2y, x2, y2);
        }
      }

      function drawChart() {
        const w = canvas.width;
        const h = canvas.height;

        ctx.clearRect(0, 0, w, h);

        if (dataPoints.length < 2) return;

        // padding inside the canvas (big readable chart)
        const padL = 70;
        const padR = 26;
        const padT = 26;
        const padB = 46;

        const innerW = w - padL - padR;
        const innerH = h - padT - padB;

        // Determine min/max, then snap to 5°C grid
        const minRaw = Math.min(...dataPoints);
        const maxRaw = Math.max(...dataPoints);

        // add a little breathing room
        const min5 = roundDownTo5(minRaw - 0.5);
        const max5 = roundUpTo5(maxRaw + 0.5);

        const range = (max5 - min5) || 1;

        // Grid (every 5°C) + labels
        drawGridAndLabels(min5, max5, padL, padR, padT, padB, innerW, innerH);

        // Build points
        const n = dataPoints.length;
        const xs = new Array(n);
        const ys = new Array(n);

        for (let i = 0; i < n; i++) {
          const x = padL + (i / (n - 1)) * innerW;
          const norm = (dataPoints[i] - min5) / range;  // 0..1
          const y = padT + (1 - norm) * innerH;
          xs[i] = x;
          ys[i] = y;
        }

        // Area fill under curve
        ctx.save();
        drawSmoothedPath(xs, ys);
        ctx.lineTo(padL + innerW, padT + innerH);
        ctx.lineTo(padL, padT + innerH);
        ctx.closePath();
        ctx.fillStyle = "rgba(255,255,255,0.10)";
        ctx.fill();
        ctx.restore();

        // Line
        ctx.save();
        ctx.lineWidth = 4;
        ctx.strokeStyle = "rgba(255,255,255,0.92)";
        ctx.shadowColor = "rgba(0,0,0,0.20)";
        ctx.shadowBlur = 10;
        drawSmoothedPath(xs, ys);
        ctx.stroke();
        ctx.restore();

        // Latest dot + value bubble
        const lx = xs[n - 1];
        const ly = ys[n - 1];
        const latest = dataPoints[n - 1];

        ctx.save();
        ctx.fillStyle = "rgba(255,255,255,0.95)";
        ctx.beginPath();
        ctx.arc(lx, ly, 6.5, 0, Math.PI * 2);
        ctx.fill();
        ctx.restore();

        // value label near latest point
        ctx.save();
        ctx.font = "16px -apple-system, BlinkMacSystemFont, Segoe UI, Roboto, Arial";
        const label = latest.toFixed(1) + "°C";
        const tw = ctx.measureText(label).width;
        const bx = Math.min(Math.max(lx - tw - 22, padL + 6), padL + innerW - tw - 14);
        const by = Math.max(ly - 34, padT + 6);

        ctx.fillStyle = "rgba(0,0,0,0.22)";
        ctx.beginPath();
        ctx.roundRect(bx, by, tw + 16, 26, 10);
        ctx.fill();

        ctx.fillStyle = "rgba(255,255,255,0.95)";
        ctx.fillText(label, bx + 8, by + 18);
        ctx.restore();
      }

      // Startup
      (async () => {
        await loadHistory();
        await tick();
        setInterval(tick, 1000);
      })();
    </script>
  </body>
</html>
)rawliteral";

WebServer server(80);

/*
static void sendHistoryJson() {
  // Return oldest -> newest
  const uint16_t count = histCount;
  const uint16_t start = (histWrite + HISTORY_LEN - count) % HISTORY_LEN;

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", ""); // headers now

  WiFiClient client = server.client();
  client.print("{\"temps\":[");

  for (uint16_t i = 0; i < count; i++) {
    uint16_t idx = (start + i) % HISTORY_LEN;
    if (i) client.print(',');
    client.print(tempHistory[idx], 2);
    // Optional: yield a bit to keep WiFi happy on long responses
    if ((i & 0xFF) == 0) delay(0);
  }

  client.print("]}");
}
  */

void setup() {
  Serial.begin(115200);
  delay(500);

  thermo.begin(MAX31865_3WIRE);

  u8g2.begin();
  u8g2.setContrast(255);
  u8g2.setBusClock(400000);
  u8g2.setFont(u8g2_font_ncenB10_tr);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    Serial.print('.');
    delay(500);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nFailed to connect to WiFi");
  }

  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", index_html);
  });

  server.on("/temp", HTTP_GET, []() {
    String response = "{\"temp\":";
    response += String(temperature, 2);  // 2 decimal places
    response += "}";

    server.send(200, "application/json", response);
  });

  server.on("/history", HTTP_GET, []() {
    String response = "{\"temps\":[";
    // Return oldest -> newest
    const uint16_t count = histCount;
    const uint16_t start = (histWrite + HISTORY_LEN - count) % HISTORY_LEN; 
    for (uint16_t i = 0; i < count; i++) {
      uint16_t idx = (start + i) % HISTORY_LEN;
      if (i) response += ",";
      response += String(tempHistory[idx], 2);
    }
    response += "]}";
    server.send(200, "application/json", response);
  });

  server.begin();

  // Init temp history with current temp to avoid weird graph on first load
  temperature = thermo.temperature(RNOMINAL, RREF);
  for (uint16_t i = 0; i < HISTORY_LEN; i++) {
    addTempSample(temperature);
  }

  lastSampleMs = millis();
}

void loop() {
  // Keep web server responsive
  server.handleClient();

  // 1 Hz sampling + display update without blocking
  uint32_t now = millis();
  if (now - lastSampleMs >= 1000) {
    lastSampleMs += 1000;
    tempAverage += temperature;
    nrOfMeasurement++;

    temperature = thermo.temperature(RNOMINAL, RREF);
    if (lastHistoryMs + 30000 <= now) { // every 30s, add to history
      lastHistoryMs = now;
      addTempSample(tempAverage / nrOfMeasurement);
      nrOfMeasurement = 0;
      tempAverage = 0.0;
    }

    u8g2.clearBuffer();
    u8g2.drawFrame(xOffset + 0, yOffset + 0, width, height);
    u8g2.setCursor(xOffset + 5, yOffset + 25);
    u8g2.printf("T: %6.2f", temperature);
    u8g2.sendBuffer();
  }
}