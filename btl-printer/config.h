// BTL Print Bridge - build-time defaults.
// You normally don't need to edit this file: Wi-Fi and the optional print queue
// are set from the web page the bridge serves (see README.md).

#define BRIDGE_NAME        "btl-printer"        // http://btl-printer.local
#define SETUP_AP_NAME      "BTL-Printer-Setup"  // Wi-Fi network shown when it isn't set up yet
#define RAW_PRINT_PORT     9100                 // standard "raw / JetDirect" printing port
#define QUEUE_POLL_SECONDS 5
#define MAX_JOB_BYTES      (2 * 1024 * 1024)    // largest queued job kept in PSRAM
#define USB_CHUNK_BYTES    16384                // bytes per USB bulk transfer

// Saved fliers: press a button and one copy prints.
#define FLIER_COUNT        3
// Buttons: wire each between this pin and GND (no resistor needed).
// GPIO 4, 5, 6 = Flier 1, 2, 3. The board's BOOT button also works: 1, 2 or 3 quick clicks.
#define FLIER_BUTTON_PINS  {4, 5, 6}
#define CAPTURE_MINUTES    5                    // how long "save the next print" waits

// Status light: the board's built-in colour LED (GPIO 48 on most ESP32-S3 DevKit boards,
// GPIO 38 on some newer ones; both are driven). Green = ready, blue = printing,
// red = no printer, yellow = waiting to save a flier, purple = setup mode.
#define LED_PINS           {48, 38}
#define LED_BRIGHTNESS     20                   // 0-255
