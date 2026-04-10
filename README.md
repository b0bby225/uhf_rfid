# UHF RFID — Flipper Zero App

Read, write, and manage UHF RFID tags (EPC Gen2 / ISO 18000-6C) using the YRM100 module on a Flipper Zero.

## Features

- **Read Tag** — Single-poll EPC read with full tag detail view (EPC, TID, User, RSSI)
- **View EPC as QR Code** — Renders the scanned EPC as a scannable QR code on-screen
- **Write Tag Banks** — Hex byte-editor input for EPC (12 B), TID (12 B), User (8 B), and RFU (4 B)
- **Set Access Password** — Write a 32-bit access password with byte-accurate hex editor
- **Set Kill Password** — Write a 32-bit kill password using the same hex editor flow
- **Kill Tag** — Permanently disable (kill) a tag using its stored kill password
- **Bulk Scan** — Continuously poll and log unique tag EPCs to SD card (`/ext/uhf_bulk_scan.txt`)
- **Saved Tags** — Browse and load previously saved tag files from SD card
- **Settings** — Inline Left/Right adjustment for Baudrate, TX Power, and Region without leaving the menu
- **Kill PW Audit** 🚧 — Brute-force kill-password auditing tool *(in development — not functional)*

## Requirements

- **Flipper Zero** (firmware 0.99+ recommended)
- **YRM100 UHF RFID Module** — [AliExpress listing](https://www.aliexpress.com/item/1005005296512846.html)
- **Wiring**: TX → pin 13, RX → pin 14, VCC → 5V (pin 1), GND → pin 8

  ![YRM100 wiring](https://static-cdn.m5stack.com/resource/docs/products/unit/uhf_rfid/uhf_rfid_sch_01.webp)

## Installation

### Quick Download (Recommended)

**Download**: [uhf_rfid.fap](dist/uhf_rfid.fap)

#### Steps
1. Download `uhf_rfid.fap`
2. Copy to your Flipper SD card: `SD Card/apps/RFID/uhf_rfid.fap`
3. Eject / unmount the SD card and reinsert — the app will appear under **Applications → RFID**

### Method 2: Build from Source

```bash
# Install ufbt
pip install ufbt

# Clone and build
git clone https://github.com/b0bby225/uhf_rfid.git
cd uhf_rfid
ufbt
```

The compiled `.fap` will appear in `dist/uhf_rfid.fap`. Copy it to `SD Card/apps/RFID/` on your Flipper.

> **Note:** Do **not** use `ufbt flash_usb` — that updates the Flipper firmware, not just the app.

## Controls

| Screen | Button | Action |
|---|---|---|
| **Main Menu** | Up / Down | Navigate items |
| **Main Menu** | OK | Select item |
| **Settings** | Up / Down | Navigate rows |
| **Settings** | Left / Right | Adjust value inline |
| **Settings** | OK | Apply highlighted setting |
| **Byte Editor** | Left / Right | Move cursor |
| **Byte Editor** | Up / Down | Change nibble value |
| **Byte Editor** | OK | Confirm entry |
| **Tag Detail / QR** | Back | Return to Tag Menu |
| **Any popup** | Back | Cancel / return |

## Usage

1. Wire the YRM100 module to the Flipper GPIO header (see wiring above)
2. Open **[UHF]RFID** from Applications → RFID
3. The app will verify the module connection on launch
4. **Read Tag** — Present a tag to the module; tag data displays after a successful read
5. From the Tag Menu you can write banks, set passwords, kill the tag, or view the EPC as a QR code
6. **Bulk Scan** — Continuously logs each unique tag EPC to `/ext/uhf_bulk_scan.txt`
7. **Settings** — Adjust baud rate, TX power, and region directly with Left/Right arrows; press OK to apply

## Feature Status

| Feature | Status |
|---|---|
| Read Tag | Stable |
| Write Tag Banks (EPC / TID / User / RFU) | Stable |
| Set Access / Kill Password | Stable |
| Kill Tag | Stable |
| Bulk Scan | Stable |
| QR Code View | Stable |
| Saved Tags | Stable |
| Settings (Baud / Power / Region) | Stable |
| Kill PW Audit | In Development |

## Protocol Reference

The YRM100 module uses the MagicRF M100 command set over UART.
Full firmware manual: [MagicRF_M100_Firmware_manual_en.pdf](assets/res/MagicRF_M100&QM100_Firmware_manual_en.pdf)

## License

MIT License — original app by [frux-c](https://github.com/frux-c/uhf_rfid). Extended by Bobby Gibbs.

## Author

Bobby Gibbs ([@b0bby225](https://github.com/b0bby225))

---
