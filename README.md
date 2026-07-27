# Knobler

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

Firmware for the [M5Stack M5Dial](https://docs.m5stack.com/en/core/M5Dial) as a physical companion display for a [Lynx](https://github.com/G8YTZ/lynx-datv-receiver) DATV receiver — a "magic eye" signal display, preset/stream browsing via the rotary encoder, and self-service Wi-Fi setup.

## Features

- Live signal status — lock state, dBm, MER — read from Lynx's API
- Classic valve-radio "magic eye" style signal indicator, including a split top/bottom eye for receive diversity
- Browse and select RF presets and saved streams via the rotary encoder
- Self-service Wi-Fi setup — connect once via a temporary access point and type your password on a phone/laptop, no need to hardcode credentials or edit code
- Automatic discovery of your Lynx receiver on the network — no fixed IP address required

## Requirements

- An M5Stack M5Dial and USB-C cable
- [Arduino IDE](https://www.arduino.cc/en/software), with ESP32/M5Stack board support
- A Lynx receiver running a version with the discovery responder (prints `Discovery responder listening on UDP :9998.` on startup)

## Installation

See [`docs/INSTALL.md`](docs/INSTALL.md) for a full step-by-step guide, written for first-time Arduino users.

## License

GPL-3.0 — see [`LICENSE`](LICENSE) for the full text.
