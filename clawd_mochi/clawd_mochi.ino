/*
 * ╔══════════════════════════════════════════════════════════════╗
 *   CLAWD MOCHI — ESP32-C3 Super Mini + ST7789 1.54" 240×240
 *
 *   Wiring:
 *     SDA → GPIO 10  (hardware SPI MOSI)
 *     SCL → GPIO 8   (hardware SPI SCK)
 *     RST → GPIO 2
 *     DC  → GPIO 1
 *     CS  → GPIO 4
 *     BL  → GPIO 3
 *     VCC → 3V3
 *     GND → GND
 *
 *   WiFi: "ClaWD-Mochi"  pw: clawd1234  → http://192.168.4.1
 * ╚══════════════════════════════════════════════════════════════╝
 */

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <math.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <time.h>
#include <Preferences.h>

// ── Pins ──────────────────────────────────────────────────────
#define TFT_CS  4
#define TFT_DC  1
#define TFT_RST 2
#define TFT_BLK 3

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// ── WiFi ──────────────────────────────────────────────────────
const char* AP_SSID = "ClaWD-Mochi";
const char* AP_PASS = "clawd1234";
WebServer server(80);

// ── WiFi Station (connect to home WiFi for MQTT) ──────────────
char STA_SSID[32] = "TP-LINK_6371";
char STA_PASS[32] = "yangdao412";
Preferences prefs;

void loadWiFiCfg() {
  prefs.begin("clawd-wifi", true);
  String s = prefs.getString("ssid", "");
  String p = prefs.getString("pass", "");
  prefs.end();
  if (s.length() > 0 && s.length() < 32) {
    strncpy(STA_SSID, s.c_str(), 31);
    STA_SSID[31] = '\0';
  }
  if (p.length() > 0 && p.length() < 32) {
    strncpy(STA_PASS, p.c_str(), 31);
    STA_PASS[31] = '\0';
  }
}

void saveWiFiCfg(const char* ssid, const char* pass) {
  prefs.begin("clawd-wifi", false);
  prefs.putString("ssid", String(ssid));
  prefs.putString("pass", String(pass));
  prefs.end();
}

// ── MQTT ──────────────────────────────────────────────────────
const char* MQTT_BROKER = "broker.hivemq.com";
const int   MQTT_PORT   = 1883;
const char* DEVICE_ID   = "clawd-001";
char        mqttTopic[64];  // 在 setup 中拼接为 "clawd/clawd-001/expression"

WiFiClient    wifiClient;
PubSubClient  mqtt(wifiClient);

// 表情状态
String       currentExpr  = "idle";
unsigned long exprEndTime = 0;
bool         exprLooping  = false;
bool         wifiStaConnected = false;

// 自动表情轮播
bool         autoExprActive = false;
unsigned long lastAutoExprTime = 0;
int          autoExprIndex = 0;
#define AUTO_EXPR_INTERVAL 60000   // 每 60 秒触发一次
#define AUTO_EXPR_DURATION 20000   // 每次显示 20 秒
const char* AUTO_EXPR_LIST[10] = {
  "happy", "sad", "wink", "blush", "awe",
  "thinking", "angry", "proud", "sleep", "idle"
};

// 用量数据（通过 MQTT usage topic 接收）
char usageTokens[32] = "--";
char usageCost[32]   = "--";
char usageModel[32]  = "--";
char usageTopic[64];  // "clawd/clawd-001/usage"

// ── Display ───────────────────────────────────────────────────
#define DISP_W 240
#define DISP_H 240

// ── Eye constants (shared by both eye views) ──────────────────
#define EYE_W   30
#define EYE_H   60
#define EYE_GAP 120
#define EYE_OX  0     // horizontal offset
#define EYE_OY  40    // vertical offset upward (subtracted from centre)

// ── Colours ───────────────────────────────────────────────────
uint16_t C_ORANGE, C_DARKBG, C_MUTED, C_GREEN, C_CLOCKBG, C_DARKTEXT;
#define C_WHITE ST77XX_WHITE
#define C_BLACK ST77XX_BLACK

// ── State ─────────────────────────────────────────────────────
#define VIEW_EYES_NORMAL 0
#define VIEW_EYES_SQUISH 1
#define VIEW_CODE        2
#define VIEW_DRAW        3

uint8_t  currentView  = VIEW_EYES_NORMAL;
bool     busy         = false;
bool     backlightOn  = true;
uint8_t  animSpeed    = 1;   // 1=slow(default) 2=normal 3=fast

uint16_t animBgColor  = 0;   // background for eye/logo animations
uint16_t drawBgColor  = 0;   // background for canvas
uint16_t currentWeekdayBg = 0;  // 当前星期背景色，供表情视图使用

// ── Terminal ──────────────────────────────────────────────────
#define TERM_COLS      15
#define TERM_ROWS       8
#define TERM_CHAR_W    12
#define TERM_CHAR_H    20
#define TERM_PAD_X      8
#define TERM_PAD_Y     18

bool    termMode    = false;
String  termLines[TERM_ROWS];
uint8_t termRow     = 0;
uint8_t termCol     = 0;

// ── Clock / Weather ──────────────────────────────────────────
#define VIEW_CLOCK 5

// NTP
const char* NTP_SERVER = "ntp.aliyun.com";
const long  GMT_OFFSET_SEC = 8 * 3600;
const int   DAYLIGHT_OFFSET_SEC = 0;

// Weather (Open-Meteo, no API key needed)
char   weatherCity[32] = "Guangzhou";
float  weatherLat = 23.1291;
float  weatherLon = 113.2644;
String weatherTemp = "--";
int    weatherCode = -1;
unsigned long lastWeatherFetch = 0;
bool   clockValid = false;
#define WEATHER_INTERVAL 1800000

// Idle timeout → return to clock
unsigned long lastUserAction = 0;
#define IDLE_TIMEOUT 30000
bool clockWasActive = false;  // true when user intentionally views clock

// ── Pixel digit bitmaps (3 wide × 5 tall, stored as 5 bytes per digit) ──
// Each byte: lower 3 bits = pixel row (MSB left)
static const uint8_t DIGIT[10][5] PROGMEM = {
  {0b111, 0b101, 0b101, 0b101, 0b111}, // 0
  {0b010, 0b110, 0b010, 0b010, 0b111}, // 1
  {0b111, 0b001, 0b111, 0b100, 0b111}, // 2
  {0b111, 0b001, 0b111, 0b001, 0b111}, // 3
  {0b101, 0b101, 0b111, 0b001, 0b001}, // 4
  {0b111, 0b100, 0b111, 0b001, 0b111}, // 5
  {0b111, 0b100, 0b111, 0b101, 0b111}, // 6
  {0b111, 0b001, 0b001, 0b001, 0b001}, // 7
  {0b111, 0b101, 0b111, 0b101, 0b111}, // 8
  {0b111, 0b101, 0b111, 0b001, 0b111}, // 9
};

#define PXD_SCALE 7       // pixel scale factor
#define PXD_W    (3 * PXD_SCALE)
#define PXD_H    (5 * PXD_SCALE)
#define PXD_GAP  4        // gap between digits

void drawPxDigit(uint8_t d, int16_t x, int16_t y, uint16_t col) {
  if (d > 9) return;
  for (uint8_t row = 0; row < 5; row++) {
    uint8_t bits = pgm_read_byte(&DIGIT[d][row]);
    for (uint8_t colBit = 0; colBit < 3; colBit++) {
      if (bits & (1 << (2 - colBit))) {
        tft.fillRect(x + colBit * PXD_SCALE, y + row * PXD_SCALE,
                     PXD_SCALE, PXD_SCALE, col);
      }
    }
  }
}

void drawPxNumber(int num, int16_t cx, int16_t y, uint16_t col, bool twoDigit) {
  if (twoDigit) {
    int8_t d1 = (num / 10) % 10;
    int8_t d0 = num % 10;
    int16_t totalW = PXD_W * 2 + PXD_GAP;
    int16_t startX = cx - totalW / 2;
    drawPxDigit(d1, startX, y, col);
    drawPxDigit(d0, startX + PXD_W + PXD_GAP, y, col);
  } else {
    int16_t startX = cx - PXD_W / 2;
    drawPxDigit(num % 10, startX, y, col);
  }
}

void drawPxColon(int16_t cx, int16_t y, uint16_t col) {
  tft.fillRect(cx - PXD_SCALE/2, y + PXD_SCALE,     PXD_SCALE, PXD_SCALE, col);
  tft.fillRect(cx - PXD_SCALE/2, y + PXD_SCALE * 3, PXD_SCALE, PXD_SCALE, col);
}

// ── Weather icon (simple pixel-art symbols) ───────────────────
void drawWeatherIcon(int code, int16_t x, int16_t y, uint16_t col) {
  // WMO codes → simple icons
  // 0=clear, 1-3=partly cloudy, 45-48=fog, 51-67=rain, 71-77=snow, 80-99=showers/thunderstorm
  if (code == 0) {
    // Sun: circle + rays
    tft.fillCircle(x + 12, y + 12, 8, col);
    for (int a = 0; a < 8; a++) {
      float rad = a * PI / 4;
      int16_t rx = x + 12 + cos(rad) * 12;
      int16_t ry = y + 12 + sin(rad) * 12;
      tft.drawLine(x + 12 + cos(rad) * 9, y + 12 + sin(rad) * 9, rx, ry, col);
    }
  } else if (code >= 1 && code <= 3) {
    // Partly cloudy: sun peeking behind cloud
    tft.fillCircle(x + 8, y + 8, 6, col);
    tft.fillRect(x + 2, y + 14, 22, 10, col);
    tft.fillRect(x + 0, y + 16, 26, 6, col);
    tft.fillRect(x + 2, y + 14, 22, 10, C_DARKBG); // cutout
    tft.fillRect(x + 2, y + 18, 22, 4, col);
  } else if (code >= 45 && code <= 48) {
    // Fog: horizontal lines
    for (int i = 0; i < 4; i++)
      tft.fillRect(x + 2, y + 5 + i * 6, 22, 3, col);
  } else if ((code >= 51 && code <= 67) || (code >= 80 && code <= 99)) {
    // Rain: cloud + drops
    tft.fillRect(x + 2, y + 2, 22, 8, col);
    tft.fillRect(x + 0, y + 4, 26, 4, col);
    for (int i = 0; i < 3; i++)
      tft.drawFastVLine(x + 5 + i * 8, y + 14, 8, col);
  } else if (code >= 71 && code <= 77) {
    // Snow: cloud + dots
    tft.fillRect(x + 2, y + 2, 22, 8, col);
    tft.fillRect(x + 0, y + 4, 26, 4, col);
    for (int i = 0; i < 3; i++)
      tft.fillCircle(x + 5 + i * 8, y + 18, 3, col);
  } else if (code >= 95) {
    // Thunderstorm: cloud + lightning
    tft.fillRect(x + 2, y + 2, 22, 8, col);
    tft.fillRect(x + 0, y + 4, 26, 4, col);
    int16_t cx2 = x + 14;
    tft.drawLine(cx2, y + 14, cx2 - 6, y + 22, col);
    tft.drawLine(cx2, y + 22, cx2 + 2, y + 18, col);
  } else {
    // Default: "?" mark
    tft.setCursor(x, y + 4); tft.setTextSize(2); tft.print("?");
  }
}

// ── Clock view ────────────────────────────────────────────────
// ── 星期背景色 ──────────────────────────────────────────────
uint16_t getWeekdayBg(int wday) {
  // 0=Sun 1=Mon ... 6=Sat，饱和度足够在 TFT 上肉眼区分
  switch (wday) {
    case 1:  return tft.color565(250, 170, 150);  // 周一 橙红
    case 2:  return tft.color565(160, 195, 240);  // 周二 蓝
    case 3:  return tft.color565(245, 180, 200);  // 周三 粉
    case 4:  return tft.color565(242, 215, 115);  // 周四 黄
    case 5:  return tft.color565(160, 225, 185);  // 周五 绿
    case 6:  return tft.color565(240, 238, 233);  // 周六 暖白
    case 0:  return tft.color565(200, 198, 193);  // 周日 暖灰
    default: return C_CLOCKBG;
  }
}

void drawClockView(bool forceRedraw = false) {
  static int lastMinute = -1;
  static int lastSecond = -1;

  struct tm ti;
  if (!getLocalTime(&ti)) {
    tft.fillScreen(C_CLOCKBG);
    tft.fillRect(0, 0, DISP_W, 3, C_ORANGE);
    tft.fillRect(0, DISP_H - 3, DISP_W, 3, C_ORANGE);
    tft.setTextColor(C_MUTED); tft.setTextSize(2);
    tft.setCursor(40, DISP_H / 2 - 10);
    tft.print("syncing...");
    clockValid = false;
    lastMinute = -1;
    return;
  }
  clockValid = true;
  uint16_t bg = getWeekdayBg(ti.tm_wday);  // 按星期几切换背景色

  // ── 布局参数 ───────────────────────────────────────────────
  int16_t timeY = 12;
  int16_t cx    = DISP_W / 2;
  int16_t barW  = 210;
  int16_t barX  = (DISP_W - barW) / 2;
  int16_t barY  = timeY + PXD_H + 14;
  int16_t barH  = 7;

  bool minuteChanged = (ti.tm_min != lastMinute);

  // ── 全量重绘（分钟变化 / 强制 / 首次） ─────────────────────
  if (forceRedraw || minuteChanged) {
    lastMinute = ti.tm_min;
    lastSecond = ti.tm_sec;

    tft.fillScreen(bg);
    currentWeekdayBg = bg;  // 同步给表情视图使用
    tft.fillRect(0, 0, DISP_W, 3, C_ORANGE);
    tft.fillRect(0, DISP_H - 3, DISP_W, 3, C_ORANGE);

    // 时间 HH:MM（像素数字，深色）
    int16_t hTotalW = PXD_W * 2 + PXD_GAP;
    int16_t hStartX = cx - hTotalW - PXD_GAP * 2;
    drawPxDigit((ti.tm_hour / 10) % 10, hStartX, timeY, C_DARKTEXT);
    drawPxDigit(ti.tm_hour % 10, hStartX + PXD_W + PXD_GAP, timeY, C_DARKTEXT);
    drawPxColon(cx, timeY, C_ORANGE);
    int16_t mStartX = cx + PXD_GAP * 2;
    drawPxDigit((ti.tm_min / 10) % 10, mStartX, timeY, C_DARKTEXT);
    drawPxDigit(ti.tm_min % 10, mStartX + PXD_W + PXD_GAP, timeY, C_DARKTEXT);

    // 秒进度条外框
    tft.drawRect(barX, barY, barW, barH, C_MUTED);

    // 日期（更大字号）
    tft.setTextColor(C_MUTED); tft.setTextSize(3);
    char dateBuf[32];
    const char* wdays[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    snprintf(dateBuf, sizeof(dateBuf), "%d/%d/%d %s",
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday, wdays[ti.tm_wday]);
    int16_t dateW = strlen(dateBuf) * 18;
    tft.setCursor(cx - dateW / 2, barY + 20);
    tft.print(dateBuf);

    // 天气
    int16_t wxY = barY + 65;
    tft.drawFastHLine(barX, wxY - 8, barW, C_MUTED);
    if (weatherCode >= 0) {
      drawWeatherIcon(weatherCode, barX + 14, wxY + 6, C_MUTED);
      tft.setTextColor(C_DARKTEXT); tft.setTextSize(3);
      tft.setCursor(barX + 60, wxY + 12);
      tft.print(weatherTemp);
      tft.setTextColor(C_MUTED); tft.setTextSize(2);
      tft.setCursor(barX + 60, wxY + 38);
      tft.print(weatherCity);
    } else {
      tft.setTextColor(C_MUTED); tft.setTextSize(2);
      tft.setCursor(barX + 14, wxY + 16);
      tft.print("fetching weather...");
    }

    // 左下角用量数据（来自 MQTT usage topic）
    if (strcmp(usageTokens, "--") != 0) {
      int16_t ux = 10, uy = DISP_H - 28;
      // 用背景色清除旧文字
      tft.setTextColor(C_MUTED); tft.setTextSize(1);
      char uBuf[48];
      snprintf(uBuf, sizeof(uBuf), "T:%s  C:$%s", usageTokens, usageCost);
      tft.setCursor(ux, uy);
      tft.print(uBuf);
    }

    // 右下角装饰小眼睛
    int16_t ex = DISP_W - 52;
    int16_t ey = DISP_H - 38;
    tft.fillRect(ex, ey, 8, 20, C_ORANGE);
    tft.fillRect(ex + 22, ey, 8, 20, C_ORANGE);
  }

  // ── 秒进度条增量更新（无屏闪） ────────────────────────────
  if (ti.tm_sec != lastSecond) {
    lastSecond = ti.tm_sec;
    // 用当前星期背景色清除旧的填充部分
    tft.fillRect(barX + 1, barY + 1, barW - 2, barH - 2, bg);
    // 绘制新的秒进度
    tft.fillRect(barX + 1, barY + 1,
                 (barW - 2) * ti.tm_sec / 60, barH - 2, C_ORANGE);
  }
}

// ── Fetch weather from Open-Meteo ─────────────────────────────
void fetchWeather() {
  if (!wifiStaConnected) return;
  HTTPClient http;
  char url[256];
  snprintf(url, sizeof(url),
    "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current_weather=true",
    weatherLat, weatherLon);
  http.begin(url);
  http.setTimeout(8000);
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    // Parse: "temperature":xx.x  "weathercode":x
    int tIdx = payload.indexOf("\"temperature\"");
    if (tIdx > 0) {
      int colon = payload.indexOf(':', tIdx);
      int end   = payload.indexOf(',', colon);
      if (end < 0) end = payload.indexOf('}', colon);
      weatherTemp = payload.substring(colon + 1, end);
      weatherTemp.trim();
      weatherTemp += "C";
    }
    int wIdx = payload.indexOf("\"weathercode\"");
    if (wIdx > 0) {
      int colon = payload.indexOf(':', wIdx);
      int end   = payload.indexOf(',', colon);
      if (end < 0) end = payload.indexOf('}', colon);
      weatherCode = payload.substring(colon + 1, end).toInt();
    }
    Serial.printf("[WEATHER] %s %d\n", weatherTemp.c_str(), weatherCode);
  }
  http.end();
}

// ── Logo data ─────────────────────────────────────────────────
#define LOGO_CX 120
#define LOGO_CY 105

#define LOGO_TRI_COUNT 162
static const int16_t LOGO_TRIS[][6] PROGMEM = {
  {120,105,65,134,100,114},{120,105,100,114,101,113},{120,105,101,113,100,112},
  {120,105,100,112,99,112},{120,105,99,112,93,111},{120,105,93,111,73,111},
  {120,105,73,111,55,110},{120,105,55,110,38,109},{120,105,38,109,34,108},
  {120,105,34,108,30,103},{120,105,30,103,30,100},{120,105,30,100,34,98},
  {120,105,34,98,39,98},{120,105,39,98,50,99},{120,105,50,99,67,100},
  {120,105,67,100,80,101},{120,105,80,101,98,103},{120,105,98,103,101,103},
  {120,105,101,103,101,102},{120,105,101,102,100,101},{120,105,100,101,100,100},
  {120,105,100,100,82,88},{120,105,82,88,63,76},{120,105,63,76,53,69},
  {120,105,53,69,48,65},{120,105,48,65,45,61},{120,105,45,61,44,54},
  {120,105,44,54,49,49},{120,105,49,49,55,49},{120,105,55,49,57,49},
  {120,105,57,49,64,55},{120,105,64,55,78,66},{120,105,78,66,96,79},
  {120,105,96,79,99,81},{120,105,99,81,100,81},{120,105,100,81,100,80},
  {120,105,100,80,99,78},{120,105,99,78,89,60},{120,105,89,60,78,41},
  {120,105,78,41,73,34},{120,105,73,34,72,29},{120,105,72,29,72,28},
  {120,105,72,28,72,27},{120,105,72,27,71,26},{120,105,71,26,71,25},
  {120,105,71,25,71,24},{120,105,71,24,77,16},{120,105,77,16,80,15},
  {120,105,80,15,87,16},{120,105,87,16,91,19},{120,105,91,19,95,29},
  {120,105,95,29,103,46},{120,105,103,46,114,68},{120,105,114,68,118,75},
  {120,105,118,75,119,81},{120,105,119,81,120,83},{120,105,120,83,121,83},
  {120,105,121,83,121,82},{120,105,121,82,122,69},{120,105,122,69,124,54},
  {120,105,124,54,126,34},{120,105,126,34,126,28},{120,105,126,28,129,21},
  {120,105,129,21,135,18},{120,105,135,18,139,20},{120,105,139,20,143,25},
  {120,105,143,25,142,28},{120,105,142,28,140,42},{120,105,140,42,136,64},
  {120,105,136,64,133,78},{120,105,133,78,135,78},{120,105,135,78,136,76},
  {120,105,136,76,144,67},{120,105,144,67,156,51},{120,105,156,51,162,45},
  {120,105,162,45,168,38},{120,105,168,38,172,35},{120,105,172,35,180,35},
  {120,105,180,35,185,43},{120,105,185,43,183,52},{120,105,183,52,175,62},
  {120,105,175,62,168,71},{120,105,168,71,159,83},{120,105,159,83,153,94},
  {120,105,153,94,154,94},{120,105,154,94,155,94},{120,105,155,94,176,90},
  {120,105,176,90,188,88},{120,105,188,88,201,85},{120,105,201,85,208,88},
  {120,105,208,88,208,91},{120,105,208,91,206,97},{120,105,206,97,191,101},
  {120,105,191,101,174,104},{120,105,174,104,148,110},{120,105,148,110,148,111},
  {120,105,148,111,148,111},{120,105,148,111,160,112},{120,105,160,112,165,112},
  {120,105,165,112,177,112},{120,105,177,112,200,114},{120,105,200,114,205,118},
  {120,105,205,118,209,123},{120,105,209,123,208,126},{120,105,208,126,199,131},
  {120,105,199,131,187,128},{120,105,187,128,159,121},{120,105,159,121,149,119},
  {120,105,149,119,147,119},{120,105,147,119,147,120},{120,105,147,120,156,128},
  {120,105,156,128,170,141},{120,105,170,141,189,158},{120,105,189,158,190,163},
  {120,105,190,163,188,166},{120,105,188,166,185,166},{120,105,185,166,169,153},
  {120,105,169,153,162,148},{120,105,162,148,148,136},{120,105,148,136,147,136},
  {120,105,147,136,147,137},{120,105,147,137,150,142},{120,105,150,142,168,168},
  {120,105,168,168,169,176},{120,105,169,176,168,179},{120,105,168,179,163,180},
  {120,105,163,180,158,179},{120,105,158,179,148,165},{120,105,148,165,137,149},
  {120,105,137,149,129,134},{120,105,129,134,128,135},{120,105,128,135,123,189},
  {120,105,123,189,120,192},{120,105,120,192,115,194},{120,105,115,194,110,191},
  {120,105,110,191,108,185},{120,105,108,185,110,174},{120,105,110,174,113,160},
  {120,105,113,160,116,148},{120,105,116,148,118,134},{120,105,118,134,119,129},
  {120,105,119,129,119,129},{120,105,119,129,118,129},{120,105,118,129,107,144},
  {120,105,107,144,91,166},{120,105,91,166,78,180},{120,105,78,180,75,181},
  {120,105,75,181,70,178},{120,105,70,178,70,173},{120,105,70,173,73,169},
  {120,105,73,169,91,146},{120,105,91,146,102,132},{120,105,102,132,109,124},
  {120,105,109,124,109,123},{120,105,109,123,108,123},{120,105,108,123,61,153},
  {120,105,61,153,52,155},{120,105,52,155,49,151},{120,105,49,151,49,146},
  {120,105,49,146,51,144},{120,105,51,144,65,134},{120,105,65,134,65,134},
};

#define LOGO_SEG_COUNT 162
static const int16_t LOGO_SEGS[][4] PROGMEM = {
  {65,134,100,114},{100,114,101,113},{101,113,100,112},{100,112,99,112},
  {99,112,93,111},{93,111,73,111},{73,111,55,110},{55,110,38,109},
  {38,109,34,108},{34,108,30,103},{30,103,30,100},{30,100,34,98},
  {34,98,39,98},{39,98,50,99},{50,99,67,100},{67,100,80,101},
  {80,101,98,103},{98,103,101,103},{101,103,101,102},{101,102,100,101},
  {100,101,100,100},{100,100,82,88},{82,88,63,76},{63,76,53,69},
  {53,69,48,65},{48,65,45,61},{45,61,44,54},{44,54,49,49},
  {49,49,55,49},{55,49,57,49},{57,49,64,55},{64,55,78,66},
  {78,66,96,79},{96,79,99,81},{99,81,100,81},{100,81,100,80},
  {100,80,99,78},{99,78,89,60},{89,60,78,41},{78,41,73,34},
  {73,34,72,29},{72,29,72,28},{72,28,72,27},{72,27,71,26},
  {71,26,71,25},{71,25,71,24},{71,24,77,16},{77,16,80,15},
  {80,15,87,16},{87,16,91,19},{91,19,95,29},{95,29,103,46},
  {103,46,114,68},{114,68,118,75},{118,75,119,81},{119,81,120,83},
  {120,83,121,83},{121,83,121,82},{121,82,122,69},{122,69,124,54},
  {124,54,126,34},{126,34,126,28},{126,28,129,21},{129,21,135,18},
  {135,18,139,20},{139,20,143,25},{143,25,142,28},{142,28,140,42},
  {140,42,136,64},{136,64,133,78},{133,78,135,78},{135,78,136,76},
  {136,76,144,67},{144,67,156,51},{156,51,162,45},{162,45,168,38},
  {168,38,172,35},{172,35,180,35},{180,35,185,43},{185,43,183,52},
  {183,52,175,62},{175,62,168,71},{168,71,159,83},{159,83,153,94},
  {153,94,154,94},{154,94,155,94},{155,94,176,90},{176,90,188,88},
  {188,88,201,85},{201,85,208,88},{208,88,208,91},{208,91,206,97},
  {206,97,191,101},{191,101,174,104},{174,104,148,110},{148,110,148,111},
  {148,111,148,111},{148,111,160,112},{160,112,165,112},{165,112,177,112},
  {177,112,200,114},{200,114,205,118},{205,118,209,123},{209,123,208,126},
  {208,126,199,131},{199,131,187,128},{187,128,159,121},{159,121,149,119},
  {149,119,147,119},{147,119,147,120},{147,120,156,128},{156,128,170,141},
  {170,141,189,158},{189,158,190,163},{190,163,188,166},{188,166,185,166},
  {185,166,169,153},{169,153,162,148},{162,148,148,136},{148,136,147,136},
  {147,136,147,137},{147,137,150,142},{150,142,168,168},{168,168,169,176},
  {169,176,168,179},{168,179,163,180},{163,180,158,179},{158,179,148,165},
  {148,165,137,149},{137,149,129,134},{129,134,128,135},{128,135,123,189},
  {123,189,120,192},{120,192,115,194},{115,194,110,191},{110,191,108,185},
  {108,185,110,174},{110,174,113,160},{113,160,116,148},{116,148,118,134},
  {118,134,119,129},{119,129,119,129},{119,129,118,129},{118,129,107,144},
  {107,144,91,166},{91,166,78,180},{78,180,75,181},{75,181,70,178},
  {70,178,70,173},{70,173,73,169},{73,169,91,146},{91,146,102,132},
  {102,132,109,124},{109,124,109,123},{109,123,108,123},{108,123,61,153},
  {61,153,52,155},{52,155,49,151},{49,151,49,146},{49,146,51,144},
  {51,144,65,134},{65,134,65,134},
};

// ═════════════════════════════════════════════════════════════
//  HELPERS
// ═════════════════════════════════════════════════════════════

int speedMs(int ms) {
  if (animSpeed == 3) return ms / 2;
  if (animSpeed == 1) return ms * 2;
  return ms;
}

uint16_t hexToRgb565(String hex) {
  hex.replace("#", "");
  if (hex.length() != 6) return C_WHITE;
  long v = strtol(hex.c_str(), nullptr, 16);
  return tft.color565((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

void setBacklight(bool on) {
  backlightOn = on;
  digitalWrite(TFT_BLK, on ? HIGH : LOW);
}

void initColours() {
  // C_ORANGE = tft.color565(170, 72, 28);
  C_ORANGE = tft.color565(218, 17, 0);
  C_DARKBG  = tft.color565(10,  12,  16);
  C_MUTED   = tft.color565(90,  88,  86);
  C_GREEN   = tft.color565(80, 220, 130);
  C_CLOCKBG = tft.color565(240, 238, 233);  // 暖灰白时钟背景
  C_DARKTEXT = tft.color565(30, 30, 36);     // 浅色背景上的深色文字
  animBgColor = C_ORANGE;
  drawBgColor = C_ORANGE;
  currentWeekdayBg = C_CLOCKBG;  // 默认灰白，时钟首次绘制后更新
}

// ═════════════════════════════════════════════════════════════
//  LOGO
// ═════════════════════════════════════════════════════════════

void drawLogoFilled(uint16_t bg, uint16_t fg) {
  tft.fillScreen(bg);
  for (uint16_t i = 0; i < LOGO_TRI_COUNT; i++) {
    tft.fillTriangle(
      pgm_read_word(&LOGO_TRIS[i][0]), pgm_read_word(&LOGO_TRIS[i][1]),
      pgm_read_word(&LOGO_TRIS[i][2]), pgm_read_word(&LOGO_TRIS[i][3]),
      pgm_read_word(&LOGO_TRIS[i][4]), pgm_read_word(&LOGO_TRIS[i][5]),
      fg);
  }
  tft.setTextColor(fg); tft.setTextSize(2);
  tft.setCursor(LOGO_CX - 54, 210); tft.print("Anthropic");
  tft.setCursor(LOGO_CX - 53, 210); tft.print("Anthropic");
}

// ═════════════════════════════════════════════════════════════
//  VIEWS
// ═════════════════════════════════════════════════════════════

// Eye helpers — shared constants via #define EYE_*
inline int16_t eyeLX(int16_t ox) {
  return (DISP_W - (EYE_W * 2 + EYE_GAP)) / 2 + EYE_OX + ox;
}
inline int16_t eyeRX(int16_t ox) { return eyeLX(ox) + EYE_W + EYE_GAP; }
inline int16_t eyeY()            { return (DISP_H - EYE_H) / 2 - EYE_OY; }
inline int16_t eyeCY()           { return eyeY() + EYE_H / 2; }

void drawNormalEyes(int16_t ox = 0, bool blink = false) {
  tft.fillScreen(animBgColor);
  const int16_t lx = eyeLX(ox), rx = eyeRX(ox), ey = eyeY();
  if (!blink) {
    tft.fillRect(lx, ey, EYE_W, EYE_H, C_BLACK);
    tft.fillRect(rx, ey, EYE_W, EYE_H, C_BLACK);
  } else {
    tft.fillRect(lx, ey + EYE_H / 2 - 3, EYE_W, 6, C_BLACK);
    tft.fillRect(rx, ey + EYE_H / 2 - 3, EYE_W, 6, C_BLACK);
  }
}

void drawChevron(int16_t cx, int16_t cy, int16_t arm, int16_t reach,
                 uint8_t thk, bool rightFacing, uint16_t col) {
  for (int8_t t = -(int8_t)thk; t <= (int8_t)thk; t++) {
    if (rightFacing) {
      tft.drawLine(cx - reach/2, cy - arm + t, cx + reach/2, cy + t,      col);
      tft.drawLine(cx + reach/2, cy + t,       cx - reach/2, cy + arm + t, col);
    } else {
      tft.drawLine(cx + reach/2, cy - arm + t, cx - reach/2, cy + t,      col);
      tft.drawLine(cx - reach/2, cy + t,       cx + reach/2, cy + arm + t, col);
    }
  }
}

void drawSquishEyes(bool closed = false) {
  tft.fillScreen(animBgColor);
  const int16_t lx = eyeLX(0), rx = eyeRX(0), cy = eyeCY();
  const int16_t arm   = EYE_H / 2;
  const int16_t reach = EYE_W / 2;
  const int16_t lcx   = lx + EYE_W / 2;
  const int16_t rcx   = rx + EYE_W / 2;
  if (!closed) {
    drawChevron(lcx, cy, arm, reach, 10, true,  C_BLACK);
    drawChevron(rcx, cy, arm, reach, 10, false, C_BLACK);
  } else {
    tft.fillRect(lx, cy - 5, EYE_W, 10, C_BLACK);
    tft.fillRect(rx, cy - 5, EYE_W, 10, C_BLACK);
  }
}

void drawCodeView() {
  termMode = false;
  tft.fillScreen(C_DARKBG);
  tft.fillRect(0, 0,          DISP_W, 4, C_ORANGE);
  tft.fillRect(0, DISP_H - 4, DISP_W, 4, C_ORANGE);
  tft.setTextColor(C_ORANGE); tft.setTextSize(4);
  tft.setCursor((DISP_W - 144) / 2, DISP_H / 2 - 52); tft.print("Claude");
  tft.setTextColor(C_WHITE);  tft.setTextSize(4);
  tft.setCursor((DISP_W - 96) / 2,  DISP_H / 2 + 8);  tft.print("Code");
  tft.fillRect((DISP_W - 96) / 2, DISP_H / 2 + 52, 96, 3, C_ORANGE);
}

// ═════════════════════════════════════════════════════════════
//  TERMINAL
// ═════════════════════════════════════════════════════════════

void termClear() {
  for (uint8_t i = 0; i < TERM_ROWS; i++) termLines[i] = "";
  termRow = 0; termCol = 0;
}

void termDrawHeader() {
  tft.fillRect(0, 0, DISP_W, TERM_PAD_Y + 1, C_DARKBG);
  tft.setTextColor(C_ORANGE); tft.setTextSize(1);
  tft.setCursor(TERM_PAD_X, 4); tft.print("clawd@mochi terminal");
  tft.drawFastHLine(0, TERM_PAD_Y, DISP_W, C_ORANGE);
}

// Prefix "clawd:~$ " in green, drawn only when the row has content
void termDrawPrefix(int16_t yy) {
  tft.setTextColor(C_GREEN); tft.setTextSize(1);
  tft.setCursor(TERM_PAD_X, yy + 6);
  tft.print("clawd:~$ ");
}

#define PREFIX_PX 54   // 9 chars × 6px = 54px at textSize 1

void termDrawLine(uint8_t r) {
  const int16_t yy = TERM_PAD_Y + 4 + r * TERM_CHAR_H;
  tft.fillRect(0, yy, DISP_W, TERM_CHAR_H, C_DARKBG);
  // show prefix only on the currently active (cursor) line
  if (r == termRow) termDrawPrefix(yy);
  tft.setTextColor(C_WHITE); tft.setTextSize(2);
  tft.setCursor(TERM_PAD_X + PREFIX_PX, yy + 1);
  tft.print(termLines[r]);
  if (r == termRow) {
    const int16_t cx = TERM_PAD_X + PREFIX_PX + termCol * TERM_CHAR_W;
    tft.fillRect(cx, yy + 1, TERM_CHAR_W - 2, TERM_CHAR_H - 2, C_GREEN);
  }
}

void termDrawLastChar() {
  if (termCol == 0) return;
  const int16_t yy    = TERM_PAD_Y + 4 + termRow * TERM_CHAR_H;
  const int16_t baseX = TERM_PAD_X + PREFIX_PX;
  const uint8_t prev  = termCol - 1;
  // erase prev cell (had cursor block)
  tft.fillRect(baseX + prev * TERM_CHAR_W, yy + 1, TERM_CHAR_W, TERM_CHAR_H - 1, C_DARKBG);
  tft.setTextColor(C_WHITE); tft.setTextSize(2);
  tft.setCursor(baseX + prev * TERM_CHAR_W, yy + 1);
  tft.print(termLines[termRow][prev]);
  // new cursor
  tft.fillRect(baseX + termCol * TERM_CHAR_W, yy + 1, TERM_CHAR_W - 2, TERM_CHAR_H - 2, C_GREEN);
}

void termDrawBackspace() {
  const int16_t yy    = TERM_PAD_Y + 4 + termRow * TERM_CHAR_H;
  const int16_t baseX = TERM_PAD_X + PREFIX_PX;
  // erase deleted char + old cursor
  tft.fillRect(baseX + termCol * TERM_CHAR_W, yy + 1, TERM_CHAR_W * 2, TERM_CHAR_H - 1, C_DARKBG);
  // new cursor
  tft.fillRect(baseX + termCol * TERM_CHAR_W, yy + 1, TERM_CHAR_W - 2, TERM_CHAR_H - 2, C_GREEN);
  // if line now empty, erase the prefix too
  if (termLines[termRow].length() == 0) {
    tft.fillRect(0, yy, TERM_PAD_X + PREFIX_PX, TERM_CHAR_H, C_DARKBG);
  }
}

void termFullRedraw() {
  tft.fillScreen(C_DARKBG);
  termDrawHeader();
  for (uint8_t r = 0; r < TERM_ROWS; r++) termDrawLine(r);
}

void termScroll() {
  for (uint8_t i = 0; i < TERM_ROWS - 1; i++) termLines[i] = termLines[i + 1];
  termLines[TERM_ROWS - 1] = "";
  termRow = TERM_ROWS - 1;
  termFullRedraw();
}

void termAddChar(char c) {
  if (c == '\n' || c == '\r') {
    const int16_t yy = TERM_PAD_Y + 4 + termRow * TERM_CHAR_H;
    // erase cursor on current row
    tft.fillRect(TERM_PAD_X + PREFIX_PX + termCol * TERM_CHAR_W,
                 yy + 1, TERM_CHAR_W, TERM_CHAR_H - 1, C_DARKBG);
    termRow++; termCol = 0;
    if (termRow >= TERM_ROWS) { termScroll(); return; }
    termDrawLine(termRow);  // draws prefix on new line
  } else if (c == '\b' || c == 127) {
    if (termCol > 0) {
      termCol--;
      termLines[termRow].remove(termLines[termRow].length() - 1);
      termDrawBackspace();
    }
  } else if (c >= 32 && c < 127) {
    if (termCol >= TERM_COLS) {
      termRow++; termCol = 0;
      if (termRow >= TERM_ROWS) { termScroll(); return; }
    }
    // draw prefix on first char of this line
    if (termCol == 0) termDrawPrefix(TERM_PAD_Y + 4 + termRow * TERM_CHAR_H);
    termLines[termRow] += c;
    termCol++;
    termDrawLastChar();
  }
}

// ═════════════════════════════════════════════════════════════
//  ANIMATIONS
// ═════════════════════════════════════════════════════════════

void animNormalEyes() {
  busy = true;
  const int16_t offs[] = {-16, 16, -16, 16, 0};
  for (uint8_t i = 0; i < 5; i++) { drawNormalEyes(offs[i]); delay(speedMs(80)); }
  drawNormalEyes(0, true);  delay(speedMs(100));
  drawNormalEyes(0, false); delay(speedMs(70));
  drawNormalEyes(0, true);  delay(speedMs(70));
  drawNormalEyes(0, false);
  busy = false;
}

void animSquishEyes() {
  busy = true;
  for (uint8_t i = 0; i < 3; i++) {
    drawSquishEyes(false); delay(speedMs(160));
    drawSquishEyes(true);  delay(speedMs(100));
  }
  drawSquishEyes(false);
  busy = false;
}

void animLogoReveal() {
  busy = true;
  tft.fillScreen(animBgColor);
  for (uint16_t i = 0; i < LOGO_SEG_COUNT; i++) {
    int16_t x1 = pgm_read_word(&LOGO_SEGS[i][0]);
    int16_t y1 = pgm_read_word(&LOGO_SEGS[i][1]);
    int16_t x2 = pgm_read_word(&LOGO_SEGS[i][2]);
    int16_t y2 = pgm_read_word(&LOGO_SEGS[i][3]);
    tft.drawLine(x1, y1, x2, y2, C_WHITE);
    tft.drawLine(x1 + 1, y1, x2 + 1, y2, C_WHITE);
    if (i % 4 == 0) { server.handleClient(); delay(speedMs(8)); }
  }
  drawLogoFilled(animBgColor, C_WHITE);
  delay(1500);
  busy = false;
}

// ═════════════════════════════════════════════════════════════
//  WEB PAGE
// ═════════════════════════════════════════════════════════════
const char INDEX_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>Clawd Mochi</title>
<style>
*{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent}
body{background:#1c1c20;font-family:'Courier New',monospace;color:#e8e4dc;
  display:flex;flex-direction:column;align-items:center;
  padding:20px 14px 52px;gap:14px;min-height:100vh}

.hdr{text-align:center;padding:2px 0 4px}
.mascot{font-size:15px;color:#c96a3e;line-height:1.3;font-weight:bold;
  font-family:'Courier New',monospace;display:block;letter-spacing:1px}
.sitename{font-size:10px;color:#5a5048;margin-top:8px;letter-spacing:3px}

.sec{width:100%;max-width:390px;font-size:10px;color:#8a8278;
  letter-spacing:2px;font-weight:bold;padding:0 2px}

/* Busy bar */
.busy{width:100%;max-width:390px;height:2px;background:#2e2a28;
  border-radius:1px;overflow:hidden;opacity:0;transition:opacity .2s}
.busy.show{opacity:1}
.busy-i{height:100%;width:30%;background:#c96a3e;border-radius:1px;
  animation:sl 1s linear infinite}
@keyframes sl{0%{margin-left:-30%}100%{margin-left:100%}}

/* Controls */
.ctrl{display:flex;gap:8px;width:100%;max-width:390px}
.cbtn{flex:1;background:#252428;border:1.5px solid #38343a;border-radius:10px;
  color:#b8b4ac;font-family:'Courier New',monospace;font-size:11px;font-weight:bold;
  padding:12px 4px;cursor:pointer;text-align:center;transition:all .12s}
.cbtn:active:not(:disabled){transform:scale(.94)}
.cbtn:disabled{opacity:.3;cursor:default}
.cbtn.on{border-color:#c96a3e;color:#c96a3e;background:#201408}
.cbtn.dim{border-color:#2e2a28;color:#4a4540}

/* View grid */
.vgrid{display:grid;grid-template-columns:1fr 1fr;gap:8px;width:100%;max-width:390px}
.vbtn{background:#252428;border:1.5px solid #38343a;border-radius:12px;
  color:#d8d4cc;font-family:'Courier New',monospace;
  padding:14px 6px 10px;cursor:pointer;text-align:center;
  transition:all .12s;user-select:none}
.vbtn:active:not(:disabled){transform:scale(.94)}
.vbtn:disabled{opacity:.3;cursor:default}
.vbtn .ic{font-size:20px;display:block;margin-bottom:4px;line-height:1;color:#c96a3e}
.vbtn .nm{font-size:12px;font-weight:bold;color:#e8e4dc}
.vbtn .ht{font-size:9px;color:#8a8278;margin-top:3px}
.vbtn.active{border-color:#c96a3e;background:#201408}
.vbtn[data-v="1"].active{border-color:#c96a3e;background:#201408}
.vbtn[data-v="2"].active{border-color:#4a8acd;background:#0c1628}
.vbtn[data-v="3"].active{border-color:#38343a;background:#201c18}

/* Speed slider */
.speed-row{width:100%;max-width:390px;display:flex;align-items:center;gap:10px}
.sl{font-size:10px;color:#6a6058;white-space:nowrap;min-width:36px}
input[type=range]{flex:1;accent-color:#c96a3e;cursor:pointer;height:20px}
.sv{font-size:11px;color:#c96a3e;min-width:44px;text-align:right;font-weight:bold}

/* Terminal */
.twrap{width:100%;max-width:390px;display:none;flex-direction:column;gap:8px}
.twrap.open{display:flex}
.thdr{display:flex;justify-content:space-between;align-items:center}
.tttl{font-size:11px;color:#28b878;letter-spacing:1px;font-weight:bold}
.tx{background:#0c1e12;border:2px solid #1a4828;border-radius:9px;
  color:#28b878;font-family:'Courier New',monospace;font-size:13px;
  font-weight:bold;padding:10px 18px;cursor:pointer}
.tx:active{background:#081410}
.trow{display:flex;gap:6px}
.tin{flex:1;background:#0c1018;border:1.5px solid #1a2820;border-radius:9px;
  color:#40d880;font-family:'Courier New',monospace;font-size:15px;
  padding:11px;outline:none}
.tin::placeholder{color:#2a3828}
.tgo{background:#1a9060;border:none;border-radius:9px;color:#fff;
  font-family:'Courier New',monospace;font-size:22px;font-weight:bold;
  padding:11px 16px;cursor:pointer;min-width:52px}
.tgo:active{background:#0f6040}

/* Canvas */
.cwrap{width:100%;max-width:390px;background:#222028;border:1.5px solid #38343a;
  border-radius:12px;padding:12px;flex-direction:column;gap:10px;display:none}
.cwrap.open{display:flex}
.crow{display:flex;gap:8px}
.ci{display:flex;flex-direction:column;align-items:center;gap:4px;flex:1}
.cl{font-size:10px;color:#7a7068;letter-spacing:1px;font-weight:bold}
.cs{width:100%;height:38px;border-radius:7px;border:1.5px solid #38343a;cursor:pointer;padding:0}
.dacts{display:flex;gap:7px}
.db{flex:1;background:#1c1820;border:1.5px solid #38343a;border-radius:9px;
  color:#c0bab8;font-family:'Courier New',monospace;font-size:11px;
  font-weight:bold;padding:11px 4px;cursor:pointer;transition:all .12s}
.db:active{transform:scale(.95);background:#281838}
.db.hi{border-color:#c96a3e;color:#c96a3e}
canvas{width:100%;border-radius:8px;border:1.5px solid #38343a;
  touch-action:none;cursor:crosshair;display:block}

/* Toast */
.toast{position:fixed;bottom:18px;left:50%;transform:translateX(-50%);
  background:#252428;border:1.5px solid #38343a;border-radius:9px;
  font-size:12px;color:#d8d4cc;padding:7px 16px;opacity:0;
  transition:opacity .18s;pointer-events:none;white-space:nowrap;z-index:99}
.toast.show{opacity:1}
</style>
</head>
<body>

<div class="hdr">
  <span class="mascot">&#x2590;&#x259B;&#x2588;&#x2588;&#x2588;&#x259C;&#x258C;<br>&#x259C;&#x2588;&#x2588;&#x2588;&#x2588;&#x2588;&#x259B;<br>&#x2598;&#x2598;&nbsp;&#x259D;&#x259D;</span>
  <div class="sitename">CLAWD &middot; MOCHI &middot; CONTROLLER</div>
</div>

<div class="busy" id="busy"><div class="busy-i"></div></div>

<div class="sec">// controls</div>
<div class="ctrl">
  <button class="cbtn on" id="blBtn" onclick="toggleBL()">&#9728; display on</button>
</div>

<div class="sec">// views</div>
<div class="vgrid">
  <button class="vbtn active" data-v="0" onclick="setView(0)">
    <span class="ic">&#9632; &#9632;</span>
    <span class="nm">Normal eyes</span>
    <span class="ht">wiggle + blink</span>
  </button>
  <button class="vbtn" data-v="1" onclick="setView(1)">
    <span class="ic">&gt; &lt;</span>
    <span class="nm">Squish eyes</span>
    <span class="ht">open / close</span>
  </button>
  <button class="vbtn" data-v="2" onclick="setView(2)">
    <span class="ic">{ }</span>
    <span class="nm">Claude Code</span>
    <span class="ht">opens terminal</span>
  </button>
  <button class="vbtn" data-v="5" onclick="setView(5)">
    <span class="ic">&#9200;</span>
    <span class="nm">Clock</span>
    <span class="ht">time + weather</span>
  </button>
  <button class="vbtn" data-v="3" onclick="toggleCanvas()">
    <span class="ic">&#11035;</span>
    <span class="nm">Canvas</span>
    <span class="ht">draw on display</span>
  </button>
</div>

<div class="sec">// speed</div>
<div class="speed-row">
  <span class="sl">slow</span>
  <input type="range" id="spd" min="1" max="3" value="1" step="1" oninput="setSpeed(this.value)">
  <span class="sv" id="spdV">slow</span>
</div>

<div class="ctrl">
  <div class="ci" style="flex:1;display:flex;flex-direction:column;gap:4px;align-items:stretch">
    <span class="cl" style="font-size:10px;color:#8a8278;letter-spacing:1px;font-weight:bold;text-align:center">BACKGROUND</span>
    <input type="color" class="cs" id="bgCol" value="#aa4818" oninput="onBgChange(this.value)">
  </div>
  <div class="ci" style="flex:1;display:flex;flex-direction:column;gap:4px;align-items:stretch">
    <span class="cl" style="font-size:10px;color:#8a8278;letter-spacing:1px;font-weight:bold;text-align:center">PEN COLOR</span>
    <input type="color" class="cs" id="penCol" value="#000000">
  </div>
</div>

<div class="sec">// weather settings</div>
<div class="wg" id="wg" style="width:100%;max-width:390px;display:none;flex-direction:column;gap:6px">
  <div style="display:flex;gap:6px">
    <input class="tin" id="wcity" type="text" placeholder="City name" style="flex:1">
  </div>
  <div style="display:flex;gap:6px">
    <input class="tin" id="wlat" type="text" placeholder="Lat (e.g. 39.90)" style="flex:1">
    <input class="tin" id="wlon" type="text" placeholder="Lon (e.g. 116.41)" style="flex:1">
  </div>
  <button class="tx" onclick="saveWeather()" style="align-self:flex-start">save location</button>
</div>

<div class="sec">// wifi settings</div>
<div class="wg" id="wf" style="width:100%;max-width:390px;display:none;flex-direction:column;gap:6px">
  <div style="display:flex;gap:6px">
    <input class="tin" id="wssid" type="text" placeholder="WiFi SSID" style="flex:1">
  </div>
  <div style="display:flex;gap:6px">
    <input class="tin" id="wpass" type="password" placeholder="WiFi password" style="flex:1">
  </div>
  <div style="display:flex;gap:8px;align-items:center">
    <button class="tx" onclick="saveWiFi()">save & reconnect</button>
    <span id="wsta" style="font-size:10px;color:#6a6058"></span>
  </div>
</div>

<div class="sec">// terminal</div>
<div class="twrap" id="twrap">
  <div class="thdr">
    <span class="tttl">&#9658; clawd:~$</span>
    <button class="tx" onclick="closeTerm()">&#x2715; exit terminal</button>
  </div>
  <div class="trow">
    <input class="tin" id="tin" type="text" placeholder="type here..."
           autocomplete="off" autocorrect="off" autocapitalize="off" spellcheck="false">
    <button class="tgo" onclick="termEnter()">&#8629;</button>
  </div>
</div>

<div class="cwrap" id="cwrap">
  <div class="dacts">
    <button class="db hi" onclick="clearAll()">&#11035; clear</button>
    <button class="db" style="border-color:#28b878;color:#28b878" onclick="toggleCanvas()">&#10003; done</button>
  </div>
  <canvas id="cvs" width="240" height="240"></canvas>
</div>

<div class="toast" id="toast"></div>

<script>
let activeView  = 0;
let termOpen    = false;
let canvasOpen  = false;
let blOn        = true;
let isBusy      = false;
let drawing     = false;
let lastX = 0, lastY = 0;
let tt;

const spdLabels = ['','slow','normal','fast'];

// ── Toast ──────────────────────────────────────────────────────
function toast(msg, ok=true) {
  const el = document.getElementById('toast');
  el.textContent = msg;
  el.style.borderColor = ok ? '#28b878' : '#c96a3e';
  el.classList.add('show');
  clearTimeout(tt);
  tt = setTimeout(() => el.classList.remove('show'), 1300);
}

// ── Busy ────────────────────────────────────────────────────────
function setBusy(b) {
  isBusy = b;
  document.getElementById('busy').classList.toggle('show', b);
  const locked = b || termOpen;
  document.querySelectorAll('.vbtn').forEach(el => {
    // when canvas open, keep canvas btn (data-v=3) active so user can exit
    el.disabled = canvasOpen ? parseInt(el.dataset.v) !== 3 : locked;
  });
  document.querySelectorAll('.lbtn').forEach(el => el.disabled = locked || canvasOpen);
  document.querySelectorAll('.cbtn').forEach(el => {
    if (el.id !== 'blBtn') el.disabled = locked;
  });
}

// ── HTTP ────────────────────────────────────────────────────────
async function req(path) {
  try { const r = await fetch(path); return r.ok; }
  catch(e) { toast('no connection', false); return false; }
}

async function waitNotBusy() {
  for (let i = 0; i < 100; i++) {
    try {
      const r = await fetch('/state');
      const j = await r.json();
      if (!j.busy) return;
    } catch(e) {}
    await new Promise(r => setTimeout(r, 150));
  }
}

// ── Background colour ───────────────────────────────────────────
async function onBgChange(hex) {
  if (canvasOpen) {
    await req('/draw/clear?bg=' + encodeURIComponent(hex));
  } else {
    await req('/redraw?bg=' + encodeURIComponent(hex));
  }
  redrawCanvas(hex);
}

// ── Speed ───────────────────────────────────────────────────────
async function setSpeed(v) {
  document.getElementById('spdV').textContent = spdLabels[v];
  await req('/speed?v=' + v);
}

// ── Views ───────────────────────────────────────────────────────
async function setView(v) {
  if (isBusy || termOpen || canvasOpen) return;
  if (v === 3) { toggleCanvas(); return; }  // canvas button in grid
  const keyMap = {0:'w', 1:'s', 2:'d', 5:'c'};
  const k = keyMap[v];
  if (!k) return;
  if (!await req('/cmd?k=' + k)) return;
  activeView = v;
  document.querySelectorAll('.vbtn').forEach(b =>
    b.classList.toggle('active', parseInt(b.dataset.v) === v));
  if (v === 2) {
    termOpen = true;
    document.getElementById('twrap').classList.add('open');
    document.getElementById('wg').style.display = 'none';
    setBusy(false);
    setBusy(false);
    document.querySelectorAll('.vbtn,.lbtn').forEach(b => b.disabled = true);
    const cvb = document.getElementById('cvBtn'); if (cvb) cvb.disabled = true;
    document.getElementById('tin').focus();
    toast('terminal open');
    return;
  }
  if (v === 5) {
    document.getElementById('wg').style.display = 'flex';
    document.getElementById('wf').style.display = 'flex';
    document.getElementById('twrap').classList.remove('open');
    termOpen = false;
    loadWeatherCfg();
    loadWiFiState();
    toast('clock mode');
    return;
  }
  document.getElementById('wg').style.display = 'none';
  document.getElementById('wf').style.display = 'none';
  setBusy(true);
  await waitNotBusy();
  setBusy(false);
}

// ── Logo animations (kept for startup, not exposed in UI) ──────

// ── Backlight ───────────────────────────────────────────────────
async function toggleBL() {
  blOn = !blOn;
  await req('/backlight?on=' + (blOn ? 1 : 0));
  const b = document.getElementById('blBtn');
  b.textContent = blOn ? '\u2600 display on' : '\u25cb display off';
  b.classList.toggle('on', blOn);
  b.classList.toggle('dim', !blOn);
}

// ── Canvas toggle ───────────────────────────────────────────────
async function toggleCanvas() {
  canvasOpen = !canvasOpen;
  document.getElementById('cwrap').classList.toggle('open', canvasOpen);
  const b = document.getElementById('cvBtn');
  if (b) { b.classList.toggle('on', canvasOpen); b.textContent = canvasOpen ? '\u2b1b canvas on' : '\u2b1b canvas'; }
  // highlight the canvas vbtn (data-v=3) in the grid
  document.querySelectorAll('.vbtn').forEach(btn =>
    btn.classList.toggle('active', canvasOpen && parseInt(btn.dataset.v) === 3));
  await req('/canvas?on=' + (canvasOpen ? 1 : 0));
  if (canvasOpen) {
    const bg = document.getElementById('bgCol').value;
    redrawCanvas(bg);
    await req('/draw/clear?bg=' + encodeURIComponent(bg));
    document.getElementById('wg').style.display = 'none';
    document.getElementById('wf').style.display = 'none';
    // lock all other buttons
    document.querySelectorAll('.vbtn,.lbtn').forEach(b => b.disabled = true);
    toast('canvas active');
  } else {
    if (activeView === 5) {
      document.getElementById('wg').style.display = 'flex';
      document.getElementById('wf').style.display = 'flex';
    }
    setBusy(false);   // re-evaluate locks
    toast('canvas off');
  }
}

// ── Terminal ────────────────────────────────────────────────────
const tin = document.getElementById('tin');
let lastVal = '';
tin.addEventListener('input', async () => {
  const cur = tin.value, prev = lastVal;
  if (cur.length > prev.length) {
    await req('/char?c=' + encodeURIComponent(cur[cur.length - 1]));
  } else if (cur.length < prev.length) {
    await req('/char?c=%08');
  }
  lastVal = cur;
});
async function termEnter() {
  await req('/char?c=%0A');
  tin.value = ''; lastVal = ''; tin.focus();
}
tin.addEventListener('keydown', e => {
  if (e.key === 'Enter') { e.preventDefault(); termEnter(); }
});
async function closeTerm() {
  await req('/cmd?k=q');
  termOpen = false;
  document.getElementById('twrap').classList.remove('open');
  setBusy(false);
  toast('terminal closed');
}

// ── Canvas drawing — send full stroke on finger lift ────────────
const cvs = document.getElementById('cvs');
const ctx = cvs.getContext('2d');
let strokePts = [];

function getPos(e) {
  const r = cvs.getBoundingClientRect();
  const sx = cvs.width / r.width, sy = cvs.height / r.height;
  const s = e.touches ? e.touches[0] : e;
  return { x: (s.clientX - r.left) * sx, y: (s.clientY - r.top) * sy };
}

function redrawCanvas(hex) {
  ctx.fillStyle = hex;
  ctx.fillRect(0, 0, cvs.width, cvs.height);
}

function startDraw(e) {
  e.preventDefault();
  drawing = true;
  strokePts = [];
  const p = getPos(e); lastX = p.x; lastY = p.y;
  strokePts.push({ x: Math.round(p.x), y: Math.round(p.y) });
  // draw dot on canvas preview only — no display send yet
  ctx.beginPath(); ctx.arc(p.x, p.y, 2, 0, Math.PI * 2);
  ctx.fillStyle = document.getElementById('penCol').value; ctx.fill();
}
function moveDraw(e) {
  if (!drawing) return; e.preventDefault();
  const p = getPos(e);
  ctx.beginPath(); ctx.moveTo(lastX, lastY); ctx.lineTo(p.x, p.y);
  ctx.strokeStyle = document.getElementById('penCol').value;
  ctx.lineWidth = 4; ctx.lineCap = 'round'; ctx.stroke();
  strokePts.push({ x: Math.round(p.x), y: Math.round(p.y) });
  lastX = p.x; lastY = p.y;
}
async function endDraw(e) {
  if (!drawing) return; drawing = false;
  if (!canvasOpen || strokePts.length < 1) return;
  const pen = document.getElementById('penCol').value.replace('#', '');
  const pts = strokePts.map(p => p.x + ',' + p.y).join(';');
  await req('/draw/stroke?pen=' + pen + '&pts=' + encodeURIComponent(pts));
  strokePts = [];
}

cvs.addEventListener('mousedown',  startDraw);
cvs.addEventListener('mousemove',  moveDraw);
cvs.addEventListener('mouseup',    endDraw);
cvs.addEventListener('mouseleave', endDraw);
cvs.addEventListener('touchstart', startDraw, {passive:false});
cvs.addEventListener('touchmove',  moveDraw,  {passive:false});
cvs.addEventListener('touchend',   endDraw);

// Clear = clear both web canvas and display
async function clearAll() {
  const bg = document.getElementById('bgCol').value;
  redrawCanvas(bg);
  await req('/draw/clear?bg=' + encodeURIComponent(bg));
  toast('cleared');
}

// \u2500\u2500 Weather settings \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
async function loadWeatherCfg() {
  try {
    const r = await fetch('/weather/set');
    const j = await r.json();
    document.getElementById('wcity').value = j.city || '';
    document.getElementById('wlat').value = j.lat || '';
    document.getElementById('wlon').value = j.lon || '';
  } catch(e) {}
}
async function saveWeather() {
  const city = document.getElementById('wcity').value;
  const lat  = document.getElementById('wlat').value;
  const lon  = document.getElementById('wlon').value;
  const params = new URLSearchParams({city, lat, lon});
  const r = await fetch('/weather/set?' + params.toString());
  if (r.ok) toast('location saved');
  else toast('save failed', false);
}

// ── WiFi settings ─────────────────────────────────────────────
async function loadWiFiState() {
  try {
    const r = await fetch('/wifi/state');
    const j = await r.json();
    document.getElementById('wssid').value = j.ssid || '';
    document.getElementById('wsta').textContent = j.connected ? '● online' : '○ offline';
    document.getElementById('wsta').style.color = j.connected ? '#28b878' : '#c96a3e';
  } catch(e) {}
}
async function saveWiFi() {
  const ssid = document.getElementById('wssid').value;
  const pass = document.getElementById('wpass').value;
  if (!ssid || !pass) { toast('fill in both fields', false); return; }
  const params = new URLSearchParams({ssid, pass});
  const r = await fetch('/wifi/set?' + params.toString());
  if (r.ok) {
    toast('saved, reconnecting...');
    document.getElementById('wpass').value = '';
    setTimeout(loadWiFiState, 4000);
  } else {
    toast('save failed', false);
  }
}

// Init: sync speed and backlight from ESP32, reset bg to default
(async () => {
  try {
    const r = await fetch('/state');
    const j = await r.json();
    // Sync speed
    const spd = j.speed || 1;
    document.getElementById('spd').value = spd;
    document.getElementById('spdV').textContent = spdLabels[spd];
    // Sync backlight
    if (j.bl === false) {
      blOn = false;
      const b = document.getElementById('blBtn');
      b.textContent = '\u25cb display off';
      b.classList.remove('on'); b.classList.add('dim');
    }
  } catch(e) {}
  // Always reset bg picker to default orange on page load
  document.getElementById('bgCol').value = '#aa4818';
  redrawCanvas('#aa4818');
})();
</script>
</body>
</html>
)rawhtml";

// ═════════════════════════════════════════════════════════════
//  WEB ROUTES
// ═════════════════════════════════════════════════════════════

void routeRoot() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.send_P(200, "text/html", INDEX_HTML);
}

void routeCmd() {
  if (!server.hasArg("k") || server.arg("k").isEmpty()) {
    server.send(400, "application/json", "{\"e\":1}"); return;
  }
  const char c = server.arg("k")[0];

  if (termMode) {
    if (c == 'q') { termMode = false; drawCodeView(); }
    server.send(200, "application/json", "{\"ok\":1}"); return;
  }

  server.send(200, "application/json", "{\"ok\":1}");
  switch (c) {
    case 'w': currentView = VIEW_EYES_NORMAL; clockWasActive = false; animNormalEyes(); break;
    case 's': currentView = VIEW_EYES_SQUISH; clockWasActive = false; animSquishEyes(); break;
    case 'd':
      currentView = VIEW_CODE; clockWasActive = false;
      drawCodeView();
      termMode = true; termClear(); termFullRedraw(); break;
    case 'a':
      currentView = VIEW_EYES_NORMAL; clockWasActive = false;
      animLogoReveal();
      break;
    case 'c':
      currentView = VIEW_CLOCK; clockWasActive = true;
      clockValid = false; drawClockView(true);
      if (wifiStaConnected) fetchWeather();
      break;
  }
  lastUserAction = millis();
}

void routeChar() {
  if (!termMode) { server.send(200, "application/json", "{\"ok\":1}"); return; }
  const String val = server.arg("c");
  if (val.length() > 0) termAddChar(val[0]);
  server.send(200, "application/json", "{\"ok\":1}");
}

void routeSpeed() {
  if (server.hasArg("v")) animSpeed = constrain(server.arg("v").toInt(), 1, 3);
  server.send(200, "application/json", "{\"ok\":1}");
}

// /redraw?bg=hex — set animBg and immediately redraw current view
void routeRedraw() {
  if (server.hasArg("bg")) {
    animBgColor = hexToRgb565(server.arg("bg"));
    drawBgColor = animBgColor;
  }
  switch (currentView) {
    case VIEW_EYES_NORMAL: drawNormalEyes(); break;
    case VIEW_EYES_SQUISH: drawSquishEyes(); break;
    case VIEW_CODE:        drawCodeView();   break;
    case VIEW_DRAW:        tft.fillScreen(drawBgColor); break;
    case VIEW_CLOCK:       drawClockView(true);  break;
  }
  server.send(200, "application/json", "{\"ok\":1}");
}

void routeCanvas() {
  const bool on = server.hasArg("on") && server.arg("on") == "1";
  if (on) { currentView = VIEW_DRAW; clockWasActive = false; tft.fillScreen(drawBgColor); }
  server.send(200, "application/json", "{\"ok\":1}");
}

void routeDrawClear() {
  const String bg = server.hasArg("bg") ? server.arg("bg") : "#aa4818";
  drawBgColor = hexToRgb565(bg);
  animBgColor = drawBgColor;  // keep in sync
  currentView = VIEW_DRAW; clockWasActive = false; termMode = false;
  tft.fillScreen(drawBgColor);
  server.send(200, "application/json", "{\"ok\":1}");
}

void routeDrawStroke() {
  if (!server.hasArg("pts") || !server.hasArg("pen")) {
    server.send(200, "application/json", "{\"ok\":1}"); return;
  }
  const uint16_t color = hexToRgb565(server.arg("pen"));
  const String   data  = server.arg("pts");
  currentView = VIEW_DRAW;

  struct Pt { int16_t x, y; };
  Pt prev = {-1, -1};
  int start = 0;
  while (start < (int)data.length()) {
    int semi = data.indexOf(';', start);
    if (semi == -1) semi = data.length();
    String entry = data.substring(start, semi);
    const int comma = entry.indexOf(',');
    if (comma > 0) {
      const int16_t x = entry.substring(0, comma).toInt();
      const int16_t y = entry.substring(comma + 1).toInt();
      if (prev.x >= 0) {
        tft.drawLine(prev.x, prev.y, x, y, color);
        tft.drawLine(prev.x + 1, prev.y, x + 1, y, color);
        tft.drawLine(prev.x, prev.y + 1, x, y + 1, color);
      } else {
        tft.fillCircle(x, y, 2, color);
      }
      prev = {x, y};
    }
    start = semi + 1;
  }
  server.send(200, "application/json", "{\"ok\":1}");
}

void routeBacklight() {
  setBacklight(server.hasArg("on") && server.arg("on") == "1");
  server.send(200, "application/json", "{\"ok\":1}");
}

// Convert RGB565 back to #RRGGBB for state endpoint
String rgb565ToHex(uint16_t c) {
  uint8_t r = ((c >> 11) & 0x1F) << 3;
  uint8_t g = ((c >> 5)  & 0x3F) << 2;
  uint8_t b = (c & 0x1F) << 3;
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
  return String(buf);
}

void routeWifiSet() {
  String ssid = server.hasArg("ssid") ? server.arg("ssid") : "";
  String pass = server.hasArg("pass") ? server.arg("pass") : "";
  if (ssid.length() > 0 && ssid.length() < 32 && pass.length() > 0 && pass.length() < 32) {
    strncpy(STA_SSID, ssid.c_str(), 31); STA_SSID[31] = '\0';
    strncpy(STA_PASS, pass.c_str(), 31); STA_PASS[31] = '\0';
    saveWiFiCfg(STA_SSID, STA_PASS);
    WiFi.begin(STA_SSID, STA_PASS);
    server.send(200, "application/json", "{\"ok\":1,\"msg\":\"saved, reconnecting...\"}");
  } else {
    server.send(200, "application/json", "{\"ok\":0,\"msg\":\"invalid params\"}");
  }
}

void routeWifiState() {
  String j = "{\"ssid\":\"" + String(STA_SSID) + "\",\"connected\":" + (wifiStaConnected ? "true" : "false") + "}";
  server.send(200, "application/json", j);
}

void routeWeatherSet() {
  if (server.hasArg("lat")) weatherLat = server.arg("lat").toFloat();
  if (server.hasArg("lon")) weatherLon = server.arg("lon").toFloat();
  if (server.hasArg("city")) {
    strncpy(weatherCity, server.arg("city").c_str(), sizeof(weatherCity) - 1);
    weatherCity[sizeof(weatherCity) - 1] = '\0';
  }
  // Trigger immediate fetch
  lastWeatherFetch = 0;
  weatherCode = -1;
  if (wifiStaConnected) {
    fetchWeather();
    if (currentView == VIEW_CLOCK) drawClockView(true);
  }
  String j = "{\"lat\":"; j += String(weatherLat, 4);
  j += ",\"lon\":"; j += String(weatherLon, 4);
  j += ",\"city\":\""; j += weatherCity; j += "\"";
  j += ",\"temp\":\""; j += weatherTemp; j += "\"";
  j += ",\"code\":"; j += weatherCode; j += "}";
  server.send(200, "application/json", j);
}

void routeState() {
  String j = "{\"view\":"; j += currentView;
  j += ",\"busy\":";   j += busy        ? "true" : "false";
  j += ",\"term\":";   j += termMode    ? "true" : "false";
  j += ",\"bl\":";     j += backlightOn ? "true" : "false";
  j += ",\"speed\":";  j += animSpeed;
  j += "}";
  server.send(200, "application/json", j);
}

void routeNotFound() { server.send(404, "text/plain", "not found"); }

// ═════════════════════════════════════════════════════════════
//  MQTT — 表情映射与回调
// ═════════════════════════════════════════════════════════════

// 简易 JSON 解析：提取 expr / duration / loop 三个字段
void parseExpressionJson(const char* json, char* expr, unsigned long* duration, bool* loop) {
  *expr = '\0'; *duration = 0; *loop = false;

  const char* p = strstr(json, "\"expr\"");
  if (p) {
    p = strchr(p, ':');
    if (p) { p = strchr(p, '"'); if (p) { p++; int i = 0;
      while (*p && *p != '"' && i < 31) expr[i++] = *p++; expr[i] = '\0'; }
    }
  }
  p = strstr(json, "\"duration\"");
  if (p) { p = strchr(p, ':'); if (p) *duration = strtoul(p + 1, nullptr, 10); }
  p = strstr(json, "\"loop\"");
  if (p) *loop = strstr(p, "true") != nullptr;
}

// 解析用量 JSON：提取 tokens / cost / model 三个字段
void parseUsageJson(const char* json) {
  const char* p = strstr(json, "\"tokens\"");
  if (p) {
    p = strchr(p, '"'); if (p) p++;  // skip opening quote of "tokens"
    p = strchr(p, '"'); if (p) p++;
    if (p) { int i = 0;
      while (*p && *p != '"' && i < 31) usageTokens[i++] = *p++;
      usageTokens[i] = '\0';
    }
  }
  p = strstr(json, "\"cost\"");
  if (p) {
    p = strchr(p, '"'); if (p) p++;
    p = strchr(p, '"'); if (p) p++;
    if (p) { int i = 0;
      while (*p && *p != '"' && i < 31) usageCost[i++] = *p++;
      usageCost[i] = '\0';
    }
  }
  p = strstr(json, "\"model\"");
  if (p) {
    p = strchr(p, '"'); if (p) p++;
    p = strchr(p, '"'); if (p) p++;
    if (p) { int i = 0;
      while (*p && *p != '"' && i < 31) usageModel[i++] = *p++;
      usageModel[i] = '\0';
    }
  }
}

// 根据表情名称执行对应动作
void setExpression(const char* expr, unsigned long duration, bool loop) {
  currentExpr = String(expr);
  exprLooping = loop;
  clockWasActive = false;  // MQTT/external trigger → allow idle return to clock

  // 收到任何指令先打开背光（睡眠恢复）
  if (!backlightOn) setBacklight(true);

  if (strcmp(expr, "idle") == 0) {
    // idle → 回到时钟
    currentView = VIEW_CLOCK;
    clockWasActive = true;
    drawClockView(true);
  } else if (strcmp(expr, "happy") == 0) {
    animBgColor = currentWeekdayBg;
    currentView = VIEW_EYES_SQUISH;
    animSquishEyes();
  } else if (strcmp(expr, "sad") == 0) {
    animBgColor = currentWeekdayBg;
    currentView = VIEW_EYES_NORMAL;
    drawNormalEyes();
  } else if (strcmp(expr, "sleep") == 0) {
    currentView = VIEW_EYES_NORMAL;
    drawNormalEyes();
    delay(300);
    setBacklight(false);
  } else if (strcmp(expr, "wink") == 0) {
    animBgColor = currentWeekdayBg;
    currentView = VIEW_EYES_NORMAL;
    busy = true;
    drawNormalEyes(0, false); delay(200);
    drawNormalEyes(0, true);  delay(120);
    drawNormalEyes(0, false);
    busy = false;
  } else if (strcmp(expr, "blush") == 0) {
    animBgColor = currentWeekdayBg;
    currentView = VIEW_EYES_NORMAL;
    drawNormalEyes();
  } else if (strcmp(expr, "awe") == 0) {
    animBgColor = currentWeekdayBg;
    currentView = VIEW_EYES_NORMAL;
    animLogoReveal();
  } else if (strcmp(expr, "thinking") == 0) {
    currentView = VIEW_CODE;
    drawCodeView();
    termMode = true; termClear(); termFullRedraw();
  } else if (strcmp(expr, "angry") == 0) {
    animBgColor = currentWeekdayBg;
    currentView = VIEW_EYES_NORMAL;
    drawNormalEyes();
  } else if (strcmp(expr, "proud") == 0) {
    animBgColor = currentWeekdayBg;
    currentView = VIEW_EYES_NORMAL;
    animLogoReveal();
  }
  // 未知表情 → 保持当前状态，不做任何事

  if (duration > 0) {
    exprEndTime = millis() + duration;
  } else {
    exprEndTime = 0;
  }
}

// MQTT 消息回调
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char buf[256];
  unsigned int len = length < 255 ? length : 255;
  memcpy(buf, payload, len);
  buf[len] = '\0';

  // 按 topic 分发
  if (strstr(topic, "/usage") != nullptr) {
    // 用量数据 topic
    parseUsageJson(buf);
    Serial.printf("[MQTT] usage: tokens=%s cost=%s model=%s\n", usageTokens, usageCost, usageModel);
    // 如果当前在时钟页，立即刷新显示用量
    if (currentView == VIEW_CLOCK) drawClockView(true);
    return;
  }

  char expr[32];
  unsigned long duration;
  bool loop;
  parseExpressionJson(buf, expr, &duration, &loop);

  if (expr[0] != '\0') {
    Serial.printf("[MQTT] expression: %s  duration: %lu  loop: %d\n", expr, duration, loop);
    setExpression(expr, duration, loop);
  }
}

// MQTT 重连
void mqttReconnect() {
  static unsigned long lastAttempt = 0;
  if (millis() - lastAttempt < 5000) return;  // 5 秒重试间隔
  lastAttempt = millis();

  if (!wifiStaConnected) {
    Serial.println("[MQTT] waiting for WiFi STA...");
    return;
  }

  Serial.print("[MQTT] connecting to broker... ");
  String clientId = "clawd-" + String(DEVICE_ID) + "-" + String(random(0xffff), HEX);
  if (mqtt.connect(clientId.c_str())) {
    mqtt.subscribe(mqttTopic);
    mqtt.subscribe(usageTopic);
    Serial.printf("OK. subscribed to %s, %s\n", mqttTopic, usageTopic);
  } else {
    Serial.printf("FAILED, rc=%d\n", mqtt.state());
  }
}

// WiFi 事件回调 — 连接/断开时打印到串口
void wifiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("[WiFi] STA connected, IP: ");
      Serial.println(WiFi.localIP());
      wifiStaConnected = true;
      configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER, "pool.ntp.org");
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("[WiFi] STA disconnected, retrying...");
      wifiStaConnected = false;
      break;
    default: break;
  }
}

// ═════════════════════════════════════════════════════════════
//  SETUP
// ═════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);

  pinMode(TFT_BLK, OUTPUT);
  setBacklight(true);

  SPI.begin(8, -1, 10, TFT_CS);   // SCK=8, MOSI=10
  tft.init(240, 240);
  tft.setSPISpeed(40000000);
  tft.setRotation(1);
  initColours();

  // ── Boot splash ────────────────────────────────────────────
  tft.fillScreen(animBgColor);
  tft.setTextColor(C_WHITE); tft.setTextSize(3);
  tft.setCursor(DISP_W / 2 - 54, DISP_H / 2 - 22); tft.print("Clawd");
  tft.setCursor(DISP_W / 2 - 54, DISP_H / 2 + 14); tft.print("Mochi");
  delay(1200);

  // ── Logo shown once at startup ─────────────────────────────
  animLogoReveal();

  // ── Start WiFi (AP + STA) ──────────────────────────────────
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);

  // 注册 WiFi 事件回调（连接/断开时打印日志）
  WiFi.onEvent(wifiEvent);

  // 从 Preferences 加载保存的 WiFi 配置（有则覆盖默认值）
  loadWiFiCfg();

  // 尝试连接家庭 WiFi
  Serial.printf("[WiFi] STA connecting to: %s\n", STA_SSID);
  WiFi.begin(STA_SSID, STA_PASS);

  // 等 10 秒看是否能连上
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] STA connected! IP: ");
    Serial.println(WiFi.localIP());
    wifiStaConnected = true;
    // Init NTP
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER, "pool.ntp.org");
  } else {
    Serial.println("[WiFi] STA not connected, will retry in background");
  }

  // ── MQTT 初始化 ────────────────────────────────────────────
  snprintf(mqttTopic, sizeof(mqttTopic), "clawd/%s/expression", DEVICE_ID);
  snprintf(usageTopic, sizeof(usageTopic), "clawd/%s/usage", DEVICE_ID);
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  // ── WiFi info screen (stays until first web request) ───────
  tft.fillScreen(C_DARKBG);
  tft.fillRect(0, 0, DISP_W, 4, C_ORANGE);
  tft.setTextColor(C_WHITE);  tft.setTextSize(2);
  tft.setCursor(12, 16);  tft.print("WiFi: ClaWD-Mochi");
  tft.setTextColor(C_MUTED);  tft.setTextSize(1);
  tft.setCursor(12, 44);  tft.print("password: clawd1234");
  tft.setTextColor(C_WHITE);  tft.setTextSize(2);
  tft.setCursor(12, 68);  tft.print("Open browser:");
  tft.setTextColor(C_ORANGE); tft.setTextSize(2);
  tft.setCursor(12, 94);  tft.print("192.168.4.1");
  tft.setTextColor(C_MUTED);  tft.setTextSize(1);
  if (wifiStaConnected) {
    tft.setCursor(12, 124);
    tft.setTextColor(C_GREEN); tft.print("STA online.");
  } else {
    tft.setCursor(12, 124);
    tft.setTextColor(tft.color565(200, 160, 0));
    tft.print("STA connecting... ");
    tft.print(STA_SSID);
  }

  // ── Register routes ────────────────────────────────────────
  server.on("/",            HTTP_GET, routeRoot);
  server.on("/cmd",         HTTP_GET, routeCmd);
  server.on("/char",        HTTP_GET, routeChar);
  server.on("/speed",       HTTP_GET, routeSpeed);
  server.on("/redraw",      HTTP_GET, routeRedraw);
  server.on("/canvas",      HTTP_GET, routeCanvas);
  server.on("/draw/clear",  HTTP_GET, routeDrawClear);
  server.on("/draw/stroke", HTTP_GET, routeDrawStroke);
  server.on("/backlight",   HTTP_GET, routeBacklight);
  server.on("/wifi/set",     HTTP_GET, routeWifiSet);
  server.on("/wifi/state",   HTTP_GET, routeWifiState);
  server.on("/weather/set",  HTTP_GET, routeWeatherSet);
  server.on("/state",       HTTP_GET, routeState);
  server.onNotFound(routeNotFound);
  server.begin();

  // WiFi info stays on screen briefly, then switch to appropriate view
  delay(2000);
  if (wifiStaConnected) {
    currentView = VIEW_CLOCK;
    clockWasActive = true;
    drawClockView(true);
    fetchWeather();
  } else {
    drawNormalEyes();
  }
  lastUserAction = millis();
}

// ═════════════════════════════════════════════════════════════
//  LOOP
// ═════════════════════════════════════════════════════════════

void loop() {
  server.handleClient();

  // MQTT 保活 + 消息处理
  if (!mqtt.connected()) {
    mqttReconnect();
  }
  mqtt.loop();

  // 表情定时恢复（非循环模式下，到达 duration 后直接回到时钟）
  if (!exprLooping && exprEndTime > 0 && millis() > exprEndTime) {
    exprEndTime = 0;
    currentView = VIEW_CLOCK;
    clockWasActive = true;
    drawClockView(true);
  }

  // Weather fetch interval
  unsigned long now = millis();
  if (wifiStaConnected && weatherCode == -1 && (now - lastWeatherFetch > 10000)) {
    lastWeatherFetch = now;
    fetchWeather();
    if (currentView == VIEW_CLOCK) drawClockView(true);
  } else if (wifiStaConnected && (now - lastWeatherFetch > WEATHER_INTERVAL)) {
    lastWeatherFetch = now;
    fetchWeather();
    if (currentView == VIEW_CLOCK) drawClockView(true);
  }

  // NTP resync every hour
  static unsigned long lastNtpSync = 0;
  if (wifiStaConnected && (now - lastNtpSync > 3600000)) {
    lastNtpSync = now;
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER, "pool.ntp.org");
  }

  // Idle timeout → return to clock
  if (wifiStaConnected && !clockWasActive && currentView != VIEW_CLOCK
      && (now - lastUserAction > IDLE_TIMEOUT) && !busy && !termMode) {
    currentView = VIEW_CLOCK;
    drawClockView(true);
  }

  // 自动表情轮播（时钟页闲置时每 60 秒触发，显示 20 秒）
  if (!busy && !termMode && currentView == VIEW_CLOCK && !autoExprActive) {
    if (now - lastAutoExprTime > AUTO_EXPR_INTERVAL) {
      autoExprActive = true;
      const char* expr = AUTO_EXPR_LIST[autoExprIndex];
      setExpression(expr, AUTO_EXPR_DURATION, false);
      autoExprIndex = (autoExprIndex + 1) % 10;
      Serial.printf("[AUTO] expression #%d: %s\n", autoExprIndex, expr);
    }
  }
  // 自动表情结束后重置标记，等待下一轮
  if (autoExprActive && exprEndTime == 0 && currentView == VIEW_CLOCK) {
    autoExprActive = false;
    lastAutoExprTime = millis();
  }

  // Refresh clock every second
  if (currentView == VIEW_CLOCK) {
    static unsigned long lastClockRefresh = 0;
    if (now - lastClockRefresh > 1000) {
      lastClockRefresh = now;
      drawClockView();
    }
  }

  // Ambient idle animation (non-blocking, does not affect clock/weather/MQTT)
  if (!busy && !termMode) {
    static unsigned long lastAmbient = 0;
    if (currentView == VIEW_EYES_NORMAL && now - lastAmbient > 4000) {
      lastAmbient = now;
      drawNormalEyes(0, true);  delay(80);
      drawNormalEyes(0, false); delay(80);
      drawNormalEyes(0, true);  delay(80);
      drawNormalEyes(0, false);
    } else if (currentView == VIEW_EYES_SQUISH && now - lastAmbient > 5000) {
      lastAmbient = now;
      drawSquishEyes(true);  delay(100);
      drawSquishEyes(false); delay(100);
      drawSquishEyes(true);  delay(80);
      drawSquishEyes(false);
    }
  }
}
