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
#include "esp_system.h"
#include "usb/usb_host.h"
#include "png_label.h"
#if __has_include("esp32s3/rom/miniz.h")
#include "esp32s3/rom/miniz.h"   // the chip's built-in inflate (tinfl), used to decode PNG labels
#else
#include "rom/miniz.h"
#endif
#include "config.h"
#include "improv_serial.h"       // Wi-Fi setup over USB from the web installer (Improv protocol)

#define FIRMWARE_VERSION "1.11.2"
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
String cfgLang = "zpl";  // printer language for queued picture labels: "zpl" or "tspl"

void loadSettings() {
  prefs.begin("bridge", true);
  cfgSsid = prefs.getString("ssid", "");
  cfgPass = prefs.getString("pass", "");
  cfgQueueUrl = prefs.getString("qurl", "");
  cfgQueueToken = prefs.getString("qtok", "");
  cfgLang = prefs.getString("lang", "zpl");
  prefs.end();
}

void saveSettings() {
  prefs.begin("bridge", false);
  prefs.putString("ssid", cfgSsid);
  prefs.putString("pass", cfgPass);
  prefs.putString("lang", cfgLang);
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


// ---------------------------------------------------------------- troubleshooting log
// The last LOG_LINES events, kept in memory (cleared by a restart) and shown on the page so
// people can copy them into a bug report. Never holds the Wi-Fi name or password, the queue
// URL or token, or the contents of any print job: only what happened, when, and how big.
#define LOG_LINES 80
#define LOG_CHARS 100
static char logText[LOG_LINES][LOG_CHARS];
static uint32_t logAt[LOG_LINES];
static uint16_t logNext = 0, logCount = 0;
static portMUX_TYPE logMux = portMUX_INITIALIZER_UNLOCKED;

// Adds a line to the log only (safe to call from any task).
void logNote(const char *s) {
  portENTER_CRITICAL(&logMux);
  strlcpy(logText[logNext], s, LOG_CHARS);
  logAt[logNext] = millis();
  logNext = (logNext + 1) % LOG_LINES;
  if (logCount < LOG_LINES) logCount++;
  portEXIT_CRITICAL(&logMux);
}

// Adds a line to the log and shows it as "Last event" on the page.
void logEvent(const String &s) {
  lastEvent = s;
  Serial.println(s);
  String t = s;
  if (cfgSsid.length()) t.replace(cfgSsid, "(Wi-Fi name)");
  logNote(t.c_str());
}

static String upTime(uint32_t ms) {
  uint32_t sec = ms / 1000; char b[24];
  if (sec >= 86400) snprintf(b, sizeof b, "%ud %02u:%02u:%02u", (unsigned)(sec / 86400), (unsigned)(sec / 3600 % 24), (unsigned)(sec / 60 % 60), (unsigned)(sec % 60));
  else snprintf(b, sizeof b, "%02u:%02u:%02u", (unsigned)(sec / 3600), (unsigned)(sec / 60 % 60), (unsigned)(sec % 60));
  return b;
}

static const char *restartReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power was plugged in";
    case ESP_RST_SW: return "restarted itself (settings saved, update, or install)";
    case ESP_RST_PANIC: return "CRASH";
    case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return "FROZE and was restarted (watchdog)";
    case ESP_RST_BROWNOUT: return "POWER DIP (weak USB power or cable)";
    case ESP_RST_EXT: return "reset button";
    default: return "other";
  }
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

static volatile int xferStatus = 0, xferSent = 0;
volatile bool printerTrouble = false;   // the last print job failed over USB (light goes red until one works)
static void xferCb(usb_transfer_t *t) {
  xferStatus = t->status; xferSent = t->actual_num_bytes;
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
  usb_device_info_t info = {};
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
  printerReady = true; printerTrouble = false;
  logEvent("Printer connected: " + printerName);
  { char b[LOG_CHARS]; snprintf(b, sizeof b, "USB: using interface %d (%s), endpoint 0x%02X, %s speed", bestIntf, bestIsPrinter ? "printer class" : "not printer class",
             bestEp, info.speed == USB_SPEED_HIGH ? "high" : info.speed == USB_SPEED_FULL ? "full" : "low"); logNote(b); }
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
  int tries = 0, waits = 0;
  while (len && ok && printerReady) {
    // After a failed transfer, carry on in small pieces: gentler on a marginal cable or adapter.
    size_t n = len > USB_CHUNK_BYTES ? USB_CHUNK_BYTES : len;
    if (tries && n > 1024) n = 1024;
    memcpy(xfer->data_buffer, data, n);
    xfer->num_bytes = n;
    xfer->device_handle = devHdl;
    xfer->bEndpointAddress = outEp;
    xfer->callback = xferCb;
    xfer->context = nullptr;
    xfer->timeout_ms = 10000;
    if (usb_host_transfer_submit(xfer) != ESP_OK) { ok = false; logNote("USB: printer refused the data (submit failed)"); break; }
    if (xSemaphoreTake(xferDone, pdMS_TO_TICKS(15000)) != pdTRUE) {
      // The printer is connected but not taking data. Cancel the stuck transfer to learn how much it took.
      usb_host_endpoint_halt(devHdl, outEp);
      usb_host_endpoint_flush(devHdl, outEp);
      bool got = xSemaphoreTake(xferDone, pdMS_TO_TICKS(2000)) == pdTRUE;
      usb_host_endpoint_clear(devHdl, outEp);
      int took = got ? constrain((int)xferSent, 0, (int)n) : 0;
      char w[LOG_CHARS];
      snprintf(w, sizeof w, "USB: printer took %d of %u bytes, then accepted nothing for 15 s", took, (unsigned)n);
      logNote(w);
      if (got && took > 0 && ++waits <= 20) { data += took; len -= took; continue; }   // slow but moving: keep going
      logNote("USB: giving up. Is the printer paused, out of labels, lid open, or showing an error light?");
      ok = false; break;
    }
    if (xferOk) { data += n; len -= n; continue; }
    int st = xferStatus, sent = constrain((int)xferSent, 0, (int)n);
    const char *why = st == USB_TRANSFER_STATUS_ERROR ? "no answer or signal errors (cable, adapter, or the printer isn't getting 5 V)"
                    : st == USB_TRANSFER_STATUS_STALL ? "the printer refused it (stall)"
                    : st == USB_TRANSFER_STATUS_NO_DEVICE ? "the printer is gone"
                    : st == USB_TRANSFER_STATUS_TIMED_OUT ? "timed out"
                    : st == USB_TRANSFER_STATUS_CANCELED ? "cancelled" : "unknown error";
    char b[LOG_CHARS];
    snprintf(b, sizeof b, "USB: failed after %d of %u bytes: %s", sent, (unsigned)n, why);
    logNote(b);
    if (st == USB_TRANSFER_STATUS_NO_DEVICE || ++tries > 3) { ok = false; break; }
    // A failed transfer leaves the connection halted: reset it, then continue from where it stopped.
    usb_host_endpoint_flush(devHdl, outEp);
    if (usb_host_endpoint_clear(devHdl, outEp) != ESP_OK) { ok = false; logNote("USB: couldn't reset the connection to the printer"); break; }
    data += sent; len -= sent;
    vTaskDelay(pdMS_TO_TICKS(100));
    snprintf(b, sizeof b, "USB: trying again in smaller pieces (attempt %d of 3)", tries);
    logNote(b);
  }
  if (printerReady) printerTrouble = !ok;
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
  else if (!printerReady || printerTrouble) ledSet(L, 0, 0);       // red: no printer, or it stopped responding
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
// Jobs printed recently. The queue's storage can keep saying "not done yet" for up to a
// minute after we confirm a job, so the same job may be offered again: never print it twice.
String recentJobs[16]; uint8_t recentNext = 0;
// The last label picture received, kept so it can be checked at /lastlabel.png if a print looks wrong.
uint8_t *lastPng = nullptr; size_t lastPngLen = 0;
static bool printedBefore(const String &id) { for (auto &j : recentJobs) if (j.length() && j == id) return true; return false; }

// ---------------------------------------------------------------- picture labels
// Queue jobs that are PNG pictures (e.g. Shippo shipping labels) are decoded here and sent to
// the printer as a 4x6 dot image in its own language, so every barcode prints exactly as drawn,
// even on printers that only understand part of ZPL. Optional text (X-Label-Text header, e.g.
// "2x G3") is added on top. The PNG is decoded one row at a time, so even large labels fit.

// (PngDec, the decoder state, lives in png_label.h so Arduino's auto-generated prototypes can see it.)

static inline bool pngDark(const PngDec &d, const uint8_t *row, uint32_t x) {
  if (d.depth < 8) {
    uint32_t bit = x * d.depth; uint8_t v = (row[bit >> 3] >> (8 - d.depth - (bit & 7))) & ((1 << d.depth) - 1);
    return d.ctype == 3 ? d.palDark[v] : v < (1 << (d.depth - 1));
  }
  const uint8_t *px = row + x * d.ch;
  switch (d.ctype) {
    case 0: return px[0] < 128;
    case 3: return d.palDark[px[0]];
    case 4: return px[1] >= 128 && px[0] < 128;
    case 6: if (px[3] < 128) return false;  // transparent = white
    default: return ((px[0] * 77 + px[1] * 150 + px[2] * 29) >> 8) < 128;
  }
}

static inline void dotOn(PngDec &d, int x, int y) { d.out[y * LBL_ROWB + (x >> 3)] |= 0x80 >> (x & 7); }

// A full source row is ready: copy it into every output row/column that samples it.
static void pngRowDone(PngDec &d, const uint8_t *row) {
  uint32_t y = d.y;
  if (!d.rot) {
    while (d.p0 < d.th && d.sy0[d.p0] < (int)y) d.p0++;
    for (int t = d.p0; t < d.th && d.sy0[t] == (int)y; t++)
      for (int tx = 0; tx < d.tw; tx++) if (pngDark(d, row, d.sx0[tx])) dotOn(d, d.ox + tx, d.oy + t);
    while (d.p1 < d.th && d.sy1[d.p1] < (int)y) d.p1++;
    for (int t = d.p1; t < d.th && d.sy1[t] == (int)y; t++)
      for (int tx = 0; tx < d.tw; tx++) if (pngDark(d, row, d.sx1[tx])) dotOn(d, d.ox + tx, d.oy + t);
  } else {  // landscape picture: source rows become output columns (turned 90 degrees)
    while (d.p0 < d.tw && d.sx0[d.p0] < (int)y) d.p0++;
    for (int t = d.p0; t < d.tw && d.sx0[t] == (int)y; t++)
      for (int ty = 0; ty < d.th; ty++) if (pngDark(d, row, d.w - 1 - d.sy0[ty])) dotOn(d, d.ox + t, d.oy + ty);
    while (d.p1 < d.tw && d.sx1[d.p1] < (int)y) d.p1++;
    for (int t = d.p1; t < d.tw && d.sx1[t] == (int)y; t++)
      for (int ty = 0; ty < d.th; ty++) if (pngDark(d, row, d.w - 1 - d.sy1[ty])) dotOn(d, d.ox + t, d.oy + ty);
  }
}

static void pngFeed(PngDec &d, const uint8_t *data, size_t n) {
  while (n && d.y < d.h) {
    size_t take = min((size_t)(d.stride + 1 - d.fill), n);
    memcpy(d.cur + d.fill, data, take); d.fill += take; data += take; n -= take;
    if (d.fill < d.stride + 1) break;
    uint8_t f = d.cur[0], *r = d.cur + 1, *pr = d.prev + 1;
    for (uint32_t x = 0; x < d.stride; x++) {
      int a = x >= d.bpp ? r[x - d.bpp] : 0, up = d.y ? pr[x] : 0, c = (d.y && x >= d.bpp) ? pr[x - d.bpp] : 0;
      if (f == 1) r[x] += a; else if (f == 2) r[x] += up; else if (f == 3) r[x] += (a + up) >> 1;
      else if (f == 4) { int p = a + up - c, pa = abs(p - a), pb = abs(p - up), pc = abs(p - c); r[x] += (pa <= pb && pa <= pc) ? a : (pb <= pc ? up : c); }
    }
    pngRowDone(d, r);
    uint8_t *t = d.prev; d.prev = d.cur; d.cur = t;
    d.fill = 0; d.y++;
  }
}

static void freePng(PngDec &d) {
  free(d.cur); free(d.prev); free(d.sx0); free(d.sx1); free(d.sy0); free(d.sy1); free(d.out);
}

// Builds the 1-bit label image from PNG bytes. Returns an error message, or "" on success.
static String decodePngLabel(const uint8_t *png, size_t len, PngDec &d) {
  static const uint8_t SIG[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
  if (len < 33 || memcmp(png, SIG, 8)) return "not a PNG";
  auto be32 = [&](size_t i) { return (uint32_t)png[i] << 24 | png[i + 1] << 16 | png[i + 2] << 8 | png[i + 3]; };
  // Gather IHDR, PLTE and all IDAT data.
  size_t p = 8, idatLen = 0; const uint8_t *pal = nullptr; size_t palLen = 0; uint8_t inter = 0;
  for (size_t q = 8; q + 8 <= len; ) {
    uint32_t n = be32(q); if (q + 12 + n > len) break;
    if (!memcmp(png + q + 4, "IDAT", 4)) idatLen += n;
    q += 12 + n;
  }
  uint8_t *idat = (uint8_t *)ps_malloc(idatLen ? idatLen : 1); if (!idat) return "out of memory";
  size_t ip = 0;
  while (p + 8 <= len) {
    uint32_t n = be32(p); const uint8_t *t = png + p + 4, *dd = png + p + 8; if (p + 12 + n > len) break;
    if (!memcmp(t, "IHDR", 4)) { d.w = be32(p + 8); d.h = be32(p + 12); d.depth = dd[8]; d.ctype = dd[9]; inter = dd[12]; }
    else if (!memcmp(t, "PLTE", 4)) { pal = dd; palLen = n; }
    else if (!memcmp(t, "IDAT", 4)) { memcpy(idat + ip, dd, n); ip += n; }
    else if (!memcmp(t, "IEND", 4)) break;
    p += 12 + n;
  }
  const uint8_t chans[7] = {1, 0, 3, 1, 2, 0, 4};
  d.ch = d.ctype <= 6 ? chans[d.ctype] : 0;
  if (!d.w || !d.h || !d.ch || inter || d.depth > 8 || d.w > 4000 || d.h > 6000) { free(idat); return "unsupported PNG"; }
  for (size_t i = 0; i < palLen / 3; i++) d.palDark[i] = ((pal[i * 3] * 77 + pal[i * 3 + 1] * 150 + pal[i * 3 + 2] * 29) >> 8) < 128;
  d.stride = (d.w * d.ch * d.depth + 7) / 8; d.bpp = max(1, (d.ch * d.depth) / 8);
  d.cur = (uint8_t *)ps_calloc(d.stride + 1, 1); d.prev = (uint8_t *)ps_calloc(d.stride + 1, 1);
  d.out = (uint8_t *)ps_calloc(LBL_ROWB * LBL_H, 1);
  // Fit the picture into 4x6, turning landscape pictures upright. Each dot samples two nearby
  // source pixels so thin barcode bars survive shrinking (Shippo pictures are higher resolution).
  d.rot = d.w > d.h;
  int sw = d.rot ? d.h : d.w, sh = d.rot ? d.w : d.h;
  d.sc = min((float)LBL_W / sw, (float)LBL_H / sh);
  d.tw = min(LBL_W, (int)lroundf(sw * d.sc)); d.th = min(LBL_H, (int)lroundf(sh * d.sc));
  d.ox = (LBL_W - d.tw) / 2; d.oy = (LBL_H - d.th) / 2;
  d.sx0 = (int16_t *)malloc(d.tw * 2); d.sx1 = (int16_t *)malloc(d.tw * 2);
  d.sy0 = (int16_t *)malloc(d.th * 2); d.sy1 = (int16_t *)malloc(d.th * 2);
  if (!d.cur || !d.prev || !d.out || !d.sx0 || !d.sx1 || !d.sy0 || !d.sy1) { free(idat); return "out of memory"; }
  for (int i = 0; i < d.tw; i++) { d.sx0[i] = min(sw - 1, (int)(i / d.sc)); d.sx1[i] = min(sw - 1, (int)((i + 0.5f) / d.sc)); }
  for (int i = 0; i < d.th; i++) { d.sy0[i] = min(sh - 1, (int)(i / d.sc)); d.sy1[i] = min(sh - 1, (int)((i + 0.5f) / d.sc)); }
  // Inflate with the chip's built-in decompressor, using a 32 KB circular window.
  tinfl_decompressor *inf = (tinfl_decompressor *)malloc(sizeof(tinfl_decompressor));
  uint8_t *win = (uint8_t *)ps_malloc(TINFL_LZ_DICT_SIZE);
  if (!inf || !win) { free(inf); free(win); free(idat); return "out of memory"; }
  tinfl_init(inf);
  size_t inPos = 0, winPos = 0; tinfl_status st;
  do {
    size_t inBytes = idatLen - inPos, outBytes = TINFL_LZ_DICT_SIZE - winPos;
    st = tinfl_decompress(inf, idat + inPos, &inBytes, win, win + winPos, &outBytes, TINFL_FLAG_PARSE_ZLIB_HEADER);
    inPos += inBytes;
    pngFeed(d, win + winPos, outBytes);
    winPos = (winPos + outBytes) & (TINFL_LZ_DICT_SIZE - 1);
  } while (st == TINFL_STATUS_HAS_MORE_OUTPUT);
  free(inf); free(win); free(idat);
  if (st != TINFL_STATUS_DONE || d.y < d.h) return "couldn't decompress the PNG";
  return "";
}

// Sends the decoded label to the printer in ZPL or TSPL, with the optional text on top.
// "note" (lines split by "|") is printed big down the label, e.g. a heads-up slip for an order
// whose label wasn't bought.
static bool printLabelImage(const PngDec &d, String text, String note) {
  note.replace("^", ""); note.replace("~", ""); note.replace("\"", "'"); note = note.substring(0, 160);
  String noteLines[8]; int nLines = 0;
  for (int a = 0; note.length() && a <= (int)note.length() && nLines < 8; ) {
    int b = note.indexOf('|', a); if (b < 0) b = note.length();
    noteLines[nLines++] = note.substring(a, b); a = b + 1;
  }
  text.replace("^", ""); text.replace("~", ""); text.replace("\"", "'"); text = text.substring(0, 40);
  static char buf[4096]; size_t n = 0;
  auto flush = [&]() { bool ok = !n || printerWrite((const uint8_t *)buf, n); n = 0; return ok; };
  auto put = [&](const char *s, size_t l) { while (l) { size_t k = min(l, sizeof buf - n); memcpy(buf + n, s, k); n += k; s += k; l -= k; if (n == sizeof buf && !flush()) return false; } return true; };
  auto puts_ = [&](const String &s) { return put(s.c_str(), s.length()); };
  // The picture is sent in bands of rows, skipping blank ones, with a short pause after each.
  // One big image can overrun a cheap printer's input buffer: lost bytes shift the rest of the
  // label sideways. Small bands keep each piece easy to digest and limit any damage to one band.
  const int BAND = 32;
  auto blank = [&](int y0, int rows) { for (int i = y0 * LBL_ROWB, e = (y0 + rows) * LBL_ROWB; i < e; i++) if (d.out[i]) return false; return true; };
  if (cfgLang == "tspl") {
    if (!puts_("SIZE 101.6 mm,152.4 mm\r\nGAP 3 mm,0 mm\r\nDIRECTION 0\r\nCLS\r\n")) return false;
    for (int y0 = 0; y0 < LBL_H; y0 += BAND) {
      int rows = min(BAND, LBL_H - y0);
      if (blank(y0, rows)) continue;
      if (!puts_("BITMAP 0," + String(y0) + "," + String(LBL_ROWB) + "," + String(rows) + ",0,")) return false;
      for (int i = y0 * LBL_ROWB, e = (y0 + rows) * LBL_ROWB; i < e; i++) { char c = (char)(~d.out[i]); if (!put(&c, 1)) return false; }  // TSPL: 0 = black
      if (!puts_("\r\n") || !flush()) return false;
      delay(15);
    }
    String tail = "";
    if (text.length()) tail += "TEXT 430,412,\"5\",0,1,1,\"" + text + "\"\r\n";
    for (int i = 0; i < nLines; i++) tail += "TEXT 30," + String(160 + i * 120) + ",\"5\",0,1,1,\"" + noteLines[i] + "\"\r\n";
    tail += "PRINT 1,1\r\n";
    return puts_(tail) && flush();
  }
  if (!puts_("^XA^PW" + String(LBL_W) + "^LL" + String(LBL_H) + "^LH0,0")) return false;
  static const char HX[] = "0123456789ABCDEF";
  for (int y0 = 0; y0 < LBL_H; y0 += BAND) {
    int rows = min(BAND, LBL_H - y0), bytes = rows * LBL_ROWB;
    if (blank(y0, rows)) continue;
    if (!puts_("^FO0," + String(y0) + "^GFA," + String(bytes) + "," + String(bytes) + "," + String(LBL_ROWB) + ",")) return false;
    for (int i = y0 * LBL_ROWB, e = (y0 + rows) * LBL_ROWB; i < e; i++) { char h2[2] = {HX[d.out[i] >> 4], HX[d.out[i] & 15]}; if (!put(h2, 2)) return false; }
    if (!puts_("^FS") || !flush()) return false;
    delay(15);
  }
  String tail = "";
  if (text.length()) tail += "^FO430,412^A0N,64,64^FB360,2,0,R^FD" + text + "^FS";
  for (int i = 0; i < nLines; i++) tail += "^FO30," + String(160 + i * 120) + "^A0N," + String(i < 2 ? 96 : 72) + "," + String(i < 2 ? 96 : 72) + "^FD" + noteLines[i] + "^FS";
  tail += "^XZ";
  return puts_(tail) && flush();
}

static bool isPng(const uint8_t *b, size_t n) { return n > 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G'; }

void pollQueue() {
  if (cfgQueueUrl.isEmpty() || !printerReady || WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastPoll < QUEUE_POLL_SECONDS * 1000UL) return;
  lastPoll = millis();

  WiFiClientSecure tls;
  tls.setInsecure();  // jobs are authenticated by the token; see README for pinning a CA
  HTTPClient http;
  const char *hdrs[] = {"X-Job-Id", "X-Label-Text", "X-Label-Note"};
  if (!http.begin(tls, cfgQueueUrl + "/next")) return;
  http.collectHeaders(hdrs, 3);
  http.addHeader("X-Bridge-Accepts", "image/png");
  http.addHeader("X-Bridge-Page", "http://" + WiFi.localIP().toString() + "/");  // lets the queue's settings page link back here
  http.addHeader("Authorization", "Bearer " + cfgQueueToken);
  int code = http.GET();
  static bool queueDown = false;
  if (code <= 0) { if (!queueDown) { queueDown = true; logNote(("Queue: couldn't reach the server (" + String(code) + ")").c_str()); } }
  else if (queueDown) { queueDown = false; logNote("Queue: server reachable again"); }
  if (code != 200) { http.end(); if (code != 204 && code > 0) logEvent("Queue: HTTP " + String(code)); return; }

  String id = http.header("X-Job-Id"), labelText = http.header("X-Label-Text"), labelNote = http.header("X-Label-Note");
  bool repeat = id.length() && printedBefore(id);
  if (repeat) http.end();
  else {
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
  bool ok = false;
  if (got == size && isPng(job, size)) {
    PngDec d;
    String err = decodePngLabel(job, size, d);
    if (err.length()) logEvent("Queue: label picture " + id + ": " + err);
    else ok = printLabelImage(d, labelText, labelNote);
    freePng(d);
    free(lastPng); lastPng = job; lastPngLen = size; job = nullptr;
  } else {
    ok = (got == size) && printerWrite(job, size);
  }
  free(job);
  if (!ok) { logEvent("Queue: job " + id + " FAILED, will retry"); return; }
  jobsPrinted++;
  logEvent("Printed queued job " + id);
  if (id.length()) { recentJobs[recentNext] = id; recentNext = (recentNext + 1) % 16; }
  }

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
// "Copy or back up fliers" card. The browser does the work: it reads each flier from this board
// (/flier-file) and posts it to another board's /flier-put, or packs them into one backup file.
const char COPY_TOOL[] PROGMEM = R"HTML(<div class=card><b>Copy or back up fliers</b><br>
<small>Copies go into the same click slots and replace what's there. Fliers only print correctly on the same kind of printer they were saved for.</small>
<label>Send all fliers to another board</label>
<div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap"><select id=cb style="flex:1;min-width:180px;padding:10px;border:1px solid #ccc;border-radius:8px;font:inherit"><option value="">Searching&hellip;</option></select>
<button id=cf type=button style="margin:0;background:#666">Search again</button></div>
<input id=ca placeholder="or type its address, e.g. 192.168.1.50" style="margin-top:8px" aria-label="Other board's address">
<button id=cs type=button>Send fliers</button> <small id=cm></small>
<label style="margin-top:16px">Backup file</label>
<small>Save all fliers to one file on this computer, or load a saved file onto this board.</small><br>
<button id=bd type=button style="background:#666">Download backup</button> <button id=br type=button style="background:#666">Restore backup</button>
<input id=bf type=file accept=".btlfliers" hidden> <small id=bm></small></div>
<script>(function(){
var $=function(i){return document.getElementById(i)},MAGIC='BTLFLIERS1';
function list(){return fetch('/fliers.json',{cache:'no-store'}).then(function(r){return r.json()})}
function get(n){return fetch('/flier-file?n='+n,{cache:'no-store'}).then(function(r){if(!r.ok)throw 'could not read flier '+n;return r.blob()})}
function put(base,f,blob){var d=new FormData();d.append('n',f.n);d.append('name',f.name||'');d.append('batch',f.batch||10);d.append('f',blob,'flier.bin');
return fetch(base+'/flier-put',{method:'POST',body:d}).then(function(r){return r.text().then(function(t){if(t!=='ok')throw(r.status==400?t:'that board needs its firmware updated first')})})}
function each(a,fn){return a.reduce(function(p,x,i){return p.then(function(){return fn(x,i)})},Promise.resolve())}
function say(m,e){return function(x){m.textContent=typeof x=='string'?x:(e||'Something went wrong.')}}
function find(){var s=$('cb');s.innerHTML='<option value="">Searching&hellip;</option>';
fetch('/boards',{cache:'no-store'}).then(function(r){return r.json()}).then(function(b){s.innerHTML=b.length?'':'<option value="">No other boards found</option>';
b.forEach(function(x){var o=document.createElement('option');o.value=x.ip;o.textContent=x.name+' ('+x.ip+')';s.appendChild(o)})}).catch(function(){s.innerHTML='<option value="">Search failed</option>'})}
$('cf').onclick=find;find();
$('cs').onclick=function(){var m=$('cm'),t=($('ca').value.trim()||$('cb').value).replace(/^https?:\/\//,'').replace(/\/+$/,'');
if(!t){m.textContent='Pick a board or type its address first.';return}
if(t==location.host){m.textContent="That's this board.";return}
var base='http://'+t,fl,btn=this;
list().then(function(j){fl=j.fliers;if(!fl.length)throw 'No fliers saved on this board yet.';
return fetch(base+'/fliers.json',{cache:'no-store'}).then(function(r){return r.json()}).catch(function(){throw "Couldn't reach that board. Is it on, on the same Wi-Fi, and updated?"})})
.then(function(o){if(!confirm('Send '+fl.length+' flier(s) to '+(o.board||t)+'? Fliers in the same slots on that board are replaced.'))throw 'Cancelled.';
btn.disabled=true;return each(fl,function(f,i){m.textContent='Sending '+(i+1)+' of '+fl.length+'...';return get(f.n).then(function(b){return put(base,f,b)})})})
.then(function(){m.textContent='Done. Sent '+fl.length+' flier(s).'}).catch(say(m,"Sending failed. Check the other board is on and try again.")).then(function(){btn.disabled=false})};
$('bd').onclick=function(){var m=$('bm'),meta,parts=[];
list().then(function(j){meta=j;if(!j.fliers.length)throw 'No fliers saved on this board yet.';m.textContent='Reading fliers...';
return each(j.fliers,function(f){return get(f.n).then(function(b){f.size=b.size;parts.push(b)})})})
.then(function(){var a=document.createElement('a');a.href=URL.createObjectURL(new Blob([MAGIC+'\n',JSON.stringify(meta)+'\n'].concat(parts)));
a.download=(meta.board||'btl')+'-fliers.btlfliers';document.body.appendChild(a);a.click();a.remove();m.textContent='Saved '+meta.fliers.length+' flier(s) to your Downloads.'}).catch(say(m,'Backup failed.'))};
$('br').onclick=function(){$('bf').value='';$('bf').click()};
$('bf').onchange=function(){var file=this.files[0],m=$('bm');if(!file)return;
file.arrayBuffer().then(function(buf){var u=new Uint8Array(buf),a=u.indexOf(10),b=u.indexOf(10,a+1),dec=new TextDecoder();
if(a<0||b<0||dec.decode(u.subarray(0,a))!==MAGIC)throw "That isn't a BTL flier backup file.";
var meta=JSON.parse(dec.decode(u.subarray(a+1,b))),at=b+1,fl=meta.fliers;
fl.forEach(function(f){f.blob=new Blob([u.subarray(at,at+f.size)]);at+=f.size});
if(at!==u.length)throw 'That backup file is damaged.';
if(!confirm('Load '+fl.length+' flier(s) onto this board? Fliers in the same slots are replaced.'))throw 'Cancelled.';
return each(fl,function(f,i){m.textContent='Loading '+(i+1)+' of '+fl.length+'...';return put('',f,f.blob)}).then(function(){m.textContent='Done. Reloading...';setTimeout(function(){location.reload()},800)})})
.catch(say(m,"Couldn't load that file."))};
})();</script>)HTML";

const char LOG_TOOL[] PROGMEM = R"HTML(<div class=card><b>Troubleshooting log</b><br><small>Something not working? Click <b>Copy log</b> and paste it into your message when you ask for help.
It has no passwords, no Wi-Fi name and nothing from your labels. It starts fresh each time the board restarts, so copy it before unplugging.</small><br>
<button id=lc type=button>Copy log</button> <button id=lr type=button style="background:#666">Refresh</button> <small id=lm></small>
<textarea id=lt readonly rows=10 wrap=off style="width:100%;box-sizing:border-box;margin-top:10px;padding:8px;border:1px solid #ccc;border-radius:8px;font:12px ui-monospace,Consolas,monospace;white-space:pre"></textarea></div>
<script>(function(){
var $=function(i){return document.getElementById(i)},t=$('lt'),m=$('lm');
function load(){return fetch('/log').then(function(r){return r.text()}).then(function(x){t.value=x;t.scrollTop=t.scrollHeight})}
$('lr').onclick=function(){load().then(function(){m.textContent='Updated.'}).catch(function(){m.textContent="Couldn't load the log."})};
$('lc').onclick=function(){load().catch(function(){}).then(function(){t.focus();t.select();var ok=false;try{ok=document.execCommand('copy')}catch(e){}
m.textContent=ok?'Copied. Paste it into your message.':'Select the text below and press Ctrl+C.'})};
load().catch(function(){});
})();</script>)HTML";

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
  h += !printerReady ? String("<span class=bad>not connected</span>")
     : printerTrouble ? "<span class=bad>stopped responding</span> " + htmlEsc(printerName) + "<br><small>The last print didn't get through. Turn the printer off and on, then try again. A shorter USB cable often fixes this too. "
                        "If it keeps happening, the printer probably needs 5 V from the board (bridge the USB-OTG pads) or a different cable. The Troubleshooting log below has details.</small>"
     : "<span class=ok>connected</span> " + htmlEsc(printerName);
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

// The other slots, for a flier's "Move to" list.
String moveOptions(int n) {
  String o;
  for (int k = 1; k <= FLIER_COUNT; k++) if (k != n)
    o += "<option value=" + String(k) + ">" + String(k) + (k == 1 ? " click" : " clicks") + (flierSize(k) ? " (swaps with " + htmlEsc(flierLabel(k)) + ")" : String(" (empty)")) + "</option>";
  return o;
}

// ---- copying fliers to another board, and backup files (driven by the page's COPY_TOOL script)
static String jsonEsc(const String &s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) { char c = s[i]; if (c == '"' || c == '\\') { o += '\\'; o += c; } else if ((uint8_t)c >= 0x20) o += c; }
  return o;
}

// Which fliers this board holds. Readable from another board's page, so it can check before sending.
void handleFliersJson() {
  String j = "{\"board\":\"" + hostName + "\",\"version\":\"" FIRMWARE_VERSION "\",\"fliers\":[";
  bool first = true;
  for (int n = 1; n <= FLIER_COUNT; n++) {
    size_t z = flierSize(n);
    if (!z) continue;
    j += String(first ? "" : ",") + "{\"n\":" + String(n) + ",\"batch\":" + String(flierBatch[n]) + ",\"size\":" + String(z) + ",\"name\":\"" + jsonEsc(flierName[n]) + "\"}";
    first = false;
  }
  j += "]}";
  web.sendHeader("Access-Control-Allow-Origin", "*");
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json", j);
}

// One saved flier's raw print data.
void handleFlierFile() {
  int n = web.arg("n").toInt();
  if (n < 1 || n > FLIER_COUNT || !flierSize(n)) { web.send(404, "text/plain", "No flier in that slot."); return; }
  File f = FFat.open(flierPath(n));
  web.sendHeader("Cache-Control", "no-store");
  web.streamFile(f, "application/octet-stream");
  f.close();
}

// Receives one flier (from another board's page or a backup file) into slot n.
File putFile; bool putOk = false; size_t putBytes = 0;
void handleFlierPutUpload() {
  HTTPUpload &u = web.upload();
  if (u.status == UPLOAD_FILE_START) { FFat.remove("/put.tmp"); putFile = FFat.open("/put.tmp", FILE_WRITE); putOk = (bool)putFile; putBytes = 0; }
  else if (u.status == UPLOAD_FILE_WRITE) { if (putOk && putFile.write(u.buf, u.currentSize) != u.currentSize) putOk = false; putBytes += u.currentSize; }
  else if (u.status == UPLOAD_FILE_END) { if (putFile) putFile.close(); }
  else if (u.status == UPLOAD_FILE_ABORTED) { if (putFile) putFile.close(); putOk = false; }
}
void handleFlierPutDone() {
  web.sendHeader("Access-Control-Allow-Origin", "*");
  int n = web.arg("n").toInt();
  const char *why = nullptr;
  if (n < 1 || n > FLIER_COUNT) why = "That board has no such flier slot.";
  else if (jobLeft || captureSlot) why = "That board is busy printing or saving a flier. Try again in a moment.";
  else if (!putOk || !putBytes) why = "The flier didn't arrive in one piece (storage full?).";
  if (!why) { FFat.remove(flierPath(n)); if (!FFat.rename("/put.tmp", flierPath(n))) why = "Couldn't store the flier."; }
  putOk = false;
  if (why) { FFat.remove("/put.tmp"); logEvent(String("Flier copy refused: ") + why); web.send(400, "text/plain", why); return; }
  String nm = web.arg("name"); nm.trim(); nm = nm.substring(0, 30);
  int b = constrain((int)web.arg("batch").toInt(), 1, 999);
  flierName[n] = nm; flierBatch[n] = b;
  Preferences p; p.begin("btl", false);
  p.putString(("f" + String(n)).c_str(), nm); p.putInt(("b" + String(n)).c_str(), b); p.end();
  logEvent("Received " + flierLabel(n) + " into slot " + String(n) + " (" + String(putBytes / 1024 + 1) + " KB)");
  web.send(200, "text/plain", "ok");
}

// Other BTL bridges on this network, found by their .local names.
void handleBoards() {
  String j = "[";
  int k = setupMode ? 0 : MDNS.queryService("http", "tcp");
  for (int i = 0; i < k; i++) {
    String h = MDNS.hostname(i);
    if (!h.startsWith(BRIDGE_NAME) || h.startsWith(hostName)) continue;
    j += String(j.length() > 1 ? "," : "") + "{\"name\":\"" + jsonEsc(h) + "\",\"ip\":\"" + MDNS.address(i).toString() + "\"}";
  }
  j += "]";
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json", j);
}

// Plain text made for pasting into a bug report.
void handleLog() {
  String t = "BTL Print Bridge log\n";
  t += "Version: " FIRMWARE_VERSION "\n";
  t += "Running for: " + upTime(millis()) + "\n";
  t += String("Last restart: ") + restartReason() + "\n";
  char ids[12]; snprintf(ids, sizeof ids, "%04x:%04x", usbVid, usbPid);
  t += "Printer: " + (printerReady ? (printerModel.length() && printerName.indexOf(printerModel) < 0 ? printerName + " / " + printerModel : printerName) + (printerName.indexOf("(USB ") < 0 ? String(" (USB ") + ids + ")" : String("")) : String("not connected")) + "\n";
  t += "Wi-Fi: " + (setupMode ? String("setup mode") : WiFi.status() == WL_CONNECTED ? "connected, signal " + String(WiFi.RSSI()) + " dBm" : String("NOT connected")) + "\n";
  t += "Queue: " + (cfgQueueUrl.length() ? "on, " + cfgLang : String("off")) + "\n";
  t += "Jobs printed: " + String(jobsPrinted) + "\n";
  t += "Memory free: " + String(ESP.getFreeHeap() / 1024) + " KB (lowest " + String(ESP.getMinFreeHeap() / 1024) + " KB)\n";
  t += "Storage: " + (fsReady ? String(FFat.freeBytes() / 1024) + " KB free of " + String(FFat.totalBytes() / 1024) + " KB" : String("not available")) + "\n";
  t += "Fliers:";
  for (int n = 1; n <= FLIER_COUNT; n++) { size_t z = flierSize(n); t += " " + String(n) + "=" + (z ? String(z / 1024 + 1) + "KB" : String("empty")); }
  t += "\n\nTime since start, then what happened (newest last):\n";
  static char line[LOG_CHARS]; uint32_t at = 0;
  for (uint16_t i = 0;; i++) {
    portENTER_CRITICAL(&logMux);
    bool more = i < logCount;
    if (more) { uint16_t k = (logNext + LOG_LINES - logCount + i) % LOG_LINES; memcpy(line, logText[k], LOG_CHARS); at = logAt[k]; }
    portEXIT_CRITICAL(&logMux);
    if (!more) break;
    t += upTime(at) + "  " + line + "\n";
  }
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "text/plain; charset=utf-8", t);
}

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
    h += "<div class=card><b>Saved fliers</b><br><small>To save one: click <b>Save next print here</b> under the flier you want, then print the flier from your computer to this printer "
         "within " + String(CAPTURE_MINUTES) + " minutes. It's stored instead of printed.<br>"
         "To print it: click the board's <b>BOOT</b> button 1 to " + String(FLIER_COUNT) + " times quickly for Flier 1 to " + String(FLIER_COUNT) + ". "
         "Hold the last click for 2 seconds to print the batch, and press again to stop.<br>"
         "Optional extra buttons: GPIO " + flierPinList() + " to GND for Flier 1 to " + String(FLIER_COUNT) + " (press = 1 copy, hold = batch).</small>";
    if (!fsReady) h += "<p class=bad>Storage isn't available on this board.</p>";
    for (int n = 1; n <= FLIER_COUNT; n++) {
      size_t z = flierSize(n);
      String dis = z ? "" : " disabled";
      h += "<form method=post action=/flier style=\"margin-top:14px;border-top:1px solid #ddd;padding-top:10px\"><input type=hidden name=n value=" + String(n) + ">"
           "<b>Flier " + String(n) + "</b> <small>(" + String(n) + (n == 1 ? " click" : " clicks") + ")</small>: " + (z ? htmlEsc(flierLabel(n)) + " <small>(" + String(z / 1024 + 1) + " KB)</small>" : String("<small>empty</small>")) +
           "<div style=\"display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:6px\">"
           "<input name=copies type=number min=1 max=999 value=1 style=\"width:80px\"" + dis + " aria-label=\"Copies to print now\"> "
           "<button name=do value=print style=margin:0" + dis + ">Print</button></div>"
           "<div style=\"display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:8px\"><small>Batch (hold the last click):</small>"
           "<input name=batch type=number min=1 max=999 value=" + String(flierBatch[n]) + " style=\"width:80px\" aria-label=\"Copies when you hold the button\">"
           "<small>copies</small><button name=do value=rename style=\"margin:0;background:#666\">Save</button></div>"
           "<label>Name</label><input name=name maxlength=30 placeholder=\"e.g. Menu\" value=\"" + htmlEsc(flierName[n]) + "\">"
           "<button name=do value=rename style=\"background:#666\">Save name</button> "
           "<button name=do value=save style=\"background:#2563c9\">Save next print here</button> "
           "<button name=do value=delete style=\"background:#999\"" + dis + ">Delete flier</button>"
           "<div style=\"display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:8px\"><small>Move to:</small>"
           "<select name=to style=\"padding:10px;border:1px solid #ccc;border-radius:8px;font:inherit\"" + dis + " aria-label=\"Slot to move this flier to\">" + moveOptions(n) + "</select>"
           "<button name=do value=move style=\"margin:0;background:#666\"" + dis + ">Move</button></div></form>";
    }
    h += "</div>";
    if (fsReady) h += FPSTR(COPY_TOOL);
  }
  if (!setupMode) { String fw = FPSTR(FW_TOOL); fw.replace("%VER%", FIRMWARE_VERSION); h += fw; }
  h += "<div class=card><form method=post action=/save><b>Settings</b>"
       "<label>Wi-Fi name</label><input name=ssid value=\"" + htmlEsc(cfgSsid) + "\" required>"
       "<label>Wi-Fi password</label><input name=pass type=password placeholder=\"(unchanged)\">"
       "<label>Print queue URL <small>(optional)</small></label><input name=qurl value=\"" + htmlEsc(cfgQueueUrl) + "\" placeholder=\"https://.../print\">"
       + (cfgQueueUrl.endsWith("/print") ? "<small><a href=\"" + htmlEsc(cfgQueueUrl.substring(0, cfgQueueUrl.length() - 6)) + "/admin\" target=_blank rel=noopener>Open the Label Autopilot settings page</a></small>" : String("")) +
       "<label>Queue token <small>(optional)</small></label><input name=qtok type=password placeholder=\"" + String(cfgQueueToken.length() ? "(unchanged)" : "") + "\">"
       "<label>Printer language <small>(for shipping labels from the queue)</small></label><select name=lang style=\"width:100%;padding:10px;border:1px solid #ccc;border-radius:8px;font:inherit\">"
       "<option value=zpl" + String(cfgLang == "zpl" ? " selected" : "") + ">ZPL (Zebra, RONGTA, most 4x6 printers)</option>"
       "<option value=tspl" + String(cfgLang == "tspl" ? " selected" : "") + ">TSPL (many budget label printers)</option></select>"
       "<button>Save and restart</button></form></div>";
  h += FPSTR(LOG_TOOL);
  web.send(200, "text/html", h);
}

void handleSave() {
  cfgSsid = web.arg("ssid");
  if (web.arg("pass").length()) cfgPass = web.arg("pass");
  cfgQueueUrl = web.arg("qurl"); cfgQueueUrl.trim();
  while (cfgQueueUrl.endsWith("/")) cfgQueueUrl.remove(cfgQueueUrl.length() - 1);
  if (web.arg("qtok").length()) cfgQueueToken = web.arg("qtok");
  if (web.arg("lang") == "tspl" || web.arg("lang") == "zpl") cfgLang = web.arg("lang");
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
    else if (act == "move") {
      // Move a flier to another slot. If that slot already holds one, the two trade places.
      int to = web.arg("to").toInt();
      if (to < 1 || to > FLIER_COUNT || to == n || !flierSize(n)) logEvent("Nothing to move");
      else if (jobLeft || captureSlot) logEvent("Can't move a flier while printing or saving one");
      else {
        const char *tmp = "/swap.bin";
        bool both = flierSize(to) > 0, ok = true;
        FFat.remove(tmp);
        if (both) ok = FFat.rename(flierPath(to), tmp);
        ok = ok && FFat.rename(flierPath(n), flierPath(to));
        if (ok && both) ok = FFat.rename(tmp, flierPath(n));
        if (ok) {
          String nm = flierName[n]; flierName[n] = flierName[to]; flierName[to] = nm;
          int b = flierBatch[n]; flierBatch[n] = flierBatch[to]; flierBatch[to] = b;
          Preferences p; p.begin("btl", false);
          for (int k : {n, to}) { p.putString(("f" + String(k)).c_str(), flierName[k]); p.putInt(("b" + String(k)).c_str(), flierBatch[k]); }
          p.end();
          logEvent("Moved " + flierLabel(to) + " to slot " + String(to) + (both ? ", swapped with slot " + String(n) : String("")));
        } else logEvent("Couldn't move the flier (storage problem)");
      }
    }
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
  logNote((String("Started, version " FIRMWARE_VERSION ". Last restart: ") + restartReason()).c_str());
  WiFi.onEvent([](WiFiEvent_t e, WiFiEventInfo_t info) {
    static bool up = false;
    if (e == ARDUINO_EVENT_WIFI_STA_GOT_IP) { if (!up) logNote("Wi-Fi: online"); up = true; }
    else if (e == ARDUINO_EVENT_WIFI_STA_DISCONNECTED && up) {
      up = false; char b[48]; snprintf(b, sizeof b, "Wi-Fi: dropped (reason %d)", (int)info.wifi_sta_disconnected.reason); logNote(b);
    }
  });
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
  web.on("/log", handleLog);
  web.on("/fliers.json", handleFliersJson);
  web.on("/flier-file", handleFlierFile);
  web.on("/flier-put", HTTP_POST, handleFlierPutDone, handleFlierPutUpload);
  web.on("/boards", handleBoards);
  web.on("/lastlabel.png", []() {
    if (!lastPng) { web.send(404, "text/plain", "No label picture received since the bridge started."); return; }
    web.send_P(200, "image/png", (const char *)lastPng, lastPngLen);
  });
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
