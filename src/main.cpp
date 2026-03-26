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
const char* WIFI_SSID = "ESP32-Timer";
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
  }
}

// ==============================================
void setOutput(bool high) {
  digitalWrite(OUT_PIN, high ? HIGH : LOW);
}

// ==============================================
void updateScreen() {
  display.clearDisplay();

  display.setCursor(0,  0);
  display.print("High: "); display.print(highTime); display.print(" s | ");
  display.print(currentState == HIGH_DELAY ? currentSecond : 0); display.print(" s");

  display.setCursor(0, 16);
  display.print("Low: ");  display.print(lowTime);  display.print(" s | ");
  display.print(currentState == LOW_DELAY ? currentSecond : 0); display.print(" s");

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
  if (millis() - lastTick >= 1000) {
    lastTick = millis();
    currentSecond--;

    if (currentSecond <= 0) {
      if (currentState == HIGH_DELAY) {
        currentState  = LOW_DELAY;
        currentSecond = lowTime;
        setRGB(0, 0, 255);
        setOutput(false);  // ← 切换到 LOW 阶段，输出低
      } else {
        currentRep++;
        if (currentRep > totalRep) { stop(); return; }
        currentState  = HIGH_DELAY;
        currentSecond = highTime;
        setRGB(255, 0, 0);
        setOutput(true);   // ← 切换到 HIGH 阶段，输出高
      }
    }
    updateScreen();
  }
}

void loadData() {
  prefs.begin("timer", true);
  highTime    = prefs.getULong("h", 50);
  lowTime     = prefs.getULong("l", 100);
  repeatCount = prefs.getInt("r",   1);
  prefs.end();
}

void saveData() {
  prefs.begin("timer", false);
  prefs.putULong("h", highTime);
  prefs.putULong("l", lowTime);
  prefs.putInt("r",   repeatCount);
  prefs.end();
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
  server.begin();
}

void handleRoot() {
  String html = "<html><head><meta charset='utf-8'><title>ESP32 Timer</title></head>";
  html += "<body style='font-size:22px;text-align:center;margin-top:30px'>";
  html += "<h3>ESP32 倒计时控制器</h3>";
  html += "<form action='/set'>";
  html += "High: <input type='number' name='h' value='"+String(highTime)+"' max='9999'><br><br>";
  html += "Low: <input type='number' name='l' value='"+String(lowTime)+"' max='9999'><br><br>";
  html += "Repeat: <input type='number' name='r' value='"+String(repeatCount)+"' min='1' max='9999'><br><br>";
  html += "<input type='submit' value='保存设置' style='width:160px;height:40px'><br><br>";
  html += "</form>";
  html += "<button onclick='window.location.href=\"/start\"' style='width:160px;height:40px'>启动</button><br><br>";
  html += "<button onclick='window.location.href=\"/stop\"' style='width:160px;height:40px'>停止</button>";
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