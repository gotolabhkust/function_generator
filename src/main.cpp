#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>
#include <Keypad.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>

// ==================== 引脚 ====================
#define OLED_SDA    19
#define OLED_SCL    20
#define RGB_PIN     48
#define OUT_PIN     2
#define OUT_PIN_MS  3

// ==================== OLED ====================
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ==================== WS2812 ====================
#define NUMPIXELS 1
Adafruit_NeoPixel pixels(NUMPIXELS, RGB_PIN, NEO_GRB + NEO_KHZ800);

// ==================== 4x4 键盘 ====================
const byte ROWS = 4, COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {10,11,12,13};
byte colPins[COLS]  = {4,5,6,7};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ==================== WiFi ====================
const char* WIFI_SSID = "ESP32-GotoLab";
const char* WIFI_PASS = "12345678";
WebServer server(80);

// ==================== 参数（两套独立） ====================
unsigned long highTime, lowTime;
int repeatCount;

unsigned long highTime_s,  lowTime_s;   int repeatCount_s;
unsigned long highTime_ms, lowTime_ms;  int repeatCount_ms;

const unsigned long MIN_VAL = 0;
const unsigned long MAX_VAL = 9999;

bool isMsMode = false;

// ==================== 运行状态（volatile，双核共享） ====================
volatile bool     isRunning   = false;
volatile bool     outputHigh  = false;
volatile int      repCount    = 0;
volatile int      g_repeat    = 1;

volatile uint32_t g_highMs    = 500;
volatile uint32_t g_lowMs     = 500;
volatile uint32_t tickCount   = 0;
volatile uint32_t tickTarget  = 0;
volatile uint32_t dbg_remainMs = 0;

// 中断通知 UI 需要更新 RGB 灯和屏幕
// 0=无事, 1=变HIGH, 2=变LOW, 3=完成
volatile uint8_t rgbEvent = 0;

// ==================== 定时器就绪标志 ====================
volatile bool waveTimerReady = false;
hw_timer_t *waveTimer = NULL;

// ==================== 长按参数 ====================
const unsigned long HOLD_DELAY = 400;
const unsigned long FAST_RATE  = 60;

char          curKey     = 0;
char          lastKey    = 0;
bool          holdActive = false;
unsigned long pressStart = 0;
unsigned long lastRepeat = 0;

// ==================== Flash 存储 ====================
Preferences prefs;

// ==================== 函数声明 ====================
void loadData(); void saveData();
void updateScreen(); void forceUpdateScreen();
void setRGB(uint8_t r, uint8_t g, uint8_t b);
void startWave(); void stopWave();
void modify(char k);
char scanKey();
void switchToSecondMode(); void switchToMsMode();
void startWiFi();
void handleRoot(); void handleSet();
void handleStart(); void handleStop();
void handleModeA(); void handleModeB();

// ==============================================
// 核心1：定时器中断，固定每 1ms 触发一次
// ==============================================
void IRAM_ATTR onWaveTimer() {
  if (!isRunning) return;

  tickCount++;
  dbg_remainMs = (tickTarget > tickCount) ? (tickTarget - tickCount) : 0;

  if (tickCount < tickTarget) return;

  if (outputHigh) {
    // 高电平结束 → 切低电平
    if (isMsMode) digitalWrite(OUT_PIN_MS, LOW);
    else          digitalWrite(OUT_PIN,    LOW);
    outputHigh   = false;
    tickCount    = 0;
    tickTarget   = g_lowMs;
    dbg_remainMs = g_lowMs;
    rgbEvent     = 2;  // 通知UI变蓝

  } else {
    // 低电平结束 → 检查 repeat
    repCount++;
    if (repCount >= g_repeat) {
      // 全部完成
      if (isMsMode) digitalWrite(OUT_PIN_MS, LOW);
      else          digitalWrite(OUT_PIN,    LOW);
      isRunning    = false;
      outputHigh   = false;
      tickCount    = 0;
      tickTarget   = 0;
      dbg_remainMs = 0;
      rgbEvent     = 3;  // 通知UI变绿+刷屏
      return;
    }
    // 继续下一个高电平
    if (isMsMode) digitalWrite(OUT_PIN_MS, HIGH);
    else          digitalWrite(OUT_PIN,    HIGH);
    outputHigh   = true;
    tickCount    = 0;
    tickTarget   = g_highMs;
    dbg_remainMs = g_highMs;
    rgbEvent     = 1;  // 通知UI变红
  }
}

// ==============================================
// 核心1 任务
// ==============================================
void wave_task(void *arg) {
  waveTimer = timerBegin(1, 80, true);
  timerAttachInterrupt(waveTimer, &onWaveTimer, true);
  timerAlarmWrite(waveTimer, 1000, true);  // 固定 1ms auto-reload
  waveTimerReady = true;
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ==============================================
// 核心0 任务：全部 UI 逻辑
// ==============================================
void ui_task(void *arg) {
  Wire.begin(OLED_SDA, OLED_SCL);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  pixels.begin();
  setRGB(0, 255, 0);  // 启动时绿色

  pinMode(OUT_PIN,    OUTPUT); digitalWrite(OUT_PIN,    LOW);
  pinMode(OUT_PIN_MS, OUTPUT); digitalWrite(OUT_PIN_MS, LOW);

  for (byte r = 0; r < ROWS; r++) {
    pinMode(rowPins[r], OUTPUT);
    digitalWrite(rowPins[r], HIGH);
  }
  for (byte c = 0; c < COLS; c++) pinMode(colPins[c], INPUT_PULLUP);

  loadData();
  startWiFi();
  updateScreen();

  while (1) {
    server.handleClient();
    curKey = scanKey();

    // ---- 处理中断发来的 RGB 事件 ----
    if (rgbEvent != 0) {
      uint8_t ev = rgbEvent;
      rgbEvent = 0;
      if (!isMsMode) {
        if      (ev == 1) setRGB(255, 0, 0);   // HIGH → 红
        else if (ev == 2) setRGB(0, 0, 255);   // LOW  → 蓝
        else if (ev == 3) setRGB(0, 255, 0);   // 完成 → 绿
      }
      if (ev == 3) {
        // 完成时强制刷屏，不受节流限制
        forceUpdateScreen();
        lastKey = 0; curKey = 0;
        vTaskDelay(pdMS_TO_TICKS(50));
        continue;
      }
    }

    // ---- 运行中：只允许按 0 停止 ----
    if (isRunning) {
      if (curKey == '0' && lastKey != '0') {
        stopWave();
      }
      lastKey = curKey;
      updateScreen();
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    // ---- 空闲：完整按键处理 ----
    unsigned long now = millis();

    if (curKey != lastKey) {
      if (curKey != 0) {
        pressStart = now;
        lastRepeat = now;
        holdActive = false;

        if (curKey == '*') {
          lastKey = curKey;
          startWave();
          continue;
        }
        if (curKey == '0') {
          lastKey = curKey;
          stopWave();
          continue;
        }

        modify(curKey);
        saveData();
        updateScreen();
      }
      lastKey = curKey;
    }

    // 长按快速调值
    if (curKey != 0 && curKey != '*' && curKey != '0') {
      if (!holdActive && now - pressStart >= HOLD_DELAY) {
        holdActive = true;
        lastRepeat = now;
      }
      if (holdActive && now - lastRepeat >= FAST_RATE) {
        modify(curKey);
        saveData();
        updateScreen();
        lastRepeat = now;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ==============================================
void setup() {
  Serial.begin(115200);
  xTaskCreatePinnedToCore(wave_task, "wave", 2048, NULL, 10, NULL, 1);
  xTaskCreatePinnedToCore(ui_task,   "ui",   8192, NULL,  5, NULL, 0);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}

// ==============================================
void startWave() {
  if (isRunning)       return;
  if (!waveTimerReady) return;

  if (isMsMode) {
    g_highMs = (uint32_t)highTime;
    g_lowMs  = (uint32_t)lowTime;
  } else {
    g_highMs = (uint32_t)highTime * 1000UL;
    g_lowMs  = (uint32_t)lowTime  * 1000UL;
  }

  g_repeat     = repeatCount;
  repCount     = 0;
  tickCount    = 0;
  tickTarget   = g_highMs;
  dbg_remainMs = g_highMs;
  outputHigh   = true;
  rgbEvent     = 0;

  if (isMsMode) digitalWrite(OUT_PIN_MS, HIGH);
  else          digitalWrite(OUT_PIN,    HIGH);

  if (!isMsMode) setRGB(255, 0, 0);  // 启动时红色

  isRunning = true;
  timerAlarmEnable(waveTimer);
  updateScreen();
}

// ==============================================
void stopWave() {
  timerAlarmDisable(waveTimer);
  isRunning    = false;
  outputHigh   = false;
  repCount     = 0;
  tickCount    = 0;
  tickTarget   = 0;
  dbg_remainMs = 0;
  rgbEvent     = 0;
  holdActive   = false;
  curKey  = 0;
  lastKey = 0;

  digitalWrite(OUT_PIN,    LOW);
  digitalWrite(OUT_PIN_MS, LOW);

  if (!isMsMode) setRGB(0, 255, 0);  // 停止时绿色
  forceUpdateScreen();
}

// ==============================================
char scanKey() {
  for (byte r = 0; r < ROWS; r++) {
    for (byte i = 0; i < ROWS; i++) digitalWrite(rowPins[i], HIGH);
    digitalWrite(rowPins[r], LOW);
    delayMicroseconds(10);
    for (byte c = 0; c < COLS; c++) {
      if (digitalRead(colPins[c]) == LOW) {
        for (byte i = 0; i < ROWS; i++) digitalWrite(rowPins[i], HIGH);
        return keys[r][c];
      }
    }
  }
  for (byte i = 0; i < ROWS; i++) digitalWrite(rowPins[i], HIGH);
  return 0;
}

// ==============================================
void modify(char k) {
  switch (k) {
    case '1': highTime    = constrain(highTime    - 1, MIN_VAL, MAX_VAL); break;
    case '2': highTime    = constrain(highTime    + 1, MIN_VAL, MAX_VAL); break;
    case '4': lowTime     = constrain(lowTime     - 1, MIN_VAL, MAX_VAL); break;
    case '5': lowTime     = constrain(lowTime     + 1, MIN_VAL, MAX_VAL); break;
    case '7': repeatCount = constrain(repeatCount - 1, 1, 9999);          break;
    case '8': repeatCount = constrain(repeatCount + 1, 1, 9999);          break;
    case 'A': switchToMsMode();     break;
    case 'B': switchToSecondMode(); break;
  }
}

// ==============================================
// 带节流的普通刷屏（运行中用）
void updateScreen() {
  static unsigned long lastScreen = 0;
  if (millis() - lastScreen < 100) return;
  lastScreen = millis();

  display.clearDisplay();
  const char* unit = isMsMode ? "ms" : "s";
  uint32_t rem = dbg_remainMs;

  display.setCursor(0, 0);
  display.print("High: "); display.print(highTime);
  display.print(" | ");
  if (isRunning && outputHigh)
    display.print(isMsMode ? rem : rem / 1000);
  else
    display.print(0);
  display.print(unit);

  display.setCursor(0, 16);
  display.print("Low: "); display.print(lowTime);
  display.print(" | ");
  if (isRunning && !outputHigh)
    display.print(isMsMode ? rem : rem / 1000);
  else
    display.print(0);
  display.print(unit);

  display.setCursor(0, 32);
  display.print("Repeat: "); display.print(repeatCount);
  display.print(" | ");
  display.print(isRunning ? (g_repeat - repCount) : 0);

  display.setCursor(0, 48);
  display.print(isRunning ? "Running ..." : "Standing by ...");

  display.display();
}

// 强制刷屏，不受节流限制（状态切换时用）
void forceUpdateScreen() {
  display.clearDisplay();
  const char* unit = isMsMode ? "ms" : "s";

  display.setCursor(0, 0);
  display.print("High:"); display.print(highTime);
  display.print(" | 0"); display.print(unit);

  display.setCursor(0, 16);
  display.print("Low: "); display.print(lowTime);
  display.print(" | 0"); display.print(unit);

  display.setCursor(0, 32);
  display.print("Rep: "); display.print(repeatCount);
  display.print(" | 0");

  display.setCursor(0, 48);
  display.print(isRunning ? "Running ..." : "Standing by ...");

  display.display();
}

// ==============================================
void setRGB(uint8_t r, uint8_t g, uint8_t b) {
  if (isMsMode) return;  // 毫秒模式不亮灯
  pixels.setPixelColor(0, pixels.Color(r, g, b));
  pixels.setBrightness(4);
  pixels.show();
}

// ==============================================
void loadData() {
  prefs.begin("timer", true);
  isMsMode       = prefs.getBool("mode",  true);
  highTime_s     = prefs.getULong("h_s",  60);
  lowTime_s      = prefs.getULong("l_s",  100);
  repeatCount_s  = prefs.getInt("r_s",    5);
  highTime_ms    = prefs.getULong("h_m",  1);
  lowTime_ms     = prefs.getULong("l_m",  19);
  repeatCount_ms = prefs.getInt("r_m",    150);
  prefs.end();

  if (isMsMode) {
    highTime = highTime_ms; lowTime = lowTime_ms; repeatCount = repeatCount_ms;
  } else {
    highTime = highTime_s;  lowTime = lowTime_s;  repeatCount = repeatCount_s;
  }
}

void saveData() {
  prefs.begin("timer", false);
  prefs.putBool("mode", isMsMode);
  if (isMsMode) {
    highTime_ms = highTime; lowTime_ms = lowTime; repeatCount_ms = repeatCount;
  } else {
    highTime_s  = highTime; lowTime_s  = lowTime; repeatCount_s  = repeatCount;
  }
  prefs.putULong("h_s", highTime_s);    prefs.putULong("l_s", lowTime_s);
  prefs.putInt("r_s",   repeatCount_s);
  prefs.putULong("h_m", highTime_ms);   prefs.putULong("l_m", lowTime_ms);
  prefs.putInt("r_m",   repeatCount_ms);
  prefs.end();
}

// ==============================================
void switchToSecondMode() {
  if (!isMsMode) return;
  saveData();
  isMsMode = false;
  highTime = highTime_s; lowTime = lowTime_s; repeatCount = repeatCount_s;
  saveData(); forceUpdateScreen();
}

void switchToMsMode() {
  if (isMsMode) return;
  saveData();
  isMsMode = true;
  highTime = highTime_ms; lowTime = lowTime_ms; repeatCount = repeatCount_ms;
  saveData(); forceUpdateScreen();
}

// ==============================================
// WiFi 网页控制
// ==============================================
void startWiFi() {
  WiFi.softAP(WIFI_SSID, WIFI_PASS);
  server.on("/",      handleRoot);
  server.on("/set",   handleSet);
  server.on("/start", handleStart);
  server.on("/stop",  handleStop);
  server.on("/modeA", handleModeA);
  server.on("/modeB", handleModeB);
  server.begin();
}

void handleRoot() {
  const char* unit = isMsMode ? "ms" : "s";
  String html = "<html><head><meta charset='utf-8'><title>ESP32 Timer</title></head>";
  html += "<body style='font-size:22px;text-align:center;margin-top:30px'>";
  html += "<h3>ESP32 波形发生器 (" + String(unit) + ")</h3>";
  html += "<p>状态: " + String(isRunning ? "运行中" : "待机") + "</p>";
  html += "<form action='/set'>";
  html += "High: <input type='number' name='h' value='"+String(highTime)+"' max='9999'> "+String(unit)+"<br><br>";
  html += "Low:  <input type='number' name='l' value='"+String(lowTime) +"' max='9999'> "+String(unit)+"<br><br>";
  html += "Repeat: <input type='number' name='r' value='"+String(repeatCount)+"' min='1' max='9999'><br><br>";
  html += "<input type='submit' value='保存设置' style='width:160px;height:40px'><br><br>";
  html += "</form>";
  html += "<button onclick='location.href=\"/start\"' style='width:160px;height:40px'>启动</button><br><br>";
  html += "<button onclick='location.href=\"/stop\"'  style='width:160px;height:40px'>停止</button><br><br>";
  html += "<button onclick='location.href=\"/modeA\"' style='width:160px;height:40px'>A(ms)</button> ";
  html += "<button onclick='location.href=\"/modeB\"' style='width:160px;height:40px'>B(s)</button>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleSet() {
  if (server.hasArg("h")) highTime    = constrain((unsigned long)server.arg("h").toInt(), 0, 9999);
  if (server.hasArg("l")) lowTime     = constrain((unsigned long)server.arg("l").toInt(), 0, 9999);
  if (server.hasArg("r")) repeatCount = constrain(server.arg("r").toInt(), 1, 9999);
  if (isMsMode) { highTime_ms = highTime; lowTime_ms = lowTime; repeatCount_ms = repeatCount; }
  else          { highTime_s  = highTime; lowTime_s  = lowTime; repeatCount_s  = repeatCount; }
  saveData(); forceUpdateScreen();
  server.sendHeader("Location", "/"); server.send(303);
}

void handleStart() { startWave(); server.sendHeader("Location", "/"); server.send(303); }
void handleStop()  { stopWave();  server.sendHeader("Location", "/"); server.send(303); }
void handleModeA() { switchToMsMode();     server.sendHeader("Location", "/"); server.send(303); }
void handleModeB() { switchToSecondMode(); server.sendHeader("Location", "/"); server.send(303); }