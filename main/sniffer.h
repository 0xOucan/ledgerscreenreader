#pragma once
#include <stdbool.h>
#include <stdint.h>

// Event layout: [31:30] type, [29:16] bit count, [15:0] value
#define EV_WORD   0u  // bits = SNIFF_WORD_BITS, value = the word
#define EV_CS_END 1u  // bits = leftover bits not forming a full word, value = total bits in transaction
#define EV_FRAME  2u  // CS was idle > FRAME_GAP_US before this transaction, value = idle time in us
#define EV_TYPE(e)  ((e) >> 30)
#define EV_BITS(e)  (((e) >> 16) & 0x3FFF)
#define EV_VALUE(e) ((e) & 0xFFFF)

// Starts the sniffer on core 1. It never returns control of that core.
void sniffer_start(void);
bool sniffer_pop(uint32_t *ev);
uint32_t sniffer_overflows(void);
