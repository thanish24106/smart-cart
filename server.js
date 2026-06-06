// Smart Cart WebSocket + HTTP Server
// Run with: node server.js
// Install deps: npm install express ws

const express = require('express');
const http = require('http');
const WebSocket = require('ws');
const path = require('path');
const os = require('os');
const fs = require('fs');

const app = express();
const server = http.createServer(app);
const wss = new WebSocket.Server({ server });
const DATA_FILE = path.join(__dirname, 'smartcart-data.json');

app.use(express.json());
app.use(express.static(path.join(__dirname)));

// ─── Store ─────────────────────────────────────────────────────────────────
const savedState = loadData();
let products = savedState.products; // { uid: { uid, name, price, category } }
let carts    = savedState.carts;    // { cartId: { cartId, items: [{uid,name,price,qty}], total } }
let clients  = []; // websocket clients
let nextClientId = 1;

// ─── Broadcast helper ──────────────────────────────────────────────────────
function loadData() {
  try {
    if (!fs.existsSync(DATA_FILE)) return { products: {}, carts: {} };
    const data = JSON.parse(fs.readFileSync(DATA_FILE, 'utf8'));
    const products = data.products || {};
    Object.values(products).forEach(product => {
      product.stock = parseStock(product.stock);
    });
    return {
      products,
      carts: data.carts || {}
    };
  } catch (err) {
    console.warn('Could not load smartcart-data.json:', err.message);
    return { products: {}, carts: {} };
  }
}

function saveData() {
  try {
    fs.writeFileSync(DATA_FILE, JSON.stringify({ products, carts }, null, 2));
  } catch (err) {
    console.warn('Could not save smartcart-data.json:', err.message);
  }
}

function parseStock(value, fallback = 0) {
  const stock = Number(value);
  if (!Number.isFinite(stock)) return fallback;
  return Math.max(0, Math.floor(stock));
}

function broadcast(msg) {
  const data = JSON.stringify(msg);
  clients.forEach(ws => {
    if (ws.readyState === WebSocket.OPEN) ws.send(data);
  });
}

function send(ws, msg) {
  if (ws.readyState !== WebSocket.OPEN) return false;
  ws.send(JSON.stringify(msg));
  return true;
}

function sendToWriters(msg) {
  let sent = 0;
  clients.forEach(ws => {
    if (ws.role === 'writer') {
      ws.pendingWrite = msg;
      if (send(ws, msg)) sent++;
    }
  });
  return sent;
}

function getLocalAddresses() {
  return Object.values(os.networkInterfaces())
    .flat()
    .filter(net => net && net.family === 'IPv4' && !net.internal)
    .map(net => net.address);
}

function getConnections() {
  return clients.map(ws => ({
    connectionId: ws.connectionId,
    role: ws.role || 'unknown',
    deviceId: ws.deviceId || ws.connectionId,
    cartId: ws.cartId || '',
    remoteAddress: ws.remoteAddress || 'unknown',
    connectedAt: ws.connectedAt,
    lastSeen: ws.lastSeen,
    state: ws.readyState === WebSocket.OPEN ? 'online' : 'offline'
  }));
}

function broadcastConnections() {
  broadcast({ type: 'CONNECTIONS_UPDATE', connections: getConnections() });
}

function setClientInfo(ws, info = {}, notify = true) {
  if (info.role) ws.role = info.role;
  if (info.deviceId) ws.deviceId = info.deviceId;
  if (info.cartId) ws.cartId = info.cartId;
  ws.lastSeen = new Date().toISOString();
  if (notify) broadcastConnections();
}

// ─── WebSocket ─────────────────────────────────────────────────────────────
wss.on('connection', (ws, req) => {
  const now = new Date().toISOString();
  ws.connectionId = `WS-${String(nextClientId++).padStart(3, '0')}`;
  ws.role = 'unknown';
  ws.deviceId = ws.connectionId;
  ws.cartId = '';
  ws.connectedAt = now;
  ws.lastSeen = now;
  ws.remoteAddress = req.socket.remoteAddress?.replace(/^::ffff:/, '') || 'unknown';
  clients.push(ws);
  console.log('WS client connected. Total:', clients.length);

  // Send current state on connect
  ws.send(JSON.stringify({ type: 'STATE', products, carts, connections: getConnections() }));
  broadcastConnections();

  ws.on('message', raw => {
    let msg;
    try { msg = JSON.parse(raw); } catch { return; }
    ws.lastSeen = new Date().toISOString();
    console.log('WS msg:', msg);

    switch (msg.type) {

      case 'DASHBOARD_HELLO': {
        setClientInfo(ws, {
          role: 'dashboard',
          deviceId: msg.deviceId || `DASHBOARD-${ws.connectionId}`
        });
        console.log('Dashboard registered');
        break;
      }

      case 'WRITER_HELLO': {
        setClientInfo(ws, {
          role: 'writer',
          deviceId: msg.deviceId || msg.writerId || `WRITER-${ws.connectionId}`
        });
        console.log('Writer ESP32 registered:', ws.deviceId);
        break;
      }

      case 'CART_HELLO': {
        setClientInfo(ws, {
          role: 'cart',
          deviceId: msg.deviceId || msg.cartId || `CART-${ws.connectionId}`,
          cartId: msg.cartId || ''
        });
        console.log('Cart ESP32 registered:', ws.deviceId);
        break;
      }

      // Web dashboard: send product details to the Writer ESP32.
      case 'PENDING_WRITE': {
        setClientInfo(ws, { role: 'dashboard', deviceId: ws.deviceId });
        const pendingWrite = {
          type: 'PENDING_WRITE',
          uid: msg.uid || '',
          name: msg.name || '',
          price: parseFloat(msg.price),
          category: msg.category || 'General',
          stock: parseStock(msg.stock)
        };
        const writerCount = sendToWriters(pendingWrite);
        ws.send(JSON.stringify({ type: 'PENDING_WRITE_ACK', ok: writerCount > 0, writerCount }));
        if (writerCount === 0) {
          console.warn('No Writer ESP32 connected for PENDING_WRITE');
        }
        break;
      }

      // Writer ESP32: scanned a tag so the dashboard can fill the UID.
      case 'TAG_SCANNED_FOR_WRITE': {
        setClientInfo(ws, { role: 'writer', deviceId: ws.deviceId });
        broadcast({ type: 'TAG_SCANNED_FOR_WRITE', uid: msg.uid });
        break;
      }

      // ── Cart ESP32: scanned a tag ────────────────────────────────────────
      case 'SCAN': {
        const { cartId, uid } = msg;
        setClientInfo(ws, { role: 'cart', deviceId: cartId || ws.deviceId, cartId: cartId || '' });
        const product = products[uid];
        if (!product) {
          broadcast({ type: 'CART_SCAN_ERROR', cartId, uid, error: 'Product not found' });
          ws.send(JSON.stringify({ type: 'SCAN_ERROR', uid, error: 'Product not found' }));
          return;
        }
        product.stock = parseStock(product.stock);
        if (product.stock <= 0) {
          broadcast({ type: 'CART_SCAN_ERROR', cartId, uid, error: 'Out of stock' });
          ws.send(JSON.stringify({ type: 'SCAN_ERROR', uid, error: 'Out of stock' }));
          return;
        }
        product.stock -= 1;
        if (!carts[cartId]) carts[cartId] = { cartId, items: [], total: 0 };
        const cart = carts[cartId];
        const existing = cart.items.find(i => i.uid === uid);
        if (existing) {
          existing.qty += 1;
        } else {
          cart.items.push({ uid, name: product.name, price: product.price, category: product.category, qty: 1 });
        }
        cart.total = cart.items.reduce((s, i) => s + i.price * i.qty, 0);
        saveData();
        broadcast({ type: 'PRODUCT_UPDATE', product, reason: 'stock' });
        broadcast({ type: 'CART_UPDATE', cart });
        ws.send(JSON.stringify({ type: 'SCAN_OK', product, cart }));
        break;
      }

      // ── Cart ESP32: remove item ──────────────────────────────────────────
      case 'REMOVE': {
        const { cartId, uid } = msg;
        setClientInfo(ws, { role: 'cart', deviceId: cartId || ws.deviceId, cartId: cartId || '' });
        if (!carts[cartId]) return;
        const cart = carts[cartId];
        const existing = cart.items.find(i => i.uid === uid);
        if (!existing) {
          ws.send(JSON.stringify({ type: 'REMOVE_ERROR', uid, error: 'Item not in cart' }));
          return;
        }
        existing.qty -= 1;
        if (existing.qty <= 0) {
          cart.items = cart.items.filter(i => i.uid !== uid);
        }
        const product = products[uid];
        if (product) {
          product.stock = parseStock(product.stock) + 1;
        }
        cart.total = cart.items.reduce((s, i) => s + i.price * i.qty, 0);
        saveData();
        if (product) broadcast({ type: 'PRODUCT_UPDATE', product, reason: 'stock' });
        broadcast({ type: 'CART_UPDATE', cart });
        ws.send(JSON.stringify({ type: 'REMOVE_OK', uid, cart }));
        break;
      }

      // ── Cart ESP32: checkout ─────────────────────────────────────────────
      case 'CHECKOUT': {
        const { cartId } = msg;
        setClientInfo(ws, { role: 'cart', deviceId: cartId || ws.deviceId, cartId: cartId || '' });
        if (!carts[cartId]) return;
        const cart = carts[cartId];
        broadcast({ type: 'CHECKOUT', cart });
        ws.send(JSON.stringify({ type: 'CHECKOUT_OK', cart }));
        // Clear cart after checkout
        setTimeout(() => {
          carts[cartId] = { cartId, items: [], total: 0 };
          saveData();
          broadcast({ type: 'CART_UPDATE', cart: carts[cartId] });
        }, 5000);
        break;
      }

      // ── Writer ESP32: register / update a product ────────────────────────
      case 'WRITE_PRODUCT': {
        const { uid, name, price, category } = msg;
        setClientInfo(ws, { role: 'writer', deviceId: ws.deviceId });
        const stockValue = msg.stock !== undefined
          ? msg.stock
          : (ws.pendingWrite?.stock ?? products[uid]?.stock ?? 0);
        products[uid] = {
          uid,
          name,
          price: parseFloat(price),
          category: category || 'General',
          stock: parseStock(stockValue)
        };
        ws.pendingWrite = null;
        saveData();
        broadcast({ type: 'PRODUCT_UPDATE', product: products[uid], reason: 'write' });
        ws.send(JSON.stringify({ type: 'WRITE_OK', product: products[uid] }));
        console.log('Product saved:', products[uid]);
        break;
      }

      // ── Web dashboard: delete product ────────────────────────────────────
      case 'DELETE_PRODUCT': {
        const { uid } = msg;
        setClientInfo(ws, { role: 'dashboard', deviceId: ws.deviceId });
        delete products[uid];
        saveData();
        broadcast({ type: 'PRODUCT_DELETED', uid });
        break;
      }
    }
  });

  ws.on('close', () => {
    clients = clients.filter(c => c !== ws);
    console.log('WS client disconnected. Total:', clients.length);
    broadcastConnections();
  });
});

// ─── REST fallbacks (optional) ─────────────────────────────────────────────
app.get('/api/products', (_, res) => res.json(products));
app.get('/api/carts',    (_, res) => res.json(carts));
app.get('/api/connections', (_, res) => res.json(getConnections()));

// ─── Start ─────────────────────────────────────────────────────────────────
const PORT = Number(process.env.PORT) || 3000;
server.listen(PORT, '0.0.0.0', () => {
  const addresses = getLocalAddresses();
  console.log(`\n✅ Smart Cart Server running`);
  console.log(`   Website  → http://localhost:${PORT}`);
  addresses.forEach(ip => {
    console.log(`   Network  → http://${ip}:${PORT}`);
    console.log(`   ESP32 WS → ws://${ip}:${PORT}`);
  });
  console.log('');
});
