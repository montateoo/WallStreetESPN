#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <map>
#include <Wire.h>
#include <EEPROM.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/Org_01.h>

// Reserve memory: 0..100 for API key, 101..104 for displayInterval
#define EEPROM_SIZE 200
#define EEPROM_APIKEY_ADDR 0
#define EEPROM_DISPLAY_ADDR 101
#define EEPROM_LED_GREEN_ADDR 105
#define EEPROM_LED_RED_ADDR   106
#define EEPROM_TRADING_EN_ADDR 107

// I2C pins for ESP8266 (D1 = SCL, D2 = SDA)
#define SCREEN_WIDTH 128 // OLED width, in pixels
#define SCREEN_HEIGHT 64 // OLED height, in pixels
#define OLED_RESET -1 // No reset pin
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
uint8_t virtualBuffer[2048]; // 256 x 64 bits

// === linked list Structure ===
struct Node {
  String data;
  Node* next;
};

struct StockData {
  float currentPrice = 0;
  float percent = 0;
};

struct tm timeinfo;

enum DisplayState {
    SHOW_STOCK,
    TRANSITION_TO_COUNTDOWN,
    SHOW_COUNTDOWN,
    TRANSITION_TO_TRADING,
    SHOW_TRADING,
    TRANSITION_TO_STOCK
};

//get the API key on https://finnhub.io/
String apiKey = "";
String ServerAdr = "";

static DisplayState displayState = SHOW_STOCK;
static unsigned long stateStartTime = 0;
static String oldSymbol = "";
static StockData oldData;
static bool firstRun = true;

std::map<String, StockData> stockCache;

Node* head = nullptr;

Node* currentNode = nullptr;
unsigned long lastQuoteRequestTime = 0;
const unsigned long quoteDelay = 1500;
bool isFetching = false;

const int wifiResetButtonPin = 12;
const int buttonPin = 14;
const int ledGreenPin = 13;
const int ledRedPin = 15;
bool serverMode = false;
int stockNum = 0;

Node* displayNode = nullptr;
unsigned long lastDisplaySwitch = 0;
unsigned long displayInterval = 0; //DEFAULT ITS 5 SECONDS
bool newStock = false;

// === Trading System ===
#define PRICE_HISTORY_LEN 10
#define SMA_SHORT_PERIOD  3
#define SMA_LONG_PERIOD   7
#define STARTING_CASH     100.0f
#define MAX_POSITION_PCT  0.35f

struct TradingPosition {
    float shares       = 0;
    float avgBuyPrice  = 0;
    float priceHistory[PRICE_HISTORY_LEN] = {};
    int   historyCount = 0;
    int   historyHead  = 0;
    bool  prevBullish  = false;
    bool  prevSet      = false;
};

struct TradeRecord {
    String symbol;
    String action;
    float  price;
    float  shares;
    float  pnl;
};

float tradingCash      = STARTING_CASH;
float totalRealizedPnL = 0;
int   ledGreenIntensity = 100;
int   ledRedIntensity   = 100;
bool  tradingEnabled    = true;
std::map<String, TradingPosition> tradingPositions;
TradeRecord tradeLog[5];
int tradeLogHead  = 0;
int tradeLogCount = 0;

//market hours (NY Stock Exchange)
const int MARKET_OPEN_HOUR = 15;
const int MARKET_OPEN_MINUTE = 30;
const int MARKET_CLOSE_HOUR = 22;
const int MARKET_CLOSE_MINUTE = 0;

String prevStatus = "";
String prevCountdown = "";

// NTP servers
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 3600;   // CET is UTC+1
const int   daylightOffset_sec = 3600; // +1 hour for CEST (DST)


//24x24 server icon bitmap
const unsigned char PROGMEM serverIcon[] = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f, 0xff, 0xf8, 0x3f, 0xff, 0xfc, 0x30, 0x00, 0x0c, 0x37, 
	0xc3, 0x6c, 0x37, 0xc3, 0x6c, 0x30, 0x00, 0x0c, 0x3f, 0xff, 0xfc, 0x3f, 0xff, 0xfc, 0x30, 0x00, 
	0x0c, 0x37, 0xc3, 0x6c, 0x37, 0xc3, 0x6c, 0x30, 0x00, 0x0c, 0x3f, 0xff, 0xfc, 0x3f, 0xff, 0xfc, 
	0x30, 0x00, 0x0c, 0x37, 0xc3, 0x6c, 0x37, 0xc3, 0x6c, 0x30, 0x00, 0x0c, 0x3f, 0xff, 0xfc, 0x1f, 
	0xff, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

//24x24 stock market icon bitmap
const unsigned char PROGMEM dollarIcon[] = {
0x00, 0x00, 0x00, 0x00, 0x7e, 0x00, 0x03, 0xff, 0xc0, 0x07, 0xff, 0xe0, 0x0f, 0xff, 0xf0, 0x1f, 
	0xe7, 0xf8, 0x1f, 0xc3, 0xf8, 0x3f, 0x81, 0xfc, 0x3f, 0x1c, 0xfc, 0x7f, 0x1f, 0xfe, 0x7f, 0x8f, 
	0xfe, 0x7f, 0xc3, 0xfe, 0x7f, 0xe1, 0xfe, 0x7f, 0xf8, 0xfe, 0x7f, 0xfc, 0xfe, 0x3f, 0x1c, 0xfc, 
	0x3f, 0x81, 0xfc, 0x1f, 0xc3, 0xf8, 0x1f, 0xe7, 0xf8, 0x0f, 0xff, 0xf0, 0x07, 0xff, 0xe0, 0x01, 
	0xff, 0x80, 0x00, 0x7e, 0x00, 0x00, 0x00, 0x00
};

// 32x32 wifi icon bitmap
const unsigned char PROGMEM wifiDraw[] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0xe0, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x03, 0xff, 0xff, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x1f, 0xff, 0xff, 0xf8, 0x00, 0x00, 
	0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x03, 0xff, 0xff, 0xff, 0xff, 0xc0, 0x00, 
	0x00, 0x0f, 0xff, 0xff, 0xff, 0xff, 0xf0, 0x00, 0x00, 0x1f, 0xff, 0xff, 0xff, 0xff, 0xf8, 0x00, 
	0x00, 0x7f, 0xff, 0x00, 0x00, 0xff, 0xfe, 0x00, 0x00, 0xff, 0xf8, 0x00, 0x00, 0x1f, 0xff, 0x00, 
	0x01, 0xff, 0xe0, 0x00, 0x00, 0x07, 0xff, 0x80, 0x03, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xc0, 
	0x07, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x7f, 0xe0, 0x07, 0xf8, 0x00, 0xff, 0xff, 0x00, 0x1f, 0xe0, 
	0x07, 0xf0, 0x07, 0xff, 0xff, 0xe0, 0x0f, 0xe0, 0x07, 0xe0, 0x3f, 0xff, 0xff, 0xfc, 0x07, 0xe0, 
	0x03, 0xc0, 0x7f, 0xff, 0xff, 0xfe, 0x03, 0xc0, 0x00, 0x01, 0xff, 0xff, 0xff, 0xff, 0x80, 0x00, 
	0x00, 0x03, 0xff, 0xff, 0xff, 0xff, 0xc0, 0x00, 0x00, 0x0f, 0xff, 0xc0, 0x07, 0xff, 0xf0, 0x00, 
	0x00, 0x0f, 0xfe, 0x00, 0x00, 0x3f, 0xf0, 0x00, 0x00, 0x1f, 0xf8, 0x00, 0x00, 0x1f, 0xf8, 0x00, 
	0x00, 0x1f, 0xe0, 0x00, 0x00, 0x07, 0xf8, 0x00, 0x00, 0x1f, 0xc0, 0x00, 0x00, 0x03, 0xf8, 0x00, 
	0x00, 0x0f, 0x00, 0x0f, 0xf8, 0x00, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x7f, 0xff, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x03, 0xff, 0xff, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x07, 0xff, 0xff, 0xe0, 0x00, 0x00, 
	0x00, 0x00, 0x0f, 0xff, 0xff, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x1f, 0xff, 0xff, 0xf8, 0x00, 0x00, 
	0x00, 0x00, 0x1f, 0xff, 0xff, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x1f, 0xe0, 0x03, 0xf8, 0x00, 0x00, 
	0x00, 0x00, 0x0f, 0x80, 0x01, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x60, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0xe0, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x1f, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f, 0xf8, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x3f, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0xfc, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x3f, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0xfc, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x3f, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0xfc, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x1f, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xf0, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x07, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

ESP8266WebServer server(80);

void drawStockFrame(String symbol, StockData data, int xOffset);
void updateLEDs(StockData data);

void setLedGreen(bool on) {
    analogWrite(ledGreenPin, on ? map(ledGreenIntensity, 0, 100, 0, 1023) : 0);
}
void setLedRed(bool on) {
    analogWrite(ledRedPin, on ? map(ledRedIntensity, 0, 100, 0, 1023) : 0);
}

void addNode(String value) {
  if (value == "") return;
  Node* newNode = new Node{value, nullptr};
  if (!head) head = newNode;
  else {
    Node* temp = head;
    while (temp->next) temp = temp->next;
    temp->next = newNode;
  }
  saveListToFS();
  stockNum++;
  newStock = true;
}

bool removeNode(String value) {
  Node* temp = head;
  Node* prev = nullptr;
  while (temp) {
    if (temp->data == value) {
      if (prev) prev->next = temp->next;
      else head = temp->next;
      delete temp;
      saveListToFS();
      stockNum--;
      return true;
    }
    prev = temp;
    temp = temp->next;
  }
  return false;
}

void clearList() {
  while (head) {
    Node* temp = head;
    head = head->next;
    delete temp;
  }
  saveListToFS();
  stockNum=0;

  currentNode = nullptr;
  isFetching = false;
}

String listToStringHTML() {
  String output = "<ul>";
  Node* temp = head;
  while (temp) {
    output += "<li>" + temp->data + "</li>";
    temp = temp->next;
  }
  output += "</ul>";
  return output;
}

void saveListToFS() {
  File file = LittleFS.open("/list.txt", "w");
  if (!file) {
    Serial.println("Errore apertura file per scrittura!");
    return;
  }
  Node* temp = head;
  while (temp) {
    file.println(temp->data);
    temp = temp->next;
  }
  file.close();
}

void freeListOnly() {
  while (head) {
    Node* temp = head;
    head = head->next;
    delete temp;
  }
  stockNum = 0;
  currentNode = nullptr;
  isFetching = false;
}


void loadListFromFS() {
  if (!LittleFS.exists("/list.txt")) {
    Serial.println("⚠️ list.txt non trovato");
    return;
  }

  File file = LittleFS.open("/list.txt", "r");
  if (!file) {
    Serial.println("⚠️ Impossibile aprire list.txt in lettura");
    return;
  }

  Node* tempHead = nullptr;
  Node* tail = nullptr;
  int count = 0;

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      Node* newNode = new Node{line, nullptr};
      if (!tempHead) {
        tempHead = newNode;
        tail = newNode;
      } else {
        tail->next = newNode;
        tail = newNode;
      }
      count++;
    }
  }
  file.close();

  if (tempHead) {
    freeListOnly();
    head = tempHead;
    stockNum = count;
    Serial.println("✅ Lista caricata da LittleFS");
  } else {
    Serial.println("⚠️ File vuoto o nessun dato valido");
  }
}

// WEB SERVER
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>WallStreetDisplay</title>
  <style>
    body {
      font-family: Arial, sans-serif;
      background-color: #f2f2f2;
      display: flex;
      justify-content: center;
      align-items: center;
      height: 100vh;
      margin: 0;
    }
    .container {
      background-color: #fff;
      padding: 30px;
      border-radius: 12px;
      box-shadow: 0 4px 8px rgba(0,0,0,0.1);
      text-align: center;
      max-width: 500px;
      width: 100%;
    }
    h1 {
      color: #333;
      font-size: 28px;
    }
    .form-row {
      margin: 10px 0;
    }
    form {
      display: flex;
    }
    .text-input {
      padding: 10px;
      font-size: 16px;
      border: 1px solid #ccc;
      border-radius: 4px 0 0 4px;
      flex: 1;
    }
    .blue-button {
      padding: 10px 20px;
      font-size: 16px;
      background-color: #2196F3;
      color: white;
      border: none;
      border-radius: 0 4px 4px 0;
      cursor: pointer;
    }
    .blue-button:hover {
      background-color: #0b7dda;
    }
    .red-button {
      padding: 10px 20px;
      background-color: #f44336;
      color: white;
      border: none;
      border-radius: 6px;
      cursor: pointer;
      margin-top: 15px;
    }
    .red-button:hover {
      background-color: #d32f2f;
    }
    .green-button {
      padding: 10px 20px;
      background-color: #4CAF50;
      color: white;
      border: none;
      border-radius: 6px;
      cursor: pointer;
      margin-top: 15px;
    }
    .green-button:hover {
      background-color: #3e8e41;
    }
    .settings-button {
      margin-top: 20px;
      background-color: #4CAF50;
      border: none;
      color: white;
      padding: 10px 20px;
      border-radius: 6px;
      cursor: pointer;
    }
    .settings-button:hover {
      background-color: #3e8e41;
    }
    #settingsPanel {
      display: none;
      margin-top: 15px;
      padding: 15px;
      border: 1px solid #ccc;
      border-radius: 8px;
      background: #fafafa;
    }
    ul {
      text-align: left;
      margin-top: 20px;
      padding-left: 20px;
    }
    li {
      background: #eee;
      margin: 4px 0;
      padding: 6px;
      border-radius: 4px;
    }
    #confirmModal {
      display: none;
      position: fixed;
      top: 0;
      left: 0;
      width: 100%;
      height: 100%;
      background: rgba(0,0,0,0.5);
      text-align: center;
    }
    .modal-content {
      background: white;
      margin: 15% auto;
      padding: 20px;
      border-radius: 8px;
      width: 300px;
    }
  </style>
  <script>
    function showCustomConfirm() {
      document.getElementById("confirmModal").style.display = "block";
    }
    function hideCustomConfirm() {
      document.getElementById("confirmModal").style.display = "none";
    }
    function proceedClear() {
      window.location.href = "/clear";
    }
    function toggleSettings() {
      const panel = document.getElementById("settingsPanel");
      panel.style.display = (panel.style.display === "none" ? "block" : "none");
    }
    function showSavedPopup() {
      const popup = document.getElementById("savedPopup");
      popup.style.display = "block";
      setTimeout(() => { popup.style.display = "none"; }, 2000);
    }
    function showErrorPopup() {
      const popup = document.getElementById("errorPopup");
      popup.style.display = "block";
      setTimeout(() => { popup.style.display = "none"; }, 2000);
    }
    window.addEventListener('DOMContentLoaded', () => {
      const params = new URLSearchParams(window.location.search);
        if (params.get("saved") === "1") {
          showSavedPopup();
          window.history.replaceState({}, document.title, window.location.pathname);
        } else if (params.get("saved") === "2") {
          showErrorPopup();
          window.history.replaceState({}, document.title, window.location.pathname);
        }
    });

  </script>
</head>
<body>
  <div class="container">
    <h1>WallStreetDisplay</h1>

    <div class="form-row">
      <form action='/add'>
        <input type='text' name='value' placeholder='Add new stock' class='text-input'>
        <button type='submit' class='blue-button'>Add</button>
      </form>
    </div>

    <div class="form-row">
      <form action='/remove'>
        <input type='text' name='value' placeholder='Remove stock' class='text-input'>
        <button type='submit' class='blue-button'>Remove</button>
      </form>
    </div>

    <button class='red-button' onclick='showCustomConfirm()'>Erase all</button>

    <h3>My stocks:</h3>
)rawliteral";

  html += listToStringHTML();

  extern String apiKey;
  html += R"rawliteral(
    <button class='settings-button' onclick='toggleSettings()'>Settings</button>
    <br><a href='/trading'><button class='green-button' style='margin-top:10px'>Trading Dashboard</button></a>
    <div id="settingsPanel">
      <h3>API Key</h3>
      <form action='/setkey?saved=1'>
        <input type='text' name='key' value=')rawliteral" + apiKey + R"rawliteral(' class='text-input'>
        <button type='submit' class='blue-button'>Save</button>
      </form>

      <h3>Display Time per Stock</h3>
      <form action='/settime'>
        <input type='number' name='time' min='0' value=')rawliteral" + String(displayInterval/1000) + R"rawliteral(' class='text-input'>
        <button type='submit' class='blue-button'>Save</button>
      </form>

      <h3>LED Brightness</h3>
      <label>Green: <span id='gv'>)rawliteral" + String(ledGreenIntensity) + R"rawliteral(</span>%</label>
      <form action='/setledgreen' style='margin-top:4px'>
        <input type='range' name='val' min='0' max='100' value=')rawliteral" + String(ledGreenIntensity) + R"rawliteral('
          oninput="document.getElementById('gv').textContent=this.value"
          style='flex:1;border:none;padding:6px'>
        <button type='submit' class='blue-button'>Save</button>
      </form>
      <label>Red: <span id='rv'>)rawliteral" + String(ledRedIntensity) + R"rawliteral(</span>%</label>
      <form action='/setledred' style='margin-top:4px'>
        <input type='range' name='val' min='0' max='100' value=')rawliteral" + String(ledRedIntensity) + R"rawliteral('
          oninput="document.getElementById('rv').textContent=this.value"
          style='flex:1;border:none;padding:6px'>
        <button type='submit' class='blue-button'>Save</button>
      </form>

      <h3>Trading Mode</h3>
    )rawliteral";
  html += "<a href='/toggletrading' style='text-decoration:none'>";
  html += "<button class='";
  html += tradingEnabled ? "green-button" : "red-button";
  html += "'>Trading: ";
  html += tradingEnabled ? "ON" : "OFF";
  html += "</button></a>";
  html += R"rawliteral(
    </div>
  )rawliteral";

  html += R"rawliteral(
  </div>

  <!-- Custom Confirm Modal -->
  <div id="confirmModal">
    <div class="modal-content">
      <h3>Are you sure?</h3>
      <p>This will delete the entire list.</p>
      <button onclick="proceedClear()" class="red-button">Yes, erase</button>
      <button onclick="hideCustomConfirm()" class="green-button">No, go back</button>
    </div>
  </div>

</body>

<div id="savedPopup" class="greenPopup">Changes Saved</div>

<div id="errorPopup" class="redPopup">Invalid Data</div>

<style>
  .greenPopup {
    display: none;
    position: fixed;
    top: 50%;
    left: 50%;
    transform: translate(-50%, -50%);
    background: #4CAF50;
    color: white;
    padding: 30px 50px;
    border-radius: 12px;
    box-shadow: 0 4px 12px rgba(0,0,0,0.4);
    font-size: 20px;
    z-index: 9999;
    text-align: center;
  }

  .redPopup {
    display: none;
    position: fixed;
    top: 50%;
    left: 50%;
    transform: translate(-50%, -50%);
    background: #f44336;
    color: white;
    padding: 30px 50px;
    border-radius: 12px;
    box-shadow: 0 4px 12px rgba(0,0,0,0.4);
    font-size: 20px;
    z-index: 9999;
    text-align: center;
  }

</style>

</html>
)rawliteral";

  server.send(200, "text/html", html);
}

void handleSetKey() {
  if (server.hasArg("key")) {
    apiKey = server.arg("key");
    saveApiKey(apiKey);
  }
  server.sendHeader("Location", "/?saved=1");
  server.send(303);
}

void saveApiKey(const String &key) {
  int maxLen = 100;
  int len = key.length();
  if (len > maxLen) len = maxLen;

  for (int i = 0; i < len; i++) {
    EEPROM.write(i, key[i]);
  }
  EEPROM.write(len, '\0');
  EEPROM.commit();
}

void loadApiKey() {
  char buf[101];
  for (int i = 0; i < 100; i++) {
    buf[i] = EEPROM.read(i);
    if (buf[i] == '\0') break;
  }
  buf[100] = '\0';
  apiKey = String(buf);
  Serial.println("Loaded API key: " + apiKey);
}

void handleSetTime() {
  if (server.hasArg("time")) {
    int newTime = server.arg("time").toInt();

    int stockCount = stockNum;
    if (stockCount == 0) stockCount = 1; // avoid divide-by-zero

    // Constraint: newTime >= 0 and newTime * stockCount <= 60000
    if (newTime >= 1 && (newTime * stockCount * 1000) <= 600000) {
      displayInterval = newTime*1000;
      saveDisplayInterval(displayInterval);

        server.sendHeader("Location", "/?saved=1");  // redirect
        server.send(303);
    } else {
      //server.send(200, "text/html", "Invalid value. Max allowed: " + String(600 / stockCount) + "s <a href='/'>Back</a>");
      server.sendHeader("Location", "/?saved=2");  // redirect
      server.send(303);
    }
  } else {
    //server.send(200, "text/html", "Missing parameter <a href='/'>Back</a>");
    server.sendHeader("Location", "/?saved=2");  // redirect
    server.send(303);
  }
}

void saveDisplayInterval(unsigned long interval) {
  EEPROM.put(EEPROM_DISPLAY_ADDR, interval);
  EEPROM.commit();
  Serial.println("✅ Saved displayInterval to EEPROM: " + String(interval));
}

void saveLedSettings() {
  EEPROM.write(EEPROM_LED_GREEN_ADDR,  (uint8_t)ledGreenIntensity);
  EEPROM.write(EEPROM_LED_RED_ADDR,    (uint8_t)ledRedIntensity);
  EEPROM.write(EEPROM_TRADING_EN_ADDR, tradingEnabled ? 1 : 0);
  EEPROM.commit();
}

void loadLedSettings() {
  uint8_t g = EEPROM.read(EEPROM_LED_GREEN_ADDR);
  uint8_t r = EEPROM.read(EEPROM_LED_RED_ADDR);
  uint8_t t = EEPROM.read(EEPROM_TRADING_EN_ADDR);
  ledGreenIntensity = (g <= 100) ? g : 100;
  ledRedIntensity   = (r <= 100) ? r : 100;
  tradingEnabled    = (t == 0 || t == 1) ? (t == 1) : true;
}

void loadDisplayInterval() {
  unsigned long stored;
  EEPROM.get(EEPROM_DISPLAY_ADDR, stored);

  if (stored >= 1000 && stored <= 600000) {
    displayInterval = stored;
    Serial.println("✅ Loaded displayInterval from EEPROM: " + String(displayInterval));
  } else {
    Serial.println("⚠️ No valid displayInterval in EEPROM, using default 5000ms");
    EEPROM.put(EEPROM_DISPLAY_ADDR, 5000);
    EEPROM.commit();
    displayInterval = 5000;
  }
}

void handleAdd() {
  String value = server.arg("value");
  if (value.length() > 0) {
    addNode(value);
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  } else {
    server.send(400, "text/plain", "Missing 'value'");
  }
}

void handleRemove() {
  String value = server.arg("value");
  if (value.length() > 0) {
    bool removed = removeNode(value);
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", removed ? "Removed" : "Not found");
  } else {
    server.send(400, "text/plain", "Missing 'value'");
  }
}

void handleSetLedGreen() {
  if (server.hasArg("val")) {
    int v = server.arg("val").toInt();
    if (v >= 0 && v <= 100) { ledGreenIntensity = v; saveLedSettings(); }
  }
  server.sendHeader("Location", "/?saved=1");
  server.send(303);
}

void handleSetLedRed() {
  if (server.hasArg("val")) {
    int v = server.arg("val").toInt();
    if (v >= 0 && v <= 100) { ledRedIntensity = v; saveLedSettings(); }
  }
  server.sendHeader("Location", "/?saved=1");
  server.send(303);
}

void handleToggleTrading() {
  tradingEnabled = !tradingEnabled;
  saveLedSettings();
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleClear() {
  clearList();
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

// === Trading Functions ===

void addPriceToHistory(TradingPosition& pos, float price) {
    pos.priceHistory[pos.historyHead] = price;
    pos.historyHead = (pos.historyHead + 1) % PRICE_HISTORY_LEN;
    if (pos.historyCount < PRICE_HISTORY_LEN) pos.historyCount++;
}

float calculateSMA(TradingPosition& pos, int period) {
    if (pos.historyCount < period) return 0;
    float sum = 0;
    int start = (pos.historyHead - 1 + PRICE_HISTORY_LEN) % PRICE_HISTORY_LEN;
    for (int i = 0; i < period; i++)
        sum += pos.priceHistory[(start - i + PRICE_HISTORY_LEN) % PRICE_HISTORY_LEN];
    return sum / period;
}

float getPortfolioValue() {
    float total = tradingCash;
    for (auto& kv : tradingPositions)
        if (kv.second.shares > 0 && stockCache.count(kv.first))
            total += kv.second.shares * stockCache[kv.first].currentPrice;
    return total;
}

void logTrade(const String& sym, const String& action, float price, float shares, float pnl) {
    tradeLog[tradeLogHead] = {sym, action, price, shares, pnl};
    tradeLogHead = (tradeLogHead + 1) % 5;
    if (tradeLogCount < 5) tradeLogCount++;
}

void executeBuy(const String& symbol, float price) {
    float spend = min(tradingCash, getPortfolioValue() * MAX_POSITION_PCT);
    if (spend < 1.0f) return;
    float shares = spend / price;
    TradingPosition& pos = tradingPositions[symbol];
    pos.avgBuyPrice = (pos.shares * pos.avgBuyPrice + shares * price) / (pos.shares + shares);
    pos.shares += shares;
    tradingCash -= spend;
    logTrade(symbol, "BUY", price, shares, 0);
    Serial.printf("[TRADE] BUY  %s: %.4f @ $%.2f\n", symbol.c_str(), shares, price);
}

void executeSell(const String& symbol, float price) {
    TradingPosition& pos = tradingPositions[symbol];
    if (pos.shares <= 0) return;
    float pnl = (price - pos.avgBuyPrice) * pos.shares;
    totalRealizedPnL += pnl;
    tradingCash += pos.shares * price;
    logTrade(symbol, "SELL", price, pos.shares, pnl);
    Serial.printf("[TRADE] SELL %s @ $%.2f  PnL $%.2f\n", symbol.c_str(), price, pnl);
    pos.shares = 0;
    pos.avgBuyPrice = 0;
}

void runTradingAlgorithm() {
    if (!tradingEnabled || !marketIsOpen()) return;
    Node* node = head;
    while (node) {
        String symbol = node->data;
        symbol.replace("/", "");
        if (!stockCache.count(symbol)) { node = node->next; continue; }
        float price = stockCache[symbol].currentPrice;
        if (price <= 0) { node = node->next; continue; }

        TradingPosition& pos = tradingPositions[symbol];
        addPriceToHistory(pos, price);
        if (pos.historyCount < SMA_LONG_PERIOD) { node = node->next; continue; }

        bool bullish = calculateSMA(pos, SMA_SHORT_PERIOD) > calculateSMA(pos, SMA_LONG_PERIOD);
        if (pos.prevSet) {
            if ( bullish && !pos.prevBullish && tradingCash > 1.0f) executeBuy(symbol,  price);
            if (!bullish &&  pos.prevBullish && pos.shares  > 0   ) executeSell(symbol, price);
        }
        pos.prevBullish = bullish;
        pos.prevSet = true;
        node = node->next;
    }
}

void saveTradingState() {
    File file = LittleFS.open("/trading.json", "w");
    if (!file) return;
    DynamicJsonDocument doc(2048);
    doc["cash"] = tradingCash;
    doc["pnl"]  = totalRealizedPnL;
    JsonArray arr = doc.createNestedArray("positions");
    for (auto& kv : tradingPositions) {
        if (kv.second.shares > 0) {
            JsonObject p = arr.createNestedObject();
            p["sym"]    = kv.first;
            p["shares"] = kv.second.shares;
            p["buy"]    = kv.second.avgBuyPrice;
        }
    }
    serializeJson(doc, file);
    file.close();
}

void loadTradingState() {
    if (!LittleFS.exists("/trading.json")) { tradingCash = STARTING_CASH; return; }
    File file = LittleFS.open("/trading.json", "r");
    if (!file) return;
    DynamicJsonDocument doc(2048);
    if (!deserializeJson(doc, file)) {
        tradingCash      = doc["cash"] | STARTING_CASH;
        totalRealizedPnL = doc["pnl"]  | 0.0f;
        for (JsonObject p : doc["positions"].as<JsonArray>()) {
            String sym = p["sym"].as<String>();
            tradingPositions[sym].shares      = p["shares"];
            tradingPositions[sym].avgBuyPrice = p["buy"];
        }
        Serial.printf("[Trading] cash=%.2f  pnl=%.2f\n", tradingCash, totalRealizedPnL);
    }
    file.close();
}

void handleTrading() {
    float pv  = getPortfolioValue();
    float pnl = pv - STARTING_CASH;
    String h = "<!DOCTYPE html><html><head><title>Trading</title><style>"
               "body{font-family:Arial;background:#f2f2f2;padding:20px}"
               ".c{background:#fff;padding:24px;border-radius:12px;max-width:520px;margin:auto}"
               "table{width:100%;border-collapse:collapse}td,th{padding:6px;border:1px solid #ddd}"
               "th{background:#f5f5f5}.g{color:green}.r{color:red}</style></head>"
               "<body><div class='c'><h2>Paper Trading</h2>";
    h += "<p><b>Cash:</b> $" + String(tradingCash, 2) + " &nbsp; ";
    h += "<b>Portfolio:</b> $" + String(pv, 2) + " &nbsp; ";
    h += "<b>PnL:</b> <span class='" + String(pnl >= 0 ? "g" : "r") + "'>$" + String(pnl, 2) + "</span></p>";
    h += "<h3>Positions</h3><table><tr><th>Symbol</th><th>Shares</th>"
         "<th>Avg Buy</th><th>Current</th><th>PnL</th></tr>";
    bool any = false;
    for (auto& kv : tradingPositions) {
        if (kv.second.shares <= 0) continue;
        any = true;
        float cur  = stockCache.count(kv.first) ? stockCache[kv.first].currentPrice : 0;
        float pp   = (cur - kv.second.avgBuyPrice) * kv.second.shares;
        h += "<tr><td>" + kv.first + "</td><td>" + String(kv.second.shares, 4) +
             "</td><td>$" + String(kv.second.avgBuyPrice, 2) +
             "</td><td>$" + String(cur, 2) +
             "</td><td class='" + String(pp >= 0 ? "g" : "r") + "'>$" + String(pp, 2) + "</td></tr>";
    }
    if (!any) h += "<tr><td colspan='5'>No open positions yet</td></tr>";
    h += "</table><h3>Recent Trades</h3><table><tr><th>Action</th><th>Symbol</th>"
         "<th>Price</th><th>Shares</th><th>PnL</th></tr>";
    for (int i = 0; i < tradeLogCount; i++) {
        int idx = (tradeLogHead - 1 - i + 5) % 5;
        TradeRecord& t = tradeLog[idx];
        h += "<tr><td>" + t.action + "</td><td>" + t.symbol +
             "</td><td>$" + String(t.price, 2) + "</td><td>" + String(t.shares, 4) +
             "</td><td>" + (t.action == "SELL"
                 ? "<span class='" + String(t.pnl >= 0 ? "g" : "r") + "'>$" + String(t.pnl, 2) + "</span>"
                 : "-") + "</td></tr>";
    }
    if (!tradeLogCount)
        h += "<tr><td colspan='5'>No trades yet (needs " + String(SMA_LONG_PERIOD) + " price samples)</td></tr>";
    h += "</table><p><small>SMA(" + String(SMA_SHORT_PERIOD) + ") / SMA(" +
         String(SMA_LONG_PERIOD) + ") crossover &bull; 10 min fetch interval</small></p>"
         "<p><a href='/'>Back</a></p></div></body></html>";
    server.send(200, "text/html", h);
}

void handleNonBlockingFetch() {
  if (!isFetching) return;
  if (!currentNode) {
    Serial.println("✅ Completato ciclo fetch.");
    isFetching = false;
    return;
  }

  if (millis() - lastQuoteRequestTime < quoteDelay) return;

  String symbol = currentNode->data;
  symbol.replace("/", "");
  String url = "https://finnhub.io/api/v1/quote?symbol=" + symbol + "&token=" + apiKey;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  Serial.println("🔎 Requesting: " + url);

  if (https.begin(client, url)) {
    https.addHeader("User-Agent", "Mozilla/5.0");
    int httpCode = https.GET();

    if (httpCode == HTTP_CODE_OK) {
      String payload = https.getString();
      DynamicJsonDocument doc(1024);
      if (deserializeJson(doc, payload)) {
        Serial.println("⚠️ JSON parsing error");
      } else {
        float currentPrice = doc["c"];
        float prevClose = doc["pc"];
        float percent = 0;
        if (prevClose != 0) {
          percent = ((currentPrice - prevClose) / prevClose) * 100.0;
        }

        stockCache[symbol] = { currentPrice, percent };
        Serial.printf("📥 %s: %.2f (%.2f%%)\n", symbol.c_str(), currentPrice, percent);

      }
    } else {
      Serial.printf("⚠️ HTTP error: %d\n", httpCode);
    }
    https.end();
  } else {
    Serial.println("[HTTPS] Connection failed");
  }

  lastQuoteRequestTime = millis();
  currentNode = currentNode->next;
}

void fetchSingleStock(String symbol) {
    symbol.replace("/", "");
    String url = "https://finnhub.io/api/v1/quote?symbol=" + symbol + "&token=" + apiKey;

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient https;
    Serial.println("🔎 Requesting: " + url);

    if (https.begin(client, url)) {
        https.addHeader("User-Agent", "Mozilla/5.0");
        int httpCode = https.GET();

        if (httpCode == HTTP_CODE_OK) {
            String payload = https.getString();
            DynamicJsonDocument doc(1024);
            if (deserializeJson(doc, payload)) {
                Serial.println("⚠️ JSON parsing error");
            } else {
                float currentPrice = doc["c"];
                float prevClose = doc["pc"];
                float percent = 0;
                if (prevClose != 0) {
                    percent = ((currentPrice - prevClose) / prevClose) * 100.0;
                }

                stockCache[symbol] = { currentPrice, percent };
                Serial.printf("📥 %s: %.2f (%.2f%%)\n", symbol.c_str(), currentPrice, percent);
            }
        } else {
            Serial.printf("⚠️ HTTP error: %d\n", httpCode);
        }
        https.end();
    } else {
        Serial.println("[HTTPS] Connection failed");
    }
}

StockData fetchSingleStockWithRetry(String symbol) {
    StockData data = stockCache[symbol];

    for (int retry = 0; retry < 2; retry++) {
        if (data.currentPrice != 0) break;
        fetchSingleStock(symbol);
        data = stockCache[symbol];
        delay(150);
    }

    return data;
}

void checkButton() {
  static bool buttonPreviouslyPressed = false;

  bool currentReading = digitalRead(buttonPin);

  if (currentReading && !buttonPreviouslyPressed) {
    serverMode = !serverMode;
    isFetching = false;
    Serial.print("🔁 Modalità cambiata: ");
    Serial.println(serverMode ? "SERVER" : "DISPLAY");

    setLedGreen(false);
    setLedRed(false);
  }

  buttonPreviouslyPressed = currentReading;
}

bool marketIsOpen() {
    int currentMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    int openMinutes = MARKET_OPEN_HOUR * 60 + MARKET_OPEN_MINUTE;
    int closeMinutes = MARKET_CLOSE_HOUR * 60 + MARKET_CLOSE_MINUTE;

    return currentMinutes >= openMinutes && currentMinutes < closeMinutes;
}

String getMarketCountdown() {
    int currentMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    int openMinutes    = MARKET_OPEN_HOUR * 60 + MARKET_OPEN_MINUTE;
    int closeMinutes   = MARKET_CLOSE_HOUR * 60 + MARKET_CLOSE_MINUTE;

    int targetMinutes;

    if (currentMinutes < openMinutes) {
        // before open → countdown to open
        targetMinutes = openMinutes;
    } else if (currentMinutes < closeMinutes) {
        // after open, before close → countdown to close
        targetMinutes = closeMinutes;
    } else {
        // after close → countdown to tomorrow’s open
        targetMinutes = openMinutes + 24 * 60;
    }

    int diff = targetMinutes - currentMinutes;
    int hoursLeft = diff / 60;
    int minutesLeft = diff % 60;

    return String(hoursLeft) + "h " + String(minutesLeft) + "m";
}

// Blit 128x64 window at horizontal offset into SSD1306 buffer
void blitWindowToDisplay(int xOffset) {
  uint8_t* dispBuf = display.getBuffer();
  memset(dispBuf, 0, 1024);

  for (int y = 0; y < 64; y++) {
    for (int x = 0; x < 128; x++) {
      int srcX = x + xOffset;
      if (srcX < 0 || srcX >= 256) continue; // virtual buffer is 256 wide

      // --- read from virtual buffer (linear 256x64 1bpp) ---
      int srcIndex = y * 256 + srcX;
      int srcByte = srcIndex / 8;
      int srcBit = 7 - (srcIndex % 8);
      bool pixel = (virtualBuffer[srcByte] >> srcBit) & 0x01;

      if (pixel) {
        // --- write into Adafruit buffer (paged 128x64) ---
        int dstIndex = x + (y / 8) * 128;
        dispBuf[dstIndex] |= (1 << (y & 7));
      }
    }
  }
}

// Helper: clear virtual buffer
void clearVirtualBuffer() {
  memset(virtualBuffer, 0, sizeof(virtualBuffer));
}

// Wrapper: draw into virtual buffer at xOffset
void drawStockFrameToVirtual(String symbol, StockData data, int xOffset) {
  display.clearDisplay();
  drawStockFrame(symbol, data, 0);   // draw into SSD1306 buffer
  uint8_t* src = display.getBuffer();

  for (int y = 0; y < 64; y++) {
    for (int x = 0; x < 128; x++) {
      int dstX = x + xOffset;
      if (dstX >= 128*2) continue; // virtual buffer is 256px wide

      // --- read from Adafruit buffer ---
      int srcIndex = x + (y / 8) * 128;
      bool pixel = (src[srcIndex] >> (y & 7)) & 0x1;

      if (pixel) {
        // --- write into virtual buffer (linear 1bpp) ---
        int dstIndex = y * 256 + dstX;  // now width is 256
        int dstByte = dstIndex / 8;
        int dstBit = 7 - (dstIndex % 8);   // MSB first
        virtualBuffer[dstByte] |= (1 << dstBit);
      }
    }
  }
}

// Horizontal slide animation using virtual 256x64 framebuffer
void slideFramesHorizontal(
    const String& oldSymbol, const StockData& oldData,
    const String& newSymbol, const StockData& newData,
    int step = 4, int delayTime = 20) 
{
  clearVirtualBuffer();

  // Draw old frame in left half
  drawStockFrameToVirtual(oldSymbol, oldData, 0);

  // Draw new frame in right half
  drawStockFrameToVirtual(newSymbol, newData, 128);

  // Animate scrolling from x=0..128
  for (int offset = 0; offset <= 128; offset += step) {
    blitWindowToDisplay(offset);
    display.display();
    delay(delayTime);
  }
}

void drawCountdownFrame(const String& statusText, const String& countdown) {
  display.clearDisplay();
  
  // First line: OPENS / CLOSES
  display.setFont(&FreeSansBold9pt7b);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(statusText, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 20);
  display.print(statusText);

  // Second line: countdown
  display.getTextBounds(countdown, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 45);
  display.print(countdown);
}

void slideStockToCountdown(
    const String& symbol, const StockData& data,
    const String& statusText, const String& countdown,
    int step = 4, int delayTime = 20) 
{
  clearVirtualBuffer();

  // Stock a sinistra
  drawStockFrameToVirtual(symbol, data, 0);

  // Countdown a destra
  drawCountdownFrameVirtual(statusText, countdown, 128);

  // Scroll
  for (int offset = 0; offset <= 128; offset += step) {
    blitWindowToDisplay(offset);
    display.display();
    delay(delayTime);
  }
}

void slideCountdownToStock(
    const String& statusText, const String& countdown,
    const String& symbol, const StockData& data,
    int step = 4, int delayTime = 20) 
{
  clearVirtualBuffer();

  // Countdown a sinistra
  drawCountdownFrameVirtual(statusText, countdown, 0);

  // Stock a destra
  drawStockFrameToVirtual(symbol, data, 128);

  // Scroll
  for (int offset = 0; offset <= 128; offset += step) {
    blitWindowToDisplay(offset);
    display.display();
    delay(delayTime);
  }
}

void drawCountdownFrameVirtual(const String& statusText, const String& countdown, int xOffset) {
  display.clearDisplay();

  display.setFont(&FreeSansBold9pt7b);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  int16_t x1, y1;
  uint16_t w, h;

  // Prima riga
  display.getTextBounds(statusText, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 20);
  display.print(statusText);

  // Seconda riga
  display.getTextBounds(countdown, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 45);
  display.print(countdown);

  // Copia nel virtual buffer
  uint8_t* src = display.getBuffer();
  for (int y = 0; y < 64; y++) {
    for (int x = 0; x < 128; x++) {
      int dstX = x + xOffset;
      if (dstX >= 256) continue;

      int srcIndex = x + (y / 8) * 128;
      bool pixel = (src[srcIndex] >> (y & 7)) & 0x1;

      if (pixel) {
        int dstIndex = y * 256 + dstX;
        int dstByte = dstIndex / 8;
        int dstBit = 7 - (dstIndex % 8);
        virtualBuffer[dstByte] |= (1 << dstBit);
      }
    }
  }
}

// ----------------- Helpers -----------------
String sanitizeSymbol(const String& raw) {
    String sym = raw;
    sym.replace("/", "");
    return sym;
}

StockData fetchStockData(const String& symbol) {
    StockData data;
    if (stockCache.count(symbol)) data = stockCache[symbol];
    if (data.currentPrice == 0) {
        data = fetchSingleStockWithRetry(symbol);
        stockCache[symbol] = data;
    }
    return data;
}

void blinkMarketLEDsNonBlocking() {
    static unsigned long lastBlink = 0;
    static bool state = false;

    if (millis() - lastBlink > 2500) { // blink interval
        lastBlink = millis();
        state = !state;

        if (marketIsOpen()) {
            setLedGreen(state);
            setLedRed(false);
        } else {
            setLedGreen(false);
            setLedRed(state);
        }
    }
}

void drawTradingStatusFrame() {
    float pv  = getPortfolioValue();
    float pnlPct = (pv - STARTING_CASH) / STARTING_CASH * 100.0f;

    int16_t x1, y1;
    uint16_t w, h;

    display.setFont(&FreeSans9pt7b);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    String label = "PORTFOLIO";
    display.getTextBounds(label, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 13);
    display.print(label);

    display.setFont(&FreeSansBold12pt7b);
    String valStr = "$" + String(pv, 2);
    display.getTextBounds(valStr, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 34);
    display.print(valStr);

    display.setFont(&FreeSans9pt7b);
    String lastStr = "NO TRADES YET";
    if (tradeLogCount > 0) {
        int idx = (tradeLogHead - 1 + 5) % 5;
        lastStr = tradeLog[idx].action + " " + tradeLog[idx].symbol;
    }
    display.getTextBounds(lastStr, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 50);
    display.print(lastStr);

    display.setFont(&FreeSansBold9pt7b);
    String pnlStr = (pnlPct >= 0 ? "+" : "") + String(pnlPct, 2) + "%";
    display.getTextBounds(pnlStr, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 63);
    display.print(pnlStr);
}

void drawTradingFrameToVirtual(int xOffset) {
    display.clearDisplay();
    drawTradingStatusFrame();
    uint8_t* src = display.getBuffer();
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 128; x++) {
            int dstX = x + xOffset;
            if (dstX >= 256) continue;
            int srcIndex = x + (y / 8) * 128;
            bool pixel = (src[srcIndex] >> (y & 7)) & 0x1;
            if (pixel) {
                int dstIndex = y * 256 + dstX;
                int dstByte = dstIndex / 8;
                int dstBit = 7 - (dstIndex % 8);
                virtualBuffer[dstByte] |= (1 << dstBit);
            }
        }
    }
}

// ----------------- Main Display Function -----------------
void updateDisplay() {
    getLocalTime(&timeinfo);

    if (!displayNode) displayNode = head;
    if (!displayNode) return;

    String statusText = marketIsOpen() ? "CLOSES" : "OPENS";
    String countdown = getMarketCountdown();

    String newSymbol = sanitizeSymbol(displayNode->data);
    StockData newData = fetchStockData(newSymbol);

    switch (displayState) {
        case SHOW_STOCK: {
            if (firstRun) {
                // First stock at startup
                drawStockFrame(newSymbol, newData, 0);
                display.display();
                updateLEDs(newData);
                firstRun = false;
                stateStartTime = millis();
                oldSymbol = newSymbol;
                oldData = newData;
                return;
            }

            if (millis() - stateStartTime > displayInterval) {
                // Advance stock
                displayNode = displayNode->next;
                if (!displayNode) {
                    displayNode = head;
                    displayState = TRANSITION_TO_COUNTDOWN;
                } else {
                    // Slide to next stock
                    String nextSymbol = sanitizeSymbol(displayNode->data);
                    StockData nextData = fetchStockData(nextSymbol);

                    updateLEDs(nextData);
                    slideFramesHorizontal(oldSymbol, oldData, nextSymbol, nextData);

                    oldSymbol = nextSymbol;
                    oldData = nextData;
                    stateStartTime = millis();
                }
            }
            break;
        }

        case TRANSITION_TO_COUNTDOWN: {
            slideStockToCountdown(oldSymbol, oldData, statusText, countdown);
            displayState = SHOW_COUNTDOWN;
            stateStartTime = millis();
            break;
        }

        case SHOW_COUNTDOWN: {
            drawCountdownFrame(statusText, countdown);
            display.display();
            blinkMarketLEDsNonBlocking();

            if (millis() - stateStartTime > displayInterval) {
                displayState = tradingEnabled ? TRANSITION_TO_TRADING : TRANSITION_TO_STOCK;
            }
            break;
        }

        case TRANSITION_TO_TRADING: {
            clearVirtualBuffer();
            drawCountdownFrameVirtual(statusText, countdown, 0);
            drawTradingFrameToVirtual(128);
            for (int offset = 0; offset <= 128; offset += 4) {
                blitWindowToDisplay(offset);
                display.display();
                delay(20);
            }
            displayState = SHOW_TRADING;
            stateStartTime = millis();
            break;
        }

        case SHOW_TRADING: {
            display.clearDisplay();
            drawTradingStatusFrame();
            display.display();
            if (millis() - stateStartTime > displayInterval) {
                displayState = TRANSITION_TO_STOCK;
            }
            break;
        }

        case TRANSITION_TO_STOCK: {
            String nextSymbol = sanitizeSymbol(displayNode->data);
            StockData nextData = fetchStockData(nextSymbol);

            if (tradingEnabled) {
                clearVirtualBuffer();
                drawTradingFrameToVirtual(0);
                drawStockFrameToVirtual(nextSymbol, nextData, 128);
                for (int offset = 0; offset <= 128; offset += 4) {
                    blitWindowToDisplay(offset);
                    display.display();
                    delay(20);
                }
            } else {
                slideCountdownToStock(statusText, countdown, nextSymbol, nextData);
            }
            updateLEDs(nextData);

            oldSymbol = nextSymbol;
            oldData = nextData;

            displayState = SHOW_STOCK;
            stateStartTime = millis();
            break;
        }
    }
}

void updateLEDs(StockData data) {
    if (data.percent > 0) {
        setLedGreen(true);
        setLedRed(false);
    } else if (data.percent < 0) {
        setLedGreen(false);
        setLedRed(true);
    } else {
        setLedGreen(false);
        setLedRed(false);
    }
}

void drawStockFrame(String symbol, StockData data, int xOffset) {
    int16_t x1, y1;
    uint16_t w, h;

    // Don’t draw if frame is completely off-screen
    if (xOffset <= -SCREEN_WIDTH || xOffset >= SCREEN_WIDTH) return;

    // --- SYMBOL ---
    display.setFont(&FreeSansBold9pt7b);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    display.getTextBounds(symbol, 0, 0, &x1, &y1, &w, &h);
    int symbolX = (SCREEN_WIDTH - w) / 2 + xOffset;
    int symbolY = h + 4;
    display.setCursor(symbolX, symbolY);
    display.print(symbol);

    // --- PRICE ---
    display.setFont(&FreeSansBold12pt7b);
    String priceStr = "$ " + String(data.currentPrice, 2);
    display.getTextBounds(priceStr, 0, 0, &x1, &y1, &w, &h);
    int priceX = (SCREEN_WIDTH - w) / 2 + xOffset;
    int priceY = 40;  
    display.setCursor(priceX, priceY);
    display.print(priceStr);

    // --- PERCENT ---
    display.setFont(&FreeSansBold9pt7b);
    String changeStr = String(data.percent, 2) + " %";
    display.getTextBounds(changeStr, 0, 0, &x1, &y1, &w, &h);
    int changeX = (SCREEN_WIDTH - w) / 2 + xOffset;
    int changeY = 60;
    display.setCursor(changeX, changeY);
    display.print(changeStr);
}

void showServerMode() {
  display.clearDisplay();
  
  display.drawBitmap((SCREEN_WIDTH - 24) / 2, 4, serverIcon, 24, 24, SSD1306_WHITE);

  display.setFont(&FreeSansBold9pt7b);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  int16_t x1, y1;
  uint16_t w, h;
  String text = "SERVER";
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);

  display.setCursor((SCREEN_WIDTH - w) / 2, 48);
  display.print(text);

  display.setFont(&Org_01);  
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.getTextBounds(ServerAdr, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 58);
  display.print(ServerAdr);

  display.display();
}

void showWallStreet() {
  display.clearDisplay();
  
  display.drawBitmap((SCREEN_WIDTH - 24) / 2, 0, dollarIcon, 24, 24, SSD1306_WHITE);

  display.setFont(&FreeSansBold9pt7b);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  int16_t x1, y1;
  uint16_t w, h;

  display.getTextBounds("WALL", 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 40);
  display.print("WALL");

  display.getTextBounds("STREET", 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 56);
  display.print("STREET");

  display.display();
}

void blinkLedsInServerMode() {
  static unsigned long lastBlinkTime = 0;
  static bool ledState = false;

  const unsigned long blinkInterval = 800;

  if (millis() - lastBlinkTime >= blinkInterval) {
    ledState = !ledState;
    setLedGreen(ledState);
    setLedRed(!ledState);
    lastBlinkTime = millis();
  }
}

void progressBar(int percentage)
{
  display.clearDisplay();

  display.drawRect(7, 24, 114, 17, WHITE);
  display.drawRect(6, 23, 116, 19, WHITE);

  int barWidth = (percentage * 110) / 100;
  display.fillRect(9, 26, barWidth, 13, WHITE);

  display.setFont(&FreeSans9pt7b);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(20, 15);
  display.print("STARTING");

  display.setFont(&FreeSansBold12pt7b);
  display.setTextColor(WHITE);
  display.setCursor(40, 60);
  display.print(percentage);
  display.print("%");

  display.display();

  setLedGreen(true);    
  delay(2000);
  setLedGreen(false);
}

void showFirstStart()
{
  display.clearDisplay();
  display.setFont(&FreeSans9pt7b);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  int16_t x1, y1;
  uint16_t w, h;

  String text1 = "NO DATA";
  display.getTextBounds(text1, 0, 0, &x1, &y1, &w, &h);
  int16_t xCentered1 = (display.width() - w) / 2;
  int16_t yPos1 = 25;
  display.setCursor(xCentered1, yPos1);
  display.print(text1);

  String text2 = "SWITCHING";
  display.getTextBounds(text2, 0, 0, &x1, &y1, &w, &h);
  int16_t xCentered2 = (display.width() - w) / 2;
  int16_t yPos2 = 55;
  display.setCursor(xCentered2, yPos2);
  display.print(text2);

  display.display();
}

void showInsertAPI()
{
  display.clearDisplay();
  display.setFont(&FreeSans9pt7b);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  int16_t x1, y1;
  uint16_t w, h;

  String text1 = "INSERT";
  display.getTextBounds(text1, 0, 0, &x1, &y1, &w, &h);
  int16_t xCentered1 = (display.width() - w) / 2;
  int16_t yPos1 = 25;
  display.setCursor(xCentered1, yPos1);
  display.print(text1);

  String text2 = "API KEY";
  display.getTextBounds(text2, 0, 0, &x1, &y1, &w, &h);
  int16_t xCentered2 = (display.width() - w) / 2;
  int16_t yPos2 = 55;
  display.setCursor(xCentered2, yPos2);
  display.print(text2);

  display.display();
}

void setup() {
  Serial.begin(115200);

  //10%
  pinMode(buttonPin, INPUT);  
  pinMode(wifiResetButtonPin, INPUT);
  pinMode(ledGreenPin, OUTPUT);
  pinMode(ledRedPin, OUTPUT);
  setLedGreen(false);
  setLedRed(false);
  //progressBar(10);

  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed");
    return;
  }

  //20%
  //progressBar(20);
  Wire.begin(D2, D1); // SDA, SCL
 
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { // Address 0x3C for most OLEDs
    Serial.println(F("SSD1306 allocation failed"));
    for (;;); // Don't proceed, loop forever
  }
  
  //30%
  progressBar(30);
  // --- Show WiFi icon at startup ---
  display.clearDisplay();
  display.drawBitmap(32, 0, wifiDraw, 64, 64, SSD1306_WHITE);
  display.display();

  // --- WiFi Manager ---
  WiFiManager wifiManager;
  if (!wifiManager.autoConnect("WallStreetDisplay", "12345678")) {
    Serial.println("Wi-Fi fallita. Riavvio...");
    delay(3000);
    ESP.restart();
  } 

  //40%
  progressBar(40);
  if (WiFi.getMode() == WIFI_AP) {
    Serial.println("📶 AP mode active: bitmap stays on OLED");
  } else {
    display.clearDisplay();
    Serial.print("Connesso. IP: ");
    ServerAdr = WiFi.localIP().toString();
    Serial.println(ServerAdr);    
  }

  //50%
  progressBar(50);
  loadListFromFS();

  // --- Web server ---
  server.on("/", handleRoot);
  server.on("/add", handleAdd);
  server.on("/remove", handleRemove);
  server.on("/clear", handleClear);
  server.on("/setkey", handleSetKey);
  server.on("/settime", handleSetTime);
  server.on("/trading", handleTrading);
  server.on("/setledgreen", handleSetLedGreen);
  server.on("/setledred", handleSetLedRed);
  server.on("/toggletrading", handleToggleTrading);

  //60%
  progressBar(60);
  server.begin();

  //70%
  progressBar(70);
  // --- Initialize time ---
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  while(!getLocalTime(&timeinfo)){
    Serial.println("Waiting for NTP time...");
    delay(1000);
  }

  //)90%
  progressBar(90);
  Serial.println("Time synchronized!");
  if (getLocalTime(&timeinfo)) {
    Serial.printf("Local time: %02d:%02d:%02d\n", 
                  timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  }

  EEPROM.begin(EEPROM_SIZE);
  loadApiKey();
  loadDisplayInterval();
  loadLedSettings();
  loadTradingState();

  //100%
  progressBar(100);
  Serial.println("🟢 Avviato in modalità DISPLAY");
  newStock = true;
}

// === Loop ===
unsigned long lastRequest = 0;

void loop() {
  // Handle WiFi reset button (hold >5s to clear credentials)
  static bool wifiResetPressed = false;
  static unsigned long wifiResetStart = 0;

  if (digitalRead(wifiResetButtonPin) == HIGH) {
    if (!wifiResetPressed) {
      wifiResetPressed = true;
      wifiResetStart = millis();
    } else if (millis() - wifiResetStart > 5000) {
      Serial.println("🔧 WiFi reset requested via button");
      setLedGreen(false);
      setLedRed(false);
      delay(500);
      WiFiManager wifiManager;
      wifiManager.resetSettings();
      delay(500);
      ESP.restart();
    }
  } else {
    wifiResetPressed = false;
  }

  if (WiFi.getMode() == WIFI_AP) {
    server.handleClient();
    return;
  }

  checkButton();

  if (serverMode) {
    server.handleClient();
    blinkLedsInServerMode();  
    showServerMode();
  } else {
    display.clearDisplay();

    if (!head) {
      showWallStreet();
      Serial.println("⚠️ No stocks to display. Use SERVER mode to add.");
      delay(4000);
      showFirstStart();
      delay(4000);
      serverMode = true;
      return;
    }

    if (apiKey == "") {
      showInsertAPI();
      return;
    }

    if ((!isFetching && millis() - lastRequest > 600000) || (newStock)) {
      currentNode = head;
      isFetching = true;
      lastQuoteRequestTime = millis() - quoteDelay;
      lastRequest = millis();
    }

    if (isFetching) {
      showWallStreet();
      handleNonBlockingFetch();
      if (!isFetching) { runTradingAlgorithm(); saveTradingState(); }
      if (newStock) newStock = false;
    } else {
      updateDisplay();
    }
  }
}
