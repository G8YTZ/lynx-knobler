# Knobler M5Dial Installation Guide

Wi-Fi setup and Lynx auto-discovery — flash it once, forget it.

Follow these steps in order.

## Before You Start

This guide walks through setting up a Knobler M5Dial from a bare board to a working Lynx display — installing the required software, flashing the firmware, and connecting it to your Wi-Fi network. It's written so you can follow it step by step with no prior Arduino experience.

You will need:

- An M5Dial device and its USB-C cable
- A Windows, Mac, or Linux computer
- Your home Wi-Fi network name (SSID) and password — you'll type these on your phone or laptop, not on the Dial itself
- A phone or laptop to briefly connect to the Dial during setup
- A [Lynx](https://github.com/G8YTZ/lynx-datv-receiver) receiver already up and running on your network

> **Important:** Your Lynx receiver needs to be running a version that supports discovery before you start on the Dial, or auto-discovery won't find it. Check your Lynx startup log for a line like `Discovery responder listening on UDP :9998.` — if you don't see it, update Lynx first (see [the Lynx repo](https://github.com/G8YTZ/lynx-datv-receiver)'s own instructions).

## Step 1: Install Arduino IDE

Arduino IDE is the free program used to load the Knobler software onto the M5Dial.

Go to <https://www.arduino.cc/en/software>, download the version for your computer (Windows / Mac / Linux), and install it as you would any other program.

## Step 2: Add M5Dial Board Support

The Dial uses a different chip to a normal Arduino, so Arduino IDE needs to be told how to talk to it.

1. Open **File → Preferences** (Windows/Linux) or **Arduino IDE → Settings** (Mac).
2. Find "Additional boards manager URLs" and paste in:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
   Click OK.
3. Go to **Tools → Board → Boards Manager**. Search for "esp32" and install the package by **Espressif Systems** (this can take a few minutes — it's a big download). Then search for "M5Stack" and install that board package too, if it's listed separately.

## Step 3: Install the Required Libraries

Go to **Sketch → Include Library → Manage Libraries**. For each one below: search for it, find the exact one named, and click Install.

- **M5Dial** — by M5Stack (screen, encoder, and touch support)
- **WiFiManager** — by tzapu (this is what puts up the Wi-Fi setup screen the first time it's used)
- **ArduinoJson** — by Benoit Blanchon (version 6.x — **not** version 7)

> **Important:** Make sure you install WiFiManager by tzapu specifically — there are other libraries with similar names. And install ArduinoJson version 6, not 7, or you may get compile errors.

## Step 4: Open the Knobler Sketch

**File → Open**, and select the `.ino` file from this repo.

## Step 5: Select the Board and Port

1. Plug the M5Dial into your computer with the USB-C cable.
2. **Tools → Board** → find **M5Stack Dial** under the ESP32 section (naming varies slightly between package versions — look for "M5Dial" if you can't find that exact name).
3. **Tools → Port** → choose the one that appeared when you plugged the Dial in.

> **Note:** On Windows this looks like "COM3", "COM4", etc. On a Mac it looks like "/dev/cu.usbmodem…". If nothing new appears, try a different USB cable — some cables are charge-only and don't carry data.

## Step 6: Upload

Click the **Upload** (→) button. It compiles the sketch, then flashes it to the Dial — this can take a minute or two the first time. When it's done, the Dial's screen will show the Knobler splash screen.

## Step 7: Connect the Dial to Wi-Fi

This only needs doing once — the Dial remembers your Wi-Fi after this.

1. On first boot, the Dial tries for about 30 seconds to find a saved network (it won't find one yet), then shows a screen saying:
   ```
   Wi-Fi Setup — Connect a phone/laptop to: LynxDial-Setup
   ```
2. On your phone or laptop, join the **LynxDial-Setup** Wi-Fi network.
3. A page should pop up automatically (if not, open a browser and go to `192.168.4.1`). Choose your home Wi-Fi network from the list, enter its password, and save.
4. The Dial restarts, connects to your home Wi-Fi, and automatically searches for your Lynx receiver on the network.

## Step 8: You're Done

Once connected, the Dial shows live status from your Lynx receiver. Turn the knob to browse presets and streams; press it to select one.

If it ever loses Wi-Fi (router reboot, power cut, etc.), it reconnects on its own — no need to redo any of this. If your saved network ever becomes genuinely unreachable, it automatically drops back into setup mode.

## Troubleshooting

**The board doesn't appear in Tools → Board**
Double-check Step 2 — the ESP32/M5Stack board package needs to finish installing before it shows up.

**No port appears in Tools → Port**
Try a different USB cable (many are charge-only) or a different USB port.

**Upload fails with a compile error mentioning `WiFiManager.h`**
The WiFiManager library (Step 3) isn't installed, or the wrong one was — search again and confirm it's by tzapu.

**The Dial connects to Wi-Fi but shows "Lynx / Connect Error"**
Make sure your Lynx receiver supports discovery (see the note in "Before You Start") and that the Dial and Lynx are on the same Wi-Fi network.

**I want to reconnect the Dial to a different Wi-Fi network**
- If the old network is simply gone or out of range: nothing to do — the Dial detects this automatically and drops into setup mode on its next boot.
- If the old network still works and you just want to switch: there's currently no on-device way to force setup mode. You'll need someone with the Arduino IDE to clear the saved credentials for you.
