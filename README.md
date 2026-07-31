# Knobler

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

Firmware for the [M5Stack M5Dial](https://docs.m5stack.com/en/core/M5Dial) as a physical companion display for a [Lynx](https://github.com/G8YTZ/lynx-datv-receiver) DATV receiver — a "magic eye" signal display, preset/stream browsing via the rotary encoder, and self-service Wi-Fi setup.

**[⚡ Install straight from your browser](https://g8ytz.github.io/lynx-knobler/install.html)** — no software to install, just Chrome or Edge and a USB cable.

## Features

- Live signal status — lock state, dBm, MER — read from Lynx's API
- Classic valve-radio "magic eye" style signal indicator, including a split top/bottom eye for receive diversity
- Browse and select RF presets and saved streams via the rotary encoder
- Self-service Wi-Fi setup — connect once via a temporary access point and type your password on a phone/laptop, no need to hardcode credentials or edit code
- Automatic discovery of your Lynx receiver on the network — no fixed IP address required

## Requirements

- An M5Stack M5Dial and USB-C cable
- A Lynx receiver running a version with the discovery responder (prints `Discovery responder listening on UDP :9998.` on startup), connected via wired Ethernet
- [Arduino IDE](https://www.arduino.cc/en/software) — only needed if you're building from source rather than using the browser installer above

## Installation

**Fastest:** use the [browser installer](https://g8ytz.github.io/lynx-knobler/install.html) linked above — nothing to install on your computer.

For building from source, or if the browser installer doesn't work for you, see [`docs/INSTALL.md`](docs/INSTALL.md) for the full Arduino IDE walkthrough, written for first-time Arduino users.

## License

GPL-3.0 — see [`LICENSE`](LICENSE) for the full text.
