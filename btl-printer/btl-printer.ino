// BTL Print Bridge
// Turns an ESP32-S3 into a Wi-Fi print server for a USB label printer.
//
//  * Network printing: computers add it as a TCP/IP printer on port 9100 and use the
//    printer's normal driver. The bridge passes the driver's bytes straight to the
//    printer over USB, so it works with most USB printers (ZPL, TSPL, EPL, raster...).
//  * Print queue (optional): polls an HTTPS URL for ready-to-print jobs (for example
//    shipping labels in ZPL) and prints them automatically.
//  * Saved fliers: save any print job to the board, then a physical button (or the page)
//    prints one copy, no computer needed.
//  * Setup page at http://btl-printer-XXXX.local or the IP shown on the serial monitor.
//
// Board: ESP32-S3 with native USB (e.g. ESP32-S3-DevKitC-1 N16R8).
// Printer plugs into the board's native "USB" port; flash and power through the "UART" port.
// MIT License - see LICENSE.

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <FFat.h>
#include <Update.h>
#include "usb/usb_host.h"
#include "config.h"
#include "improv_serial.h"       // Wi-Fi setup over USB from the web installer (Improv protocol)

#define FIRMWARE_VERSION "1.8.2"
ImprovSerial improv(Serial);
extern String cfgSsid, cfgPass;
void saveSettings();
void logEvent(const String &s);
bool restartSoon = false;
uint32_t restartAt = 0;

// Called by the web installer with the Wi-Fi name and password the user typed.
bool improvConnect(const char *ssid, const char *password) {
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(ssid, password);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(250);
  if (WiFi.status() != WL_CONNECTED) return false;
  cfgSsid = ssid; cfgPass = password;
  saveSettings();
  logEvent("Wi-Fi set over USB: " + cfgSsid + " " + WiFi.localIP().toString());
  restartSoon = true; restartAt = millis() + 4000;  // restart into normal mode after the installer shows "connected"
  return true;
}

// ---------------------------------------------------------------- settings
Preferences prefs;
String cfgSsid, cfgPass, cfgQueueUrl, cfgQueueToken;

void loadSettings() {
  prefs.begin("bridge", true);
  cfgSsid = prefs.getString("ssid", "");
  cfgPass = prefs.getString("pass", "");
  cfgQueueUrl = prefs.getString("qurl", "");
  cfgQueueToken = prefs.getString("qtok", "");
  prefs.end();
}

void saveSettings() {
  prefs.begin("bridge", false);
  prefs.putString("ssid", cfgSsid);
  prefs.putString("pass", cfgPass);
  prefs.putString("qurl", cfgQueueUrl);
  prefs.putString("qtok", cfgQueueToken);
  prefs.end();
}

// ---------------------------------------------------------------- status
volatile bool printerReady = false;
String printerName = "none";
String printerModel = "";        // the printer's own name from USB, e.g. "PM-241"
uint16_t usbVid = 0, usbPid = 0;  // of the connected printer, used to pick the Windows driver
uint32_t jobsPrinted = 0;
String lastEvent = "Starting";
String hostName;  // e.g. "btl-printer-3fa2": unique per board, so several bridges can share one network


void logEvent(const String &s) {
  lastEvent = s;
  Serial.println(s);
}

// ---------------------------------------------------------------- USB host (printer)
// Minimal driver: find the first bulk OUT endpoint (prefer interface class 7 = printer),
// claim it, and send bytes to it. The printer's own language is never interpreted.
static usb_host_client_handle_t clientHdl = nullptr;
static usb_device_handle_t devHdl = nullptr;
static uint8_t outEp = 0, claimedIntf = 0xFF;
static volatile uint8_t pendingAddr = 0;
static volatile bool devGone = false;
static SemaphoreHandle_t xferDone;
static SemaphoreHandle_t usbLock;
static usb_transfer_t *xfer = nullptr;
static volatile bool xferOk = false;

static void clientEventCb(const usb_host_client_event_msg_t *msg, void *arg) {
  if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) pendingAddr = msg->new_dev.address;
  else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) devGone = true;
}

static void xferCb(usb_transfer_t *t) {
  xferOk = (t->status == USB_TRANSFER_STATUS_COMPLETED);
  xSemaphoreGive(xferDone);
}

static void closePrinter() {
  printerReady = false;
  if (devHdl) {
    if (claimedIntf != 0xFF) usb_host_interface_release(clientHdl, devHdl, claimedIntf);
    usb_host_device_close(clientHdl, devHdl);
  }
  devHdl = nullptr; claimedIntf = 0xFF; outEp = 0; printerName = "none"; printerModel = "";
}

// USB string descriptors are UTF-16; keep the plain ASCII characters.
static String usbStr(const usb_str_desc_t *d) {
  String o; if (!d) return o;
  int n = (d->bLength - 2) / 2;
  for (int i = 0; i < n; i++) { uint16_t c = d->wData[i]; if (c >= 32 && c < 127) o += (char)c; }
  o.trim(); return o;
}

static void openPrinter(uint8_t addr) {
  if (usb_host_device_open(clientHdl, addr, &devHdl) != ESP_OK) { logEvent("USB: couldn't open device"); return; }
  const usb_config_desc_t *cfg;
  if (usb_host_get_active_config_descriptor(devHdl, &cfg) != ESP_OK) { closePrinter(); return; }

  // Walk the descriptors by hand: interface (type 4) then its endpoints (type 5).
  const uint8_t *p = (const uint8_t *)cfg, *end = p + cfg->wTotalLength;
  int curIntf = -1, curAlt = 0, curClass = -1;
  int bestIntf = -1, bestAlt = 0, bestEp = 0; bool bestIsPrinter = false;
  while (p < end && p[0] >= 2) {
    if (p[1] == 0x04) { curIntf = p[2]; curAlt = p[3]; curClass = p[5]; }
    else if (p[1] == 0x05 && curIntf >= 0) {
      uint8_t addrEp = p[2], attr = p[3];
      bool bulkOut = ((attr & 0x03) == 0x02) && !(addrEp & 0x80);
      bool isPrinter = (curClass == 0x07);
      if (bulkOut && (bestIntf < 0 || (isPrinter && !bestIsPrinter))) {
        bestIntf = curIntf; bestAlt = curAlt; bestEp = addrEp; bestIsPrinter = isPrinter;
      }
    }
    p += p[0];
  }
  if (bestIntf < 0) { logEvent("USB: device has no bulk OUT endpoint (not a printer?)"); closePrinter(); return; }
  if (usb_host_interface_claim(clientHdl, devHdl, bestIntf, bestAlt) != ESP_OK) { logEvent("USB: couldn't claim interface"); closePrinter(); return; }
  claimedIntf = bestIntf; outEp = bestEp;

  const usb_device_desc_t *dd;
  usb_host_get_device_descriptor(devHdl, &dd);
  usb_device_info_t info;
  printerModel = "";
  if (usb_host_device_info(devHdl, &info) == ESP_OK) {
    printerModel = usbStr(info.str_desc_product);
    if (printerModel.isEmpty()) printerModel = usbStr(info.str_desc_manufacturer);
  }
  if (printerModel.length() > 40) printerModel = printerModel.substring(0, 40);
  char buf[48];
  snprintf(buf, sizeof buf, "USB %04X:%04X", dd->idVendor, dd->idProduct);
  printerName = printerModel.length() ? printerModel + " (" + buf + ")" : String(buf) + (bestIsPrinter ? " (printer)" : "");
  usbVid = dd->idVendor; usbPid = dd->idProduct;
  printerReady = true;
  logEvent("Printer connected: " + printerName);
}

static void usbHostTask(void *) {
  usb_host_config_t hc = {};
  hc.skip_phy_setup = false;
  hc.intr_flags = ESP_INTR_FLAG_LEVEL1;
  ESP_ERROR_CHECK(usb_host_install(&hc));
  while (true) {
    uint32_t flags;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
  }
}

static void usbClientTask(void *) {
  usb_host_client_config_t cc = {};
  cc.is_synchronous = false;
  cc.max_num_event_msg = 5;
  cc.async.client_event_callback = clientEventCb;
  cc.async.callback_arg = nullptr;
  while (usb_host_client_register(&cc, &clientHdl) != ESP_OK) vTaskDelay(pdMS_TO_TICKS(100));
  usb_host_transfer_alloc(USB_CHUNK_BYTES, 0, &xfer);
  while (true) {
    usb_host_client_handle_events(clientHdl, pdMS_TO_TICKS(20));
    if (pendingAddr) {
      uint8_t a = pendingAddr; pendingAddr = 0;
      xSemaphoreTake(usbLock, portMAX_DELAY);
      if (!devHdl) openPrinter(a);
      xSemaphoreGive(usbLock);
    }
    if (devGone) {
      devGone = false;
      xSemaphoreTake(usbLock, portMAX_DELAY);
      closePrinter();
      xSemaphoreGive(usbLock);
      logEvent("Printer unplugged");
    }
  }
}

// Send bytes to the printer, chunked. Returns false if the printer isn't there or a transfer fails.
// ---------------------------------------------------------------- status light
const uint8_t ledPins[] = LED_PINS;
void ledSet(uint8_t r, uint8_t g, uint8_t b) {
  static uint32_t cur = 0xFFFFFFFF;
  uint32_t want = (r << 16) | (g << 8) | b;
  if (want == cur) return;
  cur = want;
  for (uint8_t pin : ledPins) rgbLedWrite(pin, r, g, b);
}

bool printerWrite(const uint8_t *data, size_t len) {
  if (!printerReady) return false;
  ledSet(0, 0, LED_BRIGHTNESS);  // blue while printing
  xSemaphoreTake(usbLock, portMAX_DELAY);
  bool ok = true;
  while (len && ok && printerReady) {
    size_t n = len > USB_CHUNK_BYTES ? USB_CHUNK_BYTES : len;
    memcpy(xfer->data_buffer, data, n);
    xfer->num_bytes = n;
    xfer->device_handle = devHdl;
    xfer->bEndpointAddress = outEp;
    xfer->callback = xferCb;
    xfer->context = nullptr;
    xfer->timeout_ms = 10000;
    if (usb_host_transfer_submit(xfer) != ESP_OK) { ok = false; break; }
    if (xSemaphoreTake(xferDone, pdMS_TO_TICKS(15000)) != pdTRUE) { ok = false; break; }
    ok = xferOk;
    data += n; len -= n;
  }
  xSemaphoreGive(usbLock);
  return ok;
}

// ---------------------------------------------------------------- raw printing (port 9100)
WiFiServer rawServer(RAW_PRINT_PORT);
static uint8_t netBuf[USB_CHUNK_BYTES];

// ---------------------------------------------------------------- saved fliers
// A flier is a print job saved byte-for-byte in flash (/flierN.bin). Because it's exactly what
// the printer's own driver produced, it reprints correctly on that printer.
bool fsReady = false;
int captureSlot = 0;          // 1..FLIER_COUNT while "save the next print" is armed
uint32_t captureUntil = 0;
String flierName[FLIER_COUNT + 1];
const uint8_t flierPins[] = FLIER_BUTTON_PINS;
static_assert(sizeof(flierPins) == FLIER_COUNT, "FLIER_BUTTON_PINS needs one pin per flier (FLIER_COUNT)");

String flierPath(int n) { return "/flier" + String(n) + ".bin"; }
size_t flierSize(int n) {
  if (!fsReady || !FFat.exists(flierPath(n))) return 0;
  File f = FFat.open(flierPath(n)); size_t z = f.size(); f.close(); return z;
}
String flierPinList() { String s; for (int i = 0; i < FLIER_COUNT; i++) s += (i ? ", " : "") + String(flierPins[i]); return s; }
String flierLabel(int n) { return flierName[n].length() ? flierName[n] : "Flier " + String(n); }

// Printing runs one copy per loop pass, so the page stays responsive and a button press can stop it.
int jobFlier = 0, jobLeft = 0, jobDone = 0;
int flierBatch[FLIER_COUNT + 1];   // copies printed by holding the button

void startFlier(int n, int copies) {
  if (n < 1 || n > FLIER_COUNT) return;
  if (!flierSize(n)) { logEvent(flierLabel(n) + " is empty: save a print to it first"); return; }
  if (!printerReady) { logEvent(flierLabel(n) + " FAILED (printer not ready)"); return; }
  jobFlier = n; jobLeft = constrain(copies, 1, 999); jobDone = 0;
}

void stopFlier() {
  if (jobLeft) logEvent("Stopped " + flierLabel(jobFlier) + " after " + String(jobDone) + " copies");
  jobLeft = 0;
}

void serviceFlierJob() {
  if (!jobLeft) return;
  File f = FFat.open(flierPath(jobFlier));
  bool ok = (bool)f;
  while (ok) {
    int r = f.read(netBuf, sizeof netBuf);
    if (r <= 0) break;
    ok = printerWrite(netBuf, r);
  }
  if (f) f.close();
  if (!ok) { logEvent(flierLabel(jobFlier) + " FAILED (printer not ready)"); jobLeft = 0; return; }
  jobsPrinted++; jobDone++; jobLeft--;
  if (!jobLeft) logEvent("Printed " + flierLabel(jobFlier) + (jobDone > 1 ? " x " + String(jobDone) : String("")));
}

void serviceButtons() {
  static uint32_t downAt[FLIER_COUNT + 1] = {};
  static bool held[FLIER_COUNT + 1] = {};
  static uint32_t lastAction = 0;
  // Wired buttons: GPIO pins, one per flier. Press = 1 copy, hold 2 s = batch.
  for (int i = 1; i <= FLIER_COUNT; i++) {
    bool down = digitalRead(flierPins[i - 1]) == LOW;
    if (down && !downAt[i]) {                       // just pressed
      downAt[i] = millis(); held[i] = false;
      if (jobLeft && millis() - lastAction > 500) { stopFlier(); held[i] = true; lastAction = millis(); }  // any press stops a batch
    } else if (down && !held[i] && millis() - downAt[i] > 2000) {   // held 2 s: batch
      held[i] = true; lastAction = millis(); startFlier(i, flierBatch[i]);
    } else if (!down && downAt[i]) {                // released
      if (!held[i] && millis() - downAt[i] > 30 && millis() - lastAction > 500) { lastAction = millis(); startFlier(i, 1); }
      downAt[i] = 0;
    }
  }
  // The board's BOOT button (GPIO 0) counts clicks: 1 to FLIER_COUNT quick clicks = that flier
  // (more clicks than that count as the last flier).
  // Holding on the last click prints that flier's batch amount. Any press while printing stops.
  static uint32_t bDown = 0, bLastUp = 0;
  static int bClicks = 0;
  static bool bHeld = false;
  bool d = digitalRead(0) == LOW;
  if (d && !bDown) {
    bDown = millis(); bHeld = false;
    if (jobLeft) { stopFlier(); bHeld = true; bClicks = 0; lastAction = millis(); }
  } else if (d && !bHeld && millis() - bDown > 2000) {
    bHeld = true; lastAction = millis();
    startFlier(min(bClicks + 1, FLIER_COUNT), flierBatch[min(bClicks + 1, FLIER_COUNT)]); bClicks = 0;
  } else if (!d && bDown) {
    if (!bHeld && millis() - bDown > 30) { bClicks++; bLastUp = millis(); }
    bDown = 0;
  }
  if (!d && bClicks && millis() - bLastUp > 600) {   // clicking finished
    lastAction = millis(); startFlier(min(bClicks, FLIER_COUNT), 1); bClicks = 0;
  }
}

extern bool setupMode;
void updateLed() {
  const uint8_t L = LED_BRIGHTNESS;
  if (setupMode) ledSet(L / 2, 0, L);                              // purple: setup mode
  else if (!printerReady) ledSet(L, 0, 0);                         // red: no printer
  else if (jobLeft) ledSet(0, 0, L);                               // blue: printing
  else if (captureSlot && millis() < captureUntil) ledSet(L, L * 2 / 3, 0);  // yellow: waiting to save a flier
  else ledSet(0, L * 2 / 3, 0);                                    // green: ready
}

void serviceRawPrinting() {
  WiFiClient c = rawServer.accept();
  if (!c) return;
  if (captureSlot && millis() > captureUntil) { captureSlot = 0; logEvent("Stopped waiting to save a flier"); }
  int slot = captureSlot;
  File out;
  if (slot) out = FFat.open("/capture.tmp", FILE_WRITE);
  size_t total = 0; bool ok = true;
  uint32_t lastData = millis();
  while (c.connected() || c.available()) {
    int n = c.read(netBuf, sizeof netBuf);
    if (n > 0) {
      lastData = millis();
      if (slot) { if (ok && out.write(netBuf, n) != (size_t)n) ok = false; }
      else if (ok && !printerWrite(netBuf, n)) ok = false;
      total += n;
    } else {
      if (millis() - lastData > 15000) break;  // sender went quiet
      delay(2);
    }
  }
  c.stop();
  if (slot) {
    out.close();
    if (!total) return;  // just a connection check, keep waiting
    captureSlot = 0;
    if (ok) {
      FFat.remove(flierPath(slot));
      FFat.rename("/capture.tmp", flierPath(slot));
      logEvent("Saved " + flierLabel(slot) + " (" + String(total / 1024 + 1) + " KB). Press its button to print.");
    } else {
      FFat.remove("/capture.tmp");
      logEvent("Couldn't save " + flierLabel(slot) + ": storage full?");
    }
    return;
  }
  if (total) {
    if (ok) jobsPrinted++;
    logEvent(String(ok ? "Printed network job, " : "Network job FAILED (printer not ready), ") + total + " bytes");
  }
}

// ---------------------------------------------------------------- print queue (HTTPS)
// GET  <QUEUE_URL>/next  -> 204 when empty, or 200 with raw printer bytes and header X-Job-Id
// POST <QUEUE_URL>/ack?id=<id>  after a job prints
uint32_t lastPoll = 0;

void pollQueue() {
  if (cfgQueueUrl.isEmpty() || !printerReady || WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastPoll < QUEUE_POLL_SECONDS * 1000UL) return;
  lastPoll = millis();

  WiFiClientSecure tls;
  tls.setInsecure();  // jobs are authenticated by the token; see README for pinning a CA
  HTTPClient http;
  const char *hdrs[] = {"X-Job-Id"};
  if (!http.begin(tls, cfgQueueUrl + "/next")) return;
  http.collectHeaders(hdrs, 1);
  http.addHeader("Authorization", "Bearer " + cfgQueueToken);
  int code = http.GET();
  if (code != 200) { http.end(); if (code != 204 && code > 0) logEvent("Queue: HTTP " + String(code)); return; }

  String id = http.header("X-Job-Id");
  int size = http.getSize();
  if (size <= 0 || size > MAX_JOB_BYTES) { http.end(); logEvent("Queue: job size not supported"); return; }
  uint8_t *job = (uint8_t *)ps_malloc(size);
  if (!job) { http.end(); logEvent("Queue: out of memory"); return; }
  WiFiClient *s = http.getStreamPtr();
  int got = 0; uint32_t t0 = millis();
  while (got < size && millis() - t0 < 20000) {
    int n = s->read(job + got, size - got);
    if (n > 0) got += n; else delay(5);
  }
  http.end();
  bool ok = (got == size) && printerWrite(job, size);
  free(job);
  if (!ok) { logEvent("Queue: job " + id + " FAILED, will retry"); return; }
  jobsPrinted++;
  logEvent("Printed queued job " + id);

  HTTPClient ack;
  WiFiClientSecure tls2; tls2.setInsecure();
  if (ack.begin(tls2, cfgQueueUrl + "/ack?id=" + id)) {
    ack.addHeader("Authorization", "Bearer " + cfgQueueToken);
    ack.POST("");
    ack.end();
  }
}

// ---------------------------------------------------------------- web page
WebServer web(80);
DNSServer dns;
bool setupMode = false;

String htmlEsc(const String &s) {
  String o; for (char ch : s) { if (ch == '<') o += "&lt;"; else if (ch == '>') o += "&gt;"; else if (ch == '"') o += "&quot;"; else if (ch == '&') o += "&amp;"; else o += ch; }
  return o;
}

const char PAGE_HEAD[] PROGMEM = R"HTML(<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>BTL Print Bridge</title><style>body{font:16px system-ui,sans-serif;max-width:560px;margin:24px auto;padding:0 16px;color:#16181d;background:#f3f2ef}
h1{font-size:24px}.card{background:#fff;border:1px solid #ddd;border-radius:10px;padding:16px;margin:12px 0}label{display:block;margin:10px 0 4px;font-weight:600}
input{width:100%;padding:10px;border:1px solid #ccc;border-radius:8px;box-sizing:border-box;font:inherit}button{margin-top:14px;padding:12px 18px;border:0;border-radius:8px;background:#d9471f;color:#fff;font:600 16px system-ui;cursor:pointer}button[disabled]{opacity:.4;cursor:default}
.ok{color:#137333;font-weight:600}.bad{color:#b3261e;font-weight:600}small{color:#666}code{background:#eee;padding:2px 5px;border-radius:4px}</style><h1>BTL Print Bridge</h1>)HTML";

// "Print text" card: the browser draws the text, turns it into a black-and-white image and
// sends it in the printer's language (ZPL or TSPL) to /raw, which streams it to the printer.
const char TEXT_TOOL[] PROGMEM = R"HTML(<div class=card><b>Print text</b><br><small>Type a few words; they're sized to fill the label.</small>
<textarea id=tt rows=3 style="width:100%;box-sizing:border-box;margin-top:8px;padding:10px;font:inherit;border:1px solid #ccc;border-radius:8px">FRAGILE</textarea>
<div style="display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-top:8px">
<span>Label</span><input id=tw type=number step=0.1 min=0.5 value=4 style=width:70px><span>&times;</span><input id=th type=number step=0.1 min=0.5 value=6 style=width:70px><span>in</span>
<select id=to style="padding:9px;border-radius:8px"><option value=1>Sideways</option><option value=0>Upright</option></select>
<label style="display:inline;margin:0;font-weight:400"><input id=tb type=checkbox checked style=width:auto> Bold</label>
<select id=tl style="padding:9px;border-radius:8px" title="Printer language"><option>ZPL</option><option>TSPL</option></select></div>
<canvas id=tc style="width:100%;margin-top:10px;border:1px solid #ccc;border-radius:6px;background:#fff;image-rendering:pixelated"></canvas>
<div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap"><span>Copies</span><input id=tn type=number min=1 max=999 value=1 style=width:80px>
<button id=tp type=button style="margin:0">Print text</button><small id=tm></small></div>
<small>Prints blank or garbled? Switch ZPL/TSPL (use whichever test label worked).</small></div>
<script>(function(){
var $=function(i){return document.getElementById(i)},K=['tw','th','to','tl','tb','tt'];
function st(k,v){try{if(v===undefined)return localStorage.getItem('btl_'+k);localStorage.setItem('btl_'+k,v)}catch(e){return null}}
K.forEach(function(k){var v=st(k);if(v!==null){if(k=='tb')$(k).checked=v=='1';else $(k).value=v}});
function draw(){var dpi=203,land=$('to').value=='1',W=Math.ceil(Math.round((parseFloat($('tw').value)||4)*dpi)/8)*8,H=Math.round((parseFloat($('th').value)||6)*dpi);
var c=$('tc');c.width=W;c.height=H;var x=c.getContext('2d');x.fillStyle='#fff';x.fillRect(0,0,W,H);x.fillStyle='#000';
var L=$('tt').value.split('\n'),aw=land?H:W,ah=land?W:H,m=Math.round(dpi*0.12),wt=$('tb').checked?'700 ':'400 ';
x.font=wt+'100px Arial,Helvetica,sans-serif';var wd=1;L.forEach(function(l){wd=Math.max(wd,x.measureText(l).width)});
var s=Math.max(8,Math.floor(Math.min((ah-2*m)/(L.length*1.15),(aw-2*m)*100/wd)));
x.save();if(land){x.translate(W,0);x.rotate(Math.PI/2)}x.font=wt+s+'px Arial,Helvetica,sans-serif';x.textAlign='center';x.textBaseline='middle';
var lh=s*1.15,top=ah/2-lh*(L.length-1)/2;L.forEach(function(l,i){x.fillText(l,aw/2,top+i*lh)});x.restore();return{x:x,W:W,H:H}}
function save(){K.forEach(function(k){st(k,k=='tb'?($(k).checked?'1':'0'):$(k).value)});draw()}
K.forEach(function(k){$(k).addEventListener('input',save)});$('tb').addEventListener('change',save);draw();
$('tp').onclick=function(){var r=draw(),W=r.W,H=r.H,bpr=W/8,d=r.x.getImageData(0,0,W,H).data,b=new Uint8Array(bpr*H),n=Math.max(1,parseInt($('tn').value)||1),job;
for(var y=0;y<H;y++)for(var i=0;i<W;i++)if(d[(y*W+i)*4]<128)b[y*bpr+(i>>3)]|=0x80>>(i&7);
if($('tl').value=='ZPL'){var h='';for(var j=0;j<b.length;j++)h+=(b[j]<16?'0':'')+b[j].toString(16);
job=['^XA^PW'+W+'^LL'+H+'^FO0,0^GFA,'+b.length+','+b.length+','+bpr+','+h.toUpperCase()+'^FS^PQ'+n+'^XZ']}
else{for(var j=0;j<b.length;j++)b[j]^=0xFF;var mm=function(v){return(v*25.4).toFixed(1)};
job=['SIZE '+mm(parseFloat($('tw').value)||4)+' mm,'+mm(parseFloat($('th').value)||6)+' mm\r\nGAP 3 mm,0 mm\r\nCLS\r\nBITMAP 0,0,'+bpr+','+H+',0,',b,'\r\nPRINT 1,'+n+'\r\n']}
var f=new FormData();f.append('f',new Blob(job),'label.prn');$('tm').textContent='Sending...';
fetch('/raw',{method:'POST',body:f}).then(function(r){return r.text()}).then(function(t){$('tm').textContent=t}).catch(function(){$('tm').textContent='Could not reach the bridge.'})};
})();</script>)HTML";

// "Firmware" card: the browser checks GitHub for a newer version, downloads it over HTTPS and
// uploads it to /update. A .bin file can also be uploaded by hand.
const char FW_TOOL[] PROGMEM = R"HTML(<div class=card><b>Firmware</b> <small>version %VER%</small><br><span id=uv><small>Checking for updates&hellip;</small></span>
<details style=margin-top:8px><summary style=cursor:pointer><small>Update from a file</small></summary><input id=uf type=file accept=.bin style=margin-top:8px><button id=ub type=button>Install file</button></details></div>
<script>(function(){
var $=function(i){return document.getElementById(i)},CUR='%VER%',BASE='https://btltech1.github.io/esp32-label-printer-bridge/';
function newer(a,b){a=a.split('.').map(Number);b=b.split('.').map(Number);for(var i=0;i<3;i++){if((a[i]||0)!=(b[i]||0))return(a[i]||0)>(b[i]||0)}return false}
function send(blob,msg){msg.textContent='Installing... (about 30 seconds, keep this page open)';var f=new FormData();f.append('f',blob,'update.bin');
fetch('/update',{method:'POST',body:f}).then(function(r){return r.text().then(function(t){if(!r.ok)throw t;msg.textContent='Updated! Restarting... this page reloads in 20 seconds.';setTimeout(function(){location.reload()},20000)})})
.catch(function(e){msg.textContent='Update failed: '+e})}
fetch(BASE+'version.json',{cache:'no-store'}).then(function(r){return r.json()}).then(function(j){
if(!newer(j.version,CUR)){$('uv').innerHTML='<small class=ok>Up to date.</small>';return}
$('uv').innerHTML='<b>Version '+j.version+' is available.</b> <button id=ug type=button>Update now</button> <small id=um></small>';
$('ug').onclick=function(){var m=$('um');this.disabled=true;m.textContent='Downloading...';
fetch(BASE+'firmware/btl-print-bridge-update.bin',{cache:'no-store'}).then(function(r){if(!r.ok)throw 'download failed';return r.blob()}).then(function(b){send(b,m)}).catch(function(e){m.textContent='Update failed: '+e})}
}).catch(function(){$('uv').innerHTML="<small>Couldn't check for updates (no internet?).</small>"});
$('ub').onclick=function(){var f=$('uf').files[0];if(f)send(f,$('uv'))};
})();</script>)HTML";

// Windows setup script, served at /windows.ps1 with this bridge's IP filled in.
// Run from an Administrator PowerShell:  irm http://<bridge-ip>/windows.ps1 | iex
const char WIN_SCRIPT[] PROGMEM = R"PS(
$ip = '%IP%'
Write-Host ''
Write-Host 'BTL Print Bridge: add your label printer over Wi-Fi' -ForegroundColor Cyan
$me = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  Write-Host 'This needs an Administrator window: right-click Start > Terminal (Admin), then paste the line again.' -ForegroundColor Yellow; return }
$skip  = 'Microsoft|OneNote|Fax|XPS|PDF|Send To|Remote Desktop|Generic / Text'
$label = 'ZPL|TSPL|EPL|Label|Thermal|Rollo|Zebra|RONGTA|Xprinter|MUNBYN|iDPRT|Phomemo|HPRT|POLONO|Jadens|Nelko|Beeprt|4BARCODE|TSC|Godex|DYMO|Brother QL|Arkscan|Zjiang'
$all = @(Get-PrinterDriver | Where-Object { $_.Name -notmatch $skip } | Select-Object -ExpandProperty Name -Unique)
$drivers = @($all | Where-Object { $_ -match $label }) + @($all | Where-Object { $_ -notmatch $label })
if ($drivers.Count -eq 0) {
  Write-Host "No printer drivers found. Install your label printer's driver first (plug the printer into this PC by USB once, or run the maker's installer), then run this again." -ForegroundColor Yellow; return }
Write-Host ''
Write-Host 'Which driver is for your label printer? (label printer drivers are listed first)'
for ($i = 0; $i -lt $drivers.Count; $i++) { Write-Host ('  [{0}] {1}' -f ($i + 1), $drivers[$i]) }
# The bridge knows the printer's USB ID; find the installed driver made for it.
$hwid = '%HWID%'
$best = 0
if ($hwid) {
  for ($i = 0; $i -lt $drivers.Count; $i++) {
    $inf = (Get-PrinterDriver -Name $drivers[$i] -ErrorAction SilentlyContinue | Select-Object -First 1).InfPath
    if ($inf -and (Test-Path $inf) -and (Select-String -Path $inf -Pattern $hwid -SimpleMatch -Quiet)) { $best = $i + 1; break } } }
if ($best) {
  Write-Host ''
  Write-Host ("Found the driver for your printer: [{0}] {1}" -f $best, $drivers[$best - 1]) -ForegroundColor Green
  $pick = Read-Host 'Press Enter to use it (or type a different number)'
  if ([string]::IsNullOrWhiteSpace($pick)) { $pick = "$best" }
} else {
  $pick = Read-Host 'Type its number and press Enter'
}
$n = 0
if (-not [int]::TryParse("$pick", [ref]$n) -or $n -lt 1 -or $n -gt $drivers.Count) { Write-Host 'That number is not in the list. Paste the line again to retry.' -ForegroundColor Yellow; return }
$driver = $drivers[$n - 1]
# Prefer the bridge's name, which keeps working if the router gives it a new IP address.
$addr = $ip
$nm = '%NAME%.local'
try { $t = New-Object Net.Sockets.TcpClient; if ($t.ConnectAsync($nm, 9100).Wait(4000)) { $addr = $nm }; $t.Close() } catch {}
if ($addr -eq $nm) { Write-Host "Using the bridge's name ($nm), so it keeps working even if its IP address changes." }
else { Write-Host "Couldn't reach $nm by name, using the IP address $ip. Tip: reserve this IP in your router so it never changes." -ForegroundColor Yellow }
# One printer entry per bridge: reuse the entry that already points at this bridge, otherwise pick a free name.
# Named after the printer (e.g. "PM-241 (Wi-Fi)"), or "Label Printer (Wi-Fi)" if it has no name.
$model = '%PRODUCT%'
$base = 'Label Printer'; if ($model) { $base = $model }
$ex = Get-Printer | Where-Object { $_.PortName -eq $ip -or $_.PortName -eq $nm } | Select-Object -First 1
$old = $null; if ($ex) { $old = $ex.Name }
# Suggest a name: keep a name you chose before, but replace an old generic "Label Printer (Wi-Fi)" with the printer's own name.
if ($old -and -not ($old -like 'Label Printer*(Wi-Fi)' -and $model)) { $suggest = $old }
else { $suggest = "$base (Wi-Fi)"; $k = 2; while ($suggest -ne $old -and (Get-Printer -Name $suggest -ErrorAction SilentlyContinue)) { $suggest = "$base $k (Wi-Fi)"; $k++ } }
Write-Host ''
$typed = Read-Host "Name for this printer in Windows (press Enter for '$suggest', or type your own)"
$name = $suggest
if (-not [string]::IsNullOrWhiteSpace($typed)) { $name = ($typed.Trim() -replace '[\\,!]', '') }
if ($name -ne $old -and (Get-Printer -Name $name -ErrorAction SilentlyContinue)) {
  Write-Host "There's already a printer called '$name'. Paste the line again and pick another name." -ForegroundColor Yellow; return }
try {
  if (-not (Get-PrinterPort -Name $addr -ErrorAction SilentlyContinue)) { Add-PrinterPort -Name $addr -PrinterHostAddress $addr -PortNumber 9100 -ErrorAction Stop }
  if ($addr -ne $ip) {
    Get-Printer | Where-Object { $_.PortName -eq $ip -and $_.Name -ne $old } | ForEach-Object {
      Set-Printer -Name $_.Name -PortName $addr; Write-Host ("Also switched '" + $_.Name + "' to $addr") } }
  if ($old) {
    Get-PrintJob -PrinterName $old -ErrorAction SilentlyContinue | Remove-PrintJob -ErrorAction SilentlyContinue
    Remove-Printer -Name $old -ErrorAction Stop }
  Add-Printer -Name $name -DriverName $driver -PortName $addr -ErrorAction Stop
} catch { Write-Host ('Something went wrong: ' + $_.Exception.Message) -ForegroundColor Red; return }
Write-Host ''
Write-Host "Added '$name'." -ForegroundColor Green
Write-Host 'Last step: in the window that opens, set the paper size to your labels (for example 4 x 6 in / 100 x 150 mm) and click OK.'
Start-Process rundll32.exe -ArgumentList "printui.dll,PrintUIEntry /e /n `"$name`"" -Wait
Get-CimInstance Win32_Printer -Filter "Name='$name'" | Invoke-CimMethod -MethodName PrintTestPage | Out-Null
Write-Host "Sent a test page. Done! Choose '$name' when you print from any app." -ForegroundColor Green
# Optional desktop shortcut to this bridge's page (saved fliers, status, updates), then open the page once.
$url = "http://$addr/"
Write-Host ''
$want = Read-Host "Want a desktop shortcut to the bridge's page (saved fliers, updates)? Type Y and press Enter, or just press Enter to skip"
if ($want -match '^\s*y') {
  try {
    $desk = [Environment]::GetFolderPath('Desktop')
    $lnk = Join-Path $desk ((('BTL Print Bridge - ' + $name) -replace '[\\/:*?"<>|]', '') + '.url')
    Set-Content -Path $lnk -Encoding ASCII -Value "[InternetShortcut]`r`nURL=$url`r`nIconFile=C:\Windows\System32\shell32.dll`r`nIconIndex=16`r`n"
    Write-Host ("Made a desktop shortcut: '" + [IO.Path]::GetFileNameWithoutExtension($lnk) + "'.")
  } catch { Write-Host "Couldn't make the shortcut. You can open $url in your browser instead." -ForegroundColor Yellow }
}
Write-Host "Opening the bridge's page: $url"
Start-Process explorer.exe $url   # explorer opens it in your normal (not Administrator) browser
$global:BTLOpened = $true
)PS";

void handleWinScript() {
  String sc = FPSTR(WIN_SCRIPT);
  sc.replace("%IP%", WiFi.localIP().toString());
  sc.replace("%NAME%", hostName);
  char hw[24] = "";
  if (printerReady && usbVid) snprintf(hw, sizeof hw, "VID_%04X&PID_%04X", usbVid, usbPid);
  sc.replace("%HWID%", hw);
  String model; for (char ch : printerModel) if (isalnum((unsigned char)ch) || ch == ' ' || ch == '-' || ch == '_' || ch == '.') model += ch;
  model.trim();
  sc.replace("%PRODUCT%", printerReady ? model : String(""));
  web.send(200, "text/plain", sc);
}

String statusHtml() {
  String h = "<b>Printer:</b> ";
  h += printerReady ? "<span class=ok>connected</span> " + htmlEsc(printerName) : String("<span class=bad>not connected</span>");
  h += "<br><b>Wi-Fi:</b> " + (setupMode ? String("setup mode") : htmlEsc(cfgSsid) + " &middot; " + WiFi.localIP().toString() + " &middot; " + hostName + ".local");
  h += "<br><b>Jobs printed:</b> " + String(jobsPrinted) + "<br><b>Last event:</b> " + htmlEsc(lastEvent);
  if (jobLeft)
    h += "<br><b class=ok>Printing " + htmlEsc(flierLabel(jobFlier)) + ": " + String(jobDone) + " done, " + String(jobLeft) + " to go</b> "
         "<form method=post action=/flier style=display:inline><button name=do value=stop style=\"margin:0 0 0 8px;padding:6px 12px\">Stop</button></form>";
  if (captureSlot && millis() < captureUntil)
    h += "<br><b class=ok>Waiting for your next print to save as " + htmlEsc(flierLabel(captureSlot)) + "&hellip;</b>";
  return h;
}

void handleStatus() { web.send(200, "text/html", statusHtml()); }

void handleRoot() {
  String h = FPSTR(PAGE_HEAD);
  h += "<div class=card id=st>" + statusHtml() + "</div>"
       "<script>setInterval(()=>fetch('/status').then(r=>r.text()).then(t=>document.getElementById('st').innerHTML=t).catch(()=>{}),3000)</script>";
  if (!setupMode) {
    String ip = WiFi.localIP().toString();
    String cmd = "irm http://" + ip + "/windows.ps1 | iex";
    h += "<div class=card><b>Add to a Windows PC</b> <small>(about 1 minute)</small><ol style=\"padding-left:20px;margin:8px 0\">"
         "<li>Right-click the <b>Start</b> button &rarr; <b>Terminal (Admin)</b> and click Yes.</li>"
         "<li>Copy this line, paste it into that window, and press Enter:<br>"
         "<input id=cmd readonly value=\"" + cmd + "\" onclick=\"this.select()\" style=\"font-family:monospace;margin-top:6px\">"
         "<button type=button onclick=\"var c=document.getElementById('cmd');c.select();document.execCommand('copy');this.textContent='Copied!'\">Copy</button></li>"
         "<li>Press Enter to use the driver it found (or pick yours from the list), set your label size, done.</li></ol>"
         "<small>Your printer's driver must be installed first (plug the printer into the PC by USB once, or use the maker's installer).<br>"
         "Mac: System Settings &rarr; Printers &amp; Scanners &rarr; Add Printer &rarr; IP. Address <code>" + ip +
         "</code>, protocol <b>HP Jetdirect &ndash; Socket</b>, then choose your printer's driver.</small></div>";
    h += "<div class=card><b>Test print</b><br><small>Label printers speak different languages. Try ZPL first; if nothing comes out or it prints gibberish, try TSPL. "
         "If neither works, that's OK: add the bridge to your computer (above) and print a test page with your printer's own driver.</small>"
         "<form method=post action=/test style=\"display:inline\"><button>Print ZPL test</button></form> "
         "<form method=post action=/test-tspl style=\"display:inline\"><button>Print TSPL test</button></form></div>";
    h += FPSTR(TEXT_TOOL);
    h += "<div class=card><b>Saved fliers</b><br><small>Save any print to a button: click <b>Save next print</b>, then print the flier from your computer to this printer "
         "within " + String(CAPTURE_MINUTES) + " minutes. It's stored instead of printed. "
         "Then <b>press</b> the button for 1 copy, <b>hold it 2 seconds</b> for the batch amount, and press any button to stop. "
         "Buttons: GPIO " + flierPinList() + " to GND (Flier 1 to " + String(FLIER_COUNT) + "). No buttons wired? Use the board's <b>BOOT</b> button: "
         "1 to " + String(FLIER_COUNT) + " quick clicks = Flier 1 to " + String(FLIER_COUNT) + " (hold on the last click for the batch).</small>";
    if (!fsReady) h += "<p class=bad>Storage isn't available on this board.</p>";
    for (int n = 1; n <= FLIER_COUNT; n++) {
      size_t z = flierSize(n);
      String dis = z ? "" : " disabled";
      h += "<form method=post action=/flier style=\"margin-top:14px;border-top:1px solid #ddd;padding-top:10px\"><input type=hidden name=n value=" + String(n) + ">"
           "<b>Button " + String(n) + ":</b> " + (z ? htmlEsc(flierLabel(n)) + " <small>(" + String(z / 1024 + 1) + " KB)</small>" : String("<small>empty</small>")) +
           "<div style=\"display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:6px\">"
           "<input name=copies type=number min=1 max=999 value=1 style=\"width:80px\"" + dis + " aria-label=\"Copies to print now\"> "
           "<button name=do value=print style=margin:0" + dis + ">Print</button></div>"
           "<div style=\"display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:8px\"><small>Hold the button to print</small>"
           "<input name=batch type=number min=1 max=999 value=" + String(flierBatch[n]) + " style=\"width:80px\" aria-label=\"Copies when you hold the button\">"
           "<small>copies</small><button name=do value=rename style=\"margin:0;background:#666\">Save</button></div>"
           "<details style=margin-top:6px><summary style=cursor:pointer><small>Change this button</small></summary>"
           "<label>Name</label><input name=name maxlength=30 placeholder=\"e.g. Menu\" value=\"" + htmlEsc(flierName[n]) + "\">"
           "<button name=do value=rename style=\"background:#666\">Save name</button> "
           "<button name=do value=save style=\"background:#2563c9\">Save next print here</button> "
           "<button name=do value=delete style=\"background:#999\"" + dis + ">Delete flier</button></details></form>";
    }
    h += "</div>";
  }
  if (!setupMode) { String fw = FPSTR(FW_TOOL); fw.replace("%VER%", FIRMWARE_VERSION); h += fw; }
  h += "<div class=card><form method=post action=/save><b>Settings</b>"
       "<label>Wi-Fi name</label><input name=ssid value=\"" + htmlEsc(cfgSsid) + "\" required>"
       "<label>Wi-Fi password</label><input name=pass type=password placeholder=\"(unchanged)\">"
       "<label>Print queue URL <small>(optional)</small></label><input name=qurl value=\"" + htmlEsc(cfgQueueUrl) + "\" placeholder=\"https://.../print\">"
       "<label>Queue token <small>(optional)</small></label><input name=qtok type=password placeholder=\"" + String(cfgQueueToken.length() ? "(unchanged)" : "") + "\">"
       "<button>Save and restart</button></form></div>";
  web.send(200, "text/html", h);
}

void handleSave() {
  cfgSsid = web.arg("ssid");
  if (web.arg("pass").length()) cfgPass = web.arg("pass");
  cfgQueueUrl = web.arg("qurl"); cfgQueueUrl.trim();
  while (cfgQueueUrl.endsWith("/")) cfgQueueUrl.remove(cfgQueueUrl.length() - 1);
  if (web.arg("qtok").length()) cfgQueueToken = web.arg("qtok");
  saveSettings();
  web.send(200, "text/html", String(FPSTR(PAGE_HEAD)) + "<div class=card>Saved. Restarting&hellip; Reconnect to your normal Wi-Fi and open <code>http://" BRIDGE_NAME ".local</code></div>");
  delay(1500);
  ESP.restart();
}

void handleTestTspl() {
  const char *tspl = "SIZE 100 mm,150 mm\r\nGAP 3 mm,0 mm\r\nCLS\r\nBOX 30,30,780,330,4\r\n"
                     "TEXT 60,60,\"4\",0,1,1,\"BTL Print Bridge\"\r\nTEXT 60,140,\"3\",0,1,1,\"Wi-Fi printing works.\"\r\n"
                     "BARCODE 60,200,\"128\",80,1,0,3,3,\"BTL-OK\"\r\nPRINT 1,1\r\n";
  bool ok = printerWrite((const uint8_t *)tspl, strlen(tspl));
  if (ok) jobsPrinted++;
  logEvent(ok ? "Printed TSPL test label" : "TSPL test FAILED (printer not ready)");
  web.sendHeader("Location", "/");
  web.send(303);
}

void handleTest() {
  const char *zpl = "^XA^PW812^FO40,40^GB730,300,4^FS^FO70,80^A0N,56,56^FDBTL Print Bridge^FS"
                    "^FO70,160^A0N,36,36^FDWi-Fi printing works.^FS^FO70,230^BY3^BCN,80,Y,N,N^FDBTL-OK^FS^XZ";
  bool ok = printerWrite((const uint8_t *)zpl, strlen(zpl));
  if (ok) jobsPrinted++;
  logEvent(ok ? "Printed test label" : "Test print FAILED (printer not ready)");
  web.sendHeader("Location", "/");
  web.send(303);
}

void handleFlier() {
  int n = web.arg("n").toInt();
  String act = web.arg("do");
  if (act == "stop") stopFlier();
  else if (n >= 1 && n <= FLIER_COUNT) {
    if (act == "print") startFlier(n, web.arg("copies").toInt());
    else if (act == "save") { captureSlot = n; captureUntil = millis() + CAPTURE_MINUTES * 60000UL;
      logEvent("Print your flier now: the next print will be saved as " + flierLabel(n)); }
    else if (act == "delete") { FFat.remove(flierPath(n)); logEvent("Deleted " + flierLabel(n)); }
    if (act == "rename" || act == "save") {
      String nm = web.arg("name"); nm.trim();
      int b = constrain((int)web.arg("batch").toInt(), 1, 999);
      flierName[n] = nm; flierBatch[n] = b;
      Preferences p; p.begin("btl", false);
      p.putString(("f" + String(n)).c_str(), nm); p.putInt(("b" + String(n)).c_str(), b); p.end();
      if (act == "rename") logEvent("Saved settings for " + flierLabel(n));
    }
  }
  web.sendHeader("Location", "/");
  web.send(303);
}

// /raw: a print job uploaded from the page, streamed straight to the printer.
bool rawOk = false; size_t rawBytes = 0;
void handleRawUpload() {
  HTTPUpload &u = web.upload();
  if (u.status == UPLOAD_FILE_START) { rawOk = printerReady; rawBytes = 0; }
  else if (u.status == UPLOAD_FILE_WRITE) { if (rawOk && !printerWrite(u.buf, u.currentSize)) rawOk = false; rawBytes += u.currentSize; }
}
void handleRawDone() {
  if (rawOk && rawBytes) { jobsPrinted++; logEvent("Printed a label from the page"); web.send(200, "text/plain", "Sent to the printer."); }
  else { logEvent("Label from the page FAILED (printer not ready)"); web.send(200, "text/plain", "Printer not ready."); }
}

// /update: new firmware uploaded from the page (Wi-Fi update).
String updErr; size_t updSeen = 0;
void handleUpdateUpload() {
  HTTPUpload &u = web.upload();
  if (u.status == UPLOAD_FILE_START) {
    updErr = ""; updSeen = 0;
    logEvent("Installing a firmware update...");
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) updErr = Update.errorString();
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (updErr.length()) return;
    if (updSeen == 0) {
      // An app image starts with 0xE9 and has the app-description magic 0xABCD5432 at byte 32.
      // This rejects the install-page file (which starts with the bootloader) and random files.
      bool okImg = u.currentSize >= 36 && u.buf[0] == 0xE9 && u.buf[32] == 0x32 && u.buf[33] == 0x54 && u.buf[34] == 0xCD && u.buf[35] == 0xAB;
      if (!okImg) { updErr = "that isn't a BTL Print Bridge update file (use btl-print-bridge-update.bin)"; Update.abort(); return; }
    }
    if (Update.write(u.buf, u.currentSize) != u.currentSize) updErr = Update.errorString();
    updSeen += u.currentSize;
  } else if (u.status == UPLOAD_FILE_END) {
    if (!updErr.length() && !Update.end(true)) updErr = Update.errorString();
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    Update.abort(); updErr = "upload interrupted";
  }
}
void handleUpdateDone() {
  if (updErr.length()) { logEvent("Update failed: " + updErr); web.send(500, "text/plain", updErr); return; }
  logEvent("Update installed, restarting");
  web.send(200, "text/plain", "ok");
  restartSoon = true; restartAt = millis() + 1500;
}

// ---------------------------------------------------------------- Wi-Fi
void startSetupMode() {
  setupMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(SETUP_AP_NAME);
  dns.start(53, "*", WiFi.softAPIP());  // send every address to the setup page
  logEvent(String("Setup mode: join Wi-Fi '") + SETUP_AP_NAME + "' and open http://" + WiFi.softAPIP().toString());
}

void startWifi() {
  if (cfgSsid.isEmpty()) { startSetupMode(); return; }
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostName.c_str());
  WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(250);
  if (WiFi.status() != WL_CONNECTED) { logEvent("Couldn't join " + cfgSsid); startSetupMode(); return; }
  WiFi.setSleep(false);  // keeps network printing responsive
  MDNS.begin(hostName.c_str());
  MDNS.setInstanceName(hostName.c_str());
  MDNS.addService("http", "tcp", 80);
  MDNS.addService("pdl-datastream", "tcp", RAW_PRINT_PORT);
  rawServer.begin();
  logEvent("Wi-Fi connected: " + WiFi.localIP().toString());
}

// ---------------------------------------------------------------- main
void setup() {
  Serial.begin(115200);
  improv.begin("BTL Print Bridge", FIRMWARE_VERSION, "ESP32-S3", "BTL Print Bridge", improvConnect);
  delay(300);
  xferDone = xSemaphoreCreateBinary();
  usbLock = xSemaphoreCreateMutex();
  loadSettings();
  fsReady = FFat.begin(true);  // formats the storage area the first time
  { Preferences p; p.begin("btl", true); for (int n = 1; n <= FLIER_COUNT; n++) { flierName[n] = p.getString(("f" + String(n)).c_str(), ""); flierBatch[n] = p.getInt(("b" + String(n)).c_str(), 10); } p.end(); }
  pinMode(0, INPUT_PULLUP);
  for (uint8_t pin : flierPins) pinMode(pin, INPUT_PULLUP);
  { uint64_t mac = ESP.getEfuseMac(); char sfx[8]; snprintf(sfx, sizeof sfx, "-%02x%02x", (uint8_t)(mac >> 32), (uint8_t)(mac >> 40)); hostName = String(BRIDGE_NAME) + sfx; }
  xTaskCreatePinnedToCore(usbHostTask, "usbhost", 4096, nullptr, 5, nullptr, 0);
  xTaskCreatePinnedToCore(usbClientTask, "usbclient", 6144, nullptr, 5, nullptr, 0);
  startWifi();
  web.on("/", handleRoot);
  web.on("/status", handleStatus);
  web.on("/windows.ps1", handleWinScript);
  web.on("/save", HTTP_POST, handleSave);
  web.on("/test", HTTP_POST, handleTest);
  web.on("/test-tspl", HTTP_POST, handleTestTspl);
  web.on("/flier", HTTP_POST, handleFlier);
  web.on("/raw", HTTP_POST, handleRawDone, handleRawUpload);
  web.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  web.onNotFound([] { web.sendHeader("Location", "/"); web.send(302); });
  web.begin();
}

void loop() {
  improv.handle();
  if (restartSoon && millis() > restartAt) ESP.restart();
  if (setupMode) dns.processNextRequest();
  web.handleClient();
  updateLed();
  if (!setupMode) {
    serviceRawPrinting();
    serviceButtons();
    serviceFlierJob();
    pollQueue();
    if (WiFi.status() != WL_CONNECTED) { static uint32_t lastTry = 0; if (millis() - lastTry > 30000) { lastTry = millis(); WiFi.reconnect(); } }
  }
  delay(1);
}
