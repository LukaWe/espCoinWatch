/*
 * ESP8266 Bitcoin Ticker with Web Configuration
 * =============================================
 * - Normal Mode: Connects to WiFi, fetches price, displays on OLED
 * - Config Mode: Creates AP "BTC-Ticker-Setup", serves config page at
 * 192.168.4.1
 * - Storage: LittleFS for persistent settings
 */

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <time.h>

// ============================================================================
// CONFIGURATION STRUCT
// ============================================================================
struct Config {
  char wifi_ssid[32] = "";
  char wifi_password[64] = "";
  char currency[10] = "EUR"; // EUR, USD, GBP, etc.
  char crypto[10] = "BTC";   // BTC, ETH, BNB, XRP, SOL, etc.
  uint8_t api_source =
      0; // 0=auto (Binance first), 1=Binance only, 2=CoinGecko first
  char time_format[3] = "DE"; // DE (24h) or US (12h)
  unsigned long poll_interval = 60000;
  int alert_low = 0;
  int alert_high = 0;
  uint8_t blink_pattern_low = 0; // 0=Slow, 1=Fast, 2=Strobe, 3=SOS
  uint8_t blink_pattern_high = 1;
  bool flip_display = false;
  // Weather Settings
  bool weather_enabled = false;     // Weather feature disabled by default
  char weather_name[11] = "Berlin"; // Location name for display (max 10 chars)
  float weather_lat = 52.52f;       // Default: Berlin latitude
  float weather_lon = 13.41f;       // Default: Berlin longitude
  unsigned long weather_poll = 1800000; // 30 minutes in milliseconds
  unsigned long btc_cycle = 120000;     // How long BTC screen shows (120s)
  unsigned long weather_cycle = 10000;  // How long weather screen shows (10s)
  uint8_t button_mode =
      0; // 0=auto cycle, 1=always weather, 2=on-demand, 3=d5 button (no cycle)
  bool wind_enabled = false; // Enable wind forecast display (default: off)
  // Security
  char web_password[32] = ""; // Empty = no password required
  // LED Alert Settings
  uint8_t alert_mode = 0;           // 0=Display, 1=LED, 2=Both
  uint8_t led_pin = 2;              // GPIO 2 (D4 on D1 Mini)
  bool led_inverted = true;         // True for D1 Mini (Active LOW)
  unsigned long alert_duration = 0; // 0=Infinite, >0=seconds
  // Touch Button Settings
  bool touch_enabled = false;
  bool d5_weather_mode = false; // D5 shows weather instead of factory reset
  bool d5_btc_mode = false;     // Inverted: Weather default, D5 shows BTC
  unsigned long d5_timeout =
      5000; // How long alternate screen stays after D5 release (ms)
};

Config config;
const char *CONFIG_FILE = "/config.json";

// ============================================================================
// HARDWARE SETTINGS
// ============================================================================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_I2C_ADDRESS 0x3C
#define SDA_PIN 4         // D2
#define SCL_PIN 5         // D1
#define RESET_BTN_PIN 14  // D5 (GPIO14) - Factory reset button
#define SCREEN_BTN_PIN 12 // D6 (GPIO12) - Screen toggle button

// TTP223 Touch Buttons
#define PIN_BTN_A 13 // D7 (GPIO13)
#define PIN_BTN_B 15 // D8 (GPIO15) - Boot fails if pulled HIGH
#define PIN_BTN_C 16 // D0 (GPIO16)

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ============================================================================
// GLOBAL STATE
// ============================================================================
ESP8266WebServer server(80);
DNSServer dnsServer;
bool g_isInConfigMode = false;

// API & Data variables
const char API_BINANCE_HOST[] PROGMEM = "api.binance.com";
const char API_COINGECKO_HOST[] PROGMEM = "api.coingecko.com";
const char API_COINBASE_HOST[] PROGMEM = "api.coinbase.com";
const char API_KRAKEN_HOST[] PROGMEM = "api.kraken.com";
const char API_OPENMETEO_HOST[] PROGMEM = "api.open-meteo.com";
WiFiClientSecure g_secureClient;

// BTC Price state
float g_currentPrice = 0.0f;
float g_priceChange24h = 0.0f;
bool g_wifiConnected = false;
bool g_hasValidData = false;
bool g_dataStale = false;
unsigned long g_lastPollTime = 0;
unsigned long g_lastSuccessTime = 0;
uint8_t g_currentProvider = 0; // 0 = Binance, 1 = CoinGecko
uint8_t g_consecutiveFailures = 0;

// Weather state
float g_temperature = 0.0f;
int g_humidity = 0;
int g_rainChance = 0;
bool g_hasWeatherData = false;
bool g_weatherStale = false;
unsigned long g_lastWeatherPoll = 0;
unsigned long g_lastWeatherSuccess = 0;
float g_windSpeed = 0.0f; // Wind speed in km/h
float g_windGusts = 0.0f; // Wind gusts in km/h (stored but not displayed)

// Screen cycling state
uint8_t g_currentScreen = 0; // 0 = BTC, 1 = Weather

unsigned long g_lastScreenSwitch = 0;

// Alert State
unsigned long g_alertStartTime = 0;
bool g_ledState = false; // Logical state (true=ON)

// Display helpers
char g_priceBuffer[20];
char g_changeBuffer[20];
char g_timeBuffer[20];

#define DEBUG_SERIAL 1

// ============================================================================
// LOG BUFFER (Ring buffer for web log viewer)
// ============================================================================
#define LOG_BUFFER_SIZE 20
#define LOG_LINE_LENGTH 100
char g_logBuffer[LOG_BUFFER_SIZE][LOG_LINE_LENGTH];
uint8_t g_logHead = 0;
uint8_t g_logCount = 0;

void addLogEntry(const char *msg) {
  if (msg == nullptr)
    return;
  strncpy(g_logBuffer[g_logHead], msg, LOG_LINE_LENGTH - 1);
  g_logBuffer[g_logHead][LOG_LINE_LENGTH - 1] = '\0';
  g_logHead = (g_logHead + 1) % LOG_BUFFER_SIZE;
  if (g_logCount < LOG_BUFFER_SIZE)
    g_logCount++;
}

// Overload for String objects
void addLogEntry(const String &msg) { addLogEntry(msg.c_str()); }

// Overload for PROGMEM strings (F() macro)
void addLogEntry(const __FlashStringHelper *msg) {
  char buf[LOG_LINE_LENGTH];
  strncpy_P(buf, (PGM_P)msg, LOG_LINE_LENGTH - 1);
  buf[LOG_LINE_LENGTH - 1] = '\0';
  addLogEntry(buf);
}

#if DEBUG_SERIAL
#define DEBUG_PRINT(x) Serial.print(x)
#define DEBUG_PRINTLN(x)                                                       \
  do {                                                                         \
    Serial.println(x);                                                         \
    addLogEntry(x);                                                            \
  } while (0)
#define DEBUG_PRINTF(...)                                                      \
  do {                                                                         \
    Serial.printf(__VA_ARGS__);                                                \
    char _buf[LOG_LINE_LENGTH];                                                \
    snprintf(_buf, sizeof(_buf), __VA_ARGS__);                                 \
    addLogEntry(_buf);                                                         \
  } while (0)
#else
#define DEBUG_PRINT(x)
#define DEBUG_PRINTLN(x) addLogEntry(x)
#define DEBUG_PRINTF(...)                                                      \
  do {                                                                         \
    char _buf[LOG_LINE_LENGTH];                                                \
    snprintf(_buf, sizeof(_buf), __VA_ARGS__);                                 \
    addLogEntry(_buf);                                                         \
  } while (0)
#endif

// ============================================================================
// FILESYSTEM HELPER FUNCTIONS
// ============================================================================
bool loadConfig() {
  if (!LittleFS.begin()) {
    DEBUG_PRINTLN(F("[FS] Failed to mount FS, formatting..."));
    LittleFS.format();
    if (!LittleFS.begin()) {
      DEBUG_PRINTLN(F("[FS] Failed to mount even after format"));
      return false;
    }
    DEBUG_PRINTLN(F("[FS] Filesystem formatted"));
  }

  if (!LittleFS.exists(CONFIG_FILE)) {
    DEBUG_PRINTLN(F("[FS] No config file found"));
    return false;
  }

  File file = LittleFS.open(CONFIG_FILE, "r");
  if (!file) {
    DEBUG_PRINTLN(F("[FS] Failed to open config"));
    return false;
  }

  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    DEBUG_PRINTLN(F("[FS] JSON parse error"));
    return false;
  }

  // Load values into struct with defaults
  strlcpy(config.wifi_ssid, doc["wifi_ssid"] | "", sizeof(config.wifi_ssid));
  strlcpy(config.wifi_password, doc["wifi_password"] | "",
          sizeof(config.wifi_password));
  strlcpy(config.currency, doc["currency"] | "EUR", sizeof(config.currency));
  strlcpy(config.crypto, doc["crypto"] | "BTC", sizeof(config.crypto));
  config.api_source = doc["api_source"] | 0;
  strlcpy(config.time_format, doc["time_format"] | "DE",
          sizeof(config.time_format));

  config.poll_interval = doc["poll_interval"] | 60000;
  config.alert_low = doc["alert_low"] | 0;
  config.alert_high = doc["alert_high"] | 0;
  config.blink_pattern_low = doc["blink_pattern_low"] | 0;
  config.blink_pattern_high = doc["blink_pattern_high"] | 1;
  config.flip_display = doc["flip_display"] | false;

  // Weather settings
  config.weather_enabled = doc["weather_enabled"] | false;
  strlcpy(config.weather_name, doc["weather_name"] | "Berlin",
          sizeof(config.weather_name));
  config.weather_lat = doc["weather_lat"] | 52.52f;
  config.weather_lon = doc["weather_lon"] | 13.41f;
  config.weather_poll = doc["weather_poll"] | 1800000UL;
  config.btc_cycle = doc["btc_cycle"] | 120000UL;
  config.weather_cycle = doc["weather_cycle"] | 10000UL;
  config.button_mode = doc["button_mode"] | 0;
  config.wind_enabled = doc["wind_enabled"] | false;

  // Security
  strlcpy(config.web_password, doc["web_password"] | "",
          sizeof(config.web_password));

  // LED Alert settings
  config.alert_mode = doc["alert_mode"] | 0;
  config.led_pin = doc["led_pin"] | 2;
  config.led_inverted = doc["led_inverted"] | true;
  config.alert_duration = doc["alert_duration"] | 0;
  config.touch_enabled = doc["touch_enabled"] | false;
  config.d5_weather_mode = doc["d5_weather_mode"] | false;
  config.d5_btc_mode = doc["d5_btc_mode"] | false;
  config.d5_timeout = doc["d5_timeout"] | 5000;

  DEBUG_PRINTLN(F("[FS] Configuration loaded!"));
  return true;
}

bool saveConfig() {
  StaticJsonDocument<1024> doc;
  doc["wifi_ssid"] = config.wifi_ssid;
  doc["wifi_password"] = config.wifi_password;
  doc["currency"] = config.currency;
  doc["crypto"] = config.crypto;
  doc["api_source"] = config.api_source;
  doc["time_format"] = config.time_format;
  doc["poll_interval"] = config.poll_interval;
  doc["alert_low"] = config.alert_low;
  doc["alert_high"] = config.alert_high;
  doc["blink_pattern_low"] = config.blink_pattern_low;
  doc["blink_pattern_high"] = config.blink_pattern_high;
  doc["flip_display"] = config.flip_display;
  doc["touch_enabled"] = config.touch_enabled;

  // Weather settings
  doc["weather_enabled"] = config.weather_enabled;
  doc["weather_name"] = config.weather_name;
  doc["weather_lat"] = config.weather_lat;
  doc["weather_lon"] = config.weather_lon;
  doc["weather_poll"] = config.weather_poll;
  doc["btc_cycle"] = config.btc_cycle;
  doc["weather_cycle"] = config.weather_cycle;
  doc["button_mode"] = config.button_mode;
  doc["wind_enabled"] = config.wind_enabled;

  // Security
  doc["web_password"] = config.web_password;

  // LED Alert settings
  doc["alert_mode"] = config.alert_mode;
  doc["led_pin"] = config.led_pin;
  doc["led_inverted"] = config.led_inverted;
  doc["alert_duration"] = config.alert_duration;
  doc["d5_weather_mode"] = config.d5_weather_mode;
  doc["d5_btc_mode"] = config.d5_btc_mode;
  doc["d5_timeout"] = config.d5_timeout;

  File file = LittleFS.open(CONFIG_FILE, "w");
  if (!file) {
    DEBUG_PRINTLN(F("[FS] Failed to open config for writing"));
    return false;
  }

  serializeJson(doc, file);
  file.close();
  DEBUG_PRINTLN(F("[FS] Configuration saved!"));
  return true;
}

// ============================================================================
// DISPLAY HELPERS
// ============================================================================
void drawWiFiIcon(int x, int y, bool connected) {
  if (connected) {
    display.drawPixel(x + 3, y + 6, SSD1306_WHITE);
    display.drawCircle(x + 3, y + 6, 2, SSD1306_WHITE);
    display.drawCircle(x + 3, y + 6, 4, SSD1306_WHITE);
  } else {
    display.drawLine(x, y, x + 6, y + 6, SSD1306_WHITE);
    display.drawLine(x + 6, y, x, y + 6, SSD1306_WHITE);
  }
}

void formatPrice(float price, char *buffer, size_t bufferSize) {
  long intPart = (long)(price + 0.5f);
  if (intPart >= 100000) {
    snprintf(buffer, bufferSize, "%ld.%03ld %s", intPart / 1000, intPart % 1000,
             config.currency);
  } else if (intPart >= 1000) {
    snprintf(buffer, bufferSize, "%ld.%03ld %s", intPart / 1000, intPart % 1000,
             config.currency);
  } else {
    snprintf(buffer, bufferSize, "%ld %s", intPart, config.currency);
  }
}

void formatChange(float change, char *buffer, size_t bufferSize) {
  char sign = change >= 0 ? '+' : '-';
  float absChange = change >= 0 ? change : -change;
  int intPart = (int)absChange;
  int decPart = (int)((absChange - intPart) * 100 + 0.5f);
  snprintf(buffer, bufferSize, "%c%d.%02d%%", sign, intPart, decPart);
}

void getCurrentTimeStr(char *buffer, size_t bufferSize) {
  time_t now = time(nullptr);
  struct tm *timeinfo = localtime(&now);

  if (timeinfo->tm_year > 100) {
    if (strcmp(config.time_format, "US") == 0) {
      int hour12 = timeinfo->tm_hour % 12;
      if (hour12 == 0)
        hour12 = 12;
      const char *ampm = (timeinfo->tm_hour >= 12) ? "PM" : "AM";
      snprintf(buffer, bufferSize, "%d:%02d %s", hour12, timeinfo->tm_min,
               ampm);
    } else {
      snprintf(buffer, bufferSize, "%02d:%02d Uhr", timeinfo->tm_hour,
               timeinfo->tm_min);
    }
  } else {
    snprintf(buffer, bufferSize, "--:--");
  }
}

void drawTrendArrow(int x, int y, bool up) {
  if (up) {
    display.drawLine(x + 3, y, x, y + 5, SSD1306_WHITE);
    display.drawLine(x + 3, y, x + 6, y + 5, SSD1306_WHITE);
    display.drawLine(x + 3, y, x + 3, y + 8, SSD1306_WHITE);
  } else {
    display.drawLine(x + 3, y + 8, x, y + 3, SSD1306_WHITE);
    display.drawLine(x + 3, y + 8, x + 6, y + 3, SSD1306_WHITE);
    display.drawLine(x + 3, y + 8, x + 3, y, SSD1306_WHITE);
  }
}

void updateDisplay() {
  display.clearDisplay();

  // Header - show selected crypto and API provider
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(config.crypto);
  display.print(F("/"));
  display.print(config.currency);
  display.print(F(" "));
  // Provider abbreviations: 0=Binance, 1=CoinGecko, 2=Coinbase, 3=Kraken
  switch (g_currentProvider) {
  case 0:
    display.print(F("Binance"));
    break;
  case 1:
    display.print(F("CoinGecko"));
    break;
  case 2:
    display.print(F("Coinbase"));
    break;
  case 3:
    display.print(F("Kraken"));
    break;
  }

  if (g_dataStale) {
    display.setCursor(94, 0);
    display.print(F("OLD"));
  }
  drawWiFiIcon(118, 0, g_wifiConnected);
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  // Price
  if (g_hasValidData) {
    formatPrice(g_currentPrice, g_priceBuffer, sizeof(g_priceBuffer));
    int priceLen = strlen(g_priceBuffer);
    int textSize = (priceLen <= 12) ? 2 : 1; // Basic dynamic sizing
    int charWidth = (textSize == 2) ? 12 : 6;
    int priceX = max(0, (128 - (priceLen * charWidth)) / 2);

    display.setTextSize(textSize);
    display.setCursor(priceX, 18);
    display.print(g_priceBuffer);

    // Change & Trend
    display.setTextSize(1);
    bool isUp = g_priceChange24h >= 0;
    drawTrendArrow(10, 40, isUp);
    formatChange(g_priceChange24h, g_changeBuffer, sizeof(g_changeBuffer));
    display.setCursor(22, 42);
    display.print(g_changeBuffer);
    display.print(F(" 24h"));
  } else {
    display.setTextSize(1);
    display.setCursor(20, 25);
    display.print(F("Fetching price..."));
  }

  // Footer
  display.setTextSize(1);
  display.setCursor(0, 56);
  display.print(F("Updated: "));
  getCurrentTimeStr(g_timeBuffer, sizeof(g_timeBuffer));
  display.print(g_timeBuffer);

  display.display();
}

// ============================================================================
// WEB SERVER HANDLERS
// ============================================================================

// Authentication helper - returns true if access allowed
bool requireAuth() {
  // Skip auth in config mode (AP mode) - no password set yet
  if (g_isInConfigMode)
    return true;
  // No password set = no auth required
  if (strlen(config.web_password) == 0)
    return true;
  // Check HTTP Basic Auth
  if (!server.authenticate("admin", config.web_password)) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

// Log endpoint - returns JSON array of recent log entries
// Log endpoint - returns JSON array of recent log entries using streaming to
// save memory
void handleLogs() {
  if (!requireAuth())
    return;

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "[");

  for (int i = 0; i < g_logCount; i++) {
    int idx = (g_logHead - g_logCount + i + LOG_BUFFER_SIZE) % LOG_BUFFER_SIZE;
    if (i > 0)
      server.sendContent(",");

    // Build JSON entry string safely
    String entry = "\"";
    const char *src = g_logBuffer[idx];
    while (*src) {
      char c = *src++;
      switch (c) {
      case '\\':
        entry += "\\\\";
        break;
      case '"':
        entry += "\\\"";
        break;
      case '\n':
        entry += "\\n";
        break;
      case '\r':
        entry += "\\r";
        break;
      case '\t':
        entry += "\\t";
        break;
      default:
        entry += c;
        break;
      }
    }
    entry += "\"";
    server.sendContent(entry);
  }
  server.sendContent("]");
  server.sendContent(""); // Terminate chunked transfer
}

// Refactored handleRoot using Chunked Transfer Encoding to prevent OOM
void handleRoot() {
  if (!requireAuth())
    return;

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", ""); // Start chunked transfer

  String chunk =
      F("<!DOCTYPE html><html><head><title>BTC Ticker Setup</title>");
  chunk +=
      F("<meta name='viewport' content='width=device-width, initial-scale=1'>");
  chunk += F("<style>body{font-family:Arial;max-width:400px;margin:0 "
             "auto;padding:20px;}");
  chunk += F("input,select{width:100%;padding:10px;margin:5px "
             "0;box-sizing:border-box;}");
  chunk += F("input[type=submit]{background:#4CAF50;color:white;border:none;"
             "cursor:pointer;}");
  chunk += F("h3{margin-top:20px;border-top:1px solid #ccc;padding-top:15px;}");
  chunk += F("</style></head><body>");
  chunk += F("<h2>BTC Ticker Configuration</h2>");
  chunk += F("<form action='/save' method='POST'>");
  server.sendContent(chunk);

  // ---- Ticker Settings Section (Collapsible, open by default) ----
  chunk = F("<details id='tickerDetails' open>");
  chunk +=
      F("<summary style='cursor:pointer;font-weight:bold;font-size:1.1em;'>");
  chunk += F("Ticker Settings</summary>");
  chunk += F("<div style='padding:10px 0;'>");
  chunk += F("<label>WiFi SSID:</label><input type='text' name='ssid' value='");
  chunk += config.wifi_ssid;
  chunk += F("' required>");
  chunk += F("<label>WiFi Password:</label><input type='password' name='pass' "
             "value='");
  chunk += config.wifi_password;
  chunk += F("' required>");
  server.sendContent(chunk);

  chunk = F("<label>Currency:</label><select name='curr'>");
  const char *currencies[] = {"EUR", "USD", "GBP", "CAD",
                              "AUD", "JPY", "CHF", "CNY"};
  for (int i = 0; i < 8; i++) {
    chunk += F("<option value='");
    chunk += currencies[i];
    chunk += F("'");
    if (strcmp(config.currency, currencies[i]) == 0)
      chunk += F(" selected");
    chunk += F(">");
    chunk += currencies[i];
    chunk += F("</option>");
  }
  chunk += F("</select>");
  server.sendContent(chunk);

  // Crypto selection dropdown
  chunk = F("<label>Cryptocurrency:</label><select name='crypt'>");
  const char *cryptos[] = {"BTC",  "ETH", "BNB", "XRP", "SOL", "TRX",
                           "DOGE", "ADA", "XMR", "LTC", "XLM"};
  const char *cryptoNames[] = {"Bitcoin", "Ethereum", "BNB",      "XRP",
                               "Solana",  "TRON",     "Dogecoin", "Cardano",
                               "Monero",  "Litecoin", "Stellar"};
  for (int i = 0; i < 11; i++) {
    chunk += F("<option value='");
    chunk += cryptos[i];
    chunk += F("'");
    if (strcmp(config.crypto, cryptos[i]) == 0)
      chunk += F(" selected");
    chunk += F(">");
    chunk += cryptoNames[i];
    chunk += F(" (");
    chunk += cryptos[i];
    chunk += F(")</option>");
  }
  chunk += F("</select>");
  server.sendContent(chunk);

  // API Source selection, Poll Interval, Time Format
  chunk = F("<label>API Source:</label><select name='apisrc'>");
  const char *apiLabels[] = {"Auto (Binance first)", "Binance only",
                             "CoinGecko first", "Coinbase first",
                             "Kraken first"};
  for (int i = 0; i < 5; i++) {
    chunk += F("<option value='");
    chunk += i;
    chunk += F("'");
    if (config.api_source == i)
      chunk += F(" selected");
    chunk += F(">");
    chunk += apiLabels[i];
    chunk += F("</option>");
  }
  chunk += F("</select>");

  chunk += F("<label>Poll Interval (ms):</label><input type='number' "
             "name='poll' value='");
  chunk += config.poll_interval;
  chunk += F("'>");

  chunk += F("<label>Time Format:</label><select name='time'>");
  chunk += F("<option value='DE'");
  if (strcmp(config.time_format, "DE") == 0)
    chunk += F(" selected");
  chunk += F(">DE (24h + Uhr)</option>");
  chunk += F("<option value='US'");
  if (strcmp(config.time_format, "US") == 0)
    chunk += F(" selected");
  chunk += F(">US (12h + AM/PM)</option>");
  chunk += F("</select>");
  server.sendContent(chunk);

  // Alerts logic
  // Alerts logic
  chunk = F("<label>Alert Low (Price):</label><input type='number' name='alow' "
            "value='");
  chunk += config.alert_low;
  chunk += F("'>");
  chunk += F("<label>Alert High (Price):</label><input type='number' "
             "name='ahigh' value='");
  chunk += config.alert_high;
  chunk += F("'>");

  // Blink Pattern Selectors
  const char *patterns[] = {"Slow (1s)", "Fast (250ms)", "Strobe (50ms)",
                            "SOS"};

  chunk += F("<label>Low Alert Pattern:</label><select name='bl_low'>");
  for (int i = 0; i < 4; i++) {
    chunk += F("<option value='");
    chunk += i;
    chunk += F("'");
    if (config.blink_pattern_low == i)
      chunk += F(" selected");
    chunk += F(">");
    chunk += patterns[i];
    chunk += F("</option>");
  }
  chunk += F("</select>");

  chunk += F("<label>High Alert Pattern:</label><select name='bl_high'>");
  for (int i = 0; i < 4; i++) {
    chunk += F("<option value='");
    chunk += i;
    chunk += F("'");
    if (config.blink_pattern_high == i)
      chunk += F(" selected");
    chunk += F(">");
    chunk += patterns[i];
    chunk += F("</option>");
  }
  chunk += F("</select>");

  server.sendContent(chunk);

  // LED Alerts
  chunk = F("<label>Alert Mode:</label><select name='amode'>");
  const char *modes[] = {"Display Only", "LED Only", "Both"};
  for (int i = 0; i < 3; i++) {
    chunk += F("<option value='");
    chunk += i;
    chunk += F("'");
    if (config.alert_mode == i)
      chunk += F(" selected");
    chunk += F(">");
    chunk += modes[i];
    chunk += F("</option>");
  }
  chunk += F("</select>");

  chunk += F(
      "<label>LED Pin (D4=2):</label><input type='number' name='lpin' value='");
  chunk += config.led_pin;
  chunk += F("'>");

  chunk += F("<label><input type='checkbox' name='linv' value='1'");
  if (config.led_inverted)
    chunk += F(" checked");
  chunk += F("> LED Inverted Logic (Low=On)</label><br>");

  chunk += F("<label>Alert Duration (sec, 0=Inf):</label><input type='number' "
             "name='adur' value='");
  chunk += config.alert_duration;
  chunk += F("'>");

  chunk += F("<label><input type='checkbox' name='flip' value='1'");
  if (config.flip_display)
    chunk += F(" checked");
  chunk += F("><label for='flip'> Flip Display (180&deg;)</label><br>");
  chunk += F("</div></details><br>");
  server.sendContent(chunk); // Send Alert Settings section

  // Touch Settings (Simple Enable)
  chunk = F("<details id='touchDetails'>");
  chunk +=
      F("<summary style='cursor:pointer;font-weight:bold;font-size:1.1em;'>");
  chunk += F("Touch Settings</summary>");
  chunk += F("<div style='padding:10px 0;'>");
  chunk += F("<label><input type='checkbox' name='touch_en' value='1'");
  if (config.touch_enabled)
    chunk += F(" checked");
  chunk += F("> Enable Touch Buttons (D0, D7, D8)</label><br>");
  chunk += F("<small>Connect TTP223 (Active High). D8 MUST be LOW at "
             "boot.</small><br><br>");
  chunk +=
      F("<label><input type='checkbox' name='d5_weather' id='d5w' value='1' "
        "onchange=\"if(this.checked)document.getElementById('d5b').checked="
        "false;\"");
  if (config.d5_weather_mode)
    chunk += F(" checked");
  chunk += F("> D5 = Weather Button (BTC default)</label><br>");
  chunk += F("<small>Press D5 to show weather. Returns to BTC after "
             "timeout.</small><br><br>");

  chunk += F("<label><input type='checkbox' name='d5_btc' id='d5b' value='1' "
             "onchange=\"if(this.checked)document.getElementById('d5w')."
             "checked=false;\"");
  if (config.d5_btc_mode)
    chunk += F(" checked");
  chunk += F("> D5 = BTC Button (Weather default)</label><br>");
  chunk += F(
      "<small>Weather is always shown. Press D5 to show BTC.</small><br><br>");

  chunk += F("<label>Screen Timeout (seconds, 0=instant):</label>");
  chunk += F("<input type='number' name='d5_timeout' min='0' max='60' value='");
  chunk += (config.d5_timeout / 1000);
  chunk += F("'><br>");
  chunk += F(
      "<small>How long alternate screen stays after releasing D5.</small><br>");
  chunk += F("</div></details><br>");

  server.sendContent(chunk);

  // ---- Weather Settings Section (Collapsible) ----
  chunk = F("<details id='weatherDetails'>");
  chunk +=
      F("<summary "
        "style='cursor:pointer;font-weight:bold;font-size:1.1em;margin-top:"
        "15px;'>");
  chunk += F("Weather Settings</summary>");
  chunk += F("<div style='padding:10px 0;'>");
  chunk += F("<label><input type='checkbox' name='weather_en' value='1'");
  if (config.weather_enabled)
    chunk += F(" checked");
  chunk += F("> Enable Weather Display</label><br>");
  chunk += F("<label><input type='checkbox' name='wind_en' value='1'");
  if (config.wind_enabled)
    chunk += F(" checked");
  chunk += F("> Enable Wind Forecast</label><br>");
  chunk +=
      F("<small>Shows wind speed in a 4-metric grid layout</small><br><br>");
  server.sendContent(chunk);

  // City presets dropdown
  chunk = F("<label>City Preset:</label>");
  chunk += F("<select id='citySelect' onchange='fillCoords()'>");
  chunk += F("<option value=''>-- Custom --</option>");
  chunk += F("<option value='52.52,13.41,Berlin'>Berlin</option>");
  chunk += F("<option value='48.1351,11.5820,Munich'>Munich</option>");
  chunk += F("<option value='53.5511,9.9937,Hamburg'>Hamburg</option>");
  chunk += F("<option value='50.1109,8.6821,Frankfurt'>Frankfurt</option>");
  chunk += F("<option value='49.4521,11.0767,Nuremberg'>Nuremberg</option>");
  chunk += F("<option value='49.0134,12.1016,Regensburg'>Regensburg</option>");
  chunk += F("<option value='50.9375,6.9603,Cologne'>Cologne</option>");
  chunk += F("<option value='48.7758,9.1829,Stuttgart'>Stuttgart</option>");
  chunk += F("<option value='51.2277,6.7735,Dusseldorf'>Dusseldorf</option>");
  chunk += F("<option value='51.3397,12.3731,Leipzig'>Leipzig</option>");
  chunk += F("<option value='51.4556,7.0116,Essen'>Essen</option>");
  chunk += F("<option value='51.5136,7.4653,Dortmund'>Dortmund</option>");
  chunk += F("<option value='48.5667,13.4319,Passau'>Passau</option>");
  chunk += F("<option value='49.7913,9.9534,Wurzburg'>Wurzburg</option>");
  chunk += F("<option value='48.3705,10.8978,Augsburg'>Augsburg</option>");
  chunk += F("<option value='49.4451,11.8635,Amberg'>Amberg</option>");
  chunk += F("</select>");
  server.sendContent(chunk);

  chunk = F("<script>");
  chunk +=
      F("function fillCoords(){var s=document.getElementById('citySelect');");
  chunk += F("if(s.value){var c=s.value.split(',');");
  chunk += F("document.getElementsByName('wlat')[0].value=c[0];");
  chunk += F("document.getElementsByName('wlon')[0].value=c[1];");
  chunk += F("document.getElementsByName('wname')[0].value=c[2];}}");
  chunk += F("</script>");
  chunk += F("<label>Location Name (max 10 chars):</label>");
  chunk += F("<input type='text' name='wname' maxlength='10' value='");
  chunk += config.weather_name;
  chunk += F("'>");
  chunk += F("<label>Latitude:</label>");
  chunk += F("<input type='number' name='wlat' step='0.0001' value='");
  chunk += String(config.weather_lat, 4);
  chunk += F("'>");
  chunk += F("<label>Longitude:</label>");
  chunk += F("<input type='number' name='wlon' step='0.0001' value='");
  chunk += String(config.weather_lon, 4);
  chunk += F("'>");
  server.sendContent(chunk);

  chunk = F("<label>BTC Screen Duration (seconds):</label>");
  chunk += F("<input type='number' name='btccycle' value='");
  chunk += (config.btc_cycle / 1000);
  chunk += F("'>");
  chunk += F("<label>Weather Screen Duration (seconds):</label>");
  chunk += F("<input type='number' name='wcycle' value='");
  chunk += (config.weather_cycle / 1000);
  chunk += F("'>");
  chunk += F("<label>Weather Poll Interval (minutes):</label>");
  chunk += F("<input type='number' name='wpoll' value='");
  chunk += (config.weather_poll / 60000);
  chunk += F("'>");

  chunk += F("<label>Button Mode:</label><select name='wbtn'>");
  chunk += F("<option value='0'");
  if (config.button_mode == 0)
    chunk += F(" selected");
  chunk += F(">Auto Cycle</option>");
  chunk += F("<option value='1'");
  if (config.button_mode == 1)
    chunk += F(" selected");
  chunk += F(">Always Weather</option>");
  chunk += F("<option value='2'");
  if (config.button_mode == 2)
    chunk += F(" selected");
  chunk += F(">On-Demand (BTC default)</option>");
  chunk += F("<option value='3'");
  if (config.button_mode == 3)
    chunk += F(" selected");
  chunk += F(">Button Press (no cycle)</option>");
  chunk += F("</select>");
  chunk += F("<small>On-Demand: Weather shown only when button pressed, "
             "auto-returns to BTC<br>Button Press: Only D5 triggers weather, "
             "no auto-cycle.</small><br>");
  chunk += F("</div></details><br>");
  server.sendContent(chunk);

  // ---- Security Section ----
  chunk = F("<h3>Security</h3>");
  chunk += F("<label>Web Password (leave empty for no auth):</label>");
  chunk +=
      F("<input type='password' name='webpw' placeholder='Set password...' "
        "value='");
  chunk += config.web_password;
  chunk += F("'>");
  chunk += F("<small>Username is 'admin'. Leave empty to disable "
             "authentication.</small><br><br>");
  server.sendContent(chunk);

  // ---- Device Logs Section ----
  chunk = F("<h3>Device Logs</h3>");
  chunk += F("<details id='logDetails'>");
  chunk += F("<summary style='cursor:pointer;'>Show Logs</summary>");
  chunk += F("<div style='margin-top:10px;'>");
  chunk += F("<label><input type='checkbox' id='liveLog'> Live Update (every "
             "2s)</label>");
  chunk += F(
      " <button type='button' onclick='fetchLogs()'>Refresh</button><br><br>");
  chunk += F("<pre id='logArea' "
             "style='background:#222;color:#0f0;padding:10px;max-height:200px;"
             "overflow-y:auto;font-size:11px;white-space:pre-wrap;word-wrap:"
             "break-word;'>Loading...</pre>");
  chunk += F("</div></details><br>");
  server.sendContent(chunk);

  // JavaScript for log fetching
  chunk = F("<script>");
  chunk += F("(function(){");
  chunk += F("var logArea=document.getElementById('logArea');");
  chunk += F("var logDetails=document.getElementById('logDetails');");
  chunk += F("var liveLog=document.getElementById('liveLog');");
  chunk += F("var isFetching=false;");
  chunk += F("function fetchLogs(e){");
  chunk += F("if(e)e.preventDefault();");
  chunk += F("if(isFetching)return;");
  chunk += F("isFetching=true;");
  chunk += F("var xhr=new XMLHttpRequest();");
  chunk += F("xhr.open('GET','/logs',true);");
  chunk += F("xhr.onreadystatechange=function(){");
  chunk += F("if(xhr.readyState===4){");
  chunk += F("isFetching=false;");
  chunk += F("if(xhr.status===200){");
  chunk += F("try{var d=JSON.parse(xhr.responseText);");
  chunk += F("if(d.length===0){logArea.textContent='No log entries yet.';}");
  chunk += F("else{logArea.textContent=d.join('\\n');logArea.scrollTop=logArea."
             "scrollHeight;}");
  chunk += F("}catch(ex){logArea.textContent='Parse error';}");
  chunk += F("}else{logArea.textContent='Error: '+xhr.status;}}};");
  chunk += F("xhr.send();");
  chunk += F("}");
  chunk += F("window.fetchLogs=fetchLogs;");
  chunk += F("logDetails.addEventListener('toggle',function(){if(this.open)"
             "fetchLogs();});");
  chunk += F("setInterval(function(){if(liveLog&&liveLog.checked&&logDetails."
             "open)fetchLogs();},2000);");
  chunk += F("})();");
  chunk += F("</script>");

  chunk +=
      F("<input type='submit' value='Save &amp; Reboot'></form></body></html>");
  server.sendContent(chunk);
  server.sendContent(""); // End chunked transfer
}

void handleSave() {
  if (!requireAuth())
    return;
  if (server.hasArg("ssid"))
    strlcpy(config.wifi_ssid, server.arg("ssid").c_str(),
            sizeof(config.wifi_ssid));
  if (server.hasArg("pass"))
    strlcpy(config.wifi_password, server.arg("pass").c_str(),
            sizeof(config.wifi_password));
  if (server.hasArg("curr"))
    strlcpy(config.currency, server.arg("curr").c_str(),
            sizeof(config.currency));
  if (server.hasArg("crypt"))
    strlcpy(config.crypto, server.arg("crypt").c_str(), sizeof(config.crypto));
  if (server.hasArg("apisrc"))
    config.api_source = server.arg("apisrc").toInt();
  if (server.hasArg("poll"))
    config.poll_interval = server.arg("poll").toInt();
  if (server.hasArg("time"))
    strlcpy(config.time_format, server.arg("time").c_str(),
            sizeof(config.time_format));
  if (server.hasArg("alow"))
    config.alert_low = server.arg("alow").toInt();
  if (server.hasArg("ahigh"))
    config.alert_high = server.arg("ahigh").toInt();
  if (server.hasArg("bl_low"))
    config.blink_pattern_low = server.arg("bl_low").toInt();
  if (server.hasArg("bl_high"))
    config.blink_pattern_high = server.arg("bl_high").toInt();

  config.flip_display = server.hasArg("flip");
  config.touch_enabled = server.hasArg("touch_en");

  // LED Alert Logic defaults for checkboxes
  config.led_inverted = false;
  if (server.hasArg("amode"))
    config.alert_mode = server.arg("amode").toInt();
  if (server.hasArg("lpin"))
    config.led_pin = server.arg("lpin").toInt();
  if (server.hasArg("linv"))
    config.led_inverted = true;
  if (server.hasArg("adur"))
    config.alert_duration = server.arg("adur").toInt();

  // Weather settings
  config.weather_enabled = server.hasArg("weather_en");
  if (server.hasArg("wname"))
    strlcpy(config.weather_name, server.arg("wname").c_str(),
            sizeof(config.weather_name));
  if (server.hasArg("wlat"))
    config.weather_lat = server.arg("wlat").toFloat();
  if (server.hasArg("wlon"))
    config.weather_lon = server.arg("wlon").toFloat();
  if (server.hasArg("btccycle"))
    config.btc_cycle = server.arg("btccycle").toInt() * 1000UL;
  if (server.hasArg("wcycle"))
    config.weather_cycle = server.arg("wcycle").toInt() * 1000UL;
  if (server.hasArg("wpoll"))
    config.weather_poll = server.arg("wpoll").toInt() * 60000UL;
  if (server.hasArg("wbtn"))
    config.button_mode = server.arg("wbtn").toInt();
  config.wind_enabled = server.hasArg("wind_en");

  // Touch settings - D5 modes are mutually exclusive
  config.d5_weather_mode = server.hasArg("d5_weather");
  config.d5_btc_mode = server.hasArg("d5_btc");

  // If both are checked, prefer the newer selection (d5_btc_mode)
  if (config.d5_weather_mode && config.d5_btc_mode) {
    config.d5_weather_mode = false;
  }

  // Parse timeout (convert seconds to ms)
  if (server.hasArg("d5_timeout"))
    config.d5_timeout = server.arg("d5_timeout").toInt() * 1000UL;

  // Force button mode 3 when either D5 mode is active
  if (config.d5_weather_mode || config.d5_btc_mode) {
    config.button_mode = 3; // Force "Button Press (no cycle)" mode
  }

  // Security
  if (server.hasArg("webpw"))
    strlcpy(config.web_password, server.arg("webpw").c_str(),
            sizeof(config.web_password));

  saveConfig();

  String html = F("<!DOCTYPE html><html><body><h2>Settings "
                  "Saved!</h2><p>Device is rebooting...</p></body></html>");
  server.send(200, "text/html", html);

  delay(1000);
  ESP.restart();
}

// ============================================================================
// MODES
// ============================================================================
void startConfigMode() {
  g_isInConfigMode = true;
  DEBUG_PRINTLN(F("[Mode] Starting Configuration Portal"));

  WiFi.mode(WIFI_AP);
  WiFi.softAP("BTC-Ticker-Setup", ""); // Open AP

  // Setup DNS for captive portal
  dnsServer.start(53, "*", WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/save", handleSave);
  server.on("/logs", handleLogs);
  // Handler for captive portal (redirect all)
  server.onNotFound([]() {
    server.sendHeader("Location",
                      String("http://") + WiFi.softAPIP().toString(), true);
    server.send(302, "text/plain", "");
  });

  server.begin();

  DEBUG_PRINT(F("[AP] IP Address: "));
  DEBUG_PRINTLN(WiFi.softAPIP().toString());

  // Show config info on OLED
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("SETUP MODE"));
  display.println(F("---------------------"));
  display.println(F("Connect WiFi:"));
  display.println(F("BTC-Ticker-Setup"));
  display.println(F(""));
  display.println(F("Go to:"));
  display.print(WiFi.softAPIP());
  display.display();
}

// ============================================================================
// API FETCHERS (Updated to use config struct)
// ============================================================================
bool fetchPriceBinance() {
  DEBUG_PRINTLN(F("[API] Trying Binance..."));
  HTTPClient http;

  // Map USD -> USDT for Binance
  const char *binanceCurrency = config.currency;
  if (strcmp(config.currency, "USD") == 0)
    binanceCurrency = "USDT";

  char url[128];
  snprintf(url, sizeof(url),
           "https://api.binance.com/api/v3/ticker/24hr?symbol=%s%s",
           config.crypto, binanceCurrency);

  g_secureClient.stop(); // Clean up previous connection state
  g_secureClient.setInsecure();
  http.begin(g_secureClient, url);
  http.setTimeout(
      5000); // 5s timeout to prevent WDT reset but allow SSL handshake

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    StaticJsonDocument<512> doc;
    String payload = http.getString();
    http.end();

    DeserializationError error = deserializeJson(doc, payload);

    if (!error) {
      const char *lastPrice = doc["lastPrice"];
      const char *changePercent = doc["priceChangePercent"];
      if (lastPrice && changePercent) {
        g_currentPrice = atof(lastPrice);
        g_priceChange24h = atof(changePercent);
        g_hasValidData = true;
        g_dataStale = false;
        g_lastSuccessTime = millis();
        DEBUG_PRINTF("[API] Binance OK: %.2f (%.2f%%)\n", g_currentPrice,
                     g_priceChange24h);
        return true;
      } else {
        DEBUG_PRINTLN(
            F("[API] Binance: Missing keys (lastPrice/priceChangePercent)"));
        DEBUG_PRINT(F("[API] Response: "));
        DEBUG_PRINTLN(payload.substring(0, 100)); // Print start of payload
      }
    } else {
      DEBUG_PRINT(F("[API] Binance JSON error: "));
      DEBUG_PRINTLN(error.c_str());
      DEBUG_PRINT(F("[API] Response: "));
      DEBUG_PRINTLN(payload.substring(0, 100));
    }
  } else {
    DEBUG_PRINTF("[API] Binance HTTP error: %d\n", httpCode);
  }
  http.end();
  return false;
}

bool fetchPriceCoinGecko() {
  DEBUG_PRINTLN(F("[API] Trying CoinGecko..."));
  HTTPClient http;

  // Map crypto symbol to CoinGecko ID
  const char *geckoId = "bitcoin"; // default
  if (strcmp(config.crypto, "ETH") == 0)
    geckoId = "ethereum";
  else if (strcmp(config.crypto, "BNB") == 0)
    geckoId = "binancecoin";
  else if (strcmp(config.crypto, "XRP") == 0)
    geckoId = "ripple";
  else if (strcmp(config.crypto, "SOL") == 0)
    geckoId = "solana";
  else if (strcmp(config.crypto, "TRX") == 0)
    geckoId = "tron";
  else if (strcmp(config.crypto, "DOGE") == 0)
    geckoId = "dogecoin";
  else if (strcmp(config.crypto, "ADA") == 0)
    geckoId = "cardano";
  else if (strcmp(config.crypto, "XMR") == 0)
    geckoId = "monero";
  else if (strcmp(config.crypto, "LTC") == 0)
    geckoId = "litecoin";
  else if (strcmp(config.crypto, "XLM") == 0)
    geckoId = "stellar";

  char currencyLower[10];
  for (int i = 0; config.currency[i] && i < 9; i++) {
    currencyLower[i] = tolower(config.currency[i]);
    currencyLower[i + 1] = '\0';
  }

  char url[256];
  snprintf(url, sizeof(url),
           "https://api.coingecko.com/api/v3/simple/"
           "price?ids=%s&vs_currencies=%s&include_24hr_change=true",
           geckoId, currencyLower);

  g_secureClient.stop();
  g_secureClient.setInsecure();
  http.begin(g_secureClient, url);
  http.setTimeout(5000);

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    http.end();

    StaticJsonDocument<512> doc;
    if (!deserializeJson(doc, payload)) {
      JsonObject crypto = doc[geckoId];
      if (crypto.containsKey(currencyLower)) {
        g_currentPrice = crypto[currencyLower].as<float>();

        char changeKey[32];
        snprintf(changeKey, sizeof(changeKey), "%s_24h_change", currencyLower);
        g_priceChange24h = crypto[changeKey].as<float>();
        g_hasValidData = true;
        g_dataStale = false;
        g_lastSuccessTime = millis();
        DEBUG_PRINTF("[API] CoinGecko OK: %.2f (%.2f%%)\n", g_currentPrice,
                     g_priceChange24h);
        return true;
      } else {
        DEBUG_PRINTLN(F("[API] CoinGecko: currency key not found"));
      }
    } else {
      DEBUG_PRINTLN(F("[API] CoinGecko JSON error"));
    }
  } else {
    DEBUG_PRINTF("[API] CoinGecko HTTP error: %d\n", httpCode);
    http.end();
  }
  return false;
}

// Coinbase API - uses format like BTC-USD
bool fetchPriceCoinbase() {
  DEBUG_PRINTLN(F("[API] Trying Coinbase..."));
  HTTPClient http;

  // Build URL: https://api.coinbase.com/v2/prices/BTC-USD/spot
  char url[128];
  snprintf(url, sizeof(url), "https://api.coinbase.com/v2/prices/%s-%s/spot",
           config.crypto, config.currency);

  g_secureClient.stop();
  g_secureClient.setInsecure();
  http.begin(g_secureClient, url);
  http.setTimeout(5000);

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    http.end();

    StaticJsonDocument<256> doc;
    if (!deserializeJson(doc, payload)) {
      const char *amount = doc["data"]["amount"];
      if (amount) {
        g_currentPrice = atof(amount);
        g_priceChange24h = 0.0f; // Coinbase spot doesn't include 24h change
        g_hasValidData = true;
        g_dataStale = false;
        g_lastSuccessTime = millis();
        DEBUG_PRINTF("[API] Coinbase OK: %.2f\n", g_currentPrice);
        return true;
      }
    } else {
      DEBUG_PRINTLN(F("[API] Coinbase JSON error"));
    }
  } else {
    DEBUG_PRINTF("[API] Coinbase HTTP error: %d\n", httpCode);
    http.end();
  }
  return false;
}

// Kraken API - uses XBT for Bitcoin, specific pair format
bool fetchPriceKraken() {
  DEBUG_PRINTLN(F("[API] Trying Kraken..."));
  HTTPClient http;

  // Map crypto symbols to Kraken format (BTC -> XBT)
  const char *krakenCrypto = config.crypto;
  if (strcmp(config.crypto, "BTC") == 0)
    krakenCrypto = "XBT";

  // Map currency to Kraken format
  const char *krakenCurrency = config.currency;
  if (strcmp(config.currency, "USD") == 0)
    krakenCurrency = "USD";

  // Build pair name (e.g., XXBTZUSD, XETHZEUR)
  char pair[16];
  snprintf(pair, sizeof(pair), "X%sZ%s", krakenCrypto, krakenCurrency);

  char url[128];
  snprintf(url, sizeof(url), "https://api.kraken.com/0/public/Ticker?pair=%s",
           pair);

  g_secureClient.stop();
  g_secureClient.setInsecure();
  http.begin(g_secureClient, url);
  http.setTimeout(5000);

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    http.end();

    StaticJsonDocument<1024> doc;
    if (!deserializeJson(doc, payload)) {
      JsonArray errors = doc["error"];
      if (errors.size() == 0) {
        JsonObject result = doc["result"];
        // Kraken returns data under the pair key
        JsonObject tickerData = result[pair];
        if (!tickerData.isNull()) {
          // "c" is close price array, first element is last trade price
          const char *lastPrice = tickerData["c"][0];
          // "o" is today's opening price for calculating 24h change
          const char *openPrice = tickerData["o"];
          if (lastPrice) {
            g_currentPrice = atof(lastPrice);
            if (openPrice && g_currentPrice > 0) {
              float open = atof(openPrice);
              g_priceChange24h = ((g_currentPrice - open) / open) * 100.0f;
            } else {
              g_priceChange24h = 0.0f;
            }
            g_hasValidData = true;
            g_dataStale = false;
            g_lastSuccessTime = millis();
            DEBUG_PRINTF("[API] Kraken OK: %.2f (%.2f%%)\n", g_currentPrice,
                         g_priceChange24h);
            return true;
          }
        } else {
          DEBUG_PRINTLN(F("[API] Kraken: pair not found"));
        }
      } else {
        DEBUG_PRINTLN(F("[API] Kraken returned error"));
      }
    } else {
      DEBUG_PRINTLN(F("[API] Kraken JSON error"));
    }
  } else {
    DEBUG_PRINTF("[API] Kraken HTTP error: %d\n", httpCode);
    http.end();
  }
  return false;
}

void fetchPrice() {
  if (!g_wifiConnected)
    return;

  bool success = false;

  // API source preference:
  // 0 = Auto (Binance -> CoinGecko -> Coinbase -> Kraken)
  // 1 = Binance only
  // 2 = CoinGecko first (-> Binance -> Coinbase -> Kraken)
  // 3 = Coinbase first (-> Binance -> CoinGecko -> Kraken)
  // 4 = Kraken first (-> Binance -> CoinGecko -> Coinbase)

  switch (config.api_source) {
  case 1: // Binance only
    g_currentProvider = 0;
    success = fetchPriceBinance();
    break;

  case 2: // CoinGecko first
    g_currentProvider = 1;
    success = fetchPriceCoinGecko();
    if (!success) {
      g_currentProvider = 0;
      success = fetchPriceBinance();
    }
    if (!success) {
      g_currentProvider = 2;
      success = fetchPriceCoinbase();
    }
    if (!success) {
      g_currentProvider = 3;
      success = fetchPriceKraken();
    }
    break;

  case 3: // Coinbase first
    g_currentProvider = 2;
    success = fetchPriceCoinbase();
    if (!success) {
      g_currentProvider = 0;
      success = fetchPriceBinance();
    }
    if (!success) {
      g_currentProvider = 1;
      success = fetchPriceCoinGecko();
    }
    if (!success) {
      g_currentProvider = 3;
      success = fetchPriceKraken();
    }
    break;

  case 4: // Kraken first
    g_currentProvider = 3;
    success = fetchPriceKraken();
    if (!success) {
      g_currentProvider = 0;
      success = fetchPriceBinance();
    }
    if (!success) {
      g_currentProvider = 1;
      success = fetchPriceCoinGecko();
    }
    if (!success) {
      g_currentProvider = 2;
      success = fetchPriceCoinbase();
    }
    break;

  default: // Auto (0): Binance first with full fallback
    g_currentProvider = 0;
    success = fetchPriceBinance();
    if (!success) {
      g_currentProvider = 1;
      success = fetchPriceCoinGecko();
    }
    if (!success) {
      g_currentProvider = 2;
      success = fetchPriceCoinbase();
    }
    if (!success) {
      g_currentProvider = 3;
      success = fetchPriceKraken();
    }
    break;
  }

  if (!success) {
    g_consecutiveFailures++;
  } else {
    g_consecutiveFailures = 0;
  }
}

// ============================================================================
// WEATHER API FETCHERS
// ============================================================================
bool fetchWeatherOpenMeteo() {
  DEBUG_PRINTLN(F("[Weather] Trying Open-Meteo..."));
  HTTPClient http;

  // Build URL with latitude and longitude
  char url[256];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?"
           "latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,wind_speed_10m,wind_"
           "gusts_10m"
           "&hourly=precipitation_probability&forecast_hours=3",
           config.weather_lat, config.weather_lon);

  DEBUG_PRINT(F("[Weather] URL: "));
  DEBUG_PRINTLN(url);

  g_secureClient.stop();
  g_secureClient.setInsecure();
  http.begin(g_secureClient, url);
  http.setTimeout(5000);

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    http.end();

    StaticJsonDocument<768> doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (!error) {
      // Extract current weather
      JsonObject current = doc["current"];
      if (current.containsKey("temperature_2m") &&
          current.containsKey("relative_humidity_2m")) {
        g_temperature = current["temperature_2m"].as<float>();
        g_humidity = current["relative_humidity_2m"].as<int>();

        // Get max precipitation probability from next 3 hours
        JsonArray precip = doc["hourly"]["precipitation_probability"];
        g_rainChance = 0;
        for (int i = 0; i < 3 && i < (int)precip.size(); i++) {
          int prob = precip[i].as<int>();
          if (prob > g_rainChance)
            g_rainChance = prob;
        }

        g_hasWeatherData = true;
        g_weatherStale = false;
        g_lastWeatherSuccess = millis();

        // Extract wind data if available
        g_windSpeed = current["wind_speed_10m"] | 0.0f;
        g_windGusts = current["wind_gusts_10m"] | 0.0f;

        DEBUG_PRINTF("[Weather] Open-Meteo OK: %.1f°C, %d%%, Rain: %d%%, Wind: "
                     "%.1fkm/h\n",
                     g_temperature, g_humidity, g_rainChance, g_windSpeed);
        return true;
      } else {
        DEBUG_PRINTLN(F("[Weather] Open-Meteo: Missing current keys"));
      }
    } else {
      DEBUG_PRINT(F("[Weather] Open-Meteo JSON error: "));
      DEBUG_PRINTLN(error.c_str());
    }
  } else {
    DEBUG_PRINTF("[Weather] Open-Meteo HTTP error: %d\n", httpCode);
    http.end();
  }
  return false;
}

bool fetchWeatherBrightSky() {
  DEBUG_PRINTLN(F("[Weather] Trying BrightSky (fallback)..."));
  HTTPClient http;

  char url[150];
  snprintf(url, sizeof(url),
           "https://api.brightsky.dev/current_weather?lat=%.4f&lon=%.4f",
           config.weather_lat, config.weather_lon);

  g_secureClient.stop();
  g_secureClient.setInsecure();
  http.begin(g_secureClient, url);
  http.setTimeout(5000);

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    http.end();

    StaticJsonDocument<768> doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (!error) {
      JsonObject weather = doc["weather"];
      if (weather.containsKey("temperature")) {
        g_temperature = weather["temperature"].as<float>();
        g_humidity = weather["relative_humidity"] | 0;
        // BrightSky current_weather doesn't have precipitation_probability
        // Keep existing value or set to 0
        g_rainChance = 0;

        // BrightSky provides wind_speed_10 in km/h
        g_windSpeed = weather["wind_speed_10"] | 0.0f;
        g_windGusts = weather["wind_gust_10"] | 0.0f;

        g_hasWeatherData = true;
        g_weatherStale = false;
        g_lastWeatherSuccess = millis();
        DEBUG_PRINTF("[Weather] BrightSky OK: %.1f°C, %d%%, Wind: %.1fkm/h\n",
                     g_temperature, g_humidity, g_windSpeed);
        return true;
      }
    } else {
      DEBUG_PRINTLN(F("[Weather] BrightSky JSON error"));
    }
  } else {
    DEBUG_PRINTF("[Weather] BrightSky HTTP error: %d\n", httpCode);
    http.end();
  }
  return false;
}

void fetchWeather() {
  if (!g_wifiConnected || !config.weather_enabled)
    return;

  bool success = fetchWeatherOpenMeteo();
  if (!success) {
    success = fetchWeatherBrightSky();
  }

  if (!success) {
    DEBUG_PRINTLN(F("[Weather] All providers failed"));
    if (g_hasWeatherData) {
      g_weatherStale = true;
    }
  }

  DEBUG_PRINTF("[Mem] Free heap after weather: %d bytes\n", ESP.getFreeHeap());
}

// ============================================================================
// WEATHER DISPLAY
// ============================================================================
void drawWeatherScreen() {
  display.clearDisplay();

  // HEADING Section (y=0-10)
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);

  // Show City Name
  display.print(config.weather_name);

  // Header logic differs based on mode
  if (config.wind_enabled && g_hasWeatherData) {
    // 4-METRIC MODE: Compact header (City + Time), No WiFi icon
    // Time right-aligned
    getCurrentTimeStr(g_timeBuffer, sizeof(g_timeBuffer));
    // Simple parsing to get HH:MM from format
    // Format is "14:32" or "14:32 Uhr" or "2:32 PM"
    // We just print what we have, right aligned
    int timeWidth = strlen(g_timeBuffer) * 6;
    display.setCursor(128 - timeWidth, 0);
    display.print(g_timeBuffer);
  } else {
    // STANDARD MODE: City + Old? + WiFi
    if (g_weatherStale) {
      display.setCursor(94, 0);
      display.print(F("OLD"));
    }
    drawWiFiIcon(118, 0, g_wifiConnected);
  }

  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  if (g_hasWeatherData) {
    if (config.wind_enabled) {
      // =====================================================================
      // 4-METRIC LAYOUT (Clean Grid Design) - Wind enabled
      // =====================================================================
      // Grid: 2x2 metrics, each quadrant has LABEL on top, VALUE below
      // No footer, compact and readable
      //
      // Layout:
      // +----------+----------+
      // |   TEMP   |   HUM    |  (labels at y=14)
      // |   2°C    |   94%    |  (values at y=24)
      // +----------+----------+
      // |   RAIN   |   WIND   |  (labels at y=40)
      // |    0%    |  6 km/h  |  (values at y=50)
      // +----------+----------+

      // Draw grid lines
      display.drawLine(64, 12, 64, 63, SSD1306_WHITE); // Vertical center
      display.drawLine(0, 37, 127, 37, SSD1306_WHITE); // Horizontal center

      // === TOP LEFT: Temperature ===
      display.setTextSize(1);
      display.setCursor(12, 14);
      display.print(F("TEMP"));

      display.setTextSize(2);
      char tempStr[8];
      int tempInt = (int)(g_temperature + (g_temperature >= 0 ? 0.5f : -0.5f));
      snprintf(tempStr, sizeof(tempStr), "%d", tempInt);
      int tempWidth = strlen(tempStr) * 12 + 6; // +6 for C
      int tempX = (64 - tempWidth) / 2;
      if (tempX < 0)
        tempX = 0;
      display.setCursor(tempX, 24);
      display.print(tempStr);
      display.setTextSize(1);
      display.print((char)247); // degree symbol
      display.print("C");

      // === TOP RIGHT: Humidity ===
      display.setTextSize(1);
      display.setCursor(64 + 14, 14);
      display.print(F("HUM"));

      display.setTextSize(2);
      char humStr[6];
      snprintf(humStr, sizeof(humStr), "%d", g_humidity);
      int humWidth = strlen(humStr) * 12 + 6; // +6 for %
      int humX = 64 + (64 - humWidth) / 2;
      display.setCursor(humX, 24);
      display.print(humStr);
      display.setTextSize(1);
      display.print(F("%"));

      // === BOTTOM LEFT: Rain ===
      display.setTextSize(1);
      display.setCursor(12, 40);
      display.print(F("RAIN"));

      display.setTextSize(2);
      char rainStr[6];
      snprintf(rainStr, sizeof(rainStr), "%d", g_rainChance);
      int rainWidth = strlen(rainStr) * 12 + 6; // +6 for %
      int rainX = (64 - rainWidth) / 2;
      if (rainX < 0)
        rainX = 0;
      display.setCursor(rainX, 50);
      display.print(rainStr);
      display.setTextSize(1);
      display.print(F("%"));

      // === BOTTOM RIGHT: Wind ===
      display.setTextSize(1);
      display.setCursor(64 + 12, 40);
      display.print(F("WIND"));

      display.setTextSize(2);
      char windStr[8];
      int windInt = (int)(g_windSpeed + 0.5f);
      if (windInt >= 100) {
        snprintf(windStr, sizeof(windStr), "99+");
      } else {
        snprintf(windStr, sizeof(windStr), "%d", windInt);
      }
      int windWidth = strlen(windStr) * 12 + 12; // +12 for "km"
      int windX = 64 + (64 - windWidth) / 2;
      display.setCursor(windX, 50);
      display.print(windStr);
      display.setTextSize(1);
      display.print(F("km"));

    } else {
      // =====================================================================
      // 3-METRIC LAYOUT (Standard) - Wind disabled
      // =====================================================================
      // Three vertical segments: TEMP | HUMID | RAIN
      // Divider lines at x=42 and x=85

      display.drawLine(42, 12, 42, 52, SSD1306_WHITE);
      display.drawLine(85, 12, 85, 52, SSD1306_WHITE);

      // === TEMPERATURE (left segment, x=0-41) ===
      display.setTextSize(2);
      char tempStr[8];
      int tempInt = (int)(g_temperature + (g_temperature >= 0 ? 0.5f : -0.5f));
      snprintf(tempStr, sizeof(tempStr), "%d", tempInt);
      int tempWidth = strlen(tempStr) * 12;
      int tempX = (42 - tempWidth - 12) / 2; // -12 for degree symbol space
      if (tempX < 0)
        tempX = 0;
      display.setCursor(tempX, 16);
      display.print(tempStr);
      display.setTextSize(1);
      display.print(F("C"));

      display.setTextSize(1);
      display.setCursor(6, 40);
      display.print(F("TEMP"));

      // === HUMIDITY (middle segment, x=43-84) ===
      display.setTextSize(2);
      char humStr[6];
      snprintf(humStr, sizeof(humStr), "%d", g_humidity);
      int humWidth = strlen(humStr) * 12;
      int humX = 43 + (42 - humWidth - 6) / 2; // -6 for % symbol
      display.setCursor(humX, 16);
      display.print(humStr);
      display.setTextSize(1);
      display.print(F("%"));

      display.setTextSize(1);
      display.setCursor(49, 40);
      display.print(F("HUMID"));

      // === RAIN CHANCE (right segment, x=86-127) ===
      display.setTextSize(2);
      char rainStr[6];
      snprintf(rainStr, sizeof(rainStr), "%d", g_rainChance);
      int rainWidth = strlen(rainStr) * 12;
      int rainX = 86 + (42 - rainWidth - 6) / 2;
      display.setCursor(rainX, 16);
      display.print(rainStr);
      display.setTextSize(1);
      display.print(F("%"));

      display.setTextSize(1);
      display.setCursor(94, 40);
      display.print(F("RAIN"));

      // Footer (Only for 3-metric mode)
      display.drawLine(0, 53, 127, 53, SSD1306_WHITE);
      display.setTextSize(1);
      display.setCursor(0, 56);
      display.print(F("Updated: "));
      getCurrentTimeStr(g_timeBuffer, sizeof(g_timeBuffer));
      display.print(g_timeBuffer);
    }
  } else {
    display.setTextSize(1);
    display.setCursor(10, 25);
    display.print(F("No weather data"));

    // Static Footer for error state
    display.drawLine(0, 53, 127, 53, SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 56);
    display.print(F("Updated: "));
    getCurrentTimeStr(g_timeBuffer, sizeof(g_timeBuffer));
    display.print(g_timeBuffer);
  }

  display.display();
}

// Helper to refresh current screen
void refreshCurrentScreen() {
  if (g_currentScreen == 0) {
    updateDisplay(); // BTC screen
  } else {
    drawWeatherScreen(); // Weather screen
  }
}

// ============================================================================
// MAIN SETUP & LOOP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  DEBUG_PRINTLN(F("\n\n=== ESP8266 BTC Ticker Boot ==="));

  // Init Display
  Wire.begin(SDA_PIN, SCL_PIN);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed"));
    for (;;)
      ;
  }
  display.clearDisplay();
  display.display();

  // Check for Factory Reset command via Serial
  pinMode(RESET_BTN_PIN, INPUT);  // TTP223 has active driver, no pullup needed
  pinMode(SCREEN_BTN_PIN, INPUT); // TTP223 has active driver, no pullup needed

  if (loadConfig()) {
    // Apply display rotation setting
    display.setRotation(config.flip_display ? 2 : 0); // 0=normal, 2=180°

    if (config.alert_mode == 1 || config.alert_mode == 2) { // LED Only or Both
      pinMode(config.led_pin, OUTPUT);
      digitalWrite(config.led_pin, config.led_inverted ? HIGH : LOW); // Off
      DEBUG_PRINTF("[LED] Initialized pin %d (Inverted: %d)\n", config.led_pin,
                   config.led_inverted);
    }

    // Initialize Touch Buttons
    if (config.touch_enabled) {
      pinMode(PIN_BTN_A, INPUT);
      pinMode(PIN_BTN_B, INPUT);
      pinMode(PIN_BTN_C, INPUT);
      DEBUG_PRINTLN(F("[Touch] Touch buttons enabled on D0, D7, D8"));
    }
  }

  DEBUG_PRINTLN(F("Press 'w' or type 'wipe' now to factory reset..."));
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("Booting..."));
  display.println(F("Send 'wipe' to reset"));
  display.display();

  unsigned long startWait = millis();
  while (millis() - startWait < 3000) {
    if (Serial.available()) {
      String input = Serial.readStringUntil('\n');
      input.trim();
      if (input == "wipe" || input == "w" || input == "reset") {
        DEBUG_PRINTLN(F("\n[Reset] Factory Reset initiated!"));
        display.clearDisplay();
        display.setCursor(0, 20);
        display.println(F("Factory Reset..."));
        display.println(F("Formatting FS..."));
        display.display();

        LittleFS.format();
        WiFi.disconnect(true); // Wipe WiFi credentials
        delay(1000);
        DEBUG_PRINTLN(F("[Reset] Done. Restarting..."));
        ESP.restart();
      }
    }
    delay(10);
  }
  display.clearDisplay();

  // Load Config
  bool configLoaded = loadConfig();

  // Apply rotation from config
  if (config.flip_display)
    display.setRotation(2);

  bool forceConfig = false;
  // If config missing or SSID empty, force config mode
  if (!configLoaded || strlen(config.wifi_ssid) == 0) {
    forceConfig = true;
  }

  if (!forceConfig) {
    // Try to connect to WiFi
    DEBUG_PRINTLN(F("[Setup] Connecting to saved WiFi..."));

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 20);
    display.println(F("Connecting WiFi..."));
    display.print(config.wifi_ssid);
    display.display();

    WiFi.mode(WIFI_STA);
    WiFi.begin(config.wifi_ssid, config.wifi_password);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      display.print(".");
      display.display();
      attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      DEBUG_PRINTLN(F("[Setup] WiFi Connected!"));
      g_wifiConnected = true;
      g_isInConfigMode = false;

      // Show IP address on display briefly
      display.clearDisplay();
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(0, 0);
      display.println(F("WiFi Connected!"));
      display.println();
      display.print(F("Config: http://"));
      display.println(WiFi.localIP());
      display.println();
      display.println(F("Starting ticker..."));
      display.display();
      delay(3000);

      // Start web server for remote configuration FIRST
      server.close(); // Close any previous instance
      server.on("/", handleRoot);
      server.on("/save", handleSave);
      server.on("/logs", handleLogs);
      server.begin();
      DEBUG_PRINT(F("[Web] Config server started at http://"));
      DEBUG_PRINTLN(WiFi.localIP().toString());

      // Setup NTP
      configTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.nist.gov");

      // Initial fetch
      fetchPrice();
      if (config.weather_enabled) {
        fetchWeather();
        g_lastWeatherPoll = millis();
      }
      g_lastScreenSwitch = millis();

      // Set initial screen based on D5 mode
      if (config.d5_btc_mode && config.weather_enabled) {
        g_currentScreen = 1; // Weather default in inverted mode
        drawWeatherScreen();
      } else {
        g_currentScreen = 0; // BTC default
        updateDisplay();
      }
    } else {
      DEBUG_PRINTLN(F("[Setup] WiFi Connection Failed!"));
      forceConfig = true; // Fallback to config mode
    }
  }

  if (forceConfig) {
    startConfigMode();
  }
}

void loop() {
  if (g_isInConfigMode) {
    // DNS handling for captive portal
    dnsServer.processNextRequest();
    server.handleClient();
  } else {
    // Normal Ticker Operation
    unsigned long currentMillis = millis();

    // Handle web server requests (for remote config)
    server.handleClient();

    // Check WiFi and Reconnect if necessary
    static unsigned long lastWiFiCheck = 0;
    if (currentMillis - lastWiFiCheck > 30000) { // Check every 30s
      lastWiFiCheck = currentMillis;

      if (WiFi.status() != WL_CONNECTED) {
        DEBUG_PRINTLN(F("[WiFi] Connection lost, reconnecting..."));
        WiFi.reconnect();

        // Update display to show we are offline
        g_wifiConnected = false;
        drawWiFiIcon(118, 0, false);
        display.display();
      } else {
        if (!g_wifiConnected) {
          DEBUG_PRINTLN(F("[WiFi] Reconnected!"));
          g_wifiConnected = true;
          // Immediately fetch new data upon reconnection
          fetchPrice();
          updateDisplay();
        }
      }
    }

    // Poll BTC Price
    if (currentMillis - g_lastPollTime >= config.poll_interval) {
      fetchPrice();
      // Check if data is stale (no success in last 3 poll intervals)
      if (millis() - g_lastSuccessTime > (config.poll_interval * 3)) {
        g_dataStale = true;
      }
      // Only update display if on BTC screen
      if (g_currentScreen == 0) {
        updateDisplay();
      }
      g_lastPollTime = currentMillis;
    }

    // Poll Weather (if enabled)
    if (config.weather_enabled &&
        (currentMillis - g_lastWeatherPoll >= config.weather_poll)) {
      fetchWeather();
      // Check if weather data is stale
      if (millis() - g_lastWeatherSuccess > (config.weather_poll * 3)) {
        g_weatherStale = true;
      }
      // Only update display if on weather screen
      if (g_currentScreen == 1) {
        drawWeatherScreen();
      }
      g_lastWeatherPoll = currentMillis;
    }

    // Screen Cycling (only in Auto Cycle mode, button_mode == 0)
    // Use btc_cycle when on BTC screen, weather_cycle when on weather screen
    if (config.weather_enabled && config.button_mode == 0) {
      unsigned long currentCycle =
          (g_currentScreen == 0) ? config.btc_cycle : config.weather_cycle;
      if (currentMillis - g_lastScreenSwitch >= currentCycle) {
        g_currentScreen = (g_currentScreen + 1) % 2;
        g_lastScreenSwitch = currentMillis;
        refreshCurrentScreen();
        DEBUG_PRINTF("[Display] Auto-cycled to screen %d\n", g_currentScreen);
      }
    }

    // On-Demand mode: Auto-return to BTC after weather_cycle
    if (config.weather_enabled && config.button_mode == 2 &&
        g_currentScreen == 1 &&
        (currentMillis - g_lastScreenSwitch >= config.weather_cycle)) {
      g_currentScreen = 0;
      g_lastScreenSwitch = currentMillis;
      updateDisplay();
      DEBUG_PRINTLN(F("[Display] On-demand: Returned to BTC"));
    }

    // Screen Toggle Button (D6) - short press to toggle screens
    static unsigned long screenBtnPressStart = 0;
    static bool screenBtnHandled = false;
    if (digitalRead(SCREEN_BTN_PIN) == LOW) {
      if (screenBtnPressStart == 0) {
        screenBtnPressStart = currentMillis;
        screenBtnHandled = false;
      }
    } else {
      // Button released
      if (screenBtnPressStart > 0 && !screenBtnHandled) {
        unsigned long pressDuration = currentMillis - screenBtnPressStart;
        if (pressDuration > 50 && pressDuration < 1000) {
          // Short press: handle based on button_mode
          if (config.weather_enabled) {
            if (config.button_mode == 0) {
              // Auto Cycle mode: Toggle between screens
              g_currentScreen = (g_currentScreen + 1) % 2;
            } else if (config.button_mode == 1) {
              // Always Weather mode: Force weather screen
              g_currentScreen = 1;
            } else if (config.button_mode == 2) {
              // On-Demand mode: Show weather (uses cached data)
              // Data is already cached from background polling
              g_currentScreen = 1;
              DEBUG_PRINTLN(F("[Button] On-demand: Showing cached weather"));
            }
            g_lastScreenSwitch = currentMillis; // Reset timer
            refreshCurrentScreen();
            DEBUG_PRINTF("[Button] Screen set to %d\n", g_currentScreen);
          }
          screenBtnHandled = true;
        }
      }
      screenBtnPressStart = 0;
    }

    // TTP223 Touch Button Polling
    if (config.touch_enabled) {
      static unsigned long lastTouchTime = 0;
      if (currentMillis - lastTouchTime > 200) { // Simple debounce
        if (digitalRead(PIN_BTN_A) == HIGH) {
          DEBUG_PRINTLN(F("[Touch] Button A (D7) pressed"));
          lastTouchTime = currentMillis;
        } else if (digitalRead(PIN_BTN_B) == HIGH) {
          DEBUG_PRINTLN(F("[Touch] Button B (D8) pressed"));
          lastTouchTime = currentMillis;
        } else if (digitalRead(PIN_BTN_C) == HIGH) {
          DEBUG_PRINTLN(F("[Touch] Button C (D0) pressed"));
          lastTouchTime = currentMillis;
        }
      }
    }

    // Check D5 Button (Factory Reset OR Weather/BTC Toggle based on config)
    static unsigned long btnPressStart = 0;
    static bool d5WeatherShowing = false;
    static bool d5BtcShowing = false;
    static unsigned long d5BtcReleaseTime = 0;

    if (config.d5_btc_mode) {
      // D5 = BTC Button Mode (Inverted: Weather is default)
      bool buttonPressed = (digitalRead(RESET_BTN_PIN) == HIGH);

      if (buttonPressed) {
        // Button is pressed - show BTC
        if (!d5BtcShowing) {
          g_currentScreen = 0; // Switch to BTC
          updateDisplay();
          d5BtcShowing = true;
          d5BtcReleaseTime = 0; // Reset timeout
          DEBUG_PRINTLN(F("[D5] BTC button pressed - showing BTC"));
        }
      } else {
        // Button is released
        if (d5BtcShowing) {
          if (config.d5_timeout == 0) {
            // Instant return
            g_currentScreen = 1;
            drawWeatherScreen();
            d5BtcShowing = false;
            DEBUG_PRINTLN(
                F("[D5] BTC button released - showing weather (instant)"));
          } else if (d5BtcReleaseTime == 0) {
            // Just released - start timeout
            d5BtcReleaseTime = currentMillis;
            DEBUG_PRINTF("[D5] BTC button released - starting %lu ms timeout\n",
                         config.d5_timeout);
          } else if (currentMillis - d5BtcReleaseTime >= config.d5_timeout) {
            // Timeout expired - return to weather
            g_currentScreen = 1;
            drawWeatherScreen();
            d5BtcShowing = false;
            d5BtcReleaseTime = 0;
            DEBUG_PRINTLN(F("[D5] Timeout expired - showing weather"));
          }
        }
      }
    } else if (config.d5_weather_mode) {
      // D5 = Weather Button Mode (BTC is default)
      static unsigned long d5WeatherReleaseTime = 0;

      if (digitalRead(RESET_BTN_PIN) == HIGH) {
        if (!d5WeatherShowing) {
          g_currentScreen = 1; // Switch to weather
          drawWeatherScreen();
          d5WeatherShowing = true;
          d5WeatherReleaseTime = 0; // Reset timeout
          DEBUG_PRINTLN(F("[D5] Weather button pressed - showing weather"));
        }
      } else {
        if (d5WeatherShowing) {
          if (config.d5_timeout == 0) {
            // Instant return
            g_currentScreen = 0;
            updateDisplay();
            d5WeatherShowing = false;
            DEBUG_PRINTLN(
                F("[D5] Weather button released - showing BTC (instant)"));
          } else if (d5WeatherReleaseTime == 0) {
            // Just released - start timeout
            d5WeatherReleaseTime = currentMillis;
            DEBUG_PRINTF(
                "[D5] Weather button released - starting %lu ms timeout\n",
                config.d5_timeout);
          } else if (currentMillis - d5WeatherReleaseTime >=
                     config.d5_timeout) {
            // Timeout expired - return to BTC
            g_currentScreen = 0;
            updateDisplay();
            d5WeatherShowing = false;
            d5WeatherReleaseTime = 0;
            DEBUG_PRINTLN(F("[D5] Timeout expired - showing BTC"));
          }
        }
      }
    } else {
      // D5 = Factory Reset Mode (original behavior)
      if (digitalRead(RESET_BTN_PIN) == HIGH) {
        if (btnPressStart == 0)
          btnPressStart = currentMillis;

        unsigned long duration = currentMillis - btnPressStart;
        if (duration > 10000) {
          DEBUG_PRINTLN(F("[Button] Factory Reset triggered!"));
          display.clearDisplay();
          display.setTextSize(2);
          display.setCursor(0, 20);
          display.println(F("RESETTING!"));
          display.display();
          delay(1000);

          LittleFS.format();
          WiFi.disconnect(true);
          ESP.restart();
        } else if (duration > 2000) {
          // Show countdown after 2 seconds hold
          int countdown = 10 - (duration / 1000);
          display.clearDisplay();
          display.setTextSize(1);
          display.setCursor(0, 0);
          display.println(F("FACTORY RESET???"));
          display.println(F("Keep holding..."));
          display.setTextSize(3);
          display.setCursor(55, 30);
          display.print(countdown);
          display.display();
          // Skip other display updates
          lastWiFiCheck = currentMillis;
        }
      } else {
        btnPressStart = 0;
      }
    }

    // Alert Logic (Display + LED)
    // Modes: 0=Display, 1=LED, 2=Both
    // Modes: 0=Display, 1=LED, 2=Both
    static unsigned long lastBlinkTime = 0;
    static bool blinkState = true;   // true=ON/Visible
    static bool ledCleanedUp = true; // Track if we've reset LED to OFF
    bool alertTriggered = false;

    if (g_hasValidData) {
      if ((config.alert_low > 0 && g_currentPrice < config.alert_low) ||
          (config.alert_high > 0 && g_currentPrice > config.alert_high)) {
        alertTriggered = true;
      }
    }

    // Handle Alert Duration
    if (alertTriggered) {
      if (g_alertStartTime == 0) {
        g_alertStartTime = currentMillis;
        // Log the alert trigger
        if (config.alert_low > 0 && g_currentPrice < config.alert_low) {
          DEBUG_PRINTF("[Alert] LOW Price: %.2f < %d\n", g_currentPrice,
                       config.alert_low);
        }
        if (config.alert_high > 0 && g_currentPrice > config.alert_high) {
          DEBUG_PRINTF("[Alert] HIGH Price: %.2f > %d\n", g_currentPrice,
                       config.alert_high);
        }
      }

      // Check if duration exceeded
      if (config.alert_duration > 0 &&
          (currentMillis - g_alertStartTime > config.alert_duration * 1000)) {
        alertTriggered = false; // Stop alerting
        // But don't reset start time so it doesn't immediately restart.
        // Logic: Alert stops until condition clears and re-triggers?
        // For now: duration ends alert until price goes back to normal range
        // then triggers again? Simpler: Just block it. To re-arm, price must
        // normalize.
      }
    } else {
      g_alertStartTime = 0; // Reset timer when condition clears
    }

    if (alertTriggered) {
      ledCleanedUp = false; // Mark that LED state is dirty/active

      // Determine active pattern
      uint8_t pattern = 0;
      if (config.alert_low > 0 && g_currentPrice < config.alert_low) {
        pattern = config.blink_pattern_low;
      } else if (config.alert_high > 0 && g_currentPrice > config.alert_high) {
        pattern = config.blink_pattern_high;
      }

      unsigned long interval = 1000; // Default Slow (1s)
      static uint8_t sosStep = 0;

      // SOS Pattern Logic (Pattern 3)
      if (pattern == 3) {
        // Reset step if just started (approximate check)
        if (currentMillis - lastBlinkTime > 5000)
          sosStep = 0;

        unsigned long duration = 200;          // Default dot/gap
        bool targetState = (sosStep % 2 == 0); // Even = ON

        // Logic: S (0-5), O (6-11), S (12-17)
        if (sosStep == 5)
          duration = 600; // Gap after S
        else if (sosStep == 11)
          duration = 600; // Gap after O
        else if (sosStep == 17)
          duration = 2000; // Gap after SOS (long pause)

        // O (Dashes): Steps 6, 8, 10 are ON
        if (sosStep >= 6 && sosStep <= 10) {
          if (sosStep % 2 == 0)
            duration = 600; // Dash is 600ms
        }

        if (currentMillis - lastBlinkTime >= duration) {
          sosStep++;
          if (sosStep > 17)
            sosStep = 0;
          blinkState = (sosStep % 2 == 0); // Next state based on new step
          lastBlinkTime = currentMillis;
        }
        // Force correct state for current step (consistency)
        blinkState = (sosStep % 2 == 0);

      } else {
        // Standard Patterns (0=Slow, 1=Fast, 2=Strobe)
        if (pattern == 1)
          interval = 250;
        else if (pattern == 2)
          interval = 50;

        if (currentMillis - lastBlinkTime >= interval) {
          blinkState = !blinkState;
          lastBlinkTime = currentMillis;
        }
      }

      // Apply State to Output
      // DISPLAY BLINKING (Modes 0 & 2)
      if (config.alert_mode == 0 || config.alert_mode == 2) {
        if (blinkState)
          updateDisplay();
        else {
          display.clearDisplay();
          display.display();
        }
      }

      // LED BLINKING (Modes 1 & 2)
      if (config.alert_mode == 1 || config.alert_mode == 2) {
        bool pinLevel = blinkState ? (config.led_inverted ? LOW : HIGH)
                                   : (config.led_inverted ? HIGH : LOW);
        digitalWrite(config.led_pin, pinLevel);
      }

    } else {
      // Idle State: Display ON, LED OFF
      if (!blinkState) {
        blinkState = true;
        refreshCurrentScreen(); // Ensure screen is visible
      }

      // Ensure LED is OFF (latching logic to avoid spamming digitalWrite)
      if (!ledCleanedUp) {
        if (config.alert_mode == 1 || config.alert_mode == 2) {
          digitalWrite(config.led_pin, config.led_inverted ? HIGH : LOW);
        }
        ledCleanedUp = true;
      }
    }

    delay(10); // Short delay for web server responsiveness
    yield();   // Allow WiFi/TCP stack to process
  }
}
