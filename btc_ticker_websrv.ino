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
  char currency[10] = "EUR";  // EUR, USD, GBP, etc.
  char time_format[3] = "DE"; // DE (24h) or US (12h)
  unsigned long poll_interval = 60000;
  int alert_low = 0;

  int alert_high = 0;
  unsigned long blink_interval = 2000;
  bool flip_display = false;
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
#define SDA_PIN 4        // D2
#define SCL_PIN 5        // D1
#define RESET_BTN_PIN 14 // D5 (GPIO14) - Push button to GND

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
WiFiClientSecure g_secureClient;

float g_currentPrice = 0.0f;
float g_priceChange24h = 0.0f;
bool g_wifiConnected = false;
bool g_hasValidData = false;
bool g_dataStale = false;
unsigned long g_lastPollTime = 0;
unsigned long g_lastSuccessTime = 0;
uint8_t g_currentProvider = 0; // 0 = Binance, 1 = CoinGecko
uint8_t g_consecutiveFailures = 0;

// Displays helpers
char g_priceBuffer[20];
char g_changeBuffer[20];
char g_timeBuffer[20];

#define DEBUG_SERIAL 1
#if DEBUG_SERIAL
#define DEBUG_PRINT(x) Serial.print(x)
#define DEBUG_PRINTLN(x) Serial.println(x)
#define DEBUG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
#define DEBUG_PRINT(x)
#define DEBUG_PRINTLN(x)
#define DEBUG_PRINTF(...)
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

  StaticJsonDocument<512> doc;
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
  strlcpy(config.time_format, doc["time_format"] | "DE",
          sizeof(config.time_format));

  config.poll_interval = doc["poll_interval"] | 60000;
  config.alert_low = doc["alert_low"] | 0;
  config.alert_high = doc["alert_high"] | 0;
  config.blink_interval = doc["blink_interval"] | 2000;
  config.flip_display = doc["flip_display"] | false;

  DEBUG_PRINTLN(F("[FS] Configuration loaded!"));
  return true;
}

bool saveConfig() {
  StaticJsonDocument<512> doc;
  doc["wifi_ssid"] = config.wifi_ssid;
  doc["wifi_password"] = config.wifi_password;
  doc["currency"] = config.currency;
  doc["time_format"] = config.time_format;
  doc["poll_interval"] = config.poll_interval;
  doc["alert_low"] = config.alert_low;
  doc["alert_high"] = config.alert_high;
  doc["blink_interval"] = config.blink_interval;
  doc["flip_display"] = config.flip_display;

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

  // Header
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(F("BTC/"));
  display.print(config.currency);
  display.print(F(" "));
  display.print(g_currentProvider == 0 ? F("Binance") : F("CoinGecko"));

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
void handleRoot() {
  String html = F("<!DOCTYPE html><html><head><title>BTC Ticker Setup</title>");
  html +=
      F("<meta name='viewport' content='width=device-width, initial-scale=1'>");
  html += F("<style>body{font-family:Arial;max-width:400px;margin:0 "
            "auto;padding:20px;}");
  html += F("input,select{width:100%;padding:10px;margin:5px "
            "0;box-sizing:border-box;}");
  html += F("input[type=submit]{background:#4CAF50;color:white;border:none;"
            "cursor:pointer;}");
  html += F("</style></head><body>");
  html += F("<h2>BTC Ticker Configuration</h2>");
  html += F("<form action='/save' method='POST'>");

  html += F("<label>WiFi SSID:</label><input type='text' name='ssid' value='");
  html += config.wifi_ssid;
  html += F("' required>");

  html += F("<label>WiFi Password:</label><input type='password' name='pass' "
            "value='");
  html += config.wifi_password;
  html += F("' required>");

  html += F("<label>Currency:</label><select name='curr'>");
  const char *currencies[] = {"EUR", "USD", "GBP", "CAD",
                              "AUD", "JPY", "CHF", "CNY"};
  for (int i = 0; i < 8; i++) {
    html += F("<option value='");
    html += currencies[i];
    html += F("'");
    if (strcmp(config.currency, currencies[i]) == 0)
      html += F(" selected");
    html += F(">");
    html += currencies[i];
    html += F("</option>");
  }
  html += F("</select>");

  html += F("<label>Poll Interval (ms):</label><input type='number' "
            "name='poll' value='");
  html += config.poll_interval;
  html += F("'>");

  html += F("<label>Time Format:</label><select name='time'>");
  html += F("<option value='DE'");
  if (strcmp(config.time_format, "DE") == 0)
    html += F(" selected");
  html += F(">DE (24h + Uhr)</option>");
  html += F("<option value='US'");
  if (strcmp(config.time_format, "US") == 0)
    html += F(" selected");
  html += F(">US (12h + AM/PM)</option>");
  html += F("</select>");

  html += F("<label>Alert Low (Price):</label><input type='number' name='alow' "
            "value='");
  html += config.alert_low;
  html += F("'>");

  html += F("<label>Alert High (Price):</label><input type='number' "
            "name='ahigh' value='");
  html += config.alert_high;
  html += F("'>");

  html += F("<label>Blink Interval (ms):</label><input type='number' "
            "name='blink' value='");
  html += config.blink_interval;
  html += F("'>");

  html += F("<label><input type='checkbox' name='flip' value='1'");
  if (config.flip_display)
    html += F(" checked");
  html += F("> Flip Display (180_)</label><br><br>");

  html += F("<input type='submit' value='Save & Reboot'></form></body></html>");

  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("ssid"))
    strlcpy(config.wifi_ssid, server.arg("ssid").c_str(),
            sizeof(config.wifi_ssid));
  if (server.hasArg("pass"))
    strlcpy(config.wifi_password, server.arg("pass").c_str(),
            sizeof(config.wifi_password));
  if (server.hasArg("curr"))
    strlcpy(config.currency, server.arg("curr").c_str(),
            sizeof(config.currency));
  if (server.hasArg("poll"))
    config.poll_interval = server.arg("poll").toInt();
  if (server.hasArg("time"))
    strlcpy(config.time_format, server.arg("time").c_str(),
            sizeof(config.time_format));
  if (server.hasArg("alow"))
    config.alert_low = server.arg("alow").toInt();
  if (server.hasArg("ahigh"))
    config.alert_high = server.arg("ahigh").toInt();
  if (server.hasArg("blink"))
    config.blink_interval = server.arg("blink").toInt();

  config.flip_display = server.hasArg("flip");

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
  // Handler for captive portal (redirect all)
  server.onNotFound([]() {
    server.sendHeader("Location",
                      String("http://") + WiFi.softAPIP().toString(), true);
    server.send(302, "text/plain", "");
  });

  server.begin();

  DEBUG_PRINT(F("[AP] IP Address: "));
  DEBUG_PRINTLN(WiFi.softAPIP());

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
           "https://api.binance.com/api/v3/ticker/24hr?symbol=BTC%s",
           binanceCurrency);

  g_secureClient.setInsecure();
  http.begin(g_secureClient, url);
  http.setTimeout(10000);

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

  char currencyLower[10];
  for (int i = 0; config.currency[i] && i < 9; i++) {
    currencyLower[i] = tolower(config.currency[i]);
    currencyLower[i + 1] = '\0';
  }

  char url[256];
  strcpy(url, "https://api.coingecko.com/api/v3/simple/price?ids=bitcoin");
  strcat(url, "&vs_currencies=");
  strcat(url, currencyLower);
  strcat(url, "&include_24hr_change=true");

  g_secureClient.setInsecure();
  http.begin(g_secureClient, url);

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    http.end();

    StaticJsonDocument<512> doc;
    if (!deserializeJson(doc, payload)) {
      JsonObject bitcoin = doc["bitcoin"];
      if (bitcoin.containsKey(currencyLower)) {
        g_currentPrice = bitcoin[currencyLower].as<float>();

        char changeKey[32];
        snprintf(changeKey, sizeof(changeKey), "%s_24h_change", currencyLower);
        g_priceChange24h = bitcoin[changeKey].as<float>();
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

void fetchPrice() {
  if (!g_wifiConnected)
    return; // Should not happen in normal mode if fetchPrice is called logic

  bool success = false;
  if (g_currentProvider == 0) {
    success = fetchPriceBinance();
    if (!success) {
      g_currentProvider = 1;
      success = fetchPriceCoinGecko();
    }
  } else {
    success = fetchPriceCoinGecko();
    if (!success) {
      g_currentProvider = 0;
      success = fetchPriceBinance();
    }
  }

  if (!success) {
    g_consecutiveFailures++;
    // If failures persist, maybe show error on screen (optional)
  } else {
    g_consecutiveFailures = 0;
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
  pinMode(RESET_BTN_PIN, INPUT_PULLUP);

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

      // Setup NTP
      configTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.nist.gov");

      // Initial fetch
      fetchPrice();
      updateDisplay();
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

    // Poll Price
    if (currentMillis - g_lastPollTime >= config.poll_interval) {
      fetchPrice();
      // Check if data is stale (no success in last 3 poll intervals or 5 mins)
      if (millis() - g_lastSuccessTime > (config.poll_interval * 3)) {
        g_dataStale = true;
      }
      updateDisplay();
      g_lastPollTime = currentMillis;
    }

    // Check physical Factory Reset Button (D5)
    static unsigned long btnPressStart = 0;
    if (digitalRead(RESET_BTN_PIN) == LOW) {
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

    // Alert Blinking
    static unsigned long lastBlinkTime = 0;
    static bool displayVisible = true;
    bool alertActive = false;

    if (g_hasValidData) {
      if (config.alert_low > 0 && g_currentPrice < config.alert_low)
        alertActive = true;
      if (config.alert_high > 0 && g_currentPrice > config.alert_high)
        alertActive = true;
    }

    if (alertActive) {
      if (currentMillis - lastBlinkTime >= config.blink_interval) {
        displayVisible = !displayVisible;
        lastBlinkTime = currentMillis;
        if (displayVisible)
          updateDisplay();
        else {
          display.clearDisplay();
          display.display();
        }
      }
    } else {
      if (!displayVisible) {
        displayVisible = true;
        updateDisplay();
      }
    }

    delay(100);
  }
}
