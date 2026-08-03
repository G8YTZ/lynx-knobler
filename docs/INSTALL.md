# Knobler M5Dial Installation Guide

Wi-Fi setup and Lynx auto-discovery — flash it once, forget it.

There are two ways to get Knobler onto your M5Dial:

- **[Option A: Browser Install](#option-a-browser-install-recommended)** — fastest, no software to install, works for most people
- **[Option B: Arduino IDE](#option-b-arduino-ide-manual-build)** — if you want to see or modify the source code, or Option A doesn't work for you

Both need the same prerequisites below.

## Before You Start

You will need:

- An M5Dial device and its USB-C cable
- A Windows, Mac, or Linux computer
- Your home Wi-Fi network name (SSID) and password — you'll type these on your phone or laptop, not on the Dial itself
- A phone or laptop to briefly connect to the Dial during setup
- A [Lynx](https://github.com/G8YTZ/lynx-datv-receiver) receiver already up and running on your network, **connected via wired Ethernet**

> **Important:** Wi-Fi on the Lynx receiver itself isn't supported. If the Lynx Pi has Wi-Fi enabled alongside Ethernet — even if you don't think you're using it — having both live on the same network can cause intermittent, hard-to-diagnose failures (things like presets loading fine but selecting one failing). This can happen without you realising: Raspberry Pi Imager remembers Wi-Fi credentials from previous SD card builds and can silently apply them to a new one even if you didn't enter anything that time. Check with `ip -4 addr show` on the Pi — if both `eth0` and `wlan0` show an address, turn Wi-Fi off (`sudo nmcli radio wifi off`, or disable it in `raspi-config`).

> **Important:** Your Lynx receiver needs to be running a version that supports discovery before you start on the Dial, or auto-discovery won't find it. Check your Lynx startup log for a line like `Discovery responder listening on UDP :9998.` — if you don't see it, update Lynx first (see [the Lynx repo](https://github.com/G8YTZ/lynx-datv-receiver)'s own instructions).

---

## Option A: Browser Install (Recommended)

Flash the firmware straight from a web page — no Arduino IDE, no libraries, no compiling.

**You'll need:**
- **Chrome or Edge** — this doesn't work in Safari or Firefox, they don't support the browser feature required (Web Serial)
- The M5Dial connected via USB-C (a real data cable, not a charge-only one)

**Steps:**

1. Go to **[the Knobler install page](https://g8ytz.github.io/lynx-knobler/install.html)**.
2. Close Arduino IDE's Serial Monitor or anything else that might be using the Dial's USB port, if you have it open.
3. Click **Install Knobler**, then choose the Dial from the port picker that pops up.
4. Wait for it to flash — this takes a minute or two. Don't unplug the Dial or close the browser tab while it's in progress.
5. Once it's done, the Dial restarts on its own. Continue to **[Connect the Dial to Wi-Fi](#connect-the-dial-to-wi-fi)** below.

**If nothing happens when you click Install**, or the port picker is empty, jump to [Browser Install Troubleshooting](#browser-install-troubleshooting).

---

## Option B: Arduino IDE (Manual Build)

Use this if you want to see or edit the source code, or if the browser method above doesn't work on your computer.

### Step 1: Install Arduino IDE

Go to <https://www.arduino.cc/en/software>, download the version for your computer (Windows / Mac / Linux), and install it as you would any other program.

### Step 2: Add M5Dial Board Support

The Dial uses a different chip to a normal Arduino, so Arduino IDE needs to be told how to talk to it. This needs **two** board manager URLs, not just one.

1. Open **File → Preferences** (Windows/Linux) or **Arduino IDE → Settings** (Mac).
2. Find "Additional boards manager URLs" and paste in **both** of these, one per line (or comma-separated, depending on your Arduino IDE version):
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json
   ```
   Click OK.
3. Go to **Tools → Board → Boards Manager**. Search for "esp32" and install the package by **Espressif Systems** (this can take a few minutes — it's a big download). Then search for "M5Stack" and install that board package too — it needs both packages installed, not just one.

> **Note:** If installing the M5Stack package fails or times out, see [Arduino IDE Troubleshooting](#arduino-ide-troubleshooting) below — this has a couple of known causes with straightforward workarounds.

### Step 3: Install the Required Libraries

Go to **Sketch → Include Library → Manage Libraries**. For each one below: search for it, find the exact one named, and click Install.

- **M5Dial** — by M5Stack (screen, encoder, and touch support)
- **WiFiManager** — by tzapu (this is what puts up the Wi-Fi setup screen the first time it's used)
- **ArduinoJson** — by Benoit Blanchon (version 6.x, **6.20 or later** — not version 7, and not an old 6.x like 6.9 either)

> **Important:** Make sure you install WiFiManager by tzapu specifically — there are other libraries with similar names. And make sure ArduinoJson is a recent 6.x release, not version 7 and not an old 6.x — older releases (like 6.9.0) are missing features this sketch relies on and will fail to compile.

### Step 4: Open the Knobler Sketch

**File → Open**, and select the `.ino` file from the `knobler/` folder in this repo.

### Step 5: Select the Board and Port

1. Plug the M5Dial into your computer with the USB-C cable.
2. **Tools → Board** → find **M5Stack Dial** under the ESP32 section (naming varies slightly between package versions — look for "M5Dial" if you can't find that exact name).
3. **Tools → Port** → choose the one that appeared when you plugged the Dial in.

> **Note:** On Windows this looks like "COM3", "COM4", etc. On a Mac it looks like "/dev/cu.usbmodem…". If nothing new appears, try a different USB cable — some cables are charge-only and don't carry data.

### Step 6: Set Flash Size and Partition Scheme

These two settings are easy to miss and cause a compile error later if they're wrong, so worth doing now rather than after a failed upload.

1. **Tools → Flash Size** → select **8MB**.
2. **Tools → Partition Scheme** → select an **8MB** option that gives the app more room — something like **"8M with spiffs (3MB APP/1.5MB SPIFFS)"**. The exact wording varies by board package version; the important part is picking one that actually uses the 8MB you just selected above, not a smaller default.

If you skip this and try to upload anyway, you'll likely see a compile error ending in something like:
```
Sketch uses 1312499 bytes (100%) of program storage space. Maximum is 1310720 bytes.
Sketch too big; see https://support.arduino.cc/hc/en-us/articles/360013825179 for tips on reducing it.
```
That specific number (1,310,720 bytes) is the giveaway — it's the size of the *default* partition scheme's app space, not the 8MB board's real capacity. It means Flash Size and Partition Scheme are two separate settings and only one of them got changed. Go back and check both.

### Step 7: Upload

Click the **Upload** (→) button. It compiles the sketch, then flashes it to the Dial — this can take a minute or two the first time. When it's done, the Dial's screen will show the Knobler splash screen.

Continue to **[Connect the Dial to Wi-Fi](#connect-the-dial-to-wi-fi)** below.

---

## Connect the Dial to Wi-Fi

This step is the same regardless of which install option you used above, and only needs doing once — the Dial remembers your Wi-Fi after this.

1. On first boot, the Dial tries for about 30 seconds to find a saved network (it won't find one yet), then shows a screen saying:
   ```
   Wi-Fi Setup — Connect a phone/laptop to: LynxDial-Setup
   ```
2. On your phone or laptop, join the **LynxDial-Setup** Wi-Fi network.
3. A page should pop up automatically (if not, open a browser and go to `192.168.4.1`). Choose your home Wi-Fi network from the list, enter its password, and save.
4. The Dial restarts, connects to your home Wi-Fi, and automatically searches for your Lynx receiver on the network.

## You're Done

Once connected, the Dial shows live status from your Lynx receiver. Turn the knob to browse presets and streams; press it to select one.

If it ever loses Wi-Fi (router reboot, power cut, etc.), it reconnects on its own — no need to redo any of this. If your saved network ever becomes genuinely unreachable, it automatically drops back into setup mode.

## Troubleshooting

### Browser Install Troubleshooting

**Nothing happens when I click Install, or there's an error about HTTPS**
Make sure you're on the real published page (`g8ytz.github.io/...`), not a local copy of the file — Web Serial requires the page to be served securely, which only works on the real site, not a file opened directly from your computer.

**The port picker is empty, or my Dial isn't listed**
- Make sure you're using Chrome or Edge — Safari and Firefox aren't supported.
- Try a different USB cable — many are charge-only and don't carry data.
- Close Arduino IDE's Serial Monitor or any other program that might already be using the Dial's port.

**It looks like it's flashing but then fails partway through**
Unplug the Dial, wait a few seconds, plug it back in, and try again — this can happen if the connection was interrupted. If it keeps failing, fall back to Option B (Arduino IDE).

### Arduino IDE Troubleshooting

**Installing the M5Stack board package fails or times out** (error mentioning `context deadline exceeded`, or `Failed to install platform: 'M5Stack:...'`)
- M5Stack's board package used to be hosted at a `.aliyuncs.com` address that's now unreliable/deprecated. Make sure the URL in **Additional boards manager URLs** (Step 2) is the current one:
  ```
  https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json
  ```
- If it still fails with the current URL: some home routers/firewalls (UniFi's GeoIP filtering is a known example) block inbound traffic from Chinese IP ranges — and even though the URL above resolves to a modern-looking domain, the actual file can still be served from infrastructure that gets blocked this way, even though your own outbound request goes through fine. If downloads through the router fail but you suspect this, try downloading via a phone hotspot instead (bypasses the router's firewall for that one request) to confirm — if that works, the fix is either staying on the hotspot for future M5Stack updates, or adding a specific allow-rule for `static-cdn.m5stack.com` in your router if it supports domain-based (not just IP-range) exceptions.

**The board doesn't appear in Tools → Board**
Double-check Step 2 — the ESP32/M5Stack board package needs to finish installing before it shows up.

**No port appears in Tools → Port**
Try a different USB cable (many are charge-only) or a different USB port.

**Upload fails with a compile error mentioning `WiFiManager.h`**
The WiFiManager library (Step 3) isn't installed, or the wrong one was — search again and confirm it's by tzapu.

**Compile error: "Sketch too big" / "text section exceeds available space"**
See Step 6 — Flash Size and Partition Scheme are two separate settings, and both need to be set to use the full 8MB.

**Compile fails with errors that don't obviously match anything above**
Check your ArduinoJson version isn't an old 6.x release (like 6.9.0) — update it to the latest 6.x via Manage Libraries. See Step 3.

### General

**The Dial connects to Wi-Fi but shows "Lynx / Connect Error"**
Make sure your Lynx receiver supports discovery (see the note in "Before You Start") and that the Dial and Lynx are on the same Wi-Fi network. If presets loaded fine but selecting one triggers this, check whether the Lynx Pi has Wi-Fi enabled alongside Ethernet — see the wired-only note in "Before You Start".

**I want to reconnect the Dial to a different Wi-Fi network**
- If the old network is simply gone or out of range: nothing to do — the Dial detects this automatically and drops into setup mode on its next boot.
- If the old network still works and you just want to switch: there's currently no on-device way to force setup mode. You'll need someone with the Arduino IDE to clear the saved credentials for you.
