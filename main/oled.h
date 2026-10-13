#pragma once
#include <stdint.h>

#define OLED_W 128
#define OLED_H 64

void oled_init(void);
// fb: 8 pages x 128 columns, LSB = top pixel of the page
void oled_flush(const uint8_t fb[OLED_H / 8][OLED_W]);
// Failed I2C transfers since boot.
uint32_t oled_errors(void);
