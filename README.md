# ESP8266 Bitcoin Live Ticker

![Bitcoin Ticker Demo](img/demo.png)

**Production-ready BTC/EUR price ticker with live weather data for Wemos D1 Mini and SSD1306 OLED display.**

---

## Recommended Installation: Weather & Enhanced Ticker

**File:** `btc_ticker_weather_websrv.ino` *(Default & Recommended)*

This is the **full-featured, production-ready version** with multi-API support, live weather integration, and complete web-based configuration.

### Key Features
- **4 Crypto API Sources** with automatic failover (Binance, CoinGecko, Coinbase, Kraken)  
- **Multi-Crypto Support** (BTC, ETH, SOL, XRP, DOGE, and more)  
- **Live Weather Display** (Temperature, Humidity, Rain Probability, Wind Speed)  
- **Web Configuration Portal** (No code editing required)  
- **Password Protection** for web interface  
- **Advanced LED Alert System** with customizable blink patterns (Slow, Fast, Strobe, SOS)  
- **City Presets** for major German cities or custom GPS coordinates  
- **Touch Button Support** (TTP223 modules)  
- **Smart Auto-Cycling** between Price and Weather screens  
- **Debug Logging** (20-line circular buffer accessible via `/logs` endpoint)  

**[Jump to Setup Instructions](#option-c-weather--enhanced-ticker-recommended)**

---

## Alternative Versions

If you need a simpler setup or want to start with basic functionality, two legacy versions are available:

| Version | File | Description | Use Case |
|---------|------|-------------|----------|
| **Option A** | `btc_ticker_websrv.ino` | Web Config (Basic) | WiFi setup via web portal, single API source |
| **Option B** | `btc_ticker.ino` | Hardcoded Config | Manual configuration in code, minimal features |

These versions are maintained for compatibility but lack the advanced features of the recommended installation.

## Supported Hardware
| Component | Specification |
|-----------|---------------|
| Microcontroller | **Wemos D1 Mini**, clones, or any standard generic **ESP8266** module (NodeMCU, etc) |
| Display | **0.96" OLED** (I2C, SSD1306, 128x64 resolution) |
| Power | USB (Micro-USB/USB-C) or 5V source |

> **Note on Display Address**: The code uses the default I2C address `0x3C`. Some displays use `0x3D` (often selectable via a solder bridge on the back). If your screen remains blank, check this address and change `#define OLED_I2C_ADDRESS 0x3C` to `0x3D` in the code if needed.

## 3D Printed Case
You can print a case to house the Wemos D1 Mini and OLED display.
*   [D1 Mini Clone Case](https://www.thingiverse.com/thing:7284649) by **LukaWe**.
    *   Designed specifically to fit **D1 Mini clones** that are slightly larger or off-size compared to the original Wemos D1 Mini.
    *   *License*: Creative Commons - Attribution - Non-Commercial.
*   [Wemos D1 Mini Case](https://www.thingiverse.com/thing:2884823) by **Qrome**.
    *   Fits original Wemos D1 Mini boards.
    *   *License*: Creative Commons - Attribution - Non-Commercial.
*   **More Options**: There are many other excellent designs available on [Thingiverse](https://www.thingiverse.com/search?q=wemos+d1+mini+oled&type=things&sort=relevant) and [Printables](https://www.printables.com/search/models?q=wemos%20d1%20mini%20oled).

## Required Libraries (Arduino IDE)

Install these libraries via **Arduino IDE Library Manager** (Sketch → Include Library → Manage Libraries):

| Library | Version | Purpose |
|---------|---------|---------|
| **Adafruit SSD1306** | Latest | OLED display driver |
| **Adafruit GFX Library** | Latest | Graphics primitives (dependency of SSD1306) |
| **ArduinoJson** | Latest (v6.x recommended) | JSON parsing for API responses |
| **ESP8266WiFi** | Included with ESP8266 board package | WiFi connectivity |
| **LittleFS** | Included with ESP8266 board package | Configuration storage |

**Board Package:**  
Install **ESP8266 by ESP8266 Community** via **Tools → Board → Boards Manager** (search "ESP8266").

---

> **Quick Start Recommendation**  
> For first-time users, we recommend using **Option C: Weather & Enhanced Ticker** (`btc_ticker_weather_websrv.ino`).  
> It includes all features, web configuration, and requires no code editing.  
> [Jump to Setup Instructions](#option-c-weather--enhanced-ticker-recommended)

---

## Wiring Diagram
| Wemos D1 Mini | SSD1306 (OLED) |
|---------------|----------------|
| 3.3V | VCC |
| GND | GND |
| D1 (GPIO5) | SCL |
| D2 (GPIO4) | SDA |

Note: Use 3.3V for the OLED power.

### Optional Factory Reset Button
Connect a momentary push button or TTP223 touch sensor to D5 (GPIO14).

| Wemos D1 Mini | Button/TTP223 |
|---------------|---------------|
| D5 (GPIO14) | Signal/OUT |
| GND | GND |
| 3.3V | VCC (TTP223 only) |

**Factory Reset**: Hold for 10 seconds. A countdown appears on display.

**Note**: D5 can be reconfigured for Weather/BTC display in Touch Settings (see Option C).


---

<a name="option-c-weather--enhanced-ticker-recommended"></a>
## Option C: Weather & Enhanced Ticker (Recommended)
**File:** `btc_ticker_weather_websrv.ino`

The **default, production-ready version** with multi-API support, live weather integration, and complete web-based configuration.

### Features
- **4 Crypto API Sources**: Binance, CoinGecko, Coinbase, Kraken (Auto-failover)
- **Multi-Crypto Support**: BTC, ETH, SOL, XRP, DOGE, LTC, ADA, DOT, MATIC, LINK, AVAX, ATOM, XMR, and more
- **Live Weather**: Current temperature, humidity, rain chance, and wind speed (optional)
- **City Presets**: Easy selection for major German cities or custom Lat/Lon coordinates
- **Customizable Cycles**: Set how long to show Price vs Weather screens
- **Advanced LED Alerts**: Separate LED output with 4 blink patterns (Slow, Fast, Strobe, SOS)
- **Password Protection**: Secure your web config interface (optional)
- **Debug Logging**: 20-line circular log buffer accessible via `/logs` endpoint
- **Touch Button Support**: TTP223 capacitive touch modules on D0, D5, D7, D8
- **Persistent Configuration**: All settings saved to flash memory

### How to Use
1. **Upload** `btc_ticker_weather_websrv.ino` to your ESP8266 using Arduino IDE.
2. **Power on** the device. It will display a **"SETUP MODE"** screen.
3. **Connect** to the WiFi network on your phone/computer:
   - **SSID:** `BTC-Ticker-Setup`
   - **Password:** (None)
4. A configuration page should open automatically.  
   If not, open a browser and go to: **http://192.168.4.1**
5. **Configure** your settings:
   - WiFi credentials
   - Currency (EUR, USD, GBP, etc.)
   - Cryptocurrency (BTC, ETH, etc.)
   - API Source (Auto or specific provider)
   - Location for weather data
   - Alert thresholds
   - Optional: Web password for security
6. Click **"Save & Reboot"**.

The device will reboot and connect to your WiFi network.

### Web Configuration Panel

Once connected to your WiFi, you can access the configuration interface anytime:

1. **Find the device's IP address:**
   - Check your router's DHCP client list, or
   - Watch the Serial Monitor during boot (115200 baud)
2. **Open a browser** and navigate to the IP (e.g., `http://192.168.1.105`)
3. **Log in** (if password protection is enabled):
   - Username: `admin`
   - Password: (as configured)

The configuration page is organized into collapsible sections:

#### 1. Ticker Settings
- **WiFi SSID/Password**: Your network credentials
- **Currency**: Fiat currency to display (EUR, USD, GBP, CHF, JPY, CNY, etc.)
- **Cryptocurrency**: Coin to track (BTC, ETH, SOL, XRP, DOGE, etc.)
- **API Source**:
  - `Auto`: Tries Binance → CoinGecko → Coinbase → Kraken until one succeeds
  - Or select a specific provider
- **Poll Interval**: How often to fetch price data (default: 60000ms = 1 min)
- **Time Format**: `DE` (24-hour) or `US` (12-hour with AM/PM)

#### 2. Alert Settings
- **Alert Low/High**: Price thresholds (set to 0 to disable)
- **Blink Pattern (Low/High)**: Choose pattern for each threshold
  - `Slow` (1000ms)
  - `Fast` (250ms)
  - `Strobe` (50ms)
  - `SOS` (Morse code pattern)
- **Alert Mode**:
  - `Display Only` – Blink the OLED screen
  - `LED Only` – External LED on GPIO pin
  - `Both` – Display and LED simultaneously
- **LED Pin**: GPIO number for alert LED (default: 2 = built-in LED on D4)
- **LED Inverted**: Enable for active-low LEDs (built-in LED on most ESP8266)
- **Alert Duration**: Stop after X seconds (0 = infinite alert)
- **Flip Display**: Rotate screen 180° for inverted mounting

#### 3. Touch Settings
- **Enable Touch Buttons**: Activates D0, D7, D8 for TTP223 capacitive touch modules
- **D5 Button Modes**:
  - **D5 = Weather Button (BTC default)**  
    Press D5 to show weather temporarily, returns to BTC after timeout.  
    *Disables Factory Reset on D5.*
  - **D5 = BTC Button (Weather default)**  
    Weather always shown. Press D5 to show BTC temporarily.  
    *Disables Factory Reset on D5.*
- **Screen Timeout**: How long alternate screen stays visible after releasing D5 (0 = instant return)

#### 4. Weather Settings
- **Enable Weather Display**: Toggle weather screen on/off
- **Enable Wind Forecast**: Shows 4-grid layout (Temp, Humidity, Rain, Wind)
- **City Preset**: Quick-fill coordinates for major German cities:
  - Berlin, Hamburg, Munich, Cologne, Frankfurt, Stuttgart, Düsseldorf, Leipzig, Dortmund, Essen, Bremen, Dresden, Hanover, Nuremberg
- **Custom Location Name**: Name displayed in header (max 10 characters)
- **Latitude/Longitude**: GPS coordinates for weather data (auto-filled by city presets)
- **Screen Durations**:
  - `BTC Screen Duration`: How long price is shown (default: 120 seconds)
  - `Weather Screen Duration`: How long weather is shown (default: 10 seconds)
- **Weather Poll Interval**: How often to update weather (default: 1800000ms = 30 minutes)
- **Button Mode**:
  - `Auto Cycle` (0): Automatically switches between Price and Weather
  - `Always Weather` (1): Shows weather permanently, no cycling
  - `On-Demand` (2): Price is default; press button to show weather for one cycle
  - `D5 Button (No Cycle)` (3): Manual control only via D5 button, no auto-cycling

#### 5. Security
- **Web Password**: Protect the configuration page with a password
  - Username is always `admin`
  - Leave empty for no password protection

#### 6. Device Logs
- **Show Logs**: View the last 20 debug messages with auto-refresh
- Useful for troubleshooting API failures, connection issues, and other events

---

## Display Layout

The display shows information in the following format:

```text
+--------------------------+
| BTC/USD Binance   [WiFi] |  <- Header: Pair + Provider
|--------------------------|
|                          |
|       98,234 USD         |  <- Price (Centered)
|                          |
|  ↑ +2.45% 24h            |  <- Trend Arrow + 24h Change
| Updated: 14:30 Uhr       |  <- Last Update Time
+--------------------------+
```

**Indicators:**
- **[WiFi]**: Dot icon indicates connection status
- **OLD**: Appears in header if data is stale (API failure)
- **Provider**: Shows active API source (Binance, CoinGecko, Coinbase, or Kraken)

---

## Weather Display Layouts

When weather is enabled, two layout modes are available:

### 1. Standard 3-Metric Layout (Wind Disabled)
Vertical separation for Temperature, Humidity, and Rain Probability.

```text
+--------------------------+
| Berlin          [WiFi]   | <- City + Status
|--------------------------|
|   12C  |   45%  |   10%  |
|        |        |        |
|  Temp  |   Hum  |  Rain  |
|        |        |        |
|--------------------------|
| Updated: 14:30           |
+--------------------------+
```

### 2. Grid 4-Metric Layout (Wind Enabled)
2x2 Grid layout showing Wind data. Footer is removed to save space.

```text
+--------------------------+
| Berlin             14:30 | <- City + Time (Header)
|--------------------------|
|      22C    |     45%    | <- Temp | Humidity
|      Temp   |     Hum    |
|                          |
|      10%    |    12km    | <- Rain | Wind Speed
|      Rain   |    Wind    |
+--------------------------+
```

---

## Legacy Versions

### Option A: Web Config Version (Basic)
**File:** `btc_ticker_websrv.ino`

A simpler version with web-based WiFi configuration but only single API source support.

### Features
- Configure WiFi, Currency, and Alerts via web browser
- Data persists after power loss (saved to flash memory)
- Automatic fallback to Setup Mode if WiFi fails

### How to Use
1. Upload `btc_ticker_websrv.ino` to your ESP8266.
2. Power on the device. It will display a **"SETUP MODE"** screen.
3. Connect to the WiFi network:
   - **SSID:** `BTC-Ticker-Setup` (No password)
4. Configuration page opens automatically, or go to: **http://192.168.4.1**
5. Enter your settings:
   - WiFi SSID and Password
   - Currency (EUR, USD, GBP, etc.)
   - Poll Interval (default 60000ms = 60s)
   - Time Format (DE=24h, US=12h)
   - Alert Thresholds (Price Low/High)
6. Click **"Save & Reboot"**.

### Configuration Options
| Setting | Description |
|---------|-------------|
| Currency | Fiat currency for price display (EUR, USD, etc.) |
| Poll Interval | How often to fetch new data in milliseconds |
| Time Format | DE (14:30 Uhr) or US (2:30 PM) |
| Alert Low/High | Blinks display if price crosses these values |
| Blink Interval | Speed of blinking during alert |
| Flip Display | Rotates screen 180 degrees (for inverted mounting) |

### Resetting the Device (Web Config Version Only)
The instructions below apply to the **Web Config Version** where settings are stored in memory.

If you need to wipe your settings (WiFi, etc.) to start over, you have three options:

**Method 1: Serial Monitor (Soft Reset)**
1. Connect via USB and open Serial Monitor (115200 baud).
2. Reset the device.
3. Within the first 3 seconds, type `wipe` or `w` and press Enter.
4. The device will format its storage and restart in Setup Mode.

**Method 2: Arduino IDE (Hard Reset)**
1. In Arduino IDE, go to **Tools -> Erase Flash**.
2. Select **All Flash Contents**.
3. Re-upload the sketch.

**Method 3: Hardware Button (Optional)**
You can connect a momentary push button between **D5** and **GND**.
- **Usage**: Hold the button for **10 seconds**.
- **Visuals**: The screen will show a countdown.
- **Result**: Device formats storage and restarts.

**Automatic Fallback:**
If the device cannot connect to your saved WiFi (e.g. password changed), it will automatically restart the `BTC-Ticker-Setup` hotspot so you can update configuration.

---

### Option B: Standard Version (Hardcoded Config)
**File:** `btc_ticker.ino`

The simplest version requiring manual code editing for all configuration.

### How to Use
1. Open `btc_ticker.ino` in Arduino IDE.
2. Edit the configuration section at the top of the file:
   
```cpp
// Network Settings
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// App Settings
#define CURRENCY "EUR"
#define TIME_FORMAT "DE"
#define ALERT_PRICE_LOW 90000
#define ALERT_PRICE_HIGH 110000
#define FLIP_DISPLAY 0 // Set to 1 to rotate screen 180 degrees
```

3. Upload to the device.



## API Information

**Recommended Version (Weather & Enhanced Ticker):**
- **Primary Provider:** Binance (BTCUSDT/BTCEUR pairs)
- **Auto-Failover:** CoinGecko → Coinbase → Kraken
- **Weather Data:** Open-Meteo and BrightSky APIs

The device automatically switches to the next provider if the current one fails or times out.

**Legacy Versions (Options A & B):**
- **Primary Provider:** Binance (BTCUSDT pair)
- **Fallback Provider:** CoinGecko
- No weather support

## Troubleshooting

| Issue | Solution |
|-------|----------|
| Display Blank | Check wiring (SDA/SCL) and power (3.3V) |
| No WiFi (Web Version) | Device will start AP mode. Connect to BTC-Ticker-Setup |
| CoinGecko Error | Increase poll interval (max 50 requests/min) |
| Invalid Input | Check Serial Monitor for JSON parsing errors |
