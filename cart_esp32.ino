/*
 * ╔═══════════════════════════════════════════════════════╗
 * ║  SMARTCART — CART ESP32                               ║
 * ║  Hardware: ESP32 + RC522 RFID + 2.8" SPI TFT         ║
 * ╚═══════════════════════════════════════════════════════╝
 *
 * LIBRARIES (install via Arduino Library Manager):
 *   - MFRC522  by GithubCommunity
 *   - TFT_eSPI by Bodmer
 *   - ArduinoJson by Benoit Blanchon
 *   - ArduinoWebsockets by Gil Maimon
 */
 // TFT_eSPI USER_SETUP — in Arduino/libraries/TFT_eSPI/User_Setup.h set:
    #define ILI9341_DRIVER
    #define TFT_CS   15
    #define TFT_DC    2
    #define TFT_RST   0
    #define TFT_MOSI 23
    #define TFT_SCLK 18
    #define TFT_MISO 19
    #define TOUCH_CS 14
 

#include <SPI.h>
#include <MFRC522.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <ArduinoWebsockets.h>
#include <ArduinoJson.h>

// ── Config ─────────────────────────────────────────────────────────────────
const char* WIFI_SSID     = "moto g62 5G";
const char* WIFI_PASSWORD = "12345678";
const char* WS_HOST       = "10.98.0.187";  
const int   WS_PORT       = 3000;
const char* CART_ID       = "CART-001";
const bool  TOUCH_ENABLED = true;

// Common XPT2046 calibration for 2.8" ILI9341 modules.
// If touches print wrong coordinates in Serial Monitor, recalibrate and update these.
uint16_t TOUCH_CAL_DATA[5] = { 275, 3620, 264, 3532, 1 };

// ── Pin definitions ────────────────────────────────────────────────────────
#define RFID_SS_PIN   5
#define RFID_RST_PIN  4

// ── Objects ────────────────────────────────────────────────────────────────
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
TFT_eSPI tft = TFT_eSPI();
using namespace websockets;
WebsocketsClient wsClient;

// ── Cart state ─────────────────────────────────────────────────────────────
struct CartItem {
  String uid;
  String name;
  String category;
  float  price;
  int    qty;
};

#define MAX_ITEMS 10
CartItem cartItems[MAX_ITEMS];
int      itemCount  = 0;
float    cartTotal  = 0.0f;
int      scrollPos  = 0;   // which item is at top of list

// ── Touch state ────────────────────────────────────────────────────────────
bool     inDeleteMode = false;
int      selectedIdx  = -1;

// ── UI colours ─────────────────────────────────────────────────────────────
#define C_BG      0x0841   // very dark blue-black
#define C_SURFACE 0x10A2
#define C_ACCENT  0x07FF   // cyan
#define C_GREEN   0x57E0
#define C_RED     0xF800
#define C_WHITE   0xFFFF
#define C_MUTED   0x7BEF
#define C_YELLOW  0xFFE0

// ── Forward declarations ───────────────────────────────────────────────────
void drawUI();
void drawHeader();
void drawCartList();
void drawFooter();
void onWsMessage(WebsocketsMessage msg);
void connectWiFi();
void connectWebSocket();
void drawConnectionStatus(String msg, uint16_t color);
void initSharedSpiPins();
void checkRfidReader();
bool readRfidUID(String &uid);
void sendCartHello();
void sendScan(String uid);
void sendRemove(String uid);
String getUID();
bool  isNewCard();

// ──────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  initSharedSpiPins();
  SPI.begin(18, 19, 23);  // SCK, MISO, MOSI

  // ── TFT ──
  tft.init();
  digitalWrite(TFT_CS, HIGH);
  digitalWrite(TOUCH_CS, HIGH);
  digitalWrite(RFID_SS_PIN, HIGH);
  tft.setRotation(1);  // landscape
  if (TOUCH_ENABLED) tft.setTouch(TOUCH_CAL_DATA);
  tft.fillScreen(C_BG);
  tft.setTextColor(C_WHITE, C_BG);
  tft.setTextSize(1);

  // Splash
  tft.setCursor(40, 100);
  tft.setTextColor(C_ACCENT);
  tft.setTextSize(3);
  tft.print("SmartCart");
  tft.setTextSize(1);
  tft.setTextColor(C_MUTED);
  tft.setCursor(100, 140);
  tft.print("Connecting...");

  // ── SPI + RFID ──
  rfid.PCD_Init();
  checkRfidReader();

  // ── WiFi ──
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  connectWiFi();

  // ── WebSocket ──
  wsClient.onMessage(onWsMessage);
  wsClient.onEvent([](WebsocketsEvent event, String data) {
    if (event == WebsocketsEvent::ConnectionOpened) {
      Serial.println("WS connected");
      sendCartHello();
      drawUI();
    } else if (event == WebsocketsEvent::ConnectionClosed) {
      Serial.println("WS disconnected — retrying…");
      tft.fillRect(0, 0, 320, 20, C_BG);
      tft.setCursor(2, 4);
      tft.setTextColor(C_RED);
      tft.print("WS DISCONNECTED");
    }
  });

  connectWebSocket();
  delay(500);
  drawUI();
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
    drawConnectionStatus("WIFI OK", C_GREEN);
  } else {
    Serial.println("\nWiFi not connected yet — will keep retrying");
    drawConnectionStatus("WIFI RETRY", C_YELLOW);
  }
}

void connectWebSocket() {
  if (WiFi.status() != WL_CONNECTED) return;
  String wsUrl = "ws://" + String(WS_HOST) + ":" + String(WS_PORT);
  Serial.println("Connecting WS: " + wsUrl);
  drawConnectionStatus("WS CONNECTING", C_YELLOW);
  wsClient.connect(wsUrl);
}

void drawConnectionStatus(String msg, uint16_t color) {
  tft.fillRect(0, 0, 320, 20, C_BG);
  tft.setTextSize(1);
  tft.setTextColor(color);
  tft.setCursor(2, 4);
  tft.print(msg);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWiFiReconnect > 5000) {
      lastWiFiReconnect = millis();
      Serial.println("WiFi lost — reconnecting...");
      drawConnectionStatus("WIFI RECONNECT", C_YELLOW);
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
    delay(20);
    return;
  }

  wsClient.poll();

  // Auto-reconnect
  if (!wsClient.available() && millis() - lastReconnect > 5000) {
    lastReconnect = millis();
    connectWebSocket();
  }

  // Touch handling
  if (TOUCH_ENABLED) handleTouch();

  // RFID scan (debounce 2 seconds)
  String uid;
  if (millis() - lastScan > 1200 && readRfidUID(uid)) {
    lastScan = millis();
    Serial.println("Scanned: " + uid);

    if (inDeleteMode) {
      // In delete mode: scan tag to remove it
      sendRemove(uid);
      inDeleteMode = false;
      showToast("Removing...", C_YELLOW);
    } else {
      showToast("Scanning...", C_ACCENT);
      sendScan(uid);
    }
  }
}

// ── Shared SPI / RFID ──────────────────────────────────────────────────────
void initSharedSpiPins() {
  pinMode(TFT_CS, OUTPUT);
  pinMode(TOUCH_CS, OUTPUT);
  pinMode(RFID_SS_PIN, OUTPUT);
  pinMode(RFID_RST_PIN, OUTPUT);

  digitalWrite(TFT_CS, HIGH);
  digitalWrite(TOUCH_CS, HIGH);
  digitalWrite(RFID_SS_PIN, HIGH);
  digitalWrite(RFID_RST_PIN, HIGH);
}

void checkRfidReader() {
  digitalWrite(TFT_CS, HIGH);
  digitalWrite(TOUCH_CS, HIGH);
  delay(50);

  byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.print("RC522 version: 0x");
  Serial.println(version, HEX);

  if (version == 0x00 || version == 0xFF) {
    Serial.println("RC522 not detected. Check SDA=5, SCK=18, MOSI=23, MISO=19, RST=4, 3.3V, GND.");
    tft.fillScreen(C_BG);
    tft.setTextColor(C_RED);
    tft.setTextSize(1);
    tft.setCursor(20, 95);
    tft.print("RFID reader not found");
    tft.setCursor(20, 112);
    tft.print("Check RC522 wiring");
    delay(2000);
  } else {
    Serial.println("RC522 ready");
  }
}

bool readRfidUID(String &uid) {
  // Keep the other SPI devices deselected before the RC522 transaction.
  digitalWrite(TFT_CS, HIGH);
  digitalWrite(TOUCH_CS, HIGH);
  digitalWrite(RFID_SS_PIN, HIGH);

  if (!rfid.PICC_IsNewCardPresent()) return false;
  if (!rfid.PICC_ReadCardSerial()) return false;

  uid = getUID();
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  return true;
}

// ── Touch ──────────────────────────────────────────────────────────────────
void handleTouch() {
  uint16_t tx, ty;
  digitalWrite(RFID_SS_PIN, HIGH);
  digitalWrite(TFT_CS, HIGH);
  digitalWrite(TOUCH_CS, HIGH);
  if (!tft.getTouch(&tx, &ty, 600)) return;
  digitalWrite(TOUCH_CS, HIGH);

  // Debounce
  static unsigned long lastTouch = 0;
  if (millis() - lastTouch < 400) return;
  lastTouch = millis();

  Serial.println("Touch x=" + String(tx) + " y=" + String(ty));

  // Footer button area. Hit zones are intentionally wider than the drawn buttons.
  if (ty >= 170) {
    if (tx < 150) {
      sendCheckout();
      return;
    }

    if (tx > 170) {
      inDeleteMode = !inDeleteMode;
      showToast(inDeleteMode ? "Scan to delete" : "Delete cancelled", inDeleteMode ? C_RED : C_MUTED);
      drawFooter();
      return;
    }
  }

  // DELETE button area fallback (bottom right, drawn at x=210..310,y=189..213)
  if (tx > 200 && tx < 320 && ty > 175 && ty < 235) {
    inDeleteMode = !inDeleteMode;
    showToast(inDeleteMode ? "Scan to delete" : "Delete cancelled", inDeleteMode ? C_RED : C_MUTED);
    drawFooter();
    return;
  }

  // CHECKOUT button area fallback (bottom left, drawn at x=8..108,y=189..213)
  if (tx > 0 && tx < 140 && ty > 175 && ty < 235) {
    sendCheckout();
    return;
  }

  // Scroll up (top strip, y < 40 in list area)
  if (ty < 55 && ty > 25 && scrollPos > 0) {
    scrollPos--;
    drawCartList();
    return;
  }

  // Scroll down (bottom of list area, ~y > 165 and < 185)
  if (ty > 165 && ty < 185 && scrollPos < itemCount - 4) {
    scrollPos++;
    drawCartList();
    return;
  }

  // Tap on list item to select for delete
  if (ty > 45 && ty < 170) {
    int row = (ty - 45) / 30;
    int idx = scrollPos + row;
    if (idx < itemCount) {
      selectedIdx = idx;
      inDeleteMode = true;
      drawCartList();
      showToast("Scan item to remove", C_RED);
    }
  }
}

// ── Draw full UI ────────────────────────────────────────────────────────────
void drawUI() {
  tft.fillScreen(C_BG);
  drawHeader();
  drawCartList();
  drawFooter();
}

void drawHeader() {
  // Header bar
  tft.fillRect(0, 0, 320, 25, C_SURFACE);
  tft.drawFastHLine(0, 25, 320, C_ACCENT);

  tft.setTextColor(C_ACCENT);
  tft.setTextSize(1);
  tft.setCursor(4, 8);
  tft.print("SMARTCART  ");
  tft.setTextColor(C_MUTED);
  tft.print(CART_ID);

  // Total at right
  tft.setTextColor(C_GREEN);
  tft.setCursor(200, 4);
  tft.setTextSize(2);
  char buf[12];
  sprintf(buf, "Rs%.2f", cartTotal);
  tft.print(buf);
}

#define LIST_Y     28
#define LIST_H     155
#define ROW_H      30
#define ROWS_VIS   5

void drawCartList() {
  tft.fillRect(0, LIST_Y, 320, LIST_H, C_BG);

  if (itemCount == 0) {
    tft.setTextColor(C_MUTED);
    tft.setTextSize(1);
    tft.setCursor(80, 95);
    tft.print("Cart is empty");
    tft.setCursor(55, 110);
    tft.print("Scan a product to add it");
    return;
  }

  int end = min(scrollPos + ROWS_VIS, itemCount);
  for (int i = scrollPos; i < end; i++) {
    int y = LIST_Y + (i - scrollPos) * ROW_H;
    bool sel = (i == selectedIdx && inDeleteMode);

    // Row background
    uint16_t rowBg = sel ? 0x3800 : ((i % 2 == 0) ? C_SURFACE : C_BG);
    tft.fillRect(0, y, 320, ROW_H - 2, rowBg);

    // Category tag (tiny)
    tft.setTextColor(C_ACCENT);
    tft.setTextSize(1);
    tft.setCursor(4, y + 3);
    String cat = cartItems[i].category.substring(0, 4);
    cat.toUpperCase();
    tft.print(cat);

    // Name
    tft.setTextColor(sel ? C_RED : C_WHITE);
    tft.setCursor(36, y + 3);
    String name = cartItems[i].name;
    if (name.length() > 14) name = name.substring(0, 13) + "~";
    tft.print(name);

    // Qty badge
    tft.setTextColor(C_MUTED);
    tft.setCursor(36, y + 15);
    tft.print("x" + String(cartItems[i].qty));

    // Price right
    tft.setTextColor(C_GREEN);
    tft.setTextSize(1);
    char buf[12];
    sprintf(buf, "Rs%.2f", cartItems[i].price * cartItems[i].qty);
    int px = 320 - strlen(buf) * 6 - 4;
    tft.setCursor(px, y + 10);
    tft.print(buf);

    // Divider
    tft.drawFastHLine(0, y + ROW_H - 2, 320, C_SURFACE);
  }

  // Scroll indicators
  if (scrollPos > 0) {
    tft.setTextColor(C_ACCENT);
    tft.setCursor(155, LIST_Y);
    tft.print("^");
  }
  if (scrollPos + ROWS_VIS < itemCount) {
    tft.setTextColor(C_ACCENT);
    tft.setCursor(155, LIST_Y + LIST_H - 10);
    tft.print("v");
  }
}

void drawFooter() {
  int fy = 185;
  tft.fillRect(0, fy, 320, 35, C_SURFACE);
  tft.drawFastHLine(0, fy, 320, C_ACCENT);

  // Checkout btn
  uint16_t checkoutColor = (itemCount > 0) ? C_GREEN : C_MUTED;
  tft.fillRoundRect(8, fy + 4, 100, 24, 4, checkoutColor);
  tft.setTextColor(C_BG);
  tft.setTextSize(1);
  tft.setCursor(20, fy + 12);
  tft.print("CHECKOUT");

  // Delete btn
  uint16_t delColor = inDeleteMode ? C_RED : C_SURFACE;
  uint16_t delText  = inDeleteMode ? C_WHITE : C_RED;
  tft.fillRoundRect(210, fy + 4, 100, 24, 4, delColor);
  tft.drawRoundRect(210, fy + 4, 100, 24, 4, C_RED);
  tft.setTextColor(delText);
  tft.setCursor(228, fy + 12);
  tft.print(inDeleteMode ? "CANCEL DEL" : "DELETE");

  // Item count in centre
  tft.setTextColor(C_MUTED);
  tft.setCursor(128, fy + 12);
  tft.print(String(itemCount) + " items");
}

// ── Toast overlay ──────────────────────────────────────────────────────────
void showToast(String msg, uint16_t color) {
  tft.fillRoundRect(60, 85, 200, 30, 6, C_SURFACE);
  tft.drawRoundRect(60, 85, 200, 30, 6, color);
  tft.setTextColor(color);
  tft.setTextSize(1);
  int cx = 60 + (200 - msg.length() * 6) / 2;
  tft.setCursor(cx, 97);
  tft.print(msg);

  // Auto-clear after 1.5s (non-blocking would need a timer — keep simple)
  delay(1200);
  drawCartList();  // redraw to remove toast
}

// ── Send scan ──────────────────────────────────────────────────────────────
void sendCartHello() {
  DynamicJsonDocument doc(96);
  doc["type"]     = "CART_HELLO";
  doc["cartId"]   = CART_ID;
  doc["deviceId"] = CART_ID;
  String out;
  serializeJson(doc, out);
  bool ok = wsClient.send(out);
  Serial.println(ok ? "Sent CART_HELLO" : "CART_HELLO send failed");
}

void sendScan(String uid) {
  if (!wsClient.available()) {
    Serial.println("SCAN not sent: WS disconnected");
    showToast("WS disconnected", C_RED);
    return;
  }
  DynamicJsonDocument doc(128);
  doc["type"]   = "SCAN";
  doc["cartId"] = CART_ID;
  doc["uid"]    = uid;
  String out;
  serializeJson(doc, out);
  bool ok = wsClient.send(out);
  Serial.println(ok ? "Sent SCAN: " + uid : "SCAN send failed");
}

void sendRemove(String uid) {
  if (!wsClient.available()) {
    Serial.println("REMOVE not sent: WS disconnected");
    showToast("WS disconnected", C_RED);
    return;
  }
  DynamicJsonDocument doc(128);
  doc["type"]   = "REMOVE";
  doc["cartId"] = CART_ID;
  doc["uid"]    = uid;
  String out;
  serializeJson(doc, out);
  bool ok = wsClient.send(out);
  Serial.println(ok ? "Sent REMOVE: " + uid : "REMOVE send failed");
}

void sendCheckout() {
  if (itemCount == 0) {
    Serial.println("CHECKOUT ignored: cart empty");
    showToast("Cart empty", C_MUTED);
    return;
  }
  if (!wsClient.available()) {
    Serial.println("CHECKOUT not sent: WS disconnected");
    showToast("WS disconnected", C_RED);
    return;
  }
  DynamicJsonDocument doc(64);
  doc["type"]   = "CHECKOUT";
  doc["cartId"] = CART_ID;
  String out;
  serializeJson(doc, out);
  bool ok = wsClient.send(out);
  Serial.println(ok ? "Sent CHECKOUT" : "CHECKOUT send failed");
  showToast("Checkout sent!", C_GREEN);
}

// ── WebSocket message handler ───────────────────────────────────────────────
void onWsMessage(WebsocketsMessage msg) {
  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, msg.data());
  if (err) return;

  const char* type = doc["type"];

  // ── Successful scan ──────────────────────────────────────────────────────
  if (strcmp(type, "SCAN_OK") == 0) {
    JsonObject cart = doc["cart"];
    // Rebuild local cart from server state
    itemCount = 0;
    cartTotal = 0;
    for (JsonObject item : cart["items"].as<JsonArray>()) {
      if (itemCount >= MAX_ITEMS) break;
      cartItems[itemCount].uid      = item["uid"].as<String>();
      cartItems[itemCount].name     = item["name"].as<String>();
      cartItems[itemCount].category = item["category"].as<String>();
      cartItems[itemCount].price    = item["price"].as<float>();
      cartItems[itemCount].qty      = item["qty"].as<int>();
      itemCount++;
    }
    cartTotal = cart["total"].as<float>();
    // Show newest item at top
    scrollPos = max(0, itemCount - ROWS_VIS);
    drawUI();
    String productName = doc["product"]["name"].as<String>();
    showToast("Added: " + productName.substring(0, 12), C_GREEN);
  }

  // ── Scan error (product not in DB) ──────────────────────────────────────
  else if (strcmp(type, "SCAN_ERROR") == 0) {
    String error = doc["error"].as<String>();
    showToast(error == "Out of stock" ? "Out of stock!" : "Unknown product!", C_RED);
  }

  // ── Remove OK ────────────────────────────────────────────────────────────
  else if (strcmp(type, "REMOVE_OK") == 0) {
    JsonObject cart = doc["cart"];
    itemCount = 0; cartTotal = 0;
    for (JsonObject item : cart["items"].as<JsonArray>()) {
      if (itemCount >= MAX_ITEMS) break;
      cartItems[itemCount].uid      = item["uid"].as<String>();
      cartItems[itemCount].name     = item["name"].as<String>();
      cartItems[itemCount].category = item["category"].as<String>();
      cartItems[itemCount].price    = item["price"].as<float>();
      cartItems[itemCount].qty      = item["qty"].as<int>();
      itemCount++;
    }
    cartTotal = cart["total"].as<float>();
    scrollPos = max(0, min(scrollPos, itemCount - 1));
    selectedIdx = -1;
    drawUI();
    showToast("Item removed", C_YELLOW);
  }

  // ── Checkout OK ──────────────────────────────────────────────────────────
  else if (strcmp(type, "CHECKOUT_OK") == 0) {
    // Show bill summary
    tft.fillScreen(C_BG);
    tft.setTextColor(C_GREEN); tft.setTextSize(2);
    tft.setCursor(70, 40);  tft.print("THANK YOU!");
    tft.setTextColor(C_WHITE); tft.setTextSize(1);
    tft.setCursor(60, 80);  tft.print("Total Paid:");
    tft.setTextColor(C_GREEN); tft.setTextSize(2);
    char buf[16]; sprintf(buf, "Rs %.2f", cartTotal);
    tft.setCursor(60, 100); tft.print(buf);
    delay(5000);
    // Clear
    itemCount = 0; cartTotal = 0; scrollPos = 0; selectedIdx = -1;
    drawUI();
  }

  // ── Full cart sync from server ───────────────────────────────────────────
  else if (strcmp(type, "CART_UPDATE") == 0) {
    JsonObject cart = doc["cart"];
    if (cart["cartId"].as<String>() != String(CART_ID)) return;
    itemCount = 0; cartTotal = 0;
    for (JsonObject item : cart["items"].as<JsonArray>()) {
      if (itemCount >= MAX_ITEMS) break;
      cartItems[itemCount].uid      = item["uid"].as<String>();
      cartItems[itemCount].name     = item["name"].as<String>();
      cartItems[itemCount].category = item["category"].as<String>();
      cartItems[itemCount].price    = item["price"].as<float>();
      cartItems[itemCount].qty      = item["qty"].as<int>();
      itemCount++;
    }
    cartTotal = cart["total"].as<float>();
    drawHeader();
  }
}

// ── Get UID from RFID ───────────────────────────────────────────────────────
String getUID() {
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}
