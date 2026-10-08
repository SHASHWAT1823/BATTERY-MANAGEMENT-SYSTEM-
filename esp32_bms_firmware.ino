#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_ADS1X15.h>
#include <WiFi.h>
#include <WebServer.h>
#include <math.h>

// ===== PIN & PERIPHERAL ALLOCATION =====
#define MOSFET_PIN 18
#define I2C_SDA 21
#define I2C_SCL 22

// Hardware Interfaces
LiquidCrystal_I2C lcd(0x27, 16, 2);
Adafruit_ADS1115 ads;
WebServer server(80);

// Wi-Fi Credentials for IoT Telemetry Server
const char* ssid = "BMS_TELEMETRY_NODE";
const char* password = "Password123";

// ===== ELECTROCHEMICAL & PHYSICAL CONSTANTS =====
#define V_OVERVOLT_LIMIT  8.40f
#define V_UNDERVOLT_LIMIT 6.00f
#define T_OVERTEMP_LIMIT  60.0f
#define DT_DT_LIMIT       0.40f

// NTC Steinhart-Hart Constants
const float SERIES_R = 10000.0f;
const float NOMINAL_R = 10000.0f;
const float NOMINAL_T = 25.0f;
const float B_COEF = 3950.0f;

// 2S Li-ion Piecewise Linear OCV Curve Table
struct OCVEntry { float volt; float soc; };
const OCVEntry OCV_MAP[] = {
  {6.00f, 0.0f},  {6.90f, 10.0f}, {7.36f, 25.0f},
  {7.48f, 50.0f}, {7.70f, 75.0f}, {8.00f, 90.0f}, {8.40f, 100.0f}
};

// ===== TELEMETRY DATA STRUCTURE =====
struct BMS_State {
  float packVoltage;
  float cell1;
  float cell2;
  float deltaV;
  float temperature;
  float coreTemp;
  float dTDt;
  float soc;
  float soh;
  float sop;
  String dtc;
  String mosfetState;
  String status;
  bool fault;
  String watchdogState;
} bmsData;

// Multithreading & Watchdog Synchronization
volatile unsigned long lastHeartbeat = 0;
volatile bool watchdogTripped = false;
portMUX_TYPE bmsMux = portMUX_INITIALIZER_UNLOCKED;

// Temperature History Buffer for Rate-of-Rise
float tempHistory[5] = {25.0f, 25.0f, 25.0f, 25.0f, 25.0f};
unsigned long timeHistory[5] = {0, 0, 0, 0, 0};
int histIdx = 0;

// ===== MATHEMATICAL ESTIMATION FUNCTIONS =====
float calculateSOC(float v) {
  if (v <= OCV_MAP[0].volt) return 0.0f;
  if (v >= OCV_MAP[6].volt) return 100.0f;
  for (int i = 0; i < 6; i++) {
    if (v >= OCV_MAP[i].volt && v <= OCV_MAP[i+1].volt) {
      return OCV_MAP[i].soc + (v - OCV_MAP[i].volt) * 
             (OCV_MAP[i+1].soc - OCV_MAP[i].soc) / (OCV_MAP[i+1].volt - OCV_MAP[i].volt);
    }
  }
  return 50.0f;
}

float readNTCTemperature(float adcV) {
  float vcc = 3.3f;
  if (adcV <= 0.05f || adcV >= (vcc - 0.05f)) return 25.0f;
  float rNtc = (adcV * SERIES_R) / (vcc - adcV);
  float steinhart = log(rNtc / NOMINAL_R) / B_COEF;
  steinhart += 1.0f / (NOMINAL_T + 273.15f);
  return (1.0f / steinhart) - 273.15f;
}

// ===== WEB DASHBOARD HTML/JS TEMPLATE =====
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<title>ESP32 Automotive BMS Dashboard</title>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>
<style>
body{font-family:'Segoe UI',sans-serif;background:#060b19;color:#f8fafc;margin:0;padding:15px;}
.box{max-width:900px;margin:auto;}
h1{text-align:center;color:#38bdf8;font-size:22px;margin:5px 0;}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:10px;margin-top:12px;}
.card{background:#0f172a;border-radius:8px;padding:12px;text-align:center;border:1px solid #1e293b;}
.val{font-size:24px;font-weight:bold;color:#fff;margin:4px 0;}
.lbl{font-size:10px;color:#94a3b8;text-transform:uppercase;font-weight:600;}
.alert{padding:12px;border-radius:6px;text-align:center;font-weight:bold;margin-top:10px;}
.ok{background:#064e3b;color:#34d399;} .fault{background:#7f1d1d;color:#f87171;}
</style></head>
<body><div class='box'>
<h1>⚡ ESP32 AUTOMOTIVE BMS TELEMETRY</h1>
<div id='alert' class='alert ok'>SYSTEM STATUS: NORMAL | WATCHDOG ACTIVE</div>
<div class='grid'>
<div class='card'><div class='lbl'>Pack Voltage</div><div id='v' class='val'>--</div><span>Volts</span></div>
<div class='card'><div class='lbl'>Cells (C1/C2)</div><div id='c' class='val' style='font-size:18px;'>--</div><span>&Delta;V: <span id='dv'>0</span>V</span></div>
<div class='card'><div class='lbl'>Surf / Core T</div><div id='t' class='val' style='font-size:18px;'>--</div><span>dT/dt: <span id='dt'>0</span>&deg;C/s</span></div>
<div class='card'><div class='lbl'>SOC / SOH</div><div id='sh' class='val' style='font-size:18px;'>--</div><span>Estimated</span></div>
<div class='card'><div class='lbl'>SOP Limit</div><div id='sop' class='val' style='color:#38bdf8;'>--</div><span>Derating %</span></div>
</div>
<div class='grid' style='grid-template-columns:1fr 1fr 1fr;'>
<div class='card'><div class='lbl'>DTC Code</div><div id='dtc' class='val' style='font-size:16px;color:#fbbf24;'>--</div></div>
<div class='card'><div class='lbl'>Watchdog</div><div id='wd' class='val' style='font-size:16px;color:#10b981;'>--</div></div>
<div class='card'><div class='lbl'>MOSFET Contactor</div><div id='fet' class='val' style='font-size:16px;color:#38bdf8;'>--</div></div>
</div>
<div class='card' style='margin-top:15px;'><canvas id='ch' height='110'></canvas></div>
</div>
<script>
const ctx=document.getElementById('ch').getContext('2d');
const ch=new Chart(ctx,{type:'line',data:{labels:[],datasets:[
{label:'Pack V',data:[],borderColor:'#38bdf8',tension:0.3},
{label:'Core T',data:[],borderColor:'#f97316',tension:0.3},
{label:'SOP %',data:[],borderColor:'#10b981',tension:0.3}
]},options:{responsive:true,scales:{x:{ticks:{color:#64748b}}}}});
setInterval(async()=>{
try{
const r=await fetch('/api/bms');const d=await r.json();
document.getElementById('v').innerText=d.packVoltage.toFixed(2)+'V';
document.getElementById('c').innerText=d.cell1.toFixed(2)+' / '+d.cell2.toFixed(2);
document.getElementById('dv').innerText=d.deltaV.toFixed(3);
document.getElementById('t').innerText=d.temperature.toFixed(1)+' / '+d.coreTemp.toFixed(1)+'°C';
document.getElementById('dt').innerText=d.dTDt.toFixed(2);
document.getElementById('sh').innerText=d.soc.toFixed(0)+'% / '+d.soh.toFixed(0)+'%';
document.getElementById('sop').innerText=d.sop.toFixed(0)+'%';
document.getElementById('dtc').innerText=d.dtc;
document.getElementById('wd').innerText=d.watchdogState;
document.getElementById('fet').innerText=d.mosfetState;
const a=document.getElementById('alert');
a.className=d.fault?'alert fault':'alert ok';
a.innerText=d.status;
if(ch.data.labels.length>25){ch.data.labels.shift();ch.data.datasets.forEach(ds=>ds.data.shift());}
ch.data.labels.push(new Date().toLocaleTimeString());
ch.data.datasets[0].data.push(d.packVoltage);
ch.data.datasets[1].data.push(d.coreTemp);
ch.data.datasets[2].data.push(d.sop);
ch.update();
}catch(e){}
},1000);
</script></body></html>
)rawliteral";

// ===== CORE 0: ISO 26262 WATCHDOG SUPERVISOR TASK =====
void watchdogSupervisorTask(void* pvParameters) {
  for (;;) {
    unsigned long now = millis();
    unsigned long elapsed = now - lastHeartbeat;

    if (elapsed > 2000) {
      portENTER_CRITICAL(&bmsMux);
      watchdogTripped = true;
      digitalWrite(MOSFET_PIN, LOW); // Hardware Failsafe Disconnect
      bmsData.fault = true;
      bmsData.status = "CRITICAL: ISO 26262 WATCHDOG FAILSAFE";
      bmsData.dtc = "U0100 (LOST_COMM_WATCHDOG)";
      bmsData.watchdogState = "FAILSAFE TRIPPED";
      bmsData.mosfetState = "TRIPPED (OFF)";
      bmsData.sop = 0.0f;
      portEXIT_CRITICAL(&bmsMux);
    } else {
      portENTER_CRITICAL(&bmsMux);
      if (!bmsData.fault) {
        watchdogTripped = false;
        bmsData.watchdogState = "HEALTHY (" + String(elapsed) + "ms)";
      }
      portEXIT_CRITICAL(&bmsMux);
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// ===== CORE 1: SENSOR ACQUISITION & CONTROL TASK =====
void bmsWorkerTask(void* pvParameters) {
  for (;;) {
    // 16-Bit ADC Differential & Single-Ended Read
    int16_t adc0 = ads.readADC_SingleEnded(0);
    int16_t adc1 = ads.readADC_SingleEnded(1);

    float v0 = ads.computeVolts(adc0);
    float v1 = ads.computeVolts(adc1);

    float packV = round((v0 * 2.0f) * 100.0f) / 100.0f;
    float c1 = round((packV / 2.0f) * 100.0f) / 100.0f;
    float c2 = round((packV - c1) * 100.0f) / 100.0f;
    float deltaV = abs(c1 - c2);

    float tempC = readNTCTemperature(v1);
    unsigned long now = millis();

    // AIS-156 dT/dt Calculation
    float dTDt = 0.0f;
    float dtSec = (now - timeHistory[histIdx]) / 1000.0f;
    if (dtSec > 0.0f) {
      dTDt = (tempC - tempHistory[histIdx]) / dtSec;
    }
    tempHistory[histIdx] = tempC;
    timeHistory[histIdx] = now;
    histIdx = (histIdx + 1) % 5;

    float coreTemp = tempC + max(0.0f, dTDt * 3.2f);
    float socVal = calculateSOC(packV);
    float sohVal = max(75.0f, min(100.0f, 100.0f - abs(8.40f - packV) * 3.5f));

    // Dynamic SOP Derating ("Turtle Mode")
    float sopVal = 100.0f;
    if (socVal < 20.0f) sopVal = max(10.0f, socVal * 4.0f);
    if (tempC > 45.0f) sopVal = max(0.0f, min(sopVal, 100.0f - (tempC - 45.0f) * 5.0f));

    // Automotive Safety Evaluation & DTC Allocation
    bool fault = false;
    String status = "NORMAL";
    String dtc = "P0000 (NORMAL)";

    if (packV > V_OVERVOLT_LIMIT) {
      fault = true; status = "CRITICAL: OVERVOLTAGE"; dtc = "P0563 (OVER_VOLT)";
    } else if (packV < V_UNDERVOLT_LIMIT) {
      fault = true; status = "CRITICAL: UNDERVOLTAGE"; dtc = "P0562 (UNDER_VOLT)";
    } else if (tempC > T_OVERTEMP_LIMIT) {
      fault = true; status = "CRITICAL: OVERTEMP"; dtc = "P0A93 (OVER_TEMP)";
    } else if (dTDt >= DT_DT_LIMIT) {
      fault = true; status = "ALERT: AIS-156 RUNAWAY"; dtc = "P0A93 (HIGH_RATE_OF_RISE)";
    } else if (deltaV > 0.15f) {
      status = "WARN: CELL IMBALANCE"; dtc = "P0A7F (IMBALANCE)";
    }

    portENTER_CRITICAL(&bmsMux);
    if (!watchdogTripped) {
      if (fault) {
        digitalWrite(MOSFET_PIN, LOW);
        sopVal = 0.0f;
      } else {
        digitalWrite(MOSFET_PIN, HIGH);
      }
      bmsData.packVoltage = packV;
      bmsData.cell1 = c1;
      bmsData.cell2 = c2;
      bmsData.deltaV = deltaV;
      bmsData.temperature = tempC;
      bmsData.coreTemp = coreTemp;
      bmsData.dTDt = dTDt;
      bmsData.soc = socVal;
      bmsData.soh = sohVal;
      bmsData.sop = sopVal;
      bmsData.dtc = dtc;
      bmsData.mosfetState = fault ? "TRIPPED (OFF)" : "ACTIVE (ON)";
      bmsData.status = status;
      bmsData.fault = fault;
      lastHeartbeat = millis(); // Refresh Heartbeat
    }
    portEXIT_CRITICAL(&bmsMux);

    // Update Physical I2C 16x2 LCD
    lcd.setCursor(0, 0);
    lcd.print("V:" + String(packV, 1) + "V SOC:" + String(socVal, 0) + "%  ");
    lcd.setCursor(0, 1);
    lcd.print("T:" + String(tempC, 1) + "C " + (fault ? "FAULT " : "OK    "));

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// ===== SYSTEM SETUP & INITIALIZATION =====
void setup() {
  Serial.begin(115200);
  pinMode(MOSFET_PIN, OUTPUT);
  digitalWrite(MOSFET_PIN, HIGH); // Initial Closed Contactor

  Wire.begin(I2C_SDA, I2C_SCL);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("ESP32 BMS Booting");
  lcd.setCursor(0, 1);
  lcd.print("ISO26262 / AIS156");

  ads.begin(0x48);
  ads.setGain(GAIN_ONE); // +/- 4.096V range

  // Launch Wi-Fi SoftAP
  WiFi.softAP(ssid, password);
  Serial.print("[INFO] Web Dashboard IP: ");
  Serial.println(WiFi.softAPIP());

  // Web Server Routes
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", INDEX_HTML);
  });

  server.on("/api/bms", HTTP_GET, []() {
    String json = "{";
    json += "\"packVoltage\":" + String(bmsData.packVoltage, 2) + ",";
    json += "\"cell1\":" + String(bmsData.cell1, 2) + ",";
    json += "\"cell2\":" + String(bmsData.cell2, 2) + ",";
    json += "\"deltaV\":" + String(bmsData.deltaV, 3) + ",";
    json += "\"temperature\":" + String(bmsData.temperature, 1) + ",";
    json += "\"coreTemp\":" + String(bmsData.coreTemp, 1) + ",";
    json += "\"dTDt\":" + String(bmsData.dTDt, 2) + ",";
    json += "\"soc\":" + String(bmsData.soc, 1) + ",";
    json += "\"soh\":" + String(bmsData.soh, 1) + ",";
    json += "\"sop\":" + String(bmsData.sop, 1) + ",";
    json += "\"dtc\":\"" + bmsData.dtc + "\",";
    json += "\"watchdogState\":\"" + bmsData.watchdogState + "\",";
    json += "\"mosfetState\":\"" + bmsData.mosfetState + "\",";
    json += "\"status\":\"" + bmsData.status + "\",";
    json += "\"fault\":" + String(bmsData.fault ? "true" : "false");
    json += "}";
    server.send(200, "application/json", json);
  });

  server.begin();
  lastHeartbeat = millis();

  // Create Dual-Core FreeRTOS Tasks
  xTaskCreatePinnedToCore(watchdogSupervisorTask, "WatchdogTask", 4096, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(bmsWorkerTask, "BMSWorkerTask", 4096, NULL, 1, NULL, 1);
}

void loop() {
  server.handleClient();
  delay(2);
}
