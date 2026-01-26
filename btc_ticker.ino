/*
 * ESP8266 Bitcoin Live Ticker
 * ============================
 * Displays real-time BTC/EUR price on SSD1306 OLED display
 *
 * Hardware: Wemos D1 Mini + 0.96" OLED (I2C)
 * Wiring:
 *   D1 (GPIO5) -> SCL
 *   D2 (GPIO4) -> SDA
 *   3.3V       -> VCC
 *   GND        -> GND
 *
 * Libraries required:
 *   - Adafruit SSD1306 (2.5.x)
 *   - Adafruit GFX (1.11.x)
 *   - ArduinoJson (6.x)
 *   - ESP8266WiFi (built-in)
 *   - ESP8266HTTPClient (built-in)
 */

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <time.h>

// ============================================================================
// CONFIGURATION - EDIT THESE VALUES
// ============================================================================
const char *WIFI_SSID = "YOUR_WIFI_SSID";
const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Polling interval in milliseconds (60 seconds recommended)
#define POLL_INTERVAL_MS 60000

// Serial debug output
#define DEBUG_SERIAL 1

// ============================================================================
// CURRENCY CONFIGURATION
// ============================================================================
// Supported currencies: EUR, USD, GBP, CAD, AUD, JPY, CHF, CNY
#define CURRENCY "EUR"

// ============================================================================
// TIME FORMAT CONFIGURATION
// ============================================================================
// "DE" = German/European 24-hour format (14:30)
// "US" = American 12-hour format (2:30 PM)
#define TIME_FORMAT "DE"

// ============================================================================
// PRICE ALERT THRESHOLDS - Set to 0 to disable
// ============================================================================
// Display will blink every 2 seconds when price is outside these bounds
// Use whole numbers only (no decimals), e.g., 90000 or 110000
#define ALERT_PRICE_LOW 0  // Blink if price falls below this (0 = disabled)
#define ALERT_PRICE_HIGH 0 // Blink if price rises above this (0 = disabled)
#define ALERT_BLINK_INTERVAL_MS 2000 // Blink interval in milliseconds
#define FLIP_DISPLAY 0               // Set to 1 to flip display 180 degrees

// ============================================================================
// DISPLAY CONFIGURATION
// ============================================================================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_I2C_ADDRESS 0x3C

// I2C pins for Wemos D1 Mini
#define SDA_PIN 4 // D2
#define SCL_PIN 5 // D1

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ============================================================================
// API ENDPOINTS
// ============================================================================
// Using HTTPS - APIs redirect HTTP to HTTPS (301)
const char API_BINANCE_HOST[] PROGMEM = "api.binance.com";
const char API_COINGECKO_HOST[] PROGMEM = "api.coingecko.com";

// Currency code for display (will be set from CURRENCY define)
const char *g_currencyCode = CURRENCY;

// Shared secure client (reused to save memory)
WiFiClientSecure g_secureClient;

// ============================================================================
// GLOBAL STATE (using fixed-size buffers, no String class)
// ============================================================================
// Price data
float g_currentPrice = 0.0f;
float g_priceChange24h = 0.0f;
float g_cachedPrice = 0.0f; // Emergency fallback
float g_cachedChange = 0.0f;
bool g_dataStale = false;
bool g_hasValidData = false;

// Timing
unsigned long g_lastPollTime = 0;
unsigned long g_lastSuccessTime = 0;
unsigned long g_lastDisplayUpdate = 0;
unsigned long g_providerResetTime = 0;

// WiFi state
bool g_wifiConnected = false;
uint8_t g_currentProvider = 0; // 0 = Binance, 1 = CoinGecko
uint8_t g_consecutiveFailures = 0;

// Buffers for display (avoid String allocations)
char g_priceBuffer[20];
char g_changeBuffer[16];
char g_timeBuffer[12];

// ============================================================================
// HELPER: Debug print (only when DEBUG_SERIAL is enabled)
// ============================================================================
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
// HELPER: Format price with thousands separator (dot) and currency suffix
// Formats value like 52345.67 into "52.345 EUR" (whole number only)
// ============================================================================
void formatPrice(float price, char *buffer, size_t bufferSize) {
  long intPart = (long)(price + 0.5f); // Round to nearest whole number

  // Format with dot as thousands separator, currency suffix
  if (intPart >= 100000) {
    snprintf(buffer, bufferSize, "%ld.%03ld %s", intPart / 1000, intPart % 1000,
             g_currencyCode);
  } else if (intPart >= 1000) {
    snprintf(buffer, bufferSize, "%ld.%03ld %s", intPart / 1000, intPart % 1000,
             g_currencyCode);
  } else {
    snprintf(buffer, bufferSize, "%ld %s", intPart, g_currencyCode);
  }
}

// ============================================================================
// HELPER: Format percentage change with sign
// Formats value like 2.45 into "+2.45%" or -1.23 into "-1.23%"
// ============================================================================
void formatChange(float change, char *buffer, size_t bufferSize) {
  char sign = change >= 0 ? '+' : '-';
  float absChange = change >= 0 ? change : -change;
  int intPart = (int)absChange;
  int decPart = (int)((absChange - intPart) * 100 + 0.5f);
  snprintf(buffer, bufferSize, "%c%d.%02d%%", sign, intPart, decPart);
}

// ============================================================================
// HELPER: Get current time string - Configurable format
// DE = 24-hour (14:30), US = 12-hour (2:30 PM)
// ============================================================================
void getCurrentTimeStr(char *buffer, size_t bufferSize) {
  time_t now = time(nullptr);
  struct tm *timeinfo = localtime(&now);

  if (timeinfo->tm_year > 100) { // Valid time (year > 2000)
    if (strcmp(TIME_FORMAT, "US") == 0) {
      // US 12-hour format with AM/PM
      int hour12 = timeinfo->tm_hour % 12;
      if (hour12 == 0)
        hour12 = 12;
      const char *ampm = (timeinfo->tm_hour >= 12) ? "PM" : "AM";
      snprintf(buffer, bufferSize, "%d:%02d %s", hour12, timeinfo->tm_min,
               ampm);
    } else {
      // German/European 24-hour format (default) with "Uhr"
      snprintf(buffer, bufferSize, "%02d:%02d Uhr", timeinfo->tm_hour,
               timeinfo->tm_min);
    }
  } else {
    // NTP not synced yet
    snprintf(buffer, bufferSize, "--:--");
  }
}

// ============================================================================
// WIFI: Initialize and connect
// ============================================================================
bool connectWiFi() {
  DEBUG_PRINTLN(F("\n[WiFi] Connecting..."));
  DEBUG_PRINT(F("[WiFi] SSID: "));
  DEBUG_PRINTLN(WIFI_SSID);

  // Show connecting status on display
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 20);
  display.println(F("Connecting to WiFi"));
  display.setCursor(0, 35);
  display.print(F("SSID: "));
  display.println(WIFI_SSID);
  display.display();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // Wait for connection with timeout (30 attempts, ~15 seconds)
  uint8_t attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    DEBUG_PRINT(F("."));

    // Update display with progress dots
    display.print(F("."));
    display.display();

    attempts++;
    yield(); // Feed watchdog
  }

  if (WiFi.status() == WL_CONNECTED) {
    g_wifiConnected = true;
    DEBUG_PRINTLN(F("\n[WiFi] Connected!"));
    DEBUG_PRINT(F("[WiFi] IP: "));
    DEBUG_PRINTLN(WiFi.localIP());

    // Show success briefly
    display.clearDisplay();
    display.setCursor(0, 25);
    display.println(F("WiFi Connected!"));
    display.setCursor(0, 40);
    display.print(F("IP: "));
    display.println(WiFi.localIP());
    display.display();
    delay(1500);

    return true;
  } else {
    g_wifiConnected = false;
    DEBUG_PRINTLN(F("\n[WiFi] FAILED to connect!"));

    display.clearDisplay();
    display.setCursor(0, 25);
    display.println(F("WiFi FAILED!"));
    display.setCursor(0, 40);
    display.println(F("Check credentials"));
    display.display();

    return false;
  }
}

// ============================================================================
// WIFI: Check and reconnect if needed
// ============================================================================
void checkWiFiConnection() {
  if (WiFi.status() == WL_CONNECTED) {
    g_wifiConnected = true;
    return;
  }

  g_wifiConnected = false;
  DEBUG_PRINTLN(F("[WiFi] Connection lost, reconnecting..."));

  // Attempt reconnection
  WiFi.disconnect();
  delay(100);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint8_t attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 10) {
    delay(500);
    attempts++;
    yield();
  }

  g_wifiConnected = (WiFi.status() == WL_CONNECTED);

  if (g_wifiConnected) {
    DEBUG_PRINTLN(F("[WiFi] Reconnected!"));
  } else {
    DEBUG_PRINTLN(F("[WiFi] Reconnection failed"));
  }
}

// ============================================================================
// API: Fetch price from Binance
// Returns true on success, false on failure
// ============================================================================
bool fetchPriceBinance() {
  DEBUG_PRINTLN(F("[API] Trying Binance..."));

  HTTPClient http;

  // Build URL dynamically with currency
  char host[32];
  strcpy_P(host, API_BINANCE_HOST);

  // Binance uses USDT (Tether) instead of USD
  // Map USD -> USDT for Binance API
  const char *binanceCurrency = g_currencyCode;
  if (strcmp(g_currencyCode, "USD") == 0) {
    binanceCurrency = "USDT";
  }

  char url[120];
  snprintf(url, sizeof(url), "https://%s/api/v3/ticker/24hr?symbol=BTC%s", host,
           binanceCurrency);

  // Use secure client with certificate validation disabled (public data)
  g_secureClient.setInsecure();
  http.begin(g_secureClient, url);
  http.setTimeout(10000); // 10 second timeout

  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    // Parse JSON response
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
        g_cachedPrice = g_currentPrice;
        g_cachedChange = g_priceChange24h;
        g_hasValidData = true;
        g_dataStale = false;

        DEBUG_PRINTF("[API] Binance OK: %.2f (%.2f%%)\n", g_currentPrice,
                     g_priceChange24h);
        g_lastSuccessTime = millis();
        return true;
      } else {
        DEBUG_PRINTLN(F("[API] Binance: Missing keys"));
        DEBUG_PRINT(F("[API] Response: "));
        DEBUG_PRINTLN(payload.substring(0, 100));
      }
    } else {
      DEBUG_PRINT(F("[API] Binance JSON error: "));
      DEBUG_PRINTLN(error.c_str());
      DEBUG_PRINT(F("[API] Response: "));
      DEBUG_PRINTLN(payload.substring(0, 100));
    }
  } else {
    DEBUG_PRINTF("[API] Binance HTTP error: %d\n", httpCode);
    http.end();
  }
  return false;
}

// ============================================================================
// API: Fetch price from CoinGecko
// Returns true on success, false on failure
// ============================================================================
bool fetchPriceCoinGecko() {
  DEBUG_PRINTLN(F("[API] Trying CoinGecko..."));

  HTTPClient http;

  // Build URL dynamically with currency
  char host[32];
  strcpy_P(host, API_COINGECKO_HOST);

  // Create lowercase currency for CoinGecko API
  char currencyLower[8];
  for (int i = 0; g_currencyCode[i] && i < 7; i++) {
    currencyLower[i] = tolower(g_currencyCode[i]);
    currencyLower[i + 1] = '\0';
  }

  char url[200];
  // Build CoinGecko API URL
  strcpy(url, "https://");
  strcat(url, host);
  strcat(url, "/api/v3/simple/price?ids=bitcoin");
  strcat(url, "&vs_currencies=");
  strcat(url, currencyLower);
  strcat(url, "&include_24hr_change=true");

  DEBUG_PRINT(F("[API] URL: "));
  DEBUG_PRINTLN(url);

  // Use secure client with certificate validation disabled (public data)
  g_secureClient.setInsecure();
  http.begin(g_secureClient, url);
  http.setTimeout(10000);

  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    // Read response as string first (more reliable than streaming)
    String payload = http.getString();
    http.end();

    DEBUG_PRINT(F("[API] CoinGecko response length: "));
    DEBUG_PRINTLN(payload.length());

    // Parse JSON response
    // CoinGecko response: {"bitcoin":{"eur":52345.67,"eur_24h_change":2.45}}
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
      DEBUG_PRINT(F("[API] CoinGecko JSON error: "));
      DEBUG_PRINTLN(error.c_str());
      DEBUG_PRINT(F("[API] Response: "));
      DEBUG_PRINTLN(
          payload.substring(0, 100)); // Print first 100 chars for debug
      return false;
    }

    // Extract values using lowercase currency key
    JsonObject bitcoin = doc["bitcoin"];
    if (bitcoin.containsKey(currencyLower)) {
      g_currentPrice = bitcoin[currencyLower].as<float>();

      // Build 24h change key (e.g., "eur_24h_change")
      char changeKey[20];
      snprintf(changeKey, sizeof(changeKey), "%s_24h_change", currencyLower);
      g_priceChange24h = bitcoin[changeKey].as<float>();

      // Update cache
      g_cachedPrice = g_currentPrice;
      g_cachedChange = g_priceChange24h;
      g_hasValidData = true;
      g_dataStale = false;

      DEBUG_PRINTF("[API] CoinGecko OK: %.2f (%.2f%%)\n", g_currentPrice,
                   g_priceChange24h);
      g_lastSuccessTime = millis();
      return true;

    } else {
      DEBUG_PRINTLN(F("[API] CoinGecko: currency key not found"));
      DEBUG_PRINT(F("[API] Response: "));
      DEBUG_PRINTLN(payload.substring(0, 100));
    }
  } else {
    DEBUG_PRINTF("[API] CoinGecko HTTP error: %d\n", httpCode);
    http.end();
  }

  http.end();
  return false;
}

// ============================================================================
// API: Main fetch function with fallback logic
// ============================================================================
void fetchPrice() {
  if (!g_wifiConnected) {
    DEBUG_PRINTLN(F("[API] No WiFi, using cached data"));
    if (g_hasValidData) {
      g_currentPrice = g_cachedPrice;
      g_priceChange24h = g_cachedChange;
      g_dataStale = true;
    }
    return;
  }

  bool success = false;

  // Reset to primary provider after 15 minutes
  if (millis() - g_providerResetTime > 900000) { // 15 min
    g_currentProvider = 0;
    g_providerResetTime = millis();
    DEBUG_PRINTLN(F("[API] Reset to primary provider"));
  }

  // Try providers in order with fallback
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
    DEBUG_PRINTF("[API] All providers failed. Failures: %d\n",
                 g_consecutiveFailures);

    // Use cached data as emergency fallback
    if (g_hasValidData) {
      g_currentPrice = g_cachedPrice;
      g_priceChange24h = g_cachedChange;
      g_dataStale = true;
      DEBUG_PRINTLN(F("[API] Using cached data"));
    }
  } else {
    g_consecutiveFailures = 0;
  }

  // Print heap status for debugging
  DEBUG_PRINTF("[Mem] Free heap: %d bytes\n", ESP.getFreeHeap());
}

// ============================================================================
// DISPLAY: Draw WiFi status icon
// ============================================================================
void drawWiFiIcon(int x, int y, bool connected) {
  if (connected) {
    // Simple WiFi icon (3 arcs)
    display.drawPixel(x + 3, y + 6, SSD1306_WHITE);
    display.drawCircle(x + 3, y + 6, 2, SSD1306_WHITE);
    display.drawCircle(x + 3, y + 6, 4, SSD1306_WHITE);
  } else {
    // X mark for disconnected
    display.drawLine(x, y, x + 6, y + 6, SSD1306_WHITE);
    display.drawLine(x + 6, y, x, y + 6, SSD1306_WHITE);
  }
}

// ============================================================================
// DISPLAY: Draw trend arrow
// ============================================================================
void drawTrendArrow(int x, int y, bool up) {
  if (up) {
    // Up arrow
    display.drawLine(x + 3, y, x, y + 5, SSD1306_WHITE);
    display.drawLine(x + 3, y, x + 6, y + 5, SSD1306_WHITE);
    display.drawLine(x + 3, y, x + 3, y + 8, SSD1306_WHITE);
  } else {
    // Down arrow
    display.drawLine(x + 3, y + 8, x, y + 3, SSD1306_WHITE);
    display.drawLine(x + 3, y + 8, x + 6, y + 3, SSD1306_WHITE);
    display.drawLine(x + 3, y + 8, x + 3, y, SSD1306_WHITE);
  }
}

// ============================================================================
// DISPLAY: Main update function
// ============================================================================
void updateDisplay() {
  display.clearDisplay();

  // ---- Header row ----
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(F("BTC/"));
  display.print(g_currencyCode);

  // Show provider next to currency pair
  display.print(F(" "));
  if (g_currentProvider == 0) {
    display.print(F("Binance"));
  } else {
    display.print(F("CoinGecko"));
  }

  // Stale data indicator
  if (g_dataStale) {
    display.setCursor(94, 0);
    display.print(F("OLD"));
  }

  // WiFi icon on right side
  drawWiFiIcon(118, 0, g_wifiConnected);

  // Horizontal separator line
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  // ---- Price display (dynamic size to fit on one line) ----
  if (g_hasValidData) {
    formatPrice(g_currentPrice, g_priceBuffer, sizeof(g_priceBuffer));

    int priceLen = strlen(g_priceBuffer);
    int textSize;
    int charWidth;

    // Dynamic text sizing based on content length
    // Screen width is 128 pixels
    // Size 2: ~12px per char, Size 1: ~6px per char
    if (priceLen <= 10) {
      textSize = 2; // "98.234 EUR" = 10 chars fits at size 2
      charWidth = 12;
    } else {
      textSize = 1; // Fallback to smaller for very long numbers
      charWidth = 6;
    }

    // Center the text
    int textWidth = priceLen * charWidth;
    int priceX = (128 - textWidth) / 2;
    if (priceX < 0)
      priceX = 0;

    display.setTextSize(textSize);
    display.setCursor(priceX, 18);
    display.print(g_priceBuffer);

    // ---- 24h change with trend arrow ----
    display.setTextSize(1);

    // Draw trend arrow
    bool isUp = g_priceChange24h >= 0;
    drawTrendArrow(10, 40, isUp);

    // Format and display change percentage
    formatChange(g_priceChange24h, g_changeBuffer, sizeof(g_changeBuffer));
    display.setCursor(22, 42);
    display.print(g_changeBuffer);
    display.print(F(" 24h"));

  } else {
    // No data yet
    display.setTextSize(1);
    display.setCursor(20, 25);
    display.print(F("Fetching price..."));
  }

  // ---- Timestamp footer ----
  display.setTextSize(1);
  display.setCursor(0, 56);
  display.print(F("Updated: "));
  getCurrentTimeStr(g_timeBuffer, sizeof(g_timeBuffer));
  display.print(g_timeBuffer);

  display.display();
}

// ============================================================================
// DISPLAY: Show initialization screen
// ============================================================================
void showSplashScreen() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(10, 10);
  display.print(F("BTC/"));
  display.print(g_currencyCode);
  display.setTextSize(1);
  display.setCursor(25, 35);
  display.println(F("Live Ticker"));
  display.setCursor(15, 50);
  display.println(F("Initializing..."));
  display.display();
  delay(2000);
}

// ============================================================================
// SETUP
// ============================================================================
void setup() {
  // Initialize Serial for debugging
  Serial.begin(115200);
  delay(100);

  DEBUG_PRINTLN(F("\n\n========================================"));
  DEBUG_PRINTLN(F("ESP8266 Bitcoin Ticker"));
  DEBUG_PRINTLN(F("========================================"));
  DEBUG_PRINTF("[Mem] Initial free heap: %d bytes\n", ESP.getFreeHeap());

  // Initialize I2C with correct pins for Wemos D1 Mini
  Wire.begin(SDA_PIN, SCL_PIN);

  // Initialize display
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS)) {
    DEBUG_PRINTLN(F("[Display] SSD1306 allocation failed!"));
    // Blink built-in LED to indicate error
    pinMode(LED_BUILTIN, OUTPUT);
    while (true) {
      digitalWrite(LED_BUILTIN, LOW);
      delay(100);
      digitalWrite(LED_BUILTIN, HIGH);
      delay(100);
    }
  }

  display.clearDisplay();
#if FLIP_DISPLAY
  display.setRotation(2);
#endif
  display.display();

  DEBUG_PRINTLN(F("[Display] Initialized OK"));

  // Show splash screen
  showSplashScreen();

  // Connect to WiFi
  if (!connectWiFi()) {
    DEBUG_PRINTLN(F("[Setup] WiFi failed, will retry in main loop"));
  } else {
    // Configure NTP for German timezone (CET = UTC+1, CEST = UTC+2 with DST)
    // TZ string: CET-1CEST,M3.5.0,M10.5.0/3 (Central European Time with DST)
    configTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.nist.gov");
    DEBUG_PRINTLN(F("[NTP] Syncing time..."));

    // Wait briefly for NTP sync
    int ntpRetries = 0;
    while (time(nullptr) < 1000000000 && ntpRetries < 10) {
      delay(500);
      ntpRetries++;
      yield();
    }
    if (time(nullptr) > 1000000000) {
      DEBUG_PRINTLN(F("[NTP] Time synced!"));
    } else {
      DEBUG_PRINTLN(F("[NTP] Sync failed, will retry"));
    }
  }

  // Fetch initial price
  fetchPrice();
  updateDisplay();

  g_lastPollTime = millis();
  g_providerResetTime = millis();

  DEBUG_PRINTLN(F("[Setup] Complete!"));
  DEBUG_PRINTF("[Mem] Free heap after setup: %d bytes\n", ESP.getFreeHeap());
}

// ============================================================================
// MAIN LOOP
// ============================================================================
void loop() {
  // Feed watchdog
  yield();

  unsigned long currentMillis = millis();

  // Check WiFi connection periodically
  static unsigned long lastWiFiCheck = 0;
  if (currentMillis - lastWiFiCheck > 5000) { // Every 5 seconds
    checkWiFiConnection();
    lastWiFiCheck = currentMillis;
  }

  // Poll for new price at interval
  if (currentMillis - g_lastPollTime >= POLL_INTERVAL_MS) {
    DEBUG_PRINTLN(F("\n--- Polling for price ---"));
    fetchPrice();
    // Check if data is stale (no success in last 3 poll intervals)
    if (millis() - g_lastSuccessTime > (POLL_INTERVAL_MS * 3)) {
      g_dataStale = true;
    }
    updateDisplay();
    g_lastPollTime = currentMillis;

    // Periodic heap report
    DEBUG_PRINTF("[Mem] Free heap: %d bytes\n", ESP.getFreeHeap());
  }

  // ---- Price Alert Blinking ----
  // Check if price is outside threshold bounds
  static unsigned long lastBlinkTime = 0;
  static bool displayVisible = true;

  bool alertActive = false;
  if (g_hasValidData) {
    if (ALERT_PRICE_LOW > 0 && g_currentPrice < ALERT_PRICE_LOW) {
      alertActive = true; // Price too low!
    }
    if (ALERT_PRICE_HIGH > 0 && g_currentPrice > ALERT_PRICE_HIGH) {
      alertActive = true; // Price too high!
    }
  }

  if (alertActive) {
    // Blink the display every ALERT_BLINK_INTERVAL_MS
    if (currentMillis - lastBlinkTime >= ALERT_BLINK_INTERVAL_MS) {
      displayVisible = !displayVisible;
      lastBlinkTime = currentMillis;

      if (displayVisible) {
        updateDisplay(); // Show display
      } else {
        display.clearDisplay(); // Hide display (blink off)
        display.display();
      }
    }
  } else {
    // No alert - ensure display is visible
    if (!displayVisible) {
      displayVisible = true;
      updateDisplay();
    }
  }

  // Small delay to prevent tight loop
  delay(100);
}
