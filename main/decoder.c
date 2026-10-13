#include "decoder.h"

#include <string.h>

src_display_t g_src;

enum { MODE_HORIZONTAL = 0, MODE_VERTICAL = 1, MODE_PAGE = 2 };

static uint8_t s_col, s_page;
static uint8_t s_col_start, s_col_end = 127, s_page_start, s_page_end = 7;
static uint8_t s_mode = MODE_PAGE;

static uint8_t s_cmd, s_args[6], s_args_got, s_args_needed;

void decoder_reset(void)
{
    memset(&g_src, 0, sizeof(g_src));
    g_src.display_on = 1;
    g_src.seg_remap = 1;  // assume upright until the device sends its init sequence
    g_src.com_remap = 1;
    s_col = s_page = 0;
    s_col_start = 0, s_col_end = 127, s_page_start = 0, s_page_end = 7;
    s_mode = MODE_PAGE;
    s_args_needed = 0;
}

uint8_t decoder_arg_count(uint8_t c)
{
    switch (c) {
    case 0x20: case 0x81: case 0x82: case 0x8D: case 0xA8: case 0xAD:
    case 0xD3: case 0xD5: case 0xD8: case 0xD9: case 0xDA: case 0xDB:
    case 0xDC: case 0xFD:
        return 1;
    case 0x21: case 0x22: case 0xA3:
        return 2;
    case 0x29: case 0x2A:
        return 5;
    case 0x26: case 0x27:
        return 6;
    default:
        return 0;
    }
}

static void apply_with_args(void)
{
    switch (s_cmd) {
    case 0x20:
        s_mode = s_args[0] & 3;
        break;
    case 0x21:
        s_col_start = s_col = s_args[0] & 0x7F;
        s_col_end = s_args[1] & 0x7F;
        break;
    case 0x22:
        s_page_start = s_page = s_args[0] & 7;
        s_page_end = s_args[1] & 7;
        break;
    case 0xDC:  // SH1107-style start line
        g_src.start_line = s_args[0] & 0x3F;
        g_src.generation++;
        break;
    }
}

static void command(uint8_t c)
{
    if (s_args_needed) {
        s_args[s_args_got++] = c;
        if (s_args_got == s_args_needed) {
            s_args_needed = 0;
            apply_with_args();
        }
        return;
    }
    uint8_t n = decoder_arg_count(c);
    if (n) {
        s_cmd = c;
        s_args_got = 0;
        s_args_needed = n;
        return;
    }

    if (c <= 0x0F) {
        s_col = (s_col & 0xF0) | c;
    } else if (c <= 0x1F) {
        s_col = (s_col & 0x0F) | ((c & 0x0F) << 4);
    } else if (c >= 0x40 && c <= 0x7F) {
        g_src.start_line = c & 0x3F;
    } else if (c >= 0xB0 && c <= 0xBF) {
        s_page = c & 7;
    } else {
        switch (c) {
        case 0xA0: g_src.seg_remap = 0; break;
        case 0xA1: g_src.seg_remap = 1; break;
        case 0xA6: g_src.inverted = 0; break;
        case 0xA7: g_src.inverted = 1; break;
        case 0xAE: g_src.display_on = 0; break;
        case 0xAF: g_src.display_on = 1; break;
        case 0xC0: g_src.com_remap = 0; break;
        case 0xC8: g_src.com_remap = 1; break;
        default: return;
        }
    }
    g_src.generation++;
}

static void data(uint8_t b)
{
    if (s_col < SRC_COLS && s_page < SRC_PAGES) {
        g_src.fb[s_page][s_col] = b;
    }
    g_src.generation++;

    switch (s_mode) {
    case MODE_HORIZONTAL:
        if (s_col >= s_col_end) {
            s_col = s_col_start;
            s_page = s_page >= s_page_end ? s_page_start : s_page + 1;
        } else {
            s_col++;
        }
        break;
    case MODE_VERTICAL:
        if (s_page >= s_page_end) {
            s_page = s_page_start;
            s_col = s_col >= s_col_end ? s_col_start : s_col + 1;
        } else {
            s_page++;
        }
        break;
    default:  // page mode: column advances, page stays
        s_col = s_col + 1 >= SRC_COLS ? 0 : s_col + 1;
        break;
    }
}

void decoder_feed(uint8_t is_data, uint8_t byte)
{
    if (is_data) {
        s_args_needed = 0;
        data(byte);
    } else {
        command(byte);
    }
}
