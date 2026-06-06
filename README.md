# SmartCart Setup Guide

SmartCart is a local RFID shopping cart system with three parts:

- `server.js`: Node.js HTTP + WebSocket server running on the laptop.
- `index.html`: Operator website for live carts, product writing, and device info.
- `cart_esp32/cart_esp32.ino`: ESP32 cart firmware with RC522 RFID, TFT, and touch.
- `writer_esp32/writer_esp32.ino`: ESP32 writer firmware with RC522 RFID only.

The laptop, cart ESP32, and writer ESP32 must all be on the same Wi-Fi network.

## 1. Install Server Dependencies

From this folder:

```powershell
cd C:\Users\thani\Downloads\files
npm.cmd install
```

Use `npm.cmd` on Windows because PowerShell may block `npm.ps1`.

## 2. Start The Website Server

```powershell
cd C:\Users\thani\Downloads\files
npm.cmd start
```

Keep this terminal open while using the system.

The server prints URLs like:

```text
Smart Cart Server running
Website  -> http://localhost:3000
Network  -> http://10.98.0.187:3000
ESP32 WS -> ws://10.98.0.187:3000
```

Open the website on the laptop:

```text
http://localhost:3000
```

Or from another device on the same Wi-Fi:

```text
http://YOUR_LAPTOP_IP:3000
```

## 3. Find The Laptop IP Address

On Windows:

```powershell
ipconfig
```

Look under `Wireless LAN adapter Wi-Fi` for:

```text
IPv4 Address
```

Example:

```text
10.98.0.187
```

This IP must match `WS_HOST` in both ESP32 sketches:

```cpp
const char* WS_HOST = "10.98.0.187";
const int WS_PORT = 3000;
```

Important: if you disconnect/reconnect Wi-Fi or hotspot, the laptop IP may change. If ESP32 Wi-Fi connects but WebSocket does not, check `ipconfig` again and update `WS_HOST`.

## 4. Configure ESP32 Wi-Fi And Server IP

In both files:

- `cart_esp32/cart_esp32.ino`
- `writer_esp32/writer_esp32.ino`

Check:

```cpp
const char* WIFI_SSID     = "moto g62 5G";
const char* WIFI_PASSWORD = "12345678";
const char* WS_HOST       = "10.98.0.187";
const int   WS_PORT       = 3000;
```

Upload both sketches after changing these values.

## 5. Arduino Libraries

Install these from Arduino IDE Library Manager:

- `MFRC522` by GithubCommunity
- `ArduinoJson` by Benoit Blanchon
- `ArduinoWebsockets` by Gil Maimon
- `TFT_eSPI` by Bodmer, cart ESP32 only

## 6. Cart ESP32 Hardware

Cart hardware:

- ESP32
- RC522 RFID reader
- 2.8 inch ILI9341 SPI TFT with touch

RC522 wiring:

```text
RC522 SDA  -> GPIO 5
RC522 SCK  -> GPIO 18
RC522 MOSI -> GPIO 23
RC522 MISO -> GPIO 19
RC522 RST  -> GPIO 4
RC522 3.3V -> 3.3V
RC522 GND  -> GND
```

TFT_eSPI setup:

Edit:

```text
Documents/Arduino/libraries/TFT_eSPI/User_Setup.h
```

Use:

```cpp
#define ILI9341_DRIVER
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST   0
#define TFT_MOSI 23
#define TFT_SCLK 18
#define TFT_MISO 19
#define TOUCH_CS 14
```

The cart code currently has touch enabled:

```cpp
const bool TOUCH_ENABLED = true;
```

Touch calibration is:

```cpp
uint16_t TOUCH_CAL_DATA[5] = { 275, 3620, 264, 3532, 1 };
```

If touch prints wrong coordinates in Serial Monitor, recalibrate and update this array.

## 7. Writer ESP32 Hardware

Writer hardware:

- ESP32
- RC522 RFID reader

RC522 wiring:

```text
RC522 SDA  -> GPIO 5
RC522 SCK  -> GPIO 18
RC522 MOSI -> GPIO 23
RC522 MISO -> GPIO 19
RC522 RST  -> GPIO 4
RC522 3.3V -> 3.3V
RC522 GND  -> GND
```

The writer identifies itself to the server as:

```cpp
const char* DEVICE_ID = "WRITER-001";
```

## 8. Website Tabs

The operator website has:

- `Products`: saved product catalog.
- `Write Tag`: form for adding product data to an RFID UID.
- `Info`: connected dashboard, cart ESP32, writer ESP32, IP address, last seen time.

If the Info tab shows zero devices while ESP32s are connected, restart the Node server and hard-refresh the browser with `Ctrl+F5`.

## 9. Add Products With Writer ESP32

Method A: scan first

1. Scan an RFID tag on the writer ESP32.
2. The website fills the UID field.
3. Enter name, price, stock quantity, and category.
4. Click `WRITE TO TAG`.
5. Product is saved in `smartcart-data.json`.

Method B: fill first

1. Open `Write Tag`.
2. Enter name, price, stock quantity, and category.
3. Leave UID blank.
4. Click `WRITE TO TAG`.
5. Scan a tag on the writer ESP32.
6. Product is saved.

Example:

```text
Product: coco cola
Price: 40
Stock Quantity: 200
```

If the cart scans 4 Coca-Cola tags, the website product list shows:

```text
196 left
```

## 10. Use The Cart

Add product:

1. Scan a product tag on the cart ESP32.
2. Server checks product UID.
3. Server reduces product stock by 1.
4. Cart TFT updates item list and total.
5. Website Live Carts and product stock update.

Remove product:

1. Tap `DELETE` on the cart TFT.
2. Scan the product tag.
3. Quantity decreases by one.
4. Product stock increases by one.
5. If quantity becomes zero, the product row is removed.

Checkout:

1. Tap `CHECKOUT` on the cart TFT.
2. Website receives checkout event.
3. Cart shows thank-you screen.
4. Server clears the cart after checkout.

## 11. Data Storage

Product and cart data are stored in:

```text
smartcart-data.json
```

This prevents product data from disappearing when the server restarts.

## 12. Useful Server Checks

Check products:

```powershell
Invoke-RestMethod http://localhost:3000/api/products | ConvertTo-Json -Depth 8
```

Check carts:

```powershell
Invoke-RestMethod http://localhost:3000/api/carts | ConvertTo-Json -Depth 8
```

Check connected devices:

```powershell
Invoke-RestMethod http://localhost:3000/api/connections | ConvertTo-Json -Depth 8
```

If `/api/connections` returns `404`, the old server is still running. Stop it and start the server again.

## 13. Stop Or Restart Server

Find the process using port `3000`:

```powershell
netstat -ano | findstr :3000
```

Kill it:

```powershell
taskkill /PID YOUR_PID /F
```

Start again:

```powershell
npm.cmd start
```

## 14. Windows Firewall

If the website works on the laptop but ESP32 cannot connect, allow inbound port `3000`.

Run PowerShell as Administrator:

```powershell
netsh advfirewall firewall add rule name="SmartCart" dir=in action=allow protocol=TCP localport=3000
```

Or allow `node.exe` in Windows Defender Firewall.

## 15. Serial Monitor Messages

Open Arduino Serial Monitor at:

```text
115200 baud
```

Good cart messages:

```text
WiFi connected: 10.98.0.xx
Connecting WS: ws://10.98.0.187:3000
WS connected
Sent CART_HELLO
RC522 version: 0x91
RC522 ready
Scanned: 5A011807
Sent SCAN: 5A011807
Touch x=...
Sent CHECKOUT
```

Good writer messages:

```text
WiFi connected: 10.98.0.xx
Connecting WS: ws://10.98.0.187:3000
WS connected
Sent WRITER_HELLO
Scanned tag: ...
```

## 16. Troubleshooting

ESP32 connects to Wi-Fi but not website:

- Check laptop IP with `ipconfig`.
- Update `WS_HOST` in both sketches.
- Restart ESP32 after upload.
- Check server is running with `npm.cmd start`.
- Check firewall allows port `3000`.

Website Info tab shows zero devices:

- Open `http://localhost:3000/api/connections`.
- If it returns `404`, restart Node server.
- Hard-refresh website with `Ctrl+F5`.

Cart RFID not reading:

- Serial Monitor should show `RC522 version: 0x91` or similar.
- If version is `0x00` or `0xFF`, check RC522 wiring and 3.3V power.
- Confirm `SDA=GPIO5` and `RST=GPIO4`.

Cart touch not working:

- Confirm `TOUCH_CS=14` in TFT_eSPI setup.
- Open Serial Monitor and tap screen.
- You should see `Touch x=... y=...`.
- If coordinates are wrong, update `TOUCH_CAL_DATA`.

Cart removes all duplicates:

- This has been fixed in `server.js`.
- Remove now decreases `qty` by one.
- Restart server after updating `server.js`.

Product not found:

- Add the product first using the writer ESP32.
- Make sure the UID shown in Products matches the scanned tag UID.

Out of stock:

- The cart will not add products with stock `0`.
- Add more stock by writing/updating that product from the `Write Tag` tab.
- Stock decreases on successful cart scan and increases if that item is removed before checkout.

Server starts but ESP32 still cannot connect:

- Make sure all devices are on the same Wi-Fi.
- Phone hotspots sometimes isolate clients; try another hotspot/router if needed.
- Make laptop IP static/reserved if possible.

## 17. Current Important Values

Current laptop IP used in ESP sketches:

```text
10.98.0.187
```

Server port:

```text
3000
```

Cart ID:

```text
CART-001
```

Writer ID:

```text
WRITER-001
```
