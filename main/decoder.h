#pragma once
#include <stdint.h>

#define SRC_COLS  132  // SH1106 RAM width (SSD1306 uses the first 128)
#define SRC_PAGES 8

// Emulated state of the source display controller (SSD1306/SH1106 command set).
typedef struct {
    uint8_t fb[SRC_PAGES][SRC_COLS];  // GDDRAM, page-major, LSB = top pixel
    uint8_t display_on;
    uint8_t inverted;
    uint8_t seg_remap;    // 0xA1
    uint8_t com_remap;    // 0xC8
    uint8_t start_line;   // 0x40..0x7F
    uint32_t generation;  // bumped on every change, for dirty detection
} src_display_t;

extern src_display_t g_src;

void decoder_reset(void);
void decoder_feed(uint8_t is_data, uint8_t byte);
// Number of argument bytes that follow command c.
uint8_t decoder_arg_count(uint8_t c);
