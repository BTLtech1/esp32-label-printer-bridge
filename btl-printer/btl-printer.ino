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
#include "esp_task_wdt.h"
#include "usb/usb_host.h"
#include "png_label.h"
#if __has_include("esp32s3/rom/miniz.h")
#include "esp32s3/rom/miniz.h"   // the chip's built-in inflate (tinfl), used to decode PNG labels
#else
#include "rom/miniz.h"
#endif
#include "config.h"
#include "improv_serial.h"       // Wi-Fi setup over USB from the web installer (Improv protocol)

#define FIRMWARE_VERSION "1.19.1"
ImprovSerial improv(Serial);
extern String cfgSsid, cfgPass;
void saveSettings();
void logEvent(const String &s);
struct MediaFix { uint8_t m = 0; uint32_t fixed = 0; };  // state for fixZplMedia(); defined up here so Arduino's generated prototypes see it
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

// Freeze guard. If the main loop stops running for FREEZE_SECONDS (a bug, or a printer or network call
// that never returns), the chip's watchdog restarts the board by itself, so the buttons and the page
// come back without anyone unplugging it. `stage` says what the loop was doing; it is kept in memory
// that survives that kind of restart, so the next log can say where it froze.
#define FREEZE_SECONDS 180
RTC_NOINIT_ATTR static char stage[28];
RTC_NOINIT_ATTR static uint32_t stageMagic;
static inline void at(const char *where) { strlcpy(stage, where, sizeof stage); stageMagic = 0xB71F00D5; esp_task_wdt_reset(); }

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

// "Reprint the last label": while a label from the computer, the page or the queue is going to
// the printer, a copy is kept in memory (never in storage; it's gone when the board restarts).
// Only done when a button is set to reprint. Fliers and test labels aren't recorded.
#define LAST_JOB_MAX (700 * 1024)
uint8_t *lastJob = nullptr;
size_t lastLen = 0;
bool lastRec = false, lastFresh = false, lastOver = false, lastValid = false;
volatile int reprintPending = 0;        // how many reprints a button asked for
#define MAX_CLICKS 20                   // most copies a wired button counts (a bouncing key can't run off a roll)
uint32_t blinkUntil = 0;                // status light flashes white while > millis(): one flash per counted click
uint32_t redUntil = 0;                  // status light flashes red: a button asked for a flier slot that's empty
bool booting = true;                    // still starting up (joining Wi-Fi): status light shows cyan

bool printerWrite(const uint8_t *data, size_t len) {
  if (lastRec && lastJob) {
    if (lastFresh) { lastLen = 0; lastOver = false; lastValid = false; lastFresh = false; }
    if (lastLen + len <= LAST_JOB_MAX) { memcpy(lastJob + lastLen, data, len); lastLen += len; } else lastOver = true;
  }
  if (!printerReady) return false;
  ledSet(0, 0, LED_BRIGHTNESS);  // blue while printing
  xSemaphoreTake(usbLock, portMAX_DELAY);
  bool ok = true;
  int tries = 0, waits = 0;
  while (len && ok && printerReady) {
    esp_task_wdt_reset();                // slow but still trying: not a freeze (each try gives up by itself)
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
bool cancelPending = false;             // send the printer's own "cancel jobs" once the current copy is done
// The board hands a whole batch to the printer in a few seconds, but the printer takes far longer to print
// it. Until it has probably finished (about 3 s a copy, at most 3 min), any button press cancels the rest
// instead of starting a new print.
uint32_t drainUntil = 0;
bool batchBusy() { return jobLeft || millis() < drainUntil; }
int flierBatch[FLIER_COUNT + 1];   // copies printed by holding the button

// What each wired button does: 1..FLIER_COUNT = print that flier (the default is its own
// number), or one of the actions below. Set on the bridge's page, kept in settings.
#define ACT_REPRINT 100
#define ACT_STOP    101
int btnAct[FLIER_COUNT + 1];

void lastBegin() {
  lastRec = false;
  bool wanted = false;
  for (int i = 1; i <= FLIER_COUNT; i++) if (btnAct[i] == ACT_REPRINT) wanted = true;
  if (!wanted) { if (lastJob) { free(lastJob); lastJob = nullptr; lastValid = false; } return; }
  if (!lastJob) lastJob = (uint8_t *)ps_malloc(LAST_JOB_MAX);
  if (!lastJob) return;
  lastFresh = true; lastRec = true;      // the old copy is kept until the new job's first bytes arrive
}
void lastEnd(bool ok) {
  if (lastRec && !lastFresh) lastValid = ok && !lastOver && lastLen > 0;
  lastRec = false;
}

void serviceReprint() {
  if (!reprintPending) return;
  int n = reprintPending; reprintPending = 0;
  if (!lastValid || !lastJob) { logEvent("Nothing to reprint yet (labels are remembered from when a button was set to Reprint, until the board restarts)"); return; }
  int done = 0;
  for (int k = 0; k < n; k++) { if (!printerWrite(lastJob, lastLen)) break; jobsPrinted++; done++; }
  if (done == n) logEvent("Reprinted the last label" + (n > 1 ? String(" x") + String(n) : String("")) + ", " + String(lastLen) + " bytes");
  else logEvent("Reprint FAILED after " + String(done) + " of " + String(n) + " (printer not ready)");
}

void startFlier(int n, int copies) {
  if (n < 1 || n > FLIER_COUNT) return;
  if (!flierSize(n)) { redUntil = millis() + 700; logEvent(flierLabel(n) + " is empty: save a print to it first"); return; }
  if (!printerReady) { logEvent(flierLabel(n) + " FAILED (printer not ready)"); return; }
  jobFlier = n; jobLeft = constrain(copies, 1, 999); jobDone = 0;
}

void stopFlier() {
  if (jobLeft) { logEvent("Stopped " + flierLabel(jobFlier) + " after " + String(jobDone) + " copies"); cancelPending = true; }
  else if (millis() < drainUntil) { logEvent("Stopped: told the printer to drop the copies of " + flierLabel(jobFlier) + " it still had"); cancelPending = true; }
  jobLeft = 0; drainUntil = 0;
}

// ZPL "^MNN" means continuous paper: gap sensing off. On die-cut labels each copy then starts a
// little lower than the one before (top cut off, the next copy's top printed at the bottom). A label
// driver set to "continuous" sends it with every job, and saved fliers keep it. This bridge is for
// labels, so on ZPL printers it is turned into ^MNW (labels with gaps) on the way to the printer.
void fixZplMedia(uint8_t *b, int n, MediaFix &s) {
  if (cfgLang != "zpl") return;
  for (int i = 0; i < n; i++) {
    uint8_t c = b[i];
    if (s.m == 3) { if (c == 'N') { b[i] = 'W'; s.fixed++; } s.m = c == '^' ? 1 : 0; continue; }
    s.m = c == '^' ? 1 : (s.m == 1 && c == 'M') ? 2 : (s.m == 2 && c == 'N') ? 3 : 0;
  }
}

void serviceFlierJob() {
  if (cancelPending && !jobLeft) {
    // ZPL printers (Rongta, Zebra-style) drop anything still queued in their memory on "~JA" (cancel all).
    // Sent only between copies, never inside one: a half-sent label can leave a printer stuck.
    cancelPending = false;
    if (cfgLang == "zpl" && printerReady) printerWrite((const uint8_t *)"~JA\r\n", 5);
  }
  if (!jobLeft) return;
  File f = FFat.open(flierPath(jobFlier));
  bool ok = (bool)f;
  MediaFix mf;
  while (ok) {
    int r = f.read(netBuf, sizeof netBuf);
    if (r <= 0) break;
    fixZplMedia(netBuf, r, mf);
    ok = printerWrite(netBuf, r);
    serviceButtons();                    // a press during the copy is noticed; the copy still finishes whole
  }
  if (f) f.close();
  if (!ok) { logEvent(flierLabel(jobFlier) + " FAILED (printer not ready)"); jobLeft = 0; return; }
  jobsPrinted++; jobDone++;
  if (!jobLeft) return;                  // stopped while this copy was going out (stopFlier logged it)
  jobLeft--;
  if (!jobLeft && jobDone > 1) drainUntil = millis() + min(jobDone * 3000UL, 180000UL);
  if (!jobLeft) logEvent("Printed " + flierLabel(jobFlier) + (jobDone > 1 ? " x " + String(jobDone) : String("")));
}

void serviceButtons() {
  static uint32_t downAt[FLIER_COUNT + 1] = {}, upAt[FLIER_COUNT + 1] = {};
  static int clicks[FLIER_COUNT + 1] = {};
  static bool held[FLIER_COUNT + 1] = {};
  static uint32_t lastAction = 0;
  // Wired buttons: GPIO pins. Each does what it's set to on the page (btnAct): by default
  // button N prints flier N. The button already says which flier, so its clicks say how many:
  // click 3 times = 3 copies (up to MAX_CLICKS). Hold 2 s = that flier's batch. Press while
  // printing = stop. (The BOOT button below is different: its clicks pick the flier.)
  auto act = [&](int i, bool hold, int count) {
    int a = btnAct[i];
    if (a == ACT_STOP) stopFlier();
    else if (a == ACT_REPRINT) reprintPending = min(reprintPending + count, MAX_CLICKS);
    else startFlier(a, hold ? flierBatch[constrain(a, 1, FLIER_COUNT)] : count);
  };
  for (int i = 1; i <= FLIER_COUNT; i++) {
    bool down = digitalRead(flierPins[i - 1]) == LOW;
    if (down && !downAt[i]) {                       // just pressed
      downAt[i] = millis(); held[i] = false;
      { char b[48]; snprintf(b, sizeof b, "Key %d pressed (GPIO %d)", i, flierPins[i - 1]); logNote(b); }
      if (batchBusy() && millis() - lastAction > 500) { stopFlier(); held[i] = true; clicks[i] = 0; lastAction = millis(); }  // any press stops a batch
    } else if (down && !held[i] && millis() - downAt[i] > 2000) {   // held 2 s: batch
      held[i] = true; clicks[i] = 0; lastAction = millis(); act(i, true, 1);
    } else if (!down && downAt[i]) {                // released: count the click
      if (!held[i] && millis() - downAt[i] > 30) {
        if (clicks[i] < MAX_CLICKS) clicks[i]++;
        upAt[i] = millis(); blinkUntil = millis() + 120;
      }
      downAt[i] = 0;
    }
    if (!down && clicks[i] && millis() - upAt[i] > 600) {   // clicking finished
      lastAction = millis(); act(i, false, clicks[i]); clicks[i] = 0;
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
    if (batchBusy()) { stopFlier(); bHeld = true; bClicks = 0; lastAction = millis(); }
  } else if (d && !bHeld && millis() - bDown > 2000) {
    bHeld = true; lastAction = millis();
    startFlier(min(bClicks + 1, FLIER_COUNT), flierBatch[min(bClicks + 1, FLIER_COUNT)]); bClicks = 0;
  } else if (!d && bDown) {
    if (!bHeld && millis() - bDown > 30) { bClicks++; bLastUp = millis(); blinkUntil = millis() + 120; }
    bDown = 0;
  }
  if (!d && bClicks && millis() - bLastUp > 600) {   // clicking finished
    lastAction = millis(); startFlier(min(bClicks, FLIER_COUNT), 1); bClicks = 0;
  }
}

extern bool setupMode;
void updateLed() {
  const uint8_t L = LED_BRIGHTNESS;
  if (millis() < blinkUntil) ledSet(L, L, L);                      // white flash: a button click was counted
  else if (millis() < redUntil) ledSet(L, 0, 0);                   // red flash: that flier slot is empty
  else if (booting) ledSet(0, L / 2, L / 2);                       // cyan: starting up
  else if (setupMode) ledSet(L / 2, 0, L);                         // purple: setup mode
  else if (!printerReady || printerTrouble) ledSet(L, 0, 0);       // red: no printer, or it stopped responding
  else if (batchBusy()) ledSet(0, 0, L);                           // blue: printing (any button stops it)
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
  MediaFix mf;
  if (!slot) lastBegin();
  while (c.connected() || c.available()) {
    int n = c.read(netBuf, sizeof netBuf);
    if (n > 0) {
      lastData = millis();
      fixZplMedia(netBuf, n, mf);
      if (slot) { if (ok && out.write(netBuf, n) != (size_t)n) ok = false; }
      else if (ok && !printerWrite(netBuf, n)) ok = false;
      total += n;
    } else {
      if (millis() - lastData > 15000) break;  // sender went quiet
      delay(2);
    }
  }
  c.stop();
  lastEnd(ok && total);
  if (mf.fixed) logNote("Changed continuous paper (^MNN) to labels with gaps in that job, so copies don't creep down");
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

// A small built-in font (5x7 dots per letter, scaled up) so the bridge can draw text into a
// picture. Some TSPL printers (Rollo, for one) only accept pictures: TSPL's TEXT, BOX and BARCODE
// commands print nothing on them and leave the printer stuck until it's turned off and on.
static const uint8_t FONT5X7[] PROGMEM = {
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x5F,0x00,0x00,0x00,0x07,0x00,0x07,0x00,0x14,0x7F,0x14,0x7F,0x14,
  0x24,0x2A,0x7F,0x2A,0x12,0x23,0x13,0x08,0x64,0x62,0x36,0x49,0x55,0x22,0x50,0x00,0x05,0x03,0x00,0x00,
  0x00,0x1C,0x22,0x41,0x00,0x00,0x41,0x22,0x1C,0x00,0x14,0x08,0x3E,0x08,0x14,0x08,0x08,0x3E,0x08,0x08,
  0x00,0x50,0x30,0x00,0x00,0x08,0x08,0x08,0x08,0x08,0x00,0x60,0x60,0x00,0x00,0x20,0x10,0x08,0x04,0x02,
  0x3E,0x51,0x49,0x45,0x3E,0x00,0x42,0x7F,0x40,0x00,0x42,0x61,0x51,0x49,0x46,0x21,0x41,0x45,0x4B,0x31,
  0x18,0x14,0x12,0x7F,0x10,0x27,0x45,0x45,0x45,0x39,0x3C,0x4A,0x49,0x49,0x30,0x01,0x71,0x09,0x05,0x03,
  0x36,0x49,0x49,0x49,0x36,0x06,0x49,0x49,0x29,0x1E,0x00,0x36,0x36,0x00,0x00,0x00,0x56,0x36,0x00,0x00,
  0x08,0x14,0x22,0x41,0x00,0x14,0x14,0x14,0x14,0x14,0x00,0x41,0x22,0x14,0x08,0x02,0x01,0x51,0x09,0x06,
  0x32,0x49,0x79,0x41,0x3E,0x7E,0x11,0x11,0x11,0x7E,0x7F,0x49,0x49,0x49,0x36,0x3E,0x41,0x41,0x41,0x22,
  0x7F,0x41,0x41,0x22,0x1C,0x7F,0x49,0x49,0x49,0x41,0x7F,0x09,0x09,0x09,0x01,0x3E,0x41,0x49,0x49,0x7A,
  0x7F,0x08,0x08,0x08,0x7F,0x00,0x41,0x7F,0x41,0x00,0x20,0x40,0x41,0x3F,0x01,0x7F,0x08,0x14,0x22,0x41,
  0x7F,0x40,0x40,0x40,0x40,0x7F,0x02,0x0C,0x02,0x7F,0x7F,0x04,0x08,0x10,0x7F,0x3E,0x41,0x41,0x41,0x3E,
  0x7F,0x09,0x09,0x09,0x06,0x3E,0x41,0x51,0x21,0x5E,0x7F,0x09,0x19,0x29,0x46,0x46,0x49,0x49,0x49,0x31,
  0x01,0x01,0x7F,0x01,0x01,0x3F,0x40,0x40,0x40,0x3F,0x1F,0x20,0x40,0x20,0x1F,0x3F,0x40,0x38,0x40,0x3F,
  0x63,0x14,0x08,0x14,0x63,0x07,0x08,0x70,0x08,0x07,0x61,0x51,0x49,0x45,0x43,0x00,0x7F,0x41,0x41,0x00,
  0x02,0x04,0x08,0x10,0x20,0x00,0x41,0x41,0x7F,0x00,0x04,0x02,0x01,0x02,0x04,0x40,0x40,0x40,0x40,0x40,
  0x00,0x01,0x02,0x04,0x00,0x20,0x54,0x54,0x54,0x78,0x7F,0x48,0x44,0x44,0x38,0x38,0x44,0x44,0x44,0x20,
  0x38,0x44,0x44,0x48,0x7F,0x38,0x54,0x54,0x54,0x18,0x08,0x7E,0x09,0x01,0x02,0x0C,0x52,0x52,0x52,0x3E,
  0x7F,0x08,0x04,0x04,0x78,0x00,0x44,0x7D,0x40,0x00,0x20,0x40,0x44,0x3D,0x00,0x7F,0x10,0x28,0x44,0x00,
  0x00,0x41,0x7F,0x40,0x00,0x7C,0x04,0x18,0x04,0x78,0x7C,0x08,0x04,0x04,0x78,0x38,0x44,0x44,0x44,0x38,
  0x7C,0x14,0x14,0x14,0x08,0x08,0x14,0x14,0x18,0x7C,0x7C,0x08,0x04,0x04,0x08,0x48,0x54,0x54,0x54,0x20,
  0x04,0x3F,0x44,0x40,0x20,0x3C,0x40,0x40,0x20,0x7C,0x1C,0x20,0x40,0x20,0x1C,0x3C,0x40,0x30,0x40,0x3C,
  0x44,0x28,0x10,0x28,0x44,0x0C,0x50,0x50,0x50,0x3C,0x44,0x64,0x54,0x4C,0x44,0x00,0x08,0x36,0x41,0x00,
  0x00,0x00,0x7F,0x00,0x00,0x00,0x41,0x36,0x08,0x00,0x08,0x04,0x08,0x10,0x08
};
// Draws s into a 1-bit picture (1 = black, rowb bytes per row), each font dot sc x sc pixels.
static void drawText(uint8_t *bm, int rowb, int W, int H, int x, int y, const String &s, int sc) {
  for (size_t i = 0; i < s.length(); i++, x += 6 * sc) {
    uint8_t ch = (uint8_t)s[i];
    if (ch < 32 || ch > 126) ch = '?';
    for (int c = 0; c < 5; c++) {
      uint8_t col = pgm_read_byte(&FONT5X7[(ch - 32) * 5 + c]);
      for (int r = 0; r < 7; r++) {
        if (!(col >> r & 1)) continue;
        for (int yy = y + r * sc; yy < y + (r + 1) * sc; yy++)
          for (int xx = x + c * sc; xx < x + (c + 1) * sc; xx++)
            if (xx >= 0 && yy >= 0 && xx < W && yy < H) bm[yy * rowb + (xx >> 3)] |= 0x80 >> (xx & 7);
      }
    }
  }
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
    // The text goes into the picture itself (see drawText): right-aligned in the same 360-dot box
    // ZPL uses for the item code, on up to two lines, and the note lines big down the label.
    if (text.length()) {
      int sc = text.length() <= 8 ? 7 : text.length() <= 24 ? 5 : 3, per = 360 / (6 * sc);
      for (int ln = 0; ln < 2 && ln * per < (int)text.length(); ln++) {
        String part = text.substring(ln * per, (ln + 1) * per);
        drawText(d.out, LBL_ROWB, LBL_W, LBL_H, 790 - (int)part.length() * 6 * sc, 412 + ln * 9 * sc, part, sc);
      }
    }
    for (int i = 0; i < nLines; i++) {
      int sc = min(i < 2 ? 12 : 9, (LBL_W - 60) / (6 * max(1, (int)noteLines[i].length())));
      drawText(d.out, LBL_ROWB, LBL_W, LBL_H, 30, 160 + i * 120, noteLines[i], max(sc, 2));
    }
    if (!puts_("SIZE 101.6 mm,152.4 mm\r\nGAP 3 mm,0 mm\r\nDIRECTION 0\r\nCLS\r\n")) return false;
    for (int y0 = 0; y0 < LBL_H; y0 += BAND) {
      int rows = min(BAND, LBL_H - y0);
      if (blank(y0, rows)) continue;
      if (!puts_("BITMAP 0," + String(y0) + "," + String(LBL_ROWB) + "," + String(rows) + ",0,")) return false;
      for (int i = y0 * LBL_ROWB, e = (y0 + rows) * LBL_ROWB; i < e; i++) { char c = (char)(~d.out[i]); if (!put(&c, 1)) return false; }  // TSPL: 0 = black
      if (!puts_("\r\n") || !flush()) return false;
      delay(15);
    }
    return puts_("PRINT 1,1\r\n") && flush();
  }
  // Set the printer up first, the way a Windows label driver does before every job: label with
  // gaps (^MNW), its width and length, normal backfeed, no offsets. Without this the label
  // inherits whatever the last job left behind. A driver set to "continuous" media (^MNN) turns
  // gap sensing off, and order labels then start wherever the paper happens to be: top cut off.
  if (!puts_("^XA^SZ2^JMA^MCY^PMN^PW" + String(LBL_W) + "^MNW^LL" + String(LBL_H) + "~JSN^JZY^LH0,0^LRN^XZ")) return false;
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
  lastBegin();
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
  lastEnd(ok);
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
.ok{color:#137333;font-weight:600}.bad{color:#b3261e;font-weight:600}small{color:#666}.fl{background:#f6f5f1;border:2px solid #cfcbc2;border-radius:12px;padding:12px 14px;margin-top:14px}.fl>b:first-of-type{font-size:18px}code{background:#eee;padding:2px 5px;border-radius:4px}:root{color-scheme:light dark}@media(prefers-color-scheme:dark){body{background:#121417;color:#eceef1}.card{background:#1b1e23;border-color:#2e333a}.fl{background:#22262c;border-color:#3a4048}small{color:#9aa1ab}code{background:#2e333a}input,textarea,select{background:#121417;color:#eceef1;border-color:#3a4048!important}.ok{color:#5fcf8f}.bad{color:#f08a5d}a{color:#f2c200}}</style><h1>BTL Print Bridge</h1>)HTML";

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

// "Stamp & print labels" card. The browser opens a PDF of 4x6 labels with the free PDF.js
// reader (loaded from cdnjs only when a file is picked), draws the item code onto each label
// and posts the finished picture to /label. The PDF never leaves this browser and this board.
const char STAMP_TOOL[] PROGMEM = R"HTML(<div class=card><b>Stamp &amp; print labels</b> <small>(if you haven't set up automatic labels)</small><br><small>Automatic labels (the print queue in Settings) add item codes like <b>G3</b> by themselves.
Selling somewhere that isn't set up for that yet? Download that site's labels as a 4x6 PDF, pick it here, type a code for each label, and print. The PDF is opened by this browser and sent only to this bridge, nowhere else.</small>
<input type=file id=sf accept=".pdf,application/pdf" style="margin-top:10px">
<div id=sx hidden>
<div style="display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-top:10px"><span>All labels</span>
<input id=sa placeholder=G3 maxlength=24 style="width:110px;text-transform:uppercase">
<button id=sb type=button style="margin:0;background:#666">Set all</button><button id=se type=button style="margin:0;background:#666">Fill empty ones</button></div>
<div style="display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-top:8px"><span>Text size</span><input id=sz type=range min=12 max=48 value=23 style="width:140px;padding:0"><small id=szv></small>
<button id=sr type=button style="margin:0;background:#999">Reset spot</button></div>
<small>Click on any label to move the code there (the right edge of the code goes where you click).</small>
<div id=sg style="display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:10px;margin-top:10px"></div>
<div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap"><button id=sp type=button>Print labels</button><button id=ss type=button style="background:#999" hidden>Stop</button><small id=sm></small></div></div>
<small id=sn style="display:block;margin-top:8px"></small></div>
<script>(function(){
var $=function(i){return document.getElementById(i)},P=[],lastAll='',stop=false,busy=false,CDN='https://cdnjs.cloudflare.com/ajax/libs/pdf.js/3.11.174/';
var DEF={x:790/812,y:390/1218};
function st(k,v){try{if(v===undefined){var r=localStorage.getItem('btl_st_'+k);return r===null?null:JSON.parse(r)}localStorage.setItem('btl_st_'+k,JSON.stringify(v))}catch(e){return null}}
var pos=st('pos')||{x:DEF.x,y:DEF.y},size=st('size')||23;$('sz').value=size;$('szv').textContent=size+' pt';
function clean(v){return(v||'').toUpperCase().replace(/[^\x20-\x7E]/g,'').replace(/\s+/g,' ').trim().slice(0,24)}
function load(u){return new Promise(function(ok,no){var s=document.createElement('script');s.src=u;s.onload=ok;s.onerror=no;document.head.appendChild(s)})}
function libs(){return window.pdfjsLib?Promise.resolve():load(CDN+'pdf.min.js').then(function(){return load(CDN+'pdf.worker.min.js')})}
function px(p){return size*Math.max(p.cv.width,p.cv.height)/432}
function paint(p){var o=p.ov,r=p.cv.clientWidth/p.cv.width;o.textContent=p.code;o.style.display=p.code?'':'none';
o.style.left=pos.x*100+'%';o.style.top=pos.y*100+'%';o.style.fontSize=px(p)*r+'px';p.in.style.borderColor=p.code?'':'#d9471f'}
function all(){P.forEach(paint);var n=P.filter(function(p){return p.code}).length;
$('sm').textContent=P.length?(n==P.length?(n==1?'The label has a code.':'All '+P.length+' labels have a code.'):n+' of '+P.length+' labels have a code. The rest print without one.'):''}
function bw(cv){var x=cv.getContext('2d'),d=x.getImageData(0,0,cv.width,cv.height),a=d.data;
for(var i=0;i<a.length;i+=4){var v=(a[i]*77+a[i+1]*150+a[i+2]*29)>>8<160?0:255;a[i]=a[i+1]=a[i+2]=v;a[i+3]=255}x.putImageData(d,0,0)}
$('sf').onchange=function(){var f=this.files[0];if(!f)return;$('sn').textContent='Opening '+f.name+'...';
libs().then(function(){return f.arrayBuffer()}).then(function(b){return pdfjsLib.getDocument({data:new Uint8Array(b)}).promise}).then(function(doc){
P=[];$('sg').innerHTML='';$('sx').hidden=false;var i=1;
function next(){if(i>doc.numPages){$('sn').textContent=doc.numPages+' label'+(doc.numPages>1?'s':'')+' from '+f.name;all();return}
return doc.getPage(i).then(function(pg){var v1=pg.getViewport({scale:1}),vp=pg.getViewport({scale:1218/Math.max(v1.width,v1.height)});
var cv=document.createElement('canvas');cv.width=Math.round(vp.width);cv.height=Math.round(vp.height);
cv.style.cssText='display:block;width:100%;height:auto;cursor:crosshair';
var box=document.createElement('div'),wrap=document.createElement('div'),ov=document.createElement('div'),inp=document.createElement('input');
wrap.style.cssText='position:relative;border:1px solid #ccc;border-radius:4px;overflow:hidden;background:#fff';
ov.style.cssText='position:absolute;transform:translateX(-100%);white-space:nowrap;pointer-events:none;font-weight:700;font-family:Arial,Helvetica,sans-serif;line-height:1;background:#fff;color:#000;outline:2px solid #fff';
inp.placeholder='Label '+i+' code';inp.maxLength=24;inp.style.cssText='margin-top:6px;text-transform:uppercase;padding:8px';inp.setAttribute('aria-label','Code for label '+i);
wrap.appendChild(cv);wrap.appendChild(ov);box.appendChild(wrap);box.appendChild(inp);$('sg').appendChild(box);
var p={cv:cv,ov:ov,in:inp,code:''};P.push(p);var k=P.length;
inp.oninput=function(){p.code=clean(inp.value);all()};
inp.onkeydown=function(e){if(e.key=='Enter'){e.preventDefault();if(P[k])P[k].in.focus()}};
cv.onclick=function(e){var r=cv.getBoundingClientRect();pos={x:Math.min(1,Math.max(0,(e.clientX-r.left)/r.width)),y:Math.min(1,Math.max(0,(e.clientY-r.top)/r.height))};st('pos',pos);all()};
return pg.render({canvasContext:cv.getContext('2d'),viewport:vp}).promise.then(function(){bw(cv);paint(p);i++;return next()})})}
return next()}).catch(function(e){$('sn').textContent='Could not open that PDF'+(window.pdfjsLib?'':' (the PDF reader needs internet on this computer)')+'.'})};
function setAll(only){var c=clean($('sa').value);P.forEach(function(p){if(!only||!p.code){p.code=c;p.in.value=c}});lastAll=c;all()}
$('sb').onclick=function(){setAll(false)};$('se').onclick=function(){setAll(true)};
$('sa').oninput=function(){var c=clean(this.value);P.forEach(function(p){if(!p.code||p.code==lastAll){p.code=c;p.in.value=c}});lastAll=c;all()};
$('sa').onkeydown=function(e){if(e.key=='Enter'){e.preventDefault();setAll(true)}};
$('sz').oninput=function(){size=+this.value;$('szv').textContent=size+' pt';st('size',size);all()};
$('sr').onclick=function(){pos={x:DEF.x,y:DEF.y};st('pos',pos);all()};
window.addEventListener('resize',function(){P.forEach(paint)});
function stamped(p){var c=document.createElement('canvas');c.width=p.cv.width;c.height=p.cv.height;var x=c.getContext('2d');x.drawImage(p.cv,0,0);
if(p.code){var s=px(p);x.font='700 '+s+'px Arial,Helvetica,sans-serif';x.textBaseline='top';var w=x.measureText(p.code).width,X=pos.x*c.width,Y=pos.y*c.height,pd=s*0.12;
x.fillStyle='#fff';x.fillRect(X-w-pd,Y-pd,w+2*pd,s+2*pd);x.fillStyle='#000';x.textAlign='right';x.fillText(p.code,X,Y)}
return new Promise(function(ok){c.toBlob(ok,'image/png')})}
$('ss').onclick=function(){stop=true;$('sm').textContent='Stopping after this label...'};
$('sp').onclick=function(){if(busy||!P.length)return;busy=true;stop=false;$('sp').disabled=true;$('ss').hidden=false;var i=0;
function done(t){busy=false;$('sp').disabled=false;$('ss').hidden=true;$('sm').textContent=t}
(function one(){if(stop)return done('Stopped after '+i+' of '+P.length+'.');if(i>=P.length)return done('Printed all '+P.length+'.');
$('sm').textContent='Printing '+(i+1)+' of '+P.length+'...';
stamped(P[i]).then(function(b){var f=new FormData();f.append('f',b,'label.png');return fetch('/label',{method:'POST',body:f})})
.then(function(r){return r.text().then(function(t){if(!r.ok)throw t;i++;one()})})
.catch(function(t){done('Label '+(i+1)+' did not print: '+(typeof t=='string'?t:'could not reach the bridge.'))})})()};
})();</script>)HTML";

// "Make a flier from a file" card. The browser turns a PDF (every page) or a picture into labels in the
// printer's language, set under Settings, and stores them in a flier slot through /flier-put.
// The file never leaves this browser and this board. PDF.js comes from cdnjs only when a PDF is picked.
const char FILE_TOOL[] PROGMEM = R"HTML(<div class=card><b>Make a flier from a file</b><br><small>Pick a PDF or a picture (PNG, JPG). Each PDF page becomes one label. It's turned into black and white and saved in the slot you pick, ready for the buttons.</small>
<input type=file id=uf accept=".pdf,application/pdf,image/png,image/jpeg,image/webp" style="margin-top:10px">
<div id=ux hidden><div style="display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-top:10px">
<span>Label</span><input id=uw type=number step=0.1 min=0.5 value=4 style=width:70px><span>&times;</span><input id=uh type=number step=0.1 min=0.5 value=6 style=width:70px><span>in</span>
<select id=ud style="padding:9px;border-radius:8px" title="Printer resolution"><option value=203>203 dpi</option><option value=300>300 dpi</option></select></div>
<div style="display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-top:8px"><span>Save as</span><select id=us style="padding:9px;border-radius:8px"></select>
<input id=un placeholder="Name" maxlength=30 style="flex:1;min-width:120px"></div>
<div style="display:flex;gap:8px;align-items:center;margin-top:8px"><span>Darkness</span><input id=ut type=range min=60 max=230 value=160 style="width:140px;padding:0"></div>
<div id=ug style="display:grid;grid-template-columns:repeat(auto-fill,minmax(110px,1fr));gap:8px;margin-top:10px"></div>
<button id=uv type=button>Save flier</button> <small id=um></small></div></div>
<script>(function(){
var $=function(i){return document.getElementById(i)},CDN='https://cdnjs.cloudflare.com/ajax/libs/pdf.js/3.11.174/',SRC=[],META={fliers:[]};
function st(k,v){try{if(v===undefined)return localStorage.getItem('btl_u'+k);localStorage.setItem('btl_u'+k,v)}catch(e){return null}}
['w','h','d'].forEach(function(k){var v=st(k);if(v)$('u'+k).value=v});
function load(u){return new Promise(function(ok,no){var s=document.createElement('script');s.src=u;s.onload=ok;s.onerror=no;document.head.appendChild(s)})}
function libs(){return window.pdfjsLib?Promise.resolve():load(CDN+'pdf.min.js').then(function(){return load(CDN+'pdf.worker.min.js')})}
function slots(){fetch('/fliers.json',{cache:'no-store'}).then(function(r){return r.json()}).then(function(j){META=j;var s=$('us'),have={};s.textContent='';
j.fliers.forEach(function(f){have[f.n]=f});for(var n=1;n<=7;n++){var o=document.createElement('option');o.value=n;o.textContent='Flier '+n+(have[n]?' (replaces '+(have[n].name||'what\'s there')+')':' (empty)');s.appendChild(o)}
for(n=1;n<=7;n++)if(!have[n]){s.value=n;break}})}
function size(){var dpi=+$('ud').value;return{dpi:dpi,W:Math.ceil(Math.round((parseFloat($('uw').value)||4)*dpi)/8)*8,H:Math.round((parseFloat($('uh').value)||6)*dpi)}}
function draw(){var z=size(),g=$('ug');g.textContent='';SRC.forEach(function(src){var c=document.createElement('canvas');c.width=z.W;c.height=z.H;
var x=c.getContext('2d'),sw=src.width,sh=src.height,rot=(sw>sh)!=(z.W>z.H);x.fillStyle='#fff';x.fillRect(0,0,z.W,z.H);x.save();
if(rot){x.translate(z.W,0);x.rotate(Math.PI/2)}var aw=rot?z.H:z.W,ah=rot?z.W:z.H,k=Math.min(aw/sw,ah/sh);
x.drawImage(src,(aw-sw*k)/2,(ah-sh*k)/2,sw*k,sh*k);x.restore();var d=x.getImageData(0,0,z.W,z.H),a=d.data,t=+$('ut').value;
for(var i=0;i<a.length;i+=4){var v=(a[i]*77+a[i+1]*150+a[i+2]*29)>>8<t?0:255;a[i]=a[i+1]=a[i+2]=v;a[i+3]=255}x.putImageData(d,0,0);
c.style.cssText='width:100%;border:1px solid #ccc;border-radius:4px;background:#fff';g.appendChild(c)})}
function save(){['w','h','d'].forEach(function(k){st(k,$('u'+k).value)});draw()}
['uw','uh','ud','ut'].forEach(function(i){$(i).addEventListener('input',save)});
$('uf').onchange=function(){var f=this.files[0];if(!f)return;$('um').textContent='Opening '+f.name+'...';SRC=[];
if(!$('un').value)$('un').value=f.name.replace(/\.[^.]+$/,'').slice(0,30);
var go=/pdf$/i.test(f.type)||/\.pdf$/i.test(f.name)?libs().then(function(){return f.arrayBuffer()}).then(function(b){return pdfjsLib.getDocument({data:new Uint8Array(b)}).promise}).then(function(doc){
var i=1;function next(){if(i>doc.numPages||i>20)return;return doc.getPage(i).then(function(pg){var v1=pg.getViewport({scale:1}),vp=pg.getViewport({scale:1800/Math.max(v1.width,v1.height)}),c=document.createElement('canvas');
c.width=Math.round(vp.width);c.height=Math.round(vp.height);return pg.render({canvasContext:c.getContext('2d'),viewport:vp}).promise.then(function(){SRC.push(c);i++;return next()})})}return next()})
:new Promise(function(ok,no){var im=new Image();im.onload=function(){SRC.push(im);ok()};im.onerror=no;im.src=URL.createObjectURL(f)});
go.then(function(){$('ux').hidden=false;slots();draw();$('um').textContent=SRC.length+' label'+(SRC.length>1?'s':'')+' from '+f.name}).catch(function(){$('um').textContent='Could not open that file'+(window.pdfjsLib||!/pdf/i.test(f.name)?'':' (the PDF reader needs internet on this computer)')+'.'})};
function job(){var z=size(),W=z.W,H=z.H,bpr=W/8,B=32,zpl=(META.lang||'zpl')!='tspl',out=[],mm=function(v){return(v*25.4).toFixed(1)};
if(zpl)out.push('^XA^SZ2^JMA^MCY^PMN^PW'+W+'^MNW^LL'+H+'~JSN^JZY^LH0,0^LRN^XZ');
[].forEach.call($('ug').children,function(c){var d=c.getContext('2d').getImageData(0,0,W,H).data,b=new Uint8Array(bpr*H);
for(var y=0;y<H;y++)for(var i=0;i<W;i++)if(d[(y*W+i)*4]<128)b[y*bpr+(i>>3)]|=0x80>>(i&7);
if(zpl){out.push('^XA^PW'+W+'^LL'+H+'^LH0,0');for(var y0=0;y0<H;y0+=B){var r=Math.min(B,H-y0),s=b.subarray(y0*bpr,(y0+r)*bpr);if(!s.some(function(v){return v}))continue;
var h='';for(var j=0;j<s.length;j++)h+=(s[j]<16?'0':'')+s[j].toString(16);out.push('^FO0,'+y0+'^GFA,'+s.length+','+s.length+','+bpr+','+h.toUpperCase()+'^FS')}out.push('^XZ')}
else{out.push('SIZE '+mm(W/z.dpi)+' mm,'+mm(H/z.dpi)+' mm\r\nGAP 3 mm,0 mm\r\nDIRECTION 0\r\nCLS\r\n');for(var y0=0;y0<H;y0+=B){var r=Math.min(B,H-y0),s=b.slice(y0*bpr,(y0+r)*bpr);
if(!s.some(function(v){return v}))continue;for(var j=0;j<s.length;j++)s[j]^=0xFF;out.push('BITMAP 0,'+y0+','+bpr+','+r+',0,',s,'\r\n')}out.push('PRINT 1,1\r\n')}});
return new Blob(out)}
$('uv').onclick=function(){var n=+$('us').value,m=$('um'),btn=this,old=META.fliers.filter(function(f){return f.n==n})[0];if(!SRC.length)return;
if(old&&!confirm('Replace Flier '+n+(old.name?' "'+old.name+'"':'')+' with this file?'))return;
var d=new FormData();d.append('n',n);d.append('name',$('un').value);d.append('batch',old?old.batch:10);d.append('f',job(),'flier.bin');
btn.disabled=true;m.textContent='Saving...';fetch('/flier-put',{method:'POST',body:d}).then(function(r){return r.text()}).then(function(t){if(t!=='ok')throw t;m.textContent='Saved as Flier '+n+'. Reloading...';setTimeout(function(){location.reload()},800)})
.catch(function(t){m.textContent=typeof t=='string'?t:'Could not reach the bridge.';btn.disabled=false})};
})();</script>)HTML";

// "Firmware" card: the browser checks GitHub for a newer version, downloads it over HTTPS and
// uploads it to /update. A .bin file can also be uploaded by hand.
// "Copy or back up fliers" card. The browser does the work: it reads each flier from this board
// (/flier-file) and posts it to another board's /flier-put, or packs them into one backup file.
const char COPY_TOOL[] PROGMEM = R"HTML(<div class=card><b>Copy or back up fliers</b><br>
<small>Copies go into the same click slots and replace what's there. Fliers only print correctly on the same kind of printer they were saved for.</small>
<label>Which fliers</label><div id=ck style="display:flex;flex-direction:column;gap:2px"><small>Loading&hellip;</small></div>
<label>Send them to another board</label>
<div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap"><select id=cb style="flex:1;min-width:180px;padding:10px;border:1px solid #ccc;border-radius:8px;font:inherit"><option value="">Searching&hellip;</option></select>
<button id=cf type=button style="margin:0;background:#666">Search again</button></div>
<input id=ca placeholder="or type its address, e.g. 192.168.1.50" style="margin-top:8px" aria-label="Other board's address">
<button id=cs type=button>Send fliers</button> <small id=cm></small>
<label style="margin-top:16px">Backup file</label>
<small>Save the ticked fliers to one file on this computer, or load a saved file onto this board.</small><br>
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
function nm(f){return 'Flier '+f.n+(f.name?' "'+f.name+'"':'')}
function boxes(){list().then(function(j){var c=$('ck');c.textContent='';if(!j.fliers.length){c.innerHTML='<small>No fliers saved on this board yet.</small>';return}
j.fliers.forEach(function(f){var l=document.createElement('label'),i=document.createElement('input');l.style.cssText='display:flex;gap:8px;align-items:center;margin:2px 0;font-weight:400';
i.type='checkbox';i.checked=true;i.value=f.n;i.style.width='auto';l.appendChild(i);l.appendChild(document.createTextNode(nm(f)));c.appendChild(l)})}).catch(function(){$('ck').innerHTML='<small>Could not read the fliers.</small>'})}
boxes();
function picked(fl){var on={};[].forEach.call(document.querySelectorAll('#ck input:checked'),function(i){on[i.value]=1});return fl.filter(function(f){return on[f.n]})}
function find(){var s=$('cb');s.innerHTML='<option value="">Searching&hellip;</option>';
fetch('/boards',{cache:'no-store'}).then(function(r){return r.json()}).then(function(b){s.innerHTML=b.length?'':'<option value="">No other boards found</option>';
b.forEach(function(x){var o=document.createElement('option');o.value=x.ip;o.textContent=x.name+' ('+x.ip+')';s.appendChild(o);
fetch('http://'+x.ip+'/fliers.json',{cache:'no-store'}).then(function(r){return r.json()}).then(function(j){o.textContent=x.name+' ('+x.ip+') \u00b7 '+(j.printer||'no printer plugged in')}).catch(function(){})})}).catch(function(){s.innerHTML='<option value="">Search failed</option>'})}
$('cf').onclick=find;find();
$('cs').onclick=function(){var m=$('cm'),t=($('ca').value.trim()||$('cb').value).replace(/^https?:\/\//,'').replace(/\/+$/,'');
if(!t){m.textContent='Pick a board or type its address first.';return}
if(t==location.host){m.textContent="That's this board.";return}
var base='http://'+t,fl,btn=this;
var me;list().then(function(j){me=j;fl=picked(j.fliers);if(!j.fliers.length)throw 'No fliers saved on this board yet.';if(!fl.length)throw 'Tick at least one flier first.';
return fetch(base+'/fliers.json',{cache:'no-store'}).then(function(r){return r.json()}).catch(function(){throw "Couldn't reach that board. Is it on, on the same Wi-Fi, and updated?"})})
.then(function(o){var old={};(o.fliers||[]).forEach(function(f){old[f.n]=f});
var msg='Send to '+(o.board||t)+' ('+(o.printer||'no printer plugged in')+')?\n\nCopying: '+fl.map(nm).join(', ')+'\n\nOn that board this replaces: '+fl.map(function(f){return old[f.n]?nm(old[f.n]):'Flier '+f.n+' (empty)'}).join(', ');
if(o.lang&&me.lang&&o.lang!=me.lang)msg+='\n\nCareful: that board is set to '+o.lang.toUpperCase()+' and this one to '+me.lang.toUpperCase()+', so these fliers will probably print garbage there.';
if(!confirm(msg))throw 'Cancelled.';
btn.disabled=true;return each(fl,function(f,i){m.textContent='Sending '+(i+1)+' of '+fl.length+'...';return get(f.n).then(function(b){return put(base,f,b)})})})
.then(function(){m.textContent='Done. Sent '+fl.length+' flier(s).'}).catch(say(m,"Sending failed. Check the other board is on and try again.")).then(function(){btn.disabled=false})};
$('bd').onclick=function(){var m=$('bm'),meta,parts=[];
list().then(function(j){meta=j;j.fliers=picked(j.fliers);if(!j.fliers.length)throw 'Tick at least one flier first.';m.textContent='Reading fliers...';
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

// "Button test" card: shows each button lighting up live while you press it, straight from the pins.
const char PIN_TOOL[] PROGMEM = R"HTML(<div class=card><b>Button test</b><br><small>Press each button: its box should turn green while it's held down and go grey when you let go.
A box that never turns green means that button's wire isn't reaching its pin (or its ground side isn't connected). A box that stays green means that pin is touching ground all the time.</small>
<div id=pt style="display:flex;flex-wrap:wrap;gap:8px;margin-top:10px"></div><small id=pm></small></div>
<script>(function(){var box=document.getElementById('pt'),m=document.getElementById('pm');
function cell(t,s,a){return '<div style="min-width:64px;padding:8px 6px;border-radius:8px;text-align:center;font-weight:700;background:'+(a?'#2e7d32':'#ddd')+';color:'+(a?'#fff':'#333')+'">'+t+'<br><small style="font-weight:400;color:inherit">'+s+'</small></div>'}
function tick(){if(document.hidden){setTimeout(tick,1000);return}fetch('/pins',{cache:'no-store'}).then(function(r){return r.json()}).then(function(j){
var h=cell('BOOT','GPIO 0',j.boot);j.keys.forEach(function(k,i){h+=cell('Key '+(i+1),'GPIO '+j.pins[i],k)});box.innerHTML=h;m.textContent='';setTimeout(tick,150)})
.catch(function(){m.textContent='Lost contact with the board, retrying...';setTimeout(tick,1000)})}tick();})();</script>)HTML";

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
  String j = "{\"board\":\"" + hostName + "\",\"version\":\"" FIRMWARE_VERSION "\",\"printer\":\"" + jsonEsc(printerReady ? printerName : String("")) + "\",\"lang\":\"" + jsonEsc(cfgLang) + "\",\"fliers\":[";
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

void handleButtons() {
  Preferences p; p.begin("btl", false);
  for (int i = 1; i <= FLIER_COUNT; i++) {
    int a = web.arg("a" + String(i)).toInt();
    if (a == ACT_REPRINT || a == ACT_STOP || (a >= 1 && a <= FLIER_COUNT)) { btnAct[i] = a; p.putInt(("a" + String(i)).c_str(), a); }
  }
  p.end();
  logEvent("Saved what the extra buttons do");
  web.sendHeader("Location", "/");
  web.send(303);
}

// Live state of every button pin, for the "Button test" card: 1 = pressed (pin pulled to GND).
void handlePins() {
  String j = "{\"boot\":" + String(digitalRead(0) == LOW ? 1 : 0) + ",\"keys\":[";
  for (int i = 0; i < FLIER_COUNT; i++) j += String(i ? "," : "") + String(digitalRead(flierPins[i]) == LOW ? 1 : 0);
  j += "],\"pins\":[";
  for (int i = 0; i < FLIER_COUNT; i++) j += String(i ? "," : "") + String(flierPins[i]);
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json", j + "]}");
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

// Saved fliers: instant feedback. "Save next print here" is sent in the background so the page
// doesn't reload (that took a few seconds and lost your place); the note under that flier says what
// to do next, and the page reloads by itself once the flier is saved. Other flier buttons show
// "Working..." straight away and come back to the same flier.
#define WAIT_NOTE "Waiting: print the flier from your computer to this printer now. It will be saved here instead of printed."
const char FLIER_JS[] PROGMEM = R"HTML(<script>(function(){var W=')HTML" WAIT_NOTE R"HTML(',poll=0;
function watch(){if(poll)return;poll=setInterval(function(){fetch('/status').then(function(r){return r.text()}).then(function(t){
if(t.indexOf('Waiting for your next print')<0){clearInterval(poll);poll=0;location.reload()}}).catch(function(){})},2000)}
document.querySelectorAll('form.fl').forEach(function(f){f.addEventListener('submit',function(e){var b=e.submitter;if(!b)return;
var d=new FormData(f);d.append(b.name,b.value);
if(b.value=='save'){e.preventDefault();b.disabled=true;b.textContent='Saving...';
fetch('/flier',{method:'POST',body:new URLSearchParams(d),redirect:'manual'}).then(function(){
document.querySelectorAll('form.fl small[id^=fw]').forEach(function(s){s.textContent=''});
document.querySelectorAll('form.fl .sv').forEach(function(x){x.textContent='Save next print here';x.disabled=false});
b.textContent='Waiting for your print';b.disabled=true;document.getElementById('fw'+f.n.value).textContent=W;watch()})
.catch(function(){b.disabled=false;b.textContent='Save next print here';alert('Could not reach the bridge. Try again.')});return}
setTimeout(function(){b.disabled=true;b.textContent='Working...'},0)})});
if(document.querySelector('form.fl small[id^=fw]:not(:empty)'))watch()})()</script>)HTML";

void handleRoot() {
  String h = FPSTR(PAGE_HEAD);
  h += "<div class=card id=st>" + statusHtml() + "</div>"
       "<script>setInterval(()=>fetch('/status').then(r=>r.text()).then(t=>document.getElementById('st').innerHTML=t).catch(()=>{}),3000)</script>";
  if (!setupMode) {
    // Each board's page gets its own name, so bookmarks for two boards are told apart
    h.replace("<title>BTL Print Bridge</title>", "<title>BTL Print Bridge (" + hostName + ")</title>");
    String ip = WiFi.localIP().toString();
    h += "<p style=\"margin:-4px 4px 12px\"><small><b>Bookmark this page</b> (Ctrl+D; on a phone, Share &rarr; Add to Home Screen). "
         "Its address never changes: <a href=\"http://" + hostName + ".local/\">" + hostName + ".local</a>. "
         "If that doesn't open on a device (some Android phones), use <a href=\"http://" + ip + "/\">" + ip + "</a>, which can change if your router restarts.</small></p>";
  }
  // Firmware card right under the status, so a waiting update is the first thing you see
  if (!setupMode) { String fw = FPSTR(FW_TOOL); fw.replace("%VER%", FIRMWARE_VERSION); h += fw; }
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
         "If neither works, that's OK: add the bridge to your computer (above) and print a test page with your printer's own driver. "
         "If the printer stops printing after a test, turn it off and on.</small><br>"
         "<form method=post action=/test style=\"display:inline\"><button>Print ZPL test</button></form> "
         "<form method=post action=/test-tspl style=\"display:inline\"><button>Print TSPL test</button></form></div>";
    h += FPSTR(TEXT_TOOL);
    h += FPSTR(STAMP_TOOL);
    h += "<div class=card><b>Saved fliers</b><br><small>To save one: click <b>Save next print here</b> under the flier you want, then print the flier from your computer to this printer "
         "within " + String(CAPTURE_MINUTES) + " minutes. It's stored instead of printed.<br>"
         "To print it: click the board's <b>BOOT</b> button 1 to " + String(FLIER_COUNT) + " times quickly for Flier 1 to " + String(FLIER_COUNT) + ". "
         "Hold the last click for 2 seconds to print the batch, and press again to stop.<br>"
         "Optional extra buttons: wire each one to a pin and to GND, then choose what it does under <b>Extra buttons</b> below.</small>";
    if (!fsReady) h += "<p class=bad>Storage isn't available on this board.</p>";
    for (int n = 1; n <= FLIER_COUNT; n++) {
      size_t z = flierSize(n);
      String dis = z ? "" : " disabled";
      h += "<form method=post action=/flier class=fl id=f" + String(n) + "><input type=hidden name=n value=" + String(n) + ">"
           "<b>Flier " + String(n) + "</b> <small>(" + String(n) + (n == 1 ? " click" : " clicks") + ")</small>: " + (z ? htmlEsc(flierLabel(n)) + " <small>(" + String(z / 1024 + 1) + " KB)</small>" : String("<small>empty</small>")) +
           "<div style=\"display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:6px\">"
           "<input name=copies type=number min=1 max=999 value=1 style=\"width:80px\"" + dis + " aria-label=\"Copies to print now\"> "
           "<button name=do value=print style=margin:0" + dis + ">Print</button></div>"
           "<div style=\"display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:8px\"><small>Batch (hold the last click):</small>"
           "<input name=batch type=number min=1 max=999 value=" + String(flierBatch[n]) + " style=\"width:80px\" aria-label=\"Copies when you hold the button\">"
           "<small>copies</small><button name=do value=rename style=\"margin:0;background:#666\">Save</button></div>"
           "<label>Name</label><input name=name maxlength=30 placeholder=\"e.g. Menu\" value=\"" + htmlEsc(flierName[n]) + "\">"
           "<button name=do value=rename style=\"background:#666\">Save name</button> "
           "<button name=do value=save class=sv style=\"background:#2563c9\">Save next print here</button> "
           "<button name=do value=delete style=\"background:#999\"" + dis + ">Delete flier</button>"
           "<div style=\"display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:8px\"><small>Move to:</small>"
           "<select name=to style=\"padding:10px;border:1px solid #ccc;border-radius:8px;font:inherit\"" + dis + " aria-label=\"Slot to move this flier to\">" + moveOptions(n) + "</select>"
           "<button name=do value=move style=\"margin:0;background:#666\"" + dis + ">Move</button></div>"
           "<small class=ok id=fw" + String(n) + ">" + (captureSlot == n && millis() < captureUntil ? String(WAIT_NOTE) : String("")) + "</small></form>";
    }
    h += FPSTR(FLIER_JS);
    h += "</div>";
    if (fsReady) { h += FPSTR(FILE_TOOL); h += FPSTR(COPY_TOOL); }
    h += "<div class=card><b>Extra buttons</b> <small>(optional)</small><br><small>Wire a push button between the pin shown and <b>GND</b>. No resistors needed. "
         "Click to print: click 3 times for 3 copies (up to " + String(MAX_CLICKS) + "). The light flashes white for each click it counts. "
         "Hold 2 seconds = that flier's batch. Press any button to stop. (The board's BOOT button is different: its clicks pick the flier.)</small><form method=post action=/buttons>";
    for (int i = 1; i <= FLIER_COUNT; i++) {
      h += "<div style=\"display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:8px\"><small style=\"min-width:150px\">Button " + String(i) + " (GPIO " + String(flierPins[i - 1]) + ")</small>"
           "<select name=a" + String(i) + " style=\"flex:1;min-width:180px;padding:10px;border:1px solid #ccc;border-radius:8px;font:inherit\" aria-label=\"What button " + String(i) + " does\">";
      for (int k = 1; k <= FLIER_COUNT; k++)
        h += "<option value=" + String(k) + (btnAct[i] == k ? " selected" : "") + ">Print " + (flierName[k].length() ? "Flier " + String(k) + ": " + htmlEsc(flierName[k]) : "Flier " + String(k)) + "</option>";
      h += "<option value=" + String(ACT_REPRINT) + (btnAct[i] == ACT_REPRINT ? " selected" : "") + ">Reprint the last label</option>"
           "<option value=" + String(ACT_STOP) + (btnAct[i] == ACT_STOP ? " selected" : "") + ">Stop printing</option></select></div>";
    }
    h += "<button>Save buttons</button></form><small>Reprint repeats the last label sent from a computer, this page or the order queue. "
         "While a button is set to Reprint, the board keeps that one label in memory until it restarts.</small></div>";
  }
  h += "<div class=card><form method=post action=/save><b>Settings</b>"
       "<label>Wi-Fi name</label><input name=ssid value=\"" + htmlEsc(cfgSsid) + "\" required>"
       "<label>Wi-Fi password</label><input name=pass type=password placeholder=\"(unchanged)\">"
       "<label>Print queue URL <small>(optional)</small></label><input name=qurl value=\"" + htmlEsc(cfgQueueUrl) + "\" placeholder=\"https://.../print\">"
       + (cfgQueueUrl.endsWith("/print") ? "<small><a href=\"" + htmlEsc(cfgQueueUrl.substring(0, cfgQueueUrl.length() - 6)) + "/admin\" target=_blank rel=noopener>Open the Label Autopilot settings page</a></small>" : String("")) +
       "<label>Queue token <small>(optional)</small></label><input name=qtok type=password placeholder=\"" + String(cfgQueueToken.length() ? "(unchanged)" : "") + "\">"
       "<label>Printer language <small>(for shipping labels from the queue)</small></label><select name=lang style=\"width:100%;padding:10px;border:1px solid #ccc;border-radius:8px;font:inherit\">"
       "<option value=zpl" + String(cfgLang == "zpl" ? " selected" : "") + ">ZPL (Zebra, RONGTA, most 4x6 printers)</option>"
       "<option value=tspl" + String(cfgLang == "tspl" ? " selected" : "") + ">TSPL (Rollo and many budget label printers)</option></select>"
       "<button>Save and restart</button></form></div>";
  h += FPSTR(PIN_TOOL);
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

// The TSPL test is sent as a picture, the one thing every TSPL printer understands.
void handleTestTspl() {
  const int W = 800, ROWB = W / 8, H = 232;
  uint8_t *bm = (uint8_t *)calloc(ROWB, H);
  bool ok = bm != nullptr;
  if (ok) {
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
      if (x < 4 || y < 4 || x >= W - 4 || y >= H - 4) bm[y * ROWB + (x >> 3)] |= 0x80 >> (x & 7);   // border
    drawText(bm, ROWB, W, H, 40, 36, "BTL Print Bridge", 7);
    drawText(bm, ROWB, W, H, 40, 110, "Wi-Fi printing works.", 5);
    drawText(bm, ROWB, W, H, 40, 170, "TSPL test (sent as a picture)", 4);
    for (int i = 0; i < ROWB * H; i++) bm[i] = ~bm[i];   // TSPL: 0 = black
    const char *head = "SIZE 100 mm,150 mm\r\nGAP 3 mm,0 mm\r\nCLS\r\n";
    ok = printerWrite((const uint8_t *)head, strlen(head));
    for (int y0 = 0; ok && y0 < H; y0 += 32) {
      int rows = min(32, H - y0);
      String cmd = "BITMAP 0," + String(40 + y0) + "," + String(ROWB) + "," + String(rows) + ",0,";
      ok = printerWrite((const uint8_t *)cmd.c_str(), cmd.length()) && printerWrite(bm + y0 * ROWB, rows * ROWB) && printerWrite((const uint8_t *)"\r\n", 2);
      delay(15);
    }
    if (ok) ok = printerWrite((const uint8_t *)"PRINT 1,1\r\n", 11);
    free(bm);
  }
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
  web.sendHeader("Location", n >= 1 && n <= FLIER_COUNT ? "/#f" + String(n) : String("/"));  // back to the same flier
  web.send(303);
}

// /raw: a print job uploaded from the page, streamed straight to the printer.
bool rawOk = false; size_t rawBytes = 0;
void handleRawUpload() {
  HTTPUpload &u = web.upload();
  if (u.status == UPLOAD_FILE_START) { rawOk = printerReady; rawBytes = 0; lastBegin(); }
  else if (u.status == UPLOAD_FILE_WRITE) { if (rawOk && !printerWrite(u.buf, u.currentSize)) rawOk = false; rawBytes += u.currentSize; }
  else if (u.status == UPLOAD_FILE_ABORTED) lastEnd(false);
}
void handleRawDone() {
  lastEnd(rawOk && rawBytes);
  if (rawOk && rawBytes) { jobsPrinted++; logEvent("Printed a label from the page"); web.send(200, "text/plain", "Sent to the printer."); }
  else { logEvent("Label from the page FAILED (printer not ready)"); web.send(200, "text/plain", "Printer not ready."); }
}

// /label: a finished label picture (PNG) from the page's "Stamp & print labels" card. The code is
// already drawn on it; it prints the same way as a shipping label from the order queue.
uint8_t *lblBuf = nullptr; size_t lblLen = 0; bool lblBad = false;
void handleLabelUpload() {
  HTTPUpload &u = web.upload();
  if (u.status == UPLOAD_FILE_START) { free(lblBuf); lblBuf = (uint8_t *)ps_malloc(MAX_JOB_BYTES); lblLen = 0; lblBad = !lblBuf; }
  else if (u.status == UPLOAD_FILE_WRITE) {
    if (!lblBad && lblLen + u.currentSize <= MAX_JOB_BYTES) { memcpy(lblBuf + lblLen, u.buf, u.currentSize); lblLen += u.currentSize; }
    else lblBad = true;
  } else if (u.status == UPLOAD_FILE_ABORTED) lblBad = true;
}
void handleLabelDone() {
  String msg; bool ok = false;
  if (!printerReady) msg = "Printer not ready.";
  else if (lblBad || !lblBuf) msg = "The label was too big for the bridge.";
  else if (!isPng(lblBuf, lblLen)) msg = "That isn't a label picture.";
  else {
    PngDec d;
    String err = decodePngLabel(lblBuf, lblLen, d);
    if (err.length()) msg = "Couldn't read the label (" + err + ").";
    else { lastBegin(); ok = printLabelImage(d, "", ""); lastEnd(ok); msg = ok ? "Printed." : "The printer stopped responding."; }
    freePng(d);
  }
  if (ok) {  // keep it for /lastlabel.png, trimmed to its real size
    jobsPrinted++;
    uint8_t *keep = (uint8_t *)ps_realloc(lblBuf, lblLen);
    if (keep) { free(lastPng); lastPng = keep; lastPngLen = lblLen; lblBuf = nullptr; }
  }
  free(lblBuf); lblBuf = nullptr;
  logEvent(ok ? String("Printed a stamped label from the page") : "Stamped label FAILED: " + msg);
  web.send(ok ? 200 : 500, "text/plain", msg);
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
    esp_task_wdt_reset();
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
  // Saved fliers don't need Wi-Fi, so keep the buttons working while it connects.
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) { serviceButtons(); serviceFlierJob(); updateLed(); delay(10); }
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
  { esp_reset_reason_t r = esp_reset_reason();
    if ((r == ESP_RST_TASK_WDT || r == ESP_RST_INT_WDT || r == ESP_RST_WDT || r == ESP_RST_PANIC) && stageMagic == 0xB71F00D5) {
      stage[sizeof stage - 1] = 0;
      logNote((String("Before that restart it was: ") + stage).c_str());
    }
    esp_task_wdt_config_t wd = {.timeout_ms = FREEZE_SECONDS * 1000, .idle_core_mask = 0, .trigger_panic = true};
    if (esp_task_wdt_reconfigure(&wd) != ESP_OK) esp_task_wdt_init(&wd);
    esp_task_wdt_add(NULL);              // watch this (the main loop's) task
    at("starting up");
  }
  WiFi.onEvent([](WiFiEvent_t e, WiFiEventInfo_t info) {
    static bool up = false;
    if (e == ARDUINO_EVENT_WIFI_STA_GOT_IP) { if (!up) logNote("Wi-Fi: online"); up = true; }
    else if (e == ARDUINO_EVENT_WIFI_STA_DISCONNECTED && up) {
      up = false; char b[48]; snprintf(b, sizeof b, "Wi-Fi: dropped (reason %d)", (int)info.wifi_sta_disconnected.reason); logNote(b);
    }
  });
  fsReady = FFat.begin(true);  // formats the storage area the first time
  { Preferences p; p.begin("btl", true); for (int n = 1; n <= FLIER_COUNT; n++) { flierName[n] = p.getString(("f" + String(n)).c_str(), ""); flierBatch[n] = p.getInt(("b" + String(n)).c_str(), 10);
      int a = p.getInt(("a" + String(n)).c_str(), n); btnAct[n] = (a == ACT_REPRINT || a == ACT_STOP || (a >= 1 && a <= FLIER_COUNT)) ? a : n; } p.end(); }
  pinMode(0, INPUT_PULLUP);
  for (uint8_t pin : flierPins) pinMode(pin, INPUT_PULLUP);
  { uint64_t mac = ESP.getEfuseMac(); char sfx[8]; snprintf(sfx, sizeof sfx, "-%02x%02x", (uint8_t)(mac >> 32), (uint8_t)(mac >> 40)); hostName = String(BRIDGE_NAME) + sfx; }
  xTaskCreatePinnedToCore(usbHostTask, "usbhost", 4096, nullptr, 5, nullptr, 0);
  xTaskCreatePinnedToCore(usbClientTask, "usbclient", 6144, nullptr, 5, nullptr, 0);
  startWifi();
  booting = false;
  web.on("/", handleRoot);
  web.on("/status", handleStatus);
  web.on("/log", handleLog);
  web.on("/pins", handlePins);
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
  web.on("/buttons", HTTP_POST, handleButtons);
  web.on("/raw", HTTP_POST, handleRawDone, handleRawUpload);
  web.on("/label", HTTP_POST, handleLabelDone, handleLabelUpload);
  web.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  web.onNotFound([] { web.sendHeader("Location", "/"); web.send(302); });
  web.begin();
}

void loop() {
  at("idle");
  improv.handle();
  if (restartSoon && millis() > restartAt) ESP.restart();
  if (setupMode) dns.processNextRequest();
  at("answering the page"); web.handleClient();
  updateLed();
  if (!setupMode) {
    at("taking a print from the computer"); serviceRawPrinting();
    at("reading the buttons"); serviceButtons();
    at("printing a saved flier"); serviceFlierJob();
    at("reprinting"); serviceReprint();
    at("checking the order queue"); pollQueue();
    at("idle");
    if (WiFi.status() != WL_CONNECTED) { static uint32_t lastTry = 0; if (millis() - lastTry > 30000) { lastTry = millis(); WiFi.reconnect(); } }
  }
  delay(1);
}
