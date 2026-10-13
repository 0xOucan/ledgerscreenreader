#pragma once

// ---------------------------------------------------------------------------
// Sniffed display bus (ESP32 pins are INPUTS ONLY, never driven)
// Avoid GPIO 0/3/45/46 (strapping), 19/20 (USB), 26-37 (flash + octal PSRAM).
// ---------------------------------------------------------------------------
#define PIN_SNIFF_CS        4
#define PIN_SNIFF_CLK       5
#define PIN_SNIFF_MOSI      6

// Bits per word. 8 = plain bytes, CS pulses per byte, D/C line not tapped:
// commands and pixels are told apart by their position inside each frame.
// 9 = 3-wire SPI where the first bit of each word is D/C.
#define SNIFF_WORD_BITS     8
// CS idle longer than this starts a new frame (screen update).
#define FRAME_GAP_US        1000
// 1 = sample data on CLK rising edge (SPI mode 0/3), 0 = falling (mode 1/2).
#define SNIFF_SAMPLE_RISING 1
// 1 = CS active low (normal).
#define SNIFF_CS_ACTIVE_LOW 1

// ---------------------------------------------------------------------------
// Source (broken) display
// ---------------------------------------------------------------------------
#define SRC_HEIGHT          64  // 32 for 128x32 panels, 64 for 128x64
#define SRC_COL_OFFSET      0   // first visible RAM column (2 for SH1106-driven glass)

// ---------------------------------------------------------------------------
// Mirror OLED: NFP1315-157B (1.3" 128x64, I2C)
// ---------------------------------------------------------------------------
#define PIN_OLED_SDA        8
#define PIN_OLED_SCL        9
#define OLED_I2C_ADDR       0x3C
#define OLED_I2C_HZ         400000
#define OLED_IS_SH1106      1   // 1.3" modules are usually SH1106; set 0 for SSD1306

// Module buttons K1..K4 (switch to GND, internal pull-ups enabled)
#define PIN_K1              10  // cycle orientation (none / flip X / flip Y / 180)
#define PIN_K2              11  // toggle 2x vertical zoom (128x32 source -> fill 64 rows)
#define PIN_K3              12  // toggle invert
#define PIN_K4              13  // dump next bus events to serial

#define MIRROR_MIN_FRAME_MS 33  // ~30 fps max refresh of the mirror
