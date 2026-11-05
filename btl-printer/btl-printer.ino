// BTL Print Bridge
// Turns an ESP32-S3 into a Wi-Fi print server for a USB label printer.
//
//  * Network printing: computers add it as a TCP/IP printer on port 9100 and use the
//    printer's normal driver. The bridge passes the driver's bytes straight to the
//    printer over USB, so it works with most USB printers (ZPL, TSPL, EPL, raster...).
//  * Print queue (optional): polls an HTTPS URL for ready-to-print jobs (for example
//    shipping labels in ZPL) and prints them automatically.
//  * Setup page at http://btl-printer.local (or the IP shown on the serial monitor).
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
#include "usb/usb_host.h"
#include "config.h"
#include <ImprovWiFiLibrary.h>   // Wi-Fi setup over USB from the web installer (Improv protocol)

#define FIRMWARE_VERSION "1.1.0"
ImprovWiFi improv(&Serial);
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
  restartSoon = true; restartAt = millis() + 4000;  // restart into normal mode after the installer shows the link
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
uint32_t jobsPrinted = 0;
String lastEvent = "Starting";

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
  devHdl = nullptr; claimedIntf = 0xFF; outEp = 0; printerName = "none";
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
  char buf[48];
  snprintf(buf, sizeof buf, "USB %04X:%04X%s", dd->idVendor, dd->idProduct, bestIsPrinter ? " (printer)" : "");
  printerName = buf;
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
bool printerWrite(const uint8_t *data, size_t len) {
  if (!printerReady) return false;
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

void serviceRawPrinting() {
  WiFiClient c = rawServer.accept();
  if (!c) return;
  size_t total = 0; bool ok = true;
  uint32_t lastData = millis();
  while (c.connected() || c.available()) {
    int n = c.read(netBuf, sizeof netBuf);
    if (n > 0) {
      lastData = millis();
      if (ok && !printerWrite(netBuf, n)) ok = false;
      total += n;
    } else {
      if (millis() - lastData > 15000) break;  // sender went quiet
      delay(2);
    }
  }
  c.stop();
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
input{width:100%;padding:10px;border:1px solid #ccc;border-radius:8px;box-sizing:border-box;font:inherit}button{margin-top:14px;padding:12px 18px;border:0;border-radius:8px;background:#d9471f;color:#fff;font:600 16px system-ui;cursor:pointer}
.ok{color:#137333;font-weight:600}.bad{color:#b3261e;font-weight:600}small{color:#666}code{background:#eee;padding:2px 5px;border-radius:4px}</style><h1>BTL Print Bridge</h1>)HTML";

void handleRoot() {
  String h = FPSTR(PAGE_HEAD);
  h += "<div class=card><b>Printer:</b> ";
  h += printerReady ? "<span class=ok>connected</span> " + htmlEsc(printerName) : String("<span class=bad>not connected</span>");
  h += "<br><b>Wi-Fi:</b> " + (setupMode ? String("setup mode") : htmlEsc(cfgSsid) + " &middot; " + WiFi.localIP().toString());
  h += "<br><b>Jobs printed:</b> " + String(jobsPrinted) + "<br><b>Last event:</b> " + htmlEsc(lastEvent) + "</div>";
  if (!setupMode) {
    h += "<div class=card><b>Add to a computer</b><br><small>Windows: Settings &rarr; Printers &rarr; Add device &rarr; Add manually &rarr; "
         "TCP/IP address <code>" + WiFi.localIP().toString() + "</code>, port <code>" + String(RAW_PRINT_PORT) +
         "</code>, protocol Raw. Then choose your printer's normal driver.</small></div>";
    h += "<div class=card><form method=post action=/test><b>Test print</b><br><small>Sends a small ZPL label. Only for printers that speak ZPL.</small><br><button>Print ZPL test</button></form></div>";
  }
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

void handleTest() {
  const char *zpl = "^XA^PW812^FO40,40^GB730,300,4^FS^FO70,80^A0N,56,56^FDBTL Print Bridge^FS"
                    "^FO70,160^A0N,36,36^FDWi-Fi printing works.^FS^FO70,230^BY3^BCN,80,Y,N,N^FDBTL-OK^FS^XZ";
  bool ok = printerWrite((const uint8_t *)zpl, strlen(zpl));
  if (ok) jobsPrinted++;
  logEvent(ok ? "Printed test label" : "Test print FAILED (printer not ready)");
  web.sendHeader("Location", "/");
  web.send(303);
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
  WiFi.setHostname(BRIDGE_NAME);
  WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(250);
  if (WiFi.status() != WL_CONNECTED) { logEvent("Couldn't join " + cfgSsid); startSetupMode(); return; }
  WiFi.setSleep(false);  // keeps network printing responsive
  MDNS.begin(BRIDGE_NAME);
  MDNS.addService("http", "tcp", 80);
  MDNS.addService("pdl-datastream", "tcp", RAW_PRINT_PORT);
  rawServer.begin();
  logEvent("Wi-Fi connected: " + WiFi.localIP().toString());
}

// ---------------------------------------------------------------- main
void setup() {
  Serial.begin(115200);
  improv.setDeviceInfo(ImprovTypes::ChipFamily::CF_ESP32_S3, "BTL Print Bridge", FIRMWARE_VERSION, "BTL Print Bridge", "http://{LOCAL_IPV4}/");
  improv.setCustomConnectWiFi(improvConnect);
  delay(300);
  xferDone = xSemaphoreCreateBinary();
  usbLock = xSemaphoreCreateMutex();
  loadSettings();
  xTaskCreatePinnedToCore(usbHostTask, "usbhost", 4096, nullptr, 5, nullptr, 0);
  xTaskCreatePinnedToCore(usbClientTask, "usbclient", 6144, nullptr, 5, nullptr, 0);
  startWifi();
  web.on("/", handleRoot);
  web.on("/save", HTTP_POST, handleSave);
  web.on("/test", HTTP_POST, handleTest);
  web.onNotFound([] { web.sendHeader("Location", "/"); web.send(302); });
  web.begin();
}

void loop() {
  improv.handleSerial();
  if (restartSoon && millis() > restartAt) ESP.restart();
  if (setupMode) dns.processNextRequest();
  web.handleClient();
  if (!setupMode) {
    serviceRawPrinting();
    pollQueue();
    if (WiFi.status() != WL_CONNECTED) { static uint32_t lastTry = 0; if (millis() - lastTry > 30000) { lastTry = millis(); WiFi.reconnect(); } }
  }
  delay(1);
}
