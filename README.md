# LedgerScreenReader

**ESP32-S3 OLED Display Mirroring & Hardware Security Research**

An open-source ESP32-S3 firmware project for studying SPI-driven monochrome
displays and reproducing their output on an external OLED and on a PC.

The project was developed during independent research into hardware wallet
display interfaces, physical security, and supply-chain threats. It also
addresses a practical problem: diagnosing a device whose original OLED screen
is damaged or no longer working.

> **Research notice:** This project is intended for authorized hardware
> diagnostics, display experimentation, and security education. Only connect it
> to devices you own or are explicitly authorized to examine. Do not use it to
> capture another person's confidential information, recovery phrases, or
> authentication data.

## Overview

LedgerScreenReader passively listens to the SPI bus between a device's
microcontroller and its display controller. It rebuilds the frame buffer from
the captured traffic and draws it on:

- an external 128×64 I²C OLED connected to the ESP32-S3, and
- a desktop viewer (`tools/viewer.py`) over the board's native USB port.

The ESP32-S3 inputs are read-only. The firmware never drives the source bus.

### Features

- Passive SPI capture of CS, CLK and MOSI at ~10× oversampling of a 1 MHz bus
- Works without the display's D/C line: commands and pixels are separated by
  the structure of each frame
- 8-bit framing (one CS pulse per byte) and 9-bit 3-wire framing (D/C bit
  embedded in each word)
- Mirroring to an external 128×64 SH1106 or SSD1306 OLED over I²C
- Desktop viewer with live image, firmware log, screenshots and remote controls
- Orientation, 2× vertical zoom and color inversion controls
- Serial diagnostics: per-frame statistics and on-demand bus dumps
- PlatformIO + ESP-IDF 5.5 build, configurable GPIO assignments

## How It Works

```
 source MCU ──CS/CLK/MOSI──► display controller (damaged screen)
                 │
                 ▼  (passive tap + common GND)
 ┌───────────────────────────── ESP32-S3 ─────────────────────────────┐
 │ core 1: sniffer                     core 0: decoder + outputs       │
 │  polls the three pins with the      groups bytes into frames,       │
 │  dedicated-GPIO CPU instruction,    parses setup commands and page  │
 │  interrupts off, shifts bits on     writes, keeps a 128×64 frame    │
 │  CLK edges while CS is active,  ──► buffer, renders it to the I²C   │
 │  marks frame starts on CS idle      OLED and streams it over USB    │
 │  gaps (lock-free ring buffer)       to the PC viewer                │
 └─────────────────────────────────────────────────────────────────────┘
```

**Capture.** Core 1 runs a tight polling loop with interrupts disabled and
samples the bus through the ESP32-S3 dedicated-GPIO instruction, which takes a
few CPU cycles per sample. This copes with any word length and with CS toggling
on every byte, which the hardware SPI slave peripheral handles poorly. A CS idle
period longer than `FRAME_GAP_US` marks the start of a new screen update.

**Decoding without D/C.** On the reference device every screen update is
exactly 1041 bytes:

```
DB xx  D5 xx  81 xx  A6  C0  A1  |  B0 [128 bytes]  B1 [128 bytes] ... B7 [128 bytes]
└─────── setup commands ───────┘    └─────── page select + one row of 8-pixel columns ───────┘
```

9 setup bytes + 8 × (1 page command + 128 pixel bytes) = 1041. The decoder
walks each frame, consumes setup commands with their argument bytes, and copies
the 128 bytes that follow each `0xB0–0xB7` page command into that page. Frames
with no page commands fall back to "the last 1024 bytes are pixels". In 9-bit
mode the D/C bit is read directly and a full SSD1306/SH1106 command emulator
is used instead.

## Hardware Requirements

| Component | Purpose |
|---|---|
| ESP32-S3 N16R8 | Capture, decoding and display reconstruction |
| NFP1315-157B OLED (1.3", 128×64, I²C, 4 buttons) | External display and controls |
| USB cable (to the native USB port) | Flashing, serial diagnostics and PC viewer |
| Thin wire / jumper wires | Bus taps and prototype connections |
| Multimeter | Finding ground and verifying logic levels |
| Device you own, or an authorized test fixture | Source of display traffic |

The display controller must be identified and configured correctly. Module part
numbers do not always tell you which controller IC is used.

## Wiring

### Source bus (inputs only)

| Source signal | ESP32-S3 |
|---|---|
| SPI CS | GPIO4 |
| SPI CLK | GPIO5 |
| SPI MOSI (data) | GPIO6 |
| GND | GND |

- **A common ground is mandatory**, even when the source runs on its own
  battery. Prefer a ground point near the display connector, or the USB
  connector's GND pin or shell after confirming continuity to system ground.
  A battery negative pad may sit behind protection or current-sense parts.
- **Verify the logic level first.** Measure CS against ground while the screen
  is idle. 3.3 V logic connects directly. 1.8 V logic may not be read reliably
  and needs a level shifter.
- Keep the taps short and run the ground wire alongside the signal wires.
  100–330 Ω series resistors on the taps add protection.

### External OLED

| OLED module | ESP32-S3 |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SDA | GPIO8 |
| SCL | GPIO9 |
| K1 | GPIO10 |
| K2 | GPIO11 |
| K3 | GPIO12 |
| K4 | GPIO13 |

Power the OLED from 3V3, not 5V, so the I²C lines stay at 3.3 V. The buttons
are expected to short to GND and use the ESP32-S3 internal pull-ups.

On the N16R8, avoid GPIO 0/3/45/46 (strapping), 19/20 (USB) and 26–37 (flash
and octal PSRAM) when changing pin assignments.

## Building and Flashing

This project uses PlatformIO with the ESP-IDF framework.

**Prerequisites:**

- PlatformIO Core or PlatformIO IDE
- ESP32-S3 development board connected through its native USB port
- Python 3 with `pyserial` for the viewer (`pip install pyserial`)

From the repository directory:

```bash
pio run                 # build
pio run -t upload       # flash
pio device monitor      # serial log (close it before starting the viewer)
```

**First flash:** if the board is running other firmware (for example an
Arduino sketch using TinyUSB), hold **BOOT**, tap **RST**, release **BOOT**,
then upload. After this firmware is installed, later uploads reset the board
automatically. If the board stays in download mode after flashing, tap **RST**.

Hardware options live in `main/config.h`.

## PC Viewer

```bash
python3 tools/viewer.py              # auto-detects the ESP32-S3
python3 tools/viewer.py --scale 8    # larger window
python3 tools/viewer.py --port /dev/ttyACM0
```

The viewer shows the mirrored screen, prints the firmware log in the terminal
and reconnects automatically when the board resets. It opens the port with
DTR/RTS released, so connecting does not reset the chip.

| Key | Action |
|---|---|
| `o` | Cycle orientation |
| `z` | Toggle 2× vertical zoom |
| `i` | Invert colors |
| `d` | Capture a bus dump to the log |
| `s` | Save a screenshot to `captures/` |
| `q` / `Esc` | Quit |

Frames are sent as 1029-byte packets (4-byte magic `A5 5A C3 3C`, 1024 bytes of
page data, XOR checksum) interleaved with log text. They are dropped when no
host is reading, so the OLED keeps working on its own.

## Button Controls

| Button | Function |
|---|---|
| K1 | Cycle display orientation: normal, flip X, flip Y, rotate 180° |
| K2 | Toggle 2× vertical zoom for 128×32 sources |
| K3 | Invert display colors |
| K4 | Dump the next screen update (bus bytes, starting at a frame boundary) to the serial log |

## Serial Diagnostics

At boot the external OLED shows a border-and-X test pattern for two seconds.
Every two seconds the firmware then prints:

```text
mirror: bytes=4164 frames=4 images=4 | transactions=4164 [len%8==0: 4164, len%9==0: 0, incomplete: 0] overflows=0 oled_errors=0
  last frame: 1041 bytes
  first bytes: DB 60 D5 20 81 38 A6 C0 A1 B0 ...
  last bytes:  ...
```

| Field | Meaning |
|---|---|
| `bytes` | Words captured |
| `frames` / `images` | Screen updates seen / decoded into an image |
| `transactions` | CS-active periods that carried at least one bit |
| `len%8==0`, `len%9==0` | How many transactions fit 8-bit or 9-bit framing |
| `incomplete` | Transactions that ended mid-word |
| `overflows` | Events lost because the ring buffer was full |
| `oled_errors` | Failed I²C transfers to the external OLED |

### Framing

- Transactions mostly divisible by 8 suggest 8-bit framing
  (`SNIFF_WORD_BITS 8`). This is the default and matches the reference device.
- Transactions mostly divisible by 9 suggest 3-wire SPI with an embedded D/C
  bit (`SNIFF_WORD_BITS 9`).
- Incomplete transactions point to a wrong sampling edge, noise, a missing
  ground, or a marginal logic level.

These are diagnostic clues, not definitive protocol identification.

### Display Issues

If the external OLED stays blank:

1. Check that the test pattern appears at boot.
2. Measure VCC to GND on the OLED module (≈3.3 V). With VCC missing, the I²C
   pull-ups can still power the controller logic enough to acknowledge writes
   (`oled_errors=0`) while the panel stays dark.
3. Confirm the I²C address (`OLED_I2C_ADDR`, usually `0x3C`).
4. Identify the controller and set `OLED_IS_SH1106` (`0` for SSD1306).

If the image is sheared, shifted or noisy:

| Symptom | Option to check |
|---|---|
| Diagonal shear | Frame layout differs from the reference device; inspect `first bytes` and a K4 dump |
| Shifted by 2 columns | `SRC_COL_OFFSET` or `OLED_IS_SH1106` |
| Random noise | `SNIFF_SAMPLE_RISING`, ground, logic level |
| Updates split into several frames | `FRAME_GAP_US` |

Not every option applies to every display controller or source protocol.

## Security Research Context

Hardware wallets typically rely on several security boundaries:

- Private-key isolation
- Secure firmware execution
- Trusted user input
- Trusted display output
- Physical device integrity
- Supply-chain integrity
- Secure companion software

A Secure Element can protect private keys while other components remain
exposed to physical observation or manipulation. Display interfaces deserve
particular attention: in many designs, the screen is driven by a general-purpose
microcontroller outside the Secure Element, so what is rendered travels over an
unencrypted bus that can be observed with a few wires.

**Important distinction:** observing a display interface does not, by itself,
demonstrate the ability to modify Secure Element state, control random-number
generation, or authorize transactions. These are separate security properties
that require separate investigation.

## Limitations

- Not a replacement for a hardware wallet's Secure Element
- Not a firmware integrity or authenticity verification tool
- Does not authenticate the source of displayed information
- Does not establish the security of a recovery workflow
- Does not prove that physical inputs can be spoofed
- Does not establish resistance to sophisticated hardware implants
- The frame parser targets the reference device's layout; other devices may
  need parser changes

## Responsible Research

- Use synthetic recovery phrases and test wallets with no real funds.
- Treat bus dumps, `last bytes` log lines, viewer screenshots and recordings as
  sensitive: they contain raw screen pixels. `captures/` is git-ignored for
  this reason.
- Avoid exposing genuine secrets in logs or public demonstrations.
- Coordinate disclosure of newly discovered vulnerabilities with affected
  vendors before publishing exploit-specific details.

## Acknowledgments

Special thanks to:

- **Mark Karpelès ([@MagicalTux](https://x.com/MagicalTux))**: hardware
  security and supply-chain attack research
- **Joe Grand ([@JoeGrand](https://x.com/JoeGrand))**: hardware security
  research and public education
- Independent researchers contributing to the security of the cryptocurrency
  ecosystem

- Research reference: https://x.com/MagicalTux/status/2109031679528702257
- Demonstration: https://x.com/0xoucan/status/2109674845315190963

## Related Project: LeekWallet

**LeekWallet** is an open-source DIY hardware wallet initiative focused on
making hardware security more accessible, transparent, and independently
verifiable.

- GitHub: https://github.com/0xOucan/LeekWallet
- Website: https://leekwallet.0xoucanproject.xyz/
- ETHGlobal showcase: https://ethglobal.com/showcase/leekdiyhardwarewallet-5qssg
- Demo: https://www.youtube.com/watch?v=Atiquy82jC0
- Support the initiative through TheDAO Security Fund:
  https://initiatives.thedao.fund/initiative/leekwallet-an-open-diy-hardware-wallet-with-a-secure-element

## License

Copyright 2026 0xOucan

Licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) for the
full text.

---

**Security is not a product feature. It is a property that must be continuously
tested, verified, and improved.**
