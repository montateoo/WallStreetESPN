# WallStreetESPN

A compact, self-contained stock market display built on the ESP8266. It fetches real-time prices from the Finnhub API, cycles through your personal portfolio on a 128x64 OLED, and runs a paper trading algorithm in the background — all manageable from a built-in web interface.

---

## Features

- Real-time stock quotes (Finnhub API, free tier)
- Smooth horizontal slide animations between stocks
- Market countdown — shows time until NYSE opens or closes
- Paper trading — SMA(3)/SMA(7) crossover algorithm with $100 virtual budget
- Built-in web UI — add/remove stocks, configure settings, view trading dashboard
- PWM-controlled LED brightness (green = price up / red = price down)
- Persistent storage — portfolio, API key, settings and trading state survive reboots
- NTP time sync (CET/CEST automatic)

---

## Hardware

| Component | Details |
|---|---|
| Microcontroller | ESP8266 (NodeMCU / Wemos D1 Mini) |
| Display | 128x64 OLED I2C (SSD1306, address 0x3C) |
| Green LED | GPIO 13 (D7) + 220Ω resistor |
| Red LED | GPIO 15 (D8) + 220Ω resistor |
| Button — Mode | GPIO 14 (D5) + 10kΩ pull-down |
| Button — WiFi Reset | GPIO 12 (D6) + 10kΩ pull-down |
| I2C SDA | GPIO 4 (D2) |
| I2C SCL | GPIO 5 (D1) |
| Power | USB-C 4.5–9V module |

See `Schematic_wallStreetDisplay_2025-08-30.pdf` for the full circuit diagram.  
3D-printable enclosure parts are in `3DprintComponents/`.

---

## Software Dependencies

Install all of these through **Arduino IDE → Sketch → Include Library → Manage Libraries**:

| Library | Author |
|---|---|
| ArduinoJson | Benoit Blanchon |
| WiFiManager | tzapu |
| Adafruit SSD1306 | Adafruit |
| Adafruit GFX Library | Adafruit |

Board package (via Boards Manager):  
`https://arduino.esp8266.com/stable/package_esp8266com_index.json`  
→ Install **esp8266 by ESP8266 Community**

---

## Setup

### 1. Get a Finnhub API key
Sign up for free at [finnhub.io](https://finnhub.io) and copy your API key.

### 2. Flash the firmware
- Open `code.ino` in Arduino IDE
- Select **Tools → Board → ESP8266 Boards → NodeMCU 1.0**
- Select the correct **Tools → Port** (use Device Manager to find it)
- Upload speed: 115200
- Serial monitor baud rate: **115200**
- Click Upload

### 3. First boot — connect to WiFi
On first boot the device creates a WiFi access point:
- SSID: `WallStreetDisplay`
- Password: `12345678`

Connect to it, enter your home WiFi credentials, and the device will reboot and connect.  
To reset WiFi credentials later: hold the WiFi Reset button for 5 seconds.

### 4. Configure via web UI
With the device connected to your network, press the **Mode button** to enter Server Mode. The OLED shows the IP address. Open it in a browser and:

- Add stock symbols (e.g. `AAPL`, `TSLA`, `MSFT`)
- Enter your Finnhub API key in Settings
- Set the display interval per stock
- Adjust LED brightness (green and red independently)
- Toggle the paper trading algorithm on/off

Press the Mode button again to return to Display Mode.

---

## Display Cycle

In Display Mode the screen cycles through:

```
Stock 1 → Stock 2 → ... → Stock N → Market Countdown → Trading Status → Stock 1 → ...
```

**Stock screen** — symbol, current price ($), daily change (%)  
**Market Countdown** — OPENS / CLOSES with hours and minutes remaining  
**Trading Status** — portfolio value, last trade, total P&L% (shown only when trading is enabled)

---

## Web Interface

| URL | Description |
|---|---|
| `http://<device-ip>/` | Portfolio management |
| `http://<device-ip>/trading` | Paper trading dashboard |
| `http://<device-ip>/add?value=AAPL` | Add a stock |
| `http://<device-ip>/remove?value=AAPL` | Remove a stock |
| `http://<device-ip>/clear` | Remove all stocks |
| `http://<device-ip>/setkey?key=...` | Set API key |
| `http://<device-ip>/settime?time=5` | Set display interval (seconds) |
| `http://<device-ip>/setledgreen?val=80` | Set green LED brightness (0–100) |
| `http://<device-ip>/setledred?val=80` | Set red LED brightness (0–100) |
| `http://<device-ip>/toggletrading` | Toggle trading algorithm on/off |

---

## Paper Trading Algorithm

The device paper-trades your portfolio stocks with a virtual $100 starting budget.

**Algorithm:** SMA(3) / SMA(7) crossover  
- Prices are sampled every 10 minutes (each API fetch cycle)  
- After 7 samples a signal is available  
- **Golden cross** (SMA3 crosses above SMA7) → BUY up to 35% of portfolio value  
- **Death cross** (SMA3 crosses below SMA7) → SELL full position  
- Only trades during NYSE market hours (15:30–22:00 CET)  
- State (cash, positions, trade log) persists in LittleFS as `/trading.json`

Trading can be enabled/disabled from the Settings panel without reflashing.

---

## Storage Layout

| Storage | Key | Content |
|---|---|---|
| LittleFS | `/list.txt` | Stock symbols (one per line) |
| LittleFS | `/trading.json` | Cash balance, open positions, realized P&L |
| EEPROM 0–100 | — | Finnhub API key |
| EEPROM 101–104 | — | Display interval (ms) |
| EEPROM 105 | — | Green LED brightness (0–100) |
| EEPROM 106 | — | Red LED brightness (0–100) |
| EEPROM 107 | — | Trading enabled (0/1) |

---

## Secrets

API keys and credentials are stored in `secrets.toml` (git-ignored):

```toml
[api]
finnhub_key = "your_key_here"
```

This file is for reference only — the key is configured on the device via the web UI and stored in EEPROM.

---

## LED Behaviour

| State | Green LED | Red LED |
|---|---|---|
| Stock price up | ON | OFF |
| Stock price down | OFF | ON |
| Market open (countdown) | Blinking | OFF |
| Market closed (countdown) | OFF | Blinking |
| Server Mode | Alternating | Alternating |

Brightness is independently configurable (0–100%) via PWM.
