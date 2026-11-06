// BTL Print Bridge - Wi-Fi setup over USB from the web installer.
//
// A small implementation of the Improv Wi-Fi Serial protocol
// (https://www.improv-wifi.com/serial/), the part ESP Web Tools uses: device info,
// current state, Wi-Fi network scan and Wi-Fi settings.
//
// It works like the "Improv WiFi Library" it replaces, except that it never sends a
// device URL. Without a URL the installer shows no "Visit Device" button, so setup
// stays on the install page from start to finish.
//
// Packet: "IMPROV" <version 1> <type> <length> <data...> <checksum> '\n'
// checksum = low byte of the sum of every byte before it. The newline is not part of
// the packet; the installer reads it as a line break (same as ESPHome does).

#pragma once
#include <Arduino.h>
#include <WiFi.h>

class ImprovSerial {
public:
  typedef bool (*ConnectFn)(const char *ssid, const char *password);

  explicit ImprovSerial(Stream &port) : io(port) {}

  void begin(const char *firmwareName, const char *firmwareVersion, const char *chip,
             const char *deviceName, ConnectFn connectFn) {
    fwName = firmwareName; fwVersion = firmwareVersion; chipName = chip; devName = deviceName;
    connect = connectFn;
  }

  // Call from loop(). Reads whatever has arrived; other serial bytes are ignored.
  void handle() {
    while (io.available() > 0) feed((uint8_t)io.read());
  }

private:
  enum : uint8_t { TYPE_CURRENT_STATE = 0x01, TYPE_ERROR_STATE = 0x02, TYPE_RPC = 0x03, TYPE_RPC_RESULT = 0x04 };
  enum : uint8_t { STATE_AUTHORIZED = 0x02, STATE_PROVISIONING = 0x03, STATE_PROVISIONED = 0x04 };
  enum : uint8_t { ERROR_NONE = 0x00, ERROR_INVALID_RPC = 0x01, ERROR_UNKNOWN_RPC = 0x02,
                   ERROR_UNABLE_TO_CONNECT = 0x03, ERROR_UNKNOWN = 0xFF };
  enum : uint8_t { CMD_WIFI_SETTINGS = 0x01, CMD_GET_CURRENT_STATE = 0x02,
                   CMD_GET_DEVICE_INFO = 0x03, CMD_GET_WIFI_NETWORKS = 0x04 };

  Stream &io;
  const char *fwName = "", *fwVersion = "", *chipName = "", *devName = "";
  ConnectFn connect = nullptr;
  uint8_t rx[9 + 255 + 1];  // header + longest data + checksum
  uint16_t rxLen = 0;

  // ---- receiving
  void feed(uint8_t b) {
    static const char HEADER[] = "IMPROV";
    if (rxLen < 6) {                                   // matching "IMPROV"
      if (b == (uint8_t)HEADER[rxLen]) rx[rxLen++] = b;
      else if (b == 'I') { rx[0] = b; rxLen = 1; }
      else rxLen = 0;
      return;
    }
    rx[rxLen++] = b;
    if (rxLen == 7 && b != 1) { rxLen = 0; return; }   // only protocol version 1
    if (rxLen < 9) return;                             // still need type and length
    uint16_t total = 9 + rx[8] + 1;
    if (rxLen < total) return;
    rxLen = 0;                                         // a whole packet is in rx[]
    uint8_t sum = 0;
    for (uint16_t i = 0; i < total - 1; i++) sum += rx[i];
    if (sum != rx[total - 1]) { sendError(ERROR_INVALID_RPC); return; }
    if (rx[7] == TYPE_RPC) handleRpc(&rx[9], rx[8]);
  }

  // data = <command> <length> <payload...>
  void handleRpc(const uint8_t *data, uint8_t len) {
    if (len < 2 || data[1] != len - 2) { sendError(ERROR_INVALID_RPC); return; }
    const uint8_t cmd = data[0];
    const uint8_t *p = data + 2;
    const uint8_t plen = data[1];

    switch (cmd) {
      case CMD_GET_CURRENT_STATE:
        if (WiFi.status() == WL_CONNECTED) {
          sendState(STATE_PROVISIONED);
          sendResult(cmd, nullptr, 0);                 // no URL: no "Visit Device" button
        } else {
          sendState(STATE_AUTHORIZED);
        }
        break;

      case CMD_WIFI_SETTINGS: {
        // payload = <ssid length> <ssid> <password length> <password>
        if (plen < 2) { sendError(ERROR_INVALID_RPC); break; }
        uint8_t ssidLen = p[0];
        if (ssidLen == 0 || 1 + ssidLen + 1 > plen) { sendError(ERROR_INVALID_RPC); break; }
        uint8_t passLen = p[1 + ssidLen];
        if (1 + ssidLen + 1 + passLen != plen) { sendError(ERROR_INVALID_RPC); break; }
        static char ssid[256], pass[256];
        memcpy(ssid, p + 1, ssidLen); ssid[ssidLen] = 0;
        memcpy(pass, p + 2 + ssidLen, passLen); pass[passLen] = 0;

        sendState(STATE_PROVISIONING);
        bool ok = connect && connect(ssid, pass);
        memset(pass, 0, sizeof pass);
        if (ok) {
          sendError(ERROR_NONE);
          sendState(STATE_PROVISIONED);
          sendResult(cmd, nullptr, 0);                 // no URL: no "Visit Device" button
        } else {
          sendError(ERROR_UNABLE_TO_CONNECT);
          sendState(STATE_AUTHORIZED);                 // ready for another try
        }
        break;
      }

      case CMD_GET_DEVICE_INFO: {
        const char *info[4] = {fwName, fwVersion, chipName, devName};
        sendResult(cmd, info, 4);
        break;
      }

      case CMD_GET_WIFI_NETWORKS: {
        // One result per network (name, signal, "YES"/"NO" for password), then an empty one.
        int n = WiFi.scanNetworks();
        for (int i = 0; i < n; i++) {
          String name = WiFi.SSID(i);
          String rssi = String(WiFi.RSSI(i));
          const char *secured = WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "NO" : "YES";
          // Skip hidden networks, and any result whose length byte would be 10: the
          // installer reads a 10 there as a line break and would lose the packet.
          if (name.length() == 0) continue;
          if (2 + (1 + name.length()) + (1 + rssi.length()) + (1 + strlen(secured)) == 10) continue;
          const char *net[3] = {name.c_str(), rssi.c_str(), secured};
          sendResult(cmd, net, 3);
          delay(1);
        }
        WiFi.scanDelete();
        sendResult(cmd, nullptr, 0);
        break;
      }

      default:
        sendError(ERROR_UNKNOWN_RPC);
        break;
    }
  }

  // ---- sending
  void sendState(uint8_t state) { sendPacket(TYPE_CURRENT_STATE, &state, 1); }
  void sendError(uint8_t error) { sendPacket(TYPE_ERROR_STATE, &error, 1); }

  // RPC result: <command> <total length> then each string as <length> <bytes>.
  void sendResult(uint8_t cmd, const char *const *strs, uint8_t count) {
    uint8_t out[255];
    uint16_t n = 2;
    out[0] = cmd;
    for (uint8_t i = 0; i < count; i++) {
      size_t sl = strlen(strs[i]);
      if (sl > 255 || n + 1 + sl > sizeof out) { sendError(ERROR_UNKNOWN); return; }
      out[n++] = (uint8_t)sl;
      memcpy(out + n, strs[i], sl);
      n += sl;
    }
    out[1] = (uint8_t)(n - 2);
    sendPacket(TYPE_RPC_RESULT, out, (uint8_t)n);
  }

  void sendPacket(uint8_t type, const uint8_t *data, uint8_t len) {
    uint8_t pkt[9 + 255 + 2];
    memcpy(pkt, "IMPROV", 6);
    pkt[6] = 1;            // protocol version
    pkt[7] = type;
    pkt[8] = len;
    memcpy(pkt + 9, data, len);
    uint8_t sum = 0;
    for (uint16_t i = 0; i < 9 + len; i++) sum += pkt[i];
    pkt[9 + len] = sum;
    pkt[10 + len] = '\n';
    io.write(pkt, 11 + len);  // in one write, so the packet goes out in one piece
  }
};
