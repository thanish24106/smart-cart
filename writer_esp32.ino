/*
 * ╔═══════════════════════════════════════════════════════╗
 * ║  SMARTCART — WRITER ESP32                             ║
 * ║  Hardware: ESP32 + RC522 RFID (no TFT needed)        ║
 * ║  Purpose : Scan tag → get UID → operator enters      ║
 * ║            product details on website → server sends  ║
 * ║            PENDING_WRITE → ESP32 writes to next scan  ║
 * ╚═══════════════════════════════════════════════════════╝
 *
 * LIBRARIES (install via Arduino Library Manager):
 *   - MFRC522  by GithubCommunity
 *   - ArduinoJson by Benoit Blanchon
 *   - ArduinoWebsockets by Gil Maimon
 *
 * Connections (Writer has no TFT — only RFID):
 *   RC522 → ESP32
 *   SDA   → GPIO 5
 *   SCK   → GPIO 18
 *   MOSI  → GPIO 23
 *   MISO  → GPIO 19
 *   RST   → GPIO 4
 *   3.3V  → 3.3V
 *   GND   → GND
 *
 * LED feedback (optional): GPIO 2 = built-in LED
 */

#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <ArduinoWebsockets.h>
#include <ArduinoJson.h>

// ── Config ─────────────────────────────────────────────────────────────────
const char* WIFI_SSID     = "moto g62 5G";
const char* WIFI_PASSWORD = "12345678";
const char* WS_HOST       = "10.98.0.187";  
const int   WS_PORT       = 3000;
const char* CART_ID       = "CART-001";
const char* DEVICE_ID     = "WRITER-001";

// ── Pins ───────────────────────────────────────────────────────────────────
#define RFID_SS_PIN   5
#define RFID_RST_PIN  4
#define LED_PIN       2   // Built-in LED

// ── Objects ────────────────────────────────────────────────────────────────
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
using namespace websockets;
WebsocketsClient wsClient;

void connectWiFi();
void connectWebSocket();
void sendWriterHello();

// ── Writer state ────────────────────────────────────────────────────────────
enum WriterState { IDLE, WAITING_FOR_TAG, WRITING };
WriterState writerState = IDLE;

struct PendingProduct {
  String uid;
  String name;
  float  price;
  String category;
  int    stock;
  bool   hasUid;  // operator pre-specified a UID
};
PendingProduct pending;

// ──────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.println("\n╔══════════════════════════╗");
  Serial.println("║  SmartCart Writer ESP32  ║");
  Serial.println("╚══════════════════════════╝");

  SPI.begin(18, 19, 23, RFID_SS_PIN);
  rfid.PCD_Init();
  Serial.println("RFID ready");

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  connectWiFi();

  // WebSocket
  wsClient.onMessage(onWsMessage);
  wsClient.onEvent([](WebsocketsEvent event, String data) {
    if (event == WebsocketsEvent::ConnectionOpened) {
      Serial.println("WS connected ✓");
      sendWriterHello();
    } else if (event == WebsocketsEvent::ConnectionClosed) {
      Serial.println("WS closed — will retry");
    }
  });

  connectWebSocket();

  Serial.println("\nReady. Waiting for PENDING_WRITE from website...");
  blinkLED(3, 100);
}

// ──────────────────────────────────────────────────────────────────────────
unsigned long lastReconnect = 0;
unsigned long lastWiFiReconnect = 0;
unsigned long lastScan      = 0;

void connectWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi not connected yet — will keep retrying");
  }
}

void connectWebSocket() {
  if (WiFi.status() != WL_CONNECTED) return;
  String wsUrl = "ws://" + String(WS_HOST) + ":" + String(WS_PORT);
  Serial.println("Connecting WS: " + wsUrl);
  wsClient.connect(wsUrl);
}

void sendWriterHello() {
  DynamicJsonDocument doc(96);
  doc["type"]     = "WRITER_HELLO";
  doc["deviceId"] = DEVICE_ID;
  String out;
  serializeJson(doc, out);
  bool ok = wsClient.send(out);
  Serial.println(ok ? "Sent WRITER_HELLO" : "WRITER_HELLO send failed");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWiFiReconnect > 5000) {
      lastWiFiReconnect = millis();
      Serial.println("WiFi lost — reconnecting...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
    delay(20);
    return;
  }

  wsClient.poll();

  // Auto reconnect
  if (!wsClient.available() && millis() - lastReconnect > 5000) {
    lastReconnect = millis();
    connectWebSocket();
  }

  // RFID scan
  if (millis() - lastScan > 1500 && rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    lastScan = millis();
    String uid = getUID();
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    Serial.println("Scanned tag: " + uid);

    switch (writerState) {

      case IDLE:
        // Just scanned a tag with no pending write queued
        // Tell website the tag UID so operator can fill details
        {
          DynamicJsonDocument doc(128);
          doc["type"] = "TAG_SCANNED_FOR_WRITE";
          doc["uid"]  = uid;
          String out; serializeJson(doc, out);
          wsClient.send(out);
          Serial.println("Reported UID to website: " + uid);
          blinkLED(1, 300);
        }
        break;

      case WAITING_FOR_TAG:
        // We have pending product data — now write it
        pending.uid = uid;
        writeProductToServer(uid);
        break;

      case WRITING:
        Serial.println("Already writing, please wait...");
        break;
    }
  }
}

// ── Write product to server (server stores it under this UID) ──────────────
void writeProductToServer(String uid) {
  writerState = WRITING;
  Serial.println("Writing product to server...");
  Serial.println("  UID:      " + uid);
  Serial.println("  Name:     " + pending.name);
  Serial.println("  Price:    " + String(pending.price));
  Serial.println("  Category: " + pending.category);
  Serial.println("  Stock:    " + String(pending.stock));

  DynamicJsonDocument doc(320);
  doc["type"]     = "WRITE_PRODUCT";
  doc["uid"]      = uid;
  doc["name"]     = pending.name;
  doc["price"]    = pending.price;
  doc["category"] = pending.category;
  doc["stock"]    = pending.stock;

  String out;
  serializeJson(doc, out);
  wsClient.send(out);
}

// ── WebSocket message handler ───────────────────────────────────────────────
void onWsMessage(WebsocketsMessage msg) {
  DynamicJsonDocument doc(512);
  DeserializationError err = deserializeJson(doc, msg.data());
  if (err) return;

  const char* type = doc["type"];

  // ── Server sends write command from operator dashboard ────────────────
  if (strcmp(type, "PENDING_WRITE") == 0) {
    pending.name     = doc["name"].as<String>();
    pending.price    = doc["price"].as<float>();
    pending.category = doc["category"].as<String>();
    pending.stock    = doc["stock"].as<int>();
    String presetUid = doc["uid"].as<String>();

    Serial.println("\n★ Write command received from website:");
    Serial.println("  Name: " + pending.name);
    Serial.println("  Price: " + String(pending.price));
    Serial.println("  Category: " + pending.category);
    Serial.println("  Stock: " + String(pending.stock));

    if (presetUid.length() > 0 && presetUid != "null") {
      // Operator already specified a UID (re-writing existing product)
      pending.uid = presetUid;
      pending.hasUid = true;
      writeProductToServer(presetUid);
    } else {
      // Need to scan a tag to get UID
      pending.hasUid = false;
      writerState = WAITING_FOR_TAG;
      Serial.println("→ Now scan the RFID tag to assign this product...");
      blinkLED(5, 80);
    }
  }

  // ── Server confirms write success ─────────────────────────────────────
  else if (strcmp(type, "WRITE_OK") == 0) {
    String name = doc["product"]["name"].as<String>();
    String uid  = doc["product"]["uid"].as<String>();
    Serial.println("✓ Product saved: " + name + " [" + uid + "]");
    writerState = IDLE;
    blinkLED(3, 150);
  }

  // ── State snapshot on connect ─────────────────────────────────────────
  else if (strcmp(type, "STATE") == 0) {
    Serial.println("Server state received — " + String(doc["products"].size()) + " products in DB");
  }
}

// ── Helpers ────────────────────────────────────────────────────────────────
String getUID() {
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

void blinkLED(int times, int ms) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH); delay(ms);
    digitalWrite(LED_PIN, LOW);  delay(ms);
  }
}
