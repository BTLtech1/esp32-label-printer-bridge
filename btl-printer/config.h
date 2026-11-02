// BTL Print Bridge - build-time defaults.
// You normally don't need to edit this file: Wi-Fi and the optional print queue
// are set from the web page the bridge serves (see README.md).

#define BRIDGE_NAME        "btl-printer"        // http://btl-printer.local
#define SETUP_AP_NAME      "BTL-Printer-Setup"  // Wi-Fi network shown when it isn't set up yet
#define RAW_PRINT_PORT     9100                 // standard "raw / JetDirect" printing port
#define QUEUE_POLL_SECONDS 5
#define MAX_JOB_BYTES      (2 * 1024 * 1024)    // largest queued job kept in PSRAM
#define USB_CHUNK_BYTES    16384                // bytes per USB bulk transfer
