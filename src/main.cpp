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
#define OLED_SDA 19
#define OLED_SCL 20
#define RGB_PIN  48
#define OUT_PIN  2       // ← 新增：高低电平输出
#define OUT_PIN_MS 3    // 毫秒模式用 GPIO3，几乎不冲突、安全脚

// ==================== OLED ====================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ==================== WS2812 ====================
#define NUMPIXELS 1
Adafruit_NeoPixel pixels(NUMPIXELS, RGB_PIN, NEO_GRB + NEO_KHZ800);

// ==================== 4x4 KEYPAD 引脚 ====================
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {10,11,12,13};
byte colPins[COLS] = {4,5,6,7};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ==================== WIFI 设置 ====================
const char* WIFI_SSID = "ESP32-GotoLab";
const char* WIFI_PASS = "12345678";
WebServer server(80);

// ==================== 参数 ====================
unsigned long highTime;
unsigned long lowTime;
int repeatCount;

const unsigned long MIN_VAL = 0;
const unsigned long MAX_VAL = 9999;

// ==================== 运行状态 ====================
bool isRunning = false;
unsigned long currentSecond = 0;
int currentRep = 0;
int totalRep   = 1;

enum State { SYS_IDLE, HIGH_DELAY, LOW_DELAY };
State currentState = SYS_IDLE;
unsigned long lastTick = 0;

// ==================== 长按参数 ====================
const unsigned long HOLD_DELAY = 400;
const unsigned long FAST_RATE  = 60;

// ==================== 按键状态 ====================
char          curKey     = 0;
char          lastKey    = 0;
bool          holdActive = false;
unsigned long pressStart = 0;
unsigned long lastRepeat = 0;

// ==================== 断电记忆 ====================
Preferences prefs;

// ==================== 模式选择 ====================
bool isMsMode = false;  // false=秒模式, true=毫秒模式
unsigned long highTime_s, lowTime_s, highTime_ms, lowTime_ms;
int repeatCount_s, repeatCount_ms;

// ==================== 函数声明 ====================
void loadData();
void saveData();
void updateScreen();
void setRGB(uint8_t r, uint8_t g, uint8_t b);
void setOutput(bool high);   // ← 新增
void start();
void stop();
void timer();
void modify(char k);
char scanKey();

void startWiFi();
void handleRoot();
void handleSet();
void handleStart();
void handleStop();
void handleModeA();
void handleModeB();

void switchToSecondMode();
void switchToMsMode();

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
void setup() {
  Serial.begin(115200);
  Wire.begin(OLED_SDA, OLED_SCL);
  loadData();

  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  pixels.begin();
  setRGB(0, 255, 0);

  // 输出引脚初始化，默认低电平
  pinMode(OUT_PIN, OUTPUT);
  digitalWrite(OUT_PIN, LOW);
  pinMode(OUT_PIN_MS, OUTPUT);
  digitalWrite(OUT_PIN_MS, LOW);

  for (byte r = 0; r < ROWS; r++) {
    pinMode(rowPins[r], OUTPUT);
    digitalWrite(rowPins[r], HIGH);
  }
  for (byte c = 0; c < COLS; c++) {
    pinMode(colPins[c], INPUT_PULLUP);
  }

  startWiFi();
  updateScreen();
}

// ==============================================
void loop() {
  server.handleClient();
  curKey = scanKey();

  // ── 运行中：只响应"0"停止 ──
  if (isRunning) {
    if (curKey == '0' && lastKey != '0') {
      stop();
      lastKey = curKey;
      return;
    }
    lastKey = curKey;
    timer();
    delay(10);
    return;
  }

  // ── 待机中：短按 + 长按 ──
  unsigned long now = millis();

  if (curKey != lastKey) {
    if (curKey != 0) {
      pressStart = now;
      lastRepeat = now;
      holdActive = false;

      modify(curKey);
      saveData();
      updateScreen();

      if (curKey == '*') { lastKey = curKey; start(); return; }
      if (curKey == '0') { lastKey = curKey; stop();  return; }
    }
    lastKey = curKey;
  }

  if (curKey != 0) {
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

  delay(10);
}

// ==============================================
void modify(char k) {
  switch (k) {
    case '1': highTime    = constrain(highTime    - 1, MIN_VAL, MAX_VAL); break;
    case '2': highTime    = constrain(highTime    + 1, MIN_VAL, MAX_VAL); break;
    case '4': lowTime     = constrain(lowTime     - 1, MIN_VAL, MAX_VAL); break;
    case '5': lowTime     = constrain(lowTime     + 1, MIN_VAL, MAX_VAL); break;
    case '7': repeatCount = constrain(repeatCount - 1, 1,       9999);    break;
    case '8': repeatCount = constrain(repeatCount + 1, 1,       9999);    break;
    case 'A': switchToMsMode();  break;
    case 'B': switchToSecondMode(); break;
  }
}

// ==============================================
void setOutput(bool high) {
  if (isMsMode) {
    digitalWrite(OUT_PIN_MS, high ? HIGH : LOW);
  } else {
    digitalWrite(OUT_PIN, high ? HIGH : LOW);
  }
}

// ==============================================
void updateScreen() {
  display.clearDisplay();

  const char* unit = isMsMode ? "ms" : " s";

  display.setCursor(0,  0);
  display.print("High: "); display.print(highTime); display.print(unit); display.print(" | ");
  display.print(currentState == HIGH_DELAY ? currentSecond : 0); display.print(unit);

  display.setCursor(0, 16);
  display.print("Low: ");  display.print(lowTime);  display.print(unit); display.print(" | ");
  display.print(currentState == LOW_DELAY ? currentSecond : 0); display.print(unit);

  display.setCursor(0, 32);
  display.print("Rep: "); display.print(repeatCount); display.print(" | ");
  display.print(isRunning ? (totalRep - currentRep + 1) : 0);

  display.setCursor(0, 48);
  display.print(isRunning ? "Running" : "Standing by ...");

  display.display();
}

// ==============================================
void setRGB(uint8_t r, uint8_t g, uint8_t b) {
  pixels.setPixelColor(0, pixels.Color(r, g, b));
  pixels.setBrightness(4);
  pixels.show();
}

void start() {
  isRunning     = true;
  currentState  = HIGH_DELAY;
  currentSecond = highTime;
  currentRep    = 1;
  totalRep      = repeatCount;
  lastTick      = millis();
  setRGB(255, 0, 0);
  setOutput(true);         // ← HIGH 阶段开始，输出高
  updateScreen();
}

void stop() {
  isRunning     = false;
  currentState  = SYS_IDLE;
  holdActive    = false;
  curKey        = 0;
  lastKey       = 0;
  currentSecond = 0;
  currentRep    = 0;
  setRGB(0, 255, 0);
  setOutput(false);        // ← 停止，输出低
  updateScreen();
}

void timer() {
  if (millis() - lastTick >= (isMsMode ? 1 : 1000)) {  // <-- 只改这行
    lastTick = millis();
    currentSecond--;

    if (currentSecond <= 0) {
      if (currentState == HIGH_DELAY) {
        currentState  = LOW_DELAY;
        currentSecond = lowTime;
        setRGB(0, 0, 255);
        setOutput(false);
      } else {
        currentRep++;
        if (currentRep > totalRep) { stop(); return; }
        currentState  = HIGH_DELAY;
        currentSecond = highTime;
        setRGB(255, 0, 0);
        setOutput(true);
      }
    }
    updateScreen();
  }
}

void loadData() {
  prefs.begin("timer", true);
  isMsMode = prefs.getBool("mode", false);
  
  highTime_s = prefs.getULong("h_s", 50);
  lowTime_s  = prefs.getULong("l_s", 100);
  repeatCount_s = prefs.getInt("r_s", 1);
  
  highTime_ms = prefs.getULong("h_m", 500);
  lowTime_ms  = prefs.getULong("l_m", 1000);
  repeatCount_ms = prefs.getInt("r_m", 1);
  
  if (isMsMode) {
    highTime = highTime_ms;
    lowTime = lowTime_ms;
    repeatCount = repeatCount_ms;
  } else {
    highTime = highTime_s;
    lowTime = lowTime_s;
    repeatCount = repeatCount_s;
  }
  prefs.end();
}

void saveData() {
  prefs.begin("timer", false);
  prefs.putBool("mode", isMsMode);
  
  if (isMsMode) {
    highTime_ms = highTime;
    lowTime_ms = lowTime;
    repeatCount_ms = repeatCount;
  } else {
    highTime_s = highTime;
    lowTime_s = lowTime;
    repeatCount_s = repeatCount;
  }
  
  prefs.putULong("h_s", highTime_s);
  prefs.putULong("l_s", lowTime_s);
  prefs.putInt("r_s", repeatCount_s);
  
  prefs.putULong("h_m", highTime_ms);
  prefs.putInt("l_m", lowTime_ms);
  prefs.putInt("r_m", repeatCount_ms);
  prefs.end();
}

void switchToSecondMode() {
  if (isMsMode == false) return;
  saveData();
  isMsMode = false;
  highTime = highTime_s;
  lowTime = lowTime_s;
  repeatCount = repeatCount_s;
  saveData();
  updateScreen();
}

void switchToMsMode() {
  if (isMsMode == true) return;
  saveData();
  isMsMode = true;
  highTime = highTime_ms;
  lowTime = lowTime_ms;
  repeatCount = repeatCount_ms;
  saveData();
  updateScreen();
}

// ==============================================
// WIFI 网页控制
// ==============================================
void startWiFi() {
  WiFi.softAP(WIFI_SSID, WIFI_PASS);
  server.on("/", handleRoot);
  server.on("/set", handleSet);
  server.on("/start", handleStart);
  server.on("/stop", handleStop);
  server.on("/modeA", handleModeA);
  server.on("/modeB", handleModeB);
  server.begin();
}

void handleRoot() {
  const char* unit = isMsMode ? "ms" : "s";
  String html = "<html><head><meta charset='utf-8'><title>ESP32 Timer</title></head>";
  html += "<body style='font-size:22px;text-align:center;margin-top:30px'>";
  html += "<h3>ESP32 波形发生器 (" + String(unit) + ")</h3>";
  html += "<form action='/set'>";
  html += "High: <input type='number' name='h' value='"+String(highTime)+"' max='9999'> "+String(unit)+"<br><br>";
  html += "Low: <input type='number' name='l' value='"+String(lowTime)+"' max='9999'> "+String(unit)+"<br><br>";
  html += "Repeat: <input type='number' name='r' value='"+String(repeatCount)+"' min='1' max='9999'><br><br>";
  html += "<input type='submit' value='保存设置' style='width:160px;height:40px'><br><br>";
  html += "</form>";
  html += "<button onclick='window.location.href=\"/start\"' style='width:160px;height:40px'>启动</button><br><br>";
  html += "<button onclick='window.location.href=\"/stop\"' style='width:160px;height:40px'>停止</button>";
  html += "<br><br>";
  html += "<button onclick='window.location.href=\"/modeA\"' style='width:160px;height:40px'>A(ms)</button>";
  html += "<button onclick='window.location.href=\"/modeB\"' style='width:160px;height:40px'>B(s)</button>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleSet() {
  if (server.hasArg("h")) highTime = server.arg("h").toInt();
  if (server.hasArg("l")) lowTime = server.arg("l").toInt();
  if (server.hasArg("r")) repeatCount = server.arg("r").toInt();
  highTime = constrain(highTime, 0,9999);
  lowTime = constrain(lowTime,0,9999);
  repeatCount = constrain(repeatCount,1,9999);

  // 保存到当前模式（秒/毫秒 自动对应）
  if (isMsMode) {
    highTime_ms = highTime;
    lowTime_ms = lowTime;
    repeatCount_ms = repeatCount;
  } else {
    highTime_s = highTime;
    lowTime_s = lowTime;
    repeatCount_s = repeatCount;
  }

  saveData();
  updateScreen();
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleStart() {
  start();
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleStop() {
  stop();
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleModeA() {
  switchToMsMode();
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleModeB() {
  switchToSecondMode();
  server.sendHeader("Location", "/");
  server.send(303);
}