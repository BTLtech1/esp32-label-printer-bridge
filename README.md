# BTL Print Bridge

Turn a cheap USB label printer into a **Wi-Fi printer** with a ~$10 ESP32-S3 board.

- **Print from any computer over Wi-Fi.** Add the bridge as a normal network printer and keep using your printer's own driver. Every app's Print button works.
- **Works with most cheap USB label printers.** The bridge doesn't need to understand your printer. It passes along exactly what your computer's driver sends (ZPL, TSPL, EPL, raster, whatever the printer uses).
- **Optional automatic printing.** The bridge can check a web address every few seconds and print any ready-made jobs it finds, for example shipping labels for new orders.

No app, no cloud account, no AI needed. Made by [BTL](https://btlstore.com).

---

## What you need

| Part | Notes |
|---|---|
| **ESP32-S3** dev board with **two USB-C ports** | Search "ESP32-S3 DevKitC N16R8". Must be an **S3**: the plain ESP32, C3 and C6 can't act as a USB host. N16R8 = 16 MB flash + 8 MB PSRAM. |
| Adapter from your printer's USB port to USB-C | Most label printers have a square USB-B port: use a "USB-B male to USB-C female" adapter plus a USB-C to USB-C cable, or the printer's normal cable plus a "USB-C male to USB-A female" OTG adapter. |
| USB-C cable + any phone charger | Powers the board. |
| Your label printer and its power supply | The printer must have its own power (the board doesn't power it). |

**Wiring:** printer → adapter → the board's port labeled **USB** (sometimes "OTG"). Power the board from its other port, labeled **UART** or "COM".

## Easiest: install from your browser

Open **https://btltech1.github.io/esp32-label-printer-bridge** in Chrome or Edge, plug in the board's UART port, and click **Install**. Not sure which port is the board? Unplug it and click Install again: the entry that disappears is the board.

## Flash the firmware with Arduino IDE (one time, about 15 minutes)

1. Install the free **Arduino IDE** (arduino.cc/en/software).
2. **File → Preferences → Additional boards manager URLs**, add:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
3. **Tools → Board → Boards Manager**, search **esp32** by Espressif, install version **3.x**.
4. Open `btl-printer/btl-printer.ino` from this folder.
5. Plug the board's **UART** port into your computer and set **Tools**:
   - Board: **ESP32S3 Dev Module**
   - USB CDC On Boot: **Disabled**
   - USB Mode: **Hardware CDC and JTAG**
   - Flash Size: **16MB**
   - PSRAM: **OPI PSRAM**
   - Partition Scheme: **16M Flash (3MB APP/9.9MB FATFS)** or any scheme with an app of 2 MB or more
   - Port: the COM port that appeared when you plugged in the board
6. Click **Upload** (→). If it doesn't start, hold the board's **BOOT** button while clicking Upload, and let go when you see "Connecting…".
7. Open **Tools → Serial Monitor** at **115200** baud to see what the bridge is doing.

## Set it up

1. On first start the bridge creates a Wi-Fi network called **BTL-Printer-Setup**. Join it with your phone or computer. A setup page should open by itself; if not, go to `http://192.168.4.1`.
2. Enter your Wi-Fi name and password (it must be a **2.4 GHz** network) and save. The bridge restarts and joins your Wi-Fi.
3. Open **http://btl-printer.local** (or the IP address printed in the Serial Monitor). You'll see whether the printer is connected.
4. In your router settings, give the bridge a **fixed IP address** (often called "DHCP reservation") so your computer can always find it.

## Add it to your computer

**Windows 10/11 (easiest, about 1 minute)**
1. Install your printer's normal driver if you haven't (plug the printer into the PC by USB once, or run the maker's installer).
2. Right-click the **Start** button → **Terminal (Admin)**.
3. Paste the line shown on the bridge's page and press Enter. It looks like this, with your bridge's IP:
   `irm http://192.168.1.50/windows.ps1 | iex`
4. Type the number of your printer's driver, set the paper size to your labels, and click OK. A test page prints.

You can read exactly what it does first: open `http://<bridge-ip>/windows.ps1` in your browser. It adds a Raw port 9100 and a printer called "Label Printer (Wi-Fi)". Running it again rebuilds that printer from scratch, which also fixes stuck jobs.

**Windows, by hand**
Settings → Printers & scanners → Add device → Add manually → "Add a printer using an IP address or hostname" → TCP/IP Device, the bridge's IP, untick "Query the printer" → if asked, Custom → Settings → Raw, port 9100, SNMP off → pick your printer's driver. If an old USB entry for the same printer gives trouble, add a new one rather than changing its port.

**macOS**
System Settings → Printers & Scanners → **Add Printer** → **IP** tab. Address = the bridge's IP, Protocol: **HP Jetdirect – Socket**, then choose your printer's driver.

Tip: in the driver's preferences, set the paper size to your labels (for example 4×6 in / 100×150 mm) and raise the darkness a little if text looks faint.

## Automatic printing (optional)

Fill in **Print queue URL** and **Queue token** on the bridge's page. Every 5 seconds the bridge asks:

```
GET  {QUEUE_URL}/next         Authorization: Bearer {TOKEN}
  204 No Content  -> nothing to print
  200 OK          -> body = raw printer data (e.g. a ZPL label), header X-Job-Id: <id>
POST {QUEUE_URL}/ack?id=<id>  Authorization: Bearer {TOKEN}   (sent after the job prints)
```

Anything that can serve those two URLs works: a Cloudflare Worker, a small server, etc. Jobs must already be in your printer's language. Many shipping-label services (Shippo, EasyPost, ShipStation) can return labels as **ZPL**, which most 4×6 thermal printers understand. A job that fails to print isn't acknowledged, so it's retried.

## Troubleshooting

| Problem | Fix |
|---|---|
| Page says **printer not connected** | Check the printer is on and plugged into the **USB** port (not UART). Try another cable or adapter. Some printers only connect when they see 5 V on the USB cable; if yours doesn't connect, see "5 V on the USB port" below. |
| Windows can't find the printer | Make sure your computer and the bridge are on the same network. Use the IP address instead of `btl-printer.local`. |
| Prints blank, garbage, or tiny | That's the driver, not the bridge. Use the same driver and settings that work over a USB cable. |
| Bridge can't join Wi-Fi | It needs a 2.4 GHz network. If it fails, it goes back to setup mode (BTL-Printer-Setup). |
| Windows test page / PDF never prints (job sits in the queue) | Usually an old printer entry that used to be on USB. Remove it and add a fresh one. In PowerShell (as Administrator), with your bridge's IP and your driver's exact name: `Add-Printer -Name "Label Wi-Fi" -DriverName "YOUR DRIVER NAME" -PortName "192.168.1.137"` (create the port first with `Add-PrinterPort -Name "192.168.1.137" -PrinterHostAddress "192.168.1.137"` if it doesn't exist). |
| ZPL test label prints garbage or nothing | Your printer may not speak ZPL. Try the **TSPL test** button. If neither works, that's fine: network printing uses your printer's own driver, so add the bridge to your computer and print a Windows test page instead. |

**5 V on the USB port.** Many dev boards don't send 5 V out of their native USB port. Printers with their own power supply usually don't need it, but a few wait for it before connecting. Some boards have a solder jumper (often marked "USB-OTG" or "IN-OUT") that lets the native port supply 5 V; check your board's documentation.

## Tested printers

| Printer | Language | Network printing | Notes |
|---|---|---|---|
| RONGTA RP421A | ZPL | works | Windows 11, "RONGTA RP421A(ZPL 203DPI)" driver, Raw port 9100 |

Tried it with another printer? Please add it to this table.

## License

MIT. See `LICENSE`.
