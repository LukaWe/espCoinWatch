# ESP8266 Bitcoin Live Ticker

![Bitcoin Ticker Demo](img/demo.png)

Bitcoin price display for Wemos D1 Mini with SSD1306 OLED.

Two versions are included in this project:
1. (Option A) Web Config Version (btc_ticker_websrv.ino) - Recommended
   Allows configuration via WiFi (phone/laptop). No coding changes needed.
2. (Option B) Standard Version (btc_ticker.ino)
   Requires hardcoding WiFi credentials in the code.
3. (Option C) Weather & Enhanced Ticker (btc_ticker_weather_websrv.ino)
   Advanced version with 4 API sources, Weather Forecast (Temp/Humidity/Rain/Wind [Optional]), and Web Config.

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

## Option A: Web Config Version (Recommended)
File: btc_ticker_websrv.ino

This version creates a WiFi hotspot for setup. All settings are saved permanently on the device.

### Features
- Configure WiFi, Currency, and Alerts via web browser
- Data persists after power loss (saved to flash memory)
- Automatic fallback to Setup Mode if WiFi fails

### How to Use
1. Upload btc_ticker_websrv.ino to your ESP8266.
2. Power on the device. It will display a "SETUP MODE" screen.
3. On your phone/computer, connect to the WiFi network:
   SSID: BTC-Ticker-Setup
   (No password)
4. A configuration page should open automatically.
   If not, open a browser and go to: http://192.168.4.1
5. Enter your settings:
   - WiFi SSID and Password
   - Currency (EUR, USD, GBP, etc.)
   - Poll Interval (default 60000ms = 60s)
   - Time Format (DE=24h, US=12h)
   - Alert Thresholds (Price Low/High)
6. Click "Save & Reboot".

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

## Option B: Standard Version
File: btc_ticker.ino

This version is simpler but requires you to edit the code to change settings.

### How to Use
1. Open btc_ticker.ino in Arduino IDE.
2. Edit the configuration section at the top of the file:
   
   // Network Settings
   const char* WIFI_SSID = "YOUR_WIFI_SSID";
   const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

   // App Settings
   #define CURRENCY "EUR"
   #define TIME_FORMAT "DE"
   #define ALERT_PRICE_LOW 90000
   #define ALERT_PRICE_HIGH 110000
   #define FLIP_DISPLAY 0 // Set to 1 to rotate screen 180 degrees

3. Upload to the device.

---

## Option C: Weather & Enhanced Ticker
File: btc_ticker_weather_websrv.ino

The most advanced version, building on Option A but adding multi-API support and live weather data.

### Extra Features
- **4 Crypto API Sources**: Binance, CoinGecko, Coinbase, Kraken (Auto-failover).
- **Live Weather**: Current temperature, humidity, rain chance, and wind speed (optional).
- **City Presets**: Easy selection for major German cities or custom Lat/Lon.
- **Customizable Cycles**: Set how long to show Price vs Weather screens.
- **Detailed Config**: All new options available via the Web Config portal.

### How to Use
1. Upload `btc_ticker_weather_websrv.ino` to your ESP8266.
2. Follow the **Setup Mode** instructions from Option A (connect to `BTC-Ticker-Setup`).
3. In the config menu, you will see additional options:
   - **API Source**: Choose your preferred price data source.
   - **Weather Settings**: Enable weather, select city, or enter coordinates.
   - **Screen Cycles**: Adjust duration (e.g., show Bitcoin for 120s, Weather for 10s).
4. Save & Reboot.

### Web Configuration Panel
Once the device connects to your WiFi, you can change settings at any time without resetting:
1. Find the device's IP address (check your router's client list or look at the Serial Monitor during boot).
2. Open a web browser and enter the IP address (e.g., `http://192.168.1.105`).
3. The configuration page is divided into collapsible sections:

#### 1. Ticker Settings
- **WiFi SSID/Password**: Your network credentials.
- **Currency**: Fiat currency to display (EUR, USD, GBP, etc.).
- **Cryptocurrency**: Coin to track (BTC, ETH, SOL, etc.).
- **API Source**:
  - `Auto`: Tries Binance -> CoinGecko -> Coinbase -> Kraken.
  - Specific providers can be forced.
- **Poll Interval**: How often to fetch price (default: 60000ms = 1 min).
- **Time Format**: `DE` (24h) or `US` (12h AM/PM).

#### 2. Alert Settings
- **Alert Low/High**: Price thresholds (0 to disable).
- **Alert Pattern**: Slow (1s), Fast (250ms), Strobe (50ms), or SOS.
- **Alert Mode**: `Display Only`, `LED Only`, or `Both`.
- **LED Pin**: GPIO pin for LED (default: 2/D4).
- **LED Inverted**: Check for built-in LED (Active LOW).
- **Alert Duration**: Stop after X seconds (0 = infinite).
- **Flip Display**: Rotate screen 180 degrees.

#### 3. Touch Settings
- **Enable Touch Buttons**: Activates D0, D7, D8 for TTP223 modules.
- **D5 = Weather Button (BTC default)**:
  - Press D5 to show weather, returns to BTC after timeout.
  - Disables Factory Reset on D5.
- **D5 = BTC Button (Weather default)**:
  - Weather always shown. Press D5 to show BTC temporarily.
  - Disables Factory Reset on D5.
- **Screen Timeout**: How long alternate screen stays after releasing D5 (0 = instant return).

#### 4. Weather Settings
- **Enable Weather Display**: Toggle weather screen on/off.
- **Enable Wind Forecast**: Shows 4-grid layout with wind speed.
- **City Preset**: Quick-fill lat/lon for major German cities.
- **Custom Location Name**: Name shown on header (max 10 chars).
- **Latitude/Longitude**: Coordinates for weather data.
- **Screen Durations**:
  - `BTC Screen Duration`: How long price is shown (default: 120s).
  - `Weather Screen Duration`: How long weather is shown (default: 10s).
- **Weather Poll Interval**: How often to update weather (default: 30 mins).
- **Button Mode**:
  - `Auto Cycle`: Switches between Price/Weather automatically.
  - `Always Weather`: Shows weather permanently.
  - `On-Demand`: Price default; button shows Weather for one cycle.
  - `Button Press (no cycle)`: Manual control only via D5 button.

#### 5. Security
- **Web Password**: Set password for config page (username: `admin`).

#### 6. Device Logs
- **Show Logs**: Last 20 debug messages with auto-refresh.




---

## Display Layout

The display shows information in the following format:

```text
+--------------------------+
| BTC/USD Binance   [WiFi] |  <- Header: Pair + Provider
|--------------------------|
|                          |
|       98.234 USD         |  <- Price (Centered)
|                          |
|  ^ +2.45% 24h            |  <- Trend Arrow + 24h Change
| Updated: 14:30 Uhr       |  <- Last Update Time
+--------------------------+
```

indicators:
- [WiFi]: Dot icon indicates connection status
- OLD: Appears in header if data is stale (API failure)
- Provider: Shows Binance or CoinGecko

## Weather Display Layouts

When weather is enabled, two layout modes are available depending on whether Wind Forecast is active.

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
2x2 Grid layout showing Wind data. NOTE: Footer is removed to save space.

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

## API Information

Primary Provider: Binance (USDT/EUR pairs)
Secondary Provider: CoinGecko (Fallback)

The device automatically switches to the next provider in line if the current one fails.

## Troubleshooting

| Issue | Solution |
|-------|----------|
| Display Blank | Check wiring (SDA/SCL) and power (3.3V) |
| No WiFi (Web Version) | Device will start AP mode. Connect to BTC-Ticker-Setup |
| CoinGecko Error | Increase poll interval (max 50 requests/min) |
| Invalid Input | Check Serial Monitor for JSON parsing errors |
