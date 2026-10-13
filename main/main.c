// Mirrors a broken SSD1306/SH1106-style display onto a local I2C OLED by
// sniffing its 3-wire (9-bit) SPI bus.
//   core 1: sniffer (bit-level capture, interrupts off)
//   core 0: this task (decode, render, buttons, stats, serial dump)
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "decoder.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "oled.h"
#include "sniffer.h"

static const char *TAG = "mirror";

#define DUMP_EVENTS 2600  // a bit more than one 1041-byte frame (word + CS event per byte)
#define FRAME_MAX   2048
#define IMAGE_BYTES 1024  // 128x64 / 8
#define FRAME_IDLE_US 20000  // flush a frame if no new bytes arrive for this long

static uint8_t s_out[OLED_H / 8][OLED_W];
static uint8_t s_orient;  // bit0 = flip X, bit1 = flip Y
static bool s_zoom = SRC_HEIGHT <= 32;
static bool s_invert;

static uint32_t s_dump[DUMP_EVENTS];
static int s_dump_len = -1;  // -1 = not dumping, -2 = armed, waiting for a frame start

// 8-bit mode: bytes of the current screen update, split on CS idle gaps
static uint8_t s_frame[FRAME_MAX];
static int s_frame_len;
static int64_t s_frame_last_us;
static uint8_t s_last_frame[FRAME_MAX];
static int s_last_frame_len;

static struct {
    uint32_t words, data_words, transactions, mod8, mod9, partial, frames, images;
} s_stats;

static inline int src_pixel(int x, int y)
{
    int col = x + SRC_COL_OFFSET;
    int row = (y + g_src.start_line) & 63;
    return (g_src.fb[row >> 3][col] >> (row & 7)) & 1;
}

static void render(void)
{
    memset(s_out, 0, sizeof(s_out));
    if (!g_src.display_on) {
        return;
    }
    // Ignore the source's A0/A1 C0/C8 remap: they describe how its own glass is
    // mounted, which says nothing about ours. Orientation is picked with K1.
    bool fx = s_orient & 1;
    bool fy = (s_orient >> 1) & 1;
    bool inv = g_src.inverted ^ s_invert;
    int src_h = s_zoom ? 32 : 64;

    for (int y = 0; y < OLED_H; y++) {
        int sy = s_zoom ? y / 2 : y;
        if (fy) {
            sy = src_h - 1 - sy;
        }
        for (int x = 0; x < OLED_W; x++) {
            int sx = fx ? OLED_W - 1 - x : x;
            if (src_pixel(sx, sy) ^ inv) {
                s_out[y >> 3][x] |= 1 << (y & 7);
            }
        }
    }
}

// Without the D/C line, commands and pixels are separated by structure. This
// device sends: setup commands (with their argument bytes), then for each page
// a page-select command 0xB0..0xB7 followed by 128 pixel bytes.
// Frames with no page commands fall back to "last 1024 bytes are pixels".
static void finish_frame(void)
{
    if (s_frame_len == 0) {
        return;
    }
    s_stats.frames++;
    memcpy(s_last_frame, s_frame, s_frame_len);
    s_last_frame_len = s_frame_len;

    int pages = 0;
    for (int i = 0; i < s_frame_len;) {
        uint8_t c = s_frame[i++];
        if (c >= 0xB0 && c <= 0xB7) {
            int n = s_frame_len - i < OLED_W ? s_frame_len - i : OLED_W;
            memcpy(g_src.fb[c & 7], s_frame + i, n);
            i += n;
            pages++;
            continue;
        }
        // setup command: let the decoder track invert / display on-off
        decoder_feed(0, c);
        for (int a = decoder_arg_count(c); a > 0 && i < s_frame_len; a--) {
            decoder_feed(0, s_frame[i++]);
        }
    }
    if (pages == 0 && s_frame_len >= IMAGE_BYTES) {
        const uint8_t *img = s_frame + s_frame_len - IMAGE_BYTES;
        for (int p = 0; p < 8; p++) {
            memcpy(g_src.fb[p], img + p * OLED_W, OLED_W);
        }
        pages = 8;
    }
    if (pages) {
        g_src.generation++;
        s_stats.images++;
    }
    s_frame_len = 0;
}

static void handle_event(uint32_t ev)
{
    if (s_dump_len == -2 && EV_TYPE(ev) == EV_FRAME) {
        s_dump_len = 0;
    }
    if (s_dump_len >= 0 && s_dump_len < DUMP_EVENTS) {
        s_dump[s_dump_len++] = ev;
    }
    if (EV_TYPE(ev) == EV_FRAME) {
        finish_frame();
        return;
    }
    if (EV_TYPE(ev) == EV_CS_END) {
        uint32_t total = EV_VALUE(ev);
        if (total) {
            s_stats.transactions++;
            s_stats.mod8 += total % 8 == 0;
            s_stats.mod9 += total % 9 == 0;
            s_stats.partial += EV_BITS(ev) != 0;
        }
        return;
    }
    uint32_t w = EV_VALUE(ev);
    s_stats.words++;
#if SNIFF_WORD_BITS == 9
    uint8_t dc = (w >> 8) & 1;
    s_stats.data_words += dc;
    decoder_feed(dc, w & 0xFF);
#else
    if (s_frame_len < FRAME_MAX) {
        s_frame[s_frame_len++] = w;
    }
    s_frame_last_us = esp_timer_get_time();
#endif
}

static void print_dump(void)
{
    printf("\n---- bus dump (starts at a frame boundary; ! = CS released mid-byte) ----");
    int col = 0, offset = 0;
    for (int i = 0; i < s_dump_len; i++) {
        uint32_t ev = s_dump[i];
        if (EV_TYPE(ev) == EV_FRAME) {
            printf("\n== new frame after %lu us idle ==", (unsigned long)EV_VALUE(ev));
            col = 0, offset = 0;
            continue;
        }
        if (EV_TYPE(ev) == EV_CS_END) {
            if (EV_BITS(ev)) {
                printf(" !%lu", (unsigned long)EV_BITS(ev));
            }
            continue;
        }
        if (col == 0) {
            printf("\n%04d:", offset);
        }
        uint32_t w = EV_VALUE(ev);
#if SNIFF_WORD_BITS == 9
        printf(" %c%02lX", (w & 0x100) ? 'D' : 'C', (unsigned long)(w & 0xFF));
#else
        printf(" %02lX", (unsigned long)w);
#endif
        offset++;
        if (++col == 32) {
            col = 0;
        }
    }
    printf("\n---- end dump ----\n");
}

static void print_hex(const char *label, const uint8_t *b, int n)
{
    printf("%s", label);
    for (int i = 0; i < n; i++) {
        printf(" %02X", b[i]);
    }
    printf("\n");
}

static void print_stats(void)
{
    ESP_LOGI(TAG, "bytes=%lu frames=%lu images=%lu | transactions=%lu [len%%8==0: %lu, "
                  "len%%9==0: %lu, incomplete: %lu] overflows=%lu oled_errors=%lu",
             (unsigned long)s_stats.words, (unsigned long)s_stats.frames,
             (unsigned long)s_stats.images, (unsigned long)s_stats.transactions,
             (unsigned long)s_stats.mod8, (unsigned long)s_stats.mod9,
             (unsigned long)s_stats.partial, (unsigned long)sniffer_overflows(),
             (unsigned long)oled_errors());
    if (s_stats.frames && s_last_frame_len) {
        int pre = s_last_frame_len >= IMAGE_BYTES ? s_last_frame_len - IMAGE_BYTES : s_last_frame_len;
        printf("  last frame: %d bytes\n", s_last_frame_len);
        print_hex("  first bytes:", s_last_frame, pre > 32 ? 32 : pre);
        if (s_last_frame_len > 24) {
            print_hex("  last bytes: ", s_last_frame + s_last_frame_len - 24, 24);
        }
    }
    memset(&s_stats, 0, sizeof(s_stats));
}

// Frame packet for the PC viewer: 4 magic bytes, 1024 bytes of OLED pages, XOR checksum.
// Sent non-blocking: dropped whole if the host isn't reading.
static const uint8_t FRAME_MAGIC[4] = {0xA5, 0x5A, 0xC3, 0x3C};

static void usb_init(void)
{
    usb_serial_jtag_driver_config_t cfg = {.tx_buffer_size = 8192, .rx_buffer_size = 256};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    usb_serial_jtag_vfs_use_driver();
}

static void send_frame(void)
{
    static uint8_t pkt[sizeof(FRAME_MAGIC) + sizeof(s_out) + 1];
    memcpy(pkt, FRAME_MAGIC, sizeof(FRAME_MAGIC));
    memcpy(pkt + sizeof(FRAME_MAGIC), s_out, sizeof(s_out));
    uint8_t x = 0;
    for (size_t i = 0; i < sizeof(s_out); i++) {
        x ^= pkt[sizeof(FRAME_MAGIC) + i];
    }
    pkt[sizeof(pkt) - 1] = x;
    fflush(stdout);  // keep pending log text from splitting the packet
    usb_serial_jtag_write_bytes(pkt, sizeof(pkt), 0);
}

// Single-letter commands from the PC viewer, mirroring K1..K4.
static uint8_t pc_commands(void)
{
    uint8_t buf[16], p = 0;
    int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), 0);
    for (int i = 0; i < n; i++) {
        switch (buf[i]) {
        case 'o': p |= 1; break;
        case 'z': p |= 2; break;
        case 'i': p |= 4; break;
        case 'd': p |= 8; break;
        case 'r': p |= 16; break;
        }
    }
    return p;
}

static const int s_btn_pins[4] = {PIN_K1, PIN_K2, PIN_K3, PIN_K4};

static void buttons_init(void)
{
    for (int i = 0; i < 4; i++) {
        gpio_reset_pin(s_btn_pins[i]);
        gpio_set_direction(s_btn_pins[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(s_btn_pins[i], GPIO_PULLUP_ONLY);
    }
}

// Returns a bitmask of buttons that were just pressed (debounced by polling period).
static uint8_t buttons_poll(void)
{
    static uint8_t prev;
    uint8_t now = 0;
    for (int i = 0; i < 4; i++) {
        now |= (gpio_get_level(s_btn_pins[i]) == 0) << i;
    }
    uint8_t pressed = now & ~prev;
    prev = now;
    return pressed;
}

// Border + X: lets you check the mirror OLED draws detail, not just all-on/off.
static void test_pattern(void)
{
    memset(s_out, 0, sizeof(s_out));
    for (int x = 0; x < OLED_W; x++) {
        int ys[] = {0, OLED_H - 1, x / 2, OLED_H - 1 - x / 2};
        for (int k = 0; k < 4; k++) {
            s_out[ys[k] >> 3][x] |= 1 << (ys[k] & 7);
        }
    }
    for (int p = 0; p < OLED_H / 8; p++) {
        s_out[p][0] = s_out[p][OLED_W - 1] = 0xFF;
    }
}

void app_main(void)
{
    usb_init();
    decoder_reset();
    buttons_init();
    oled_init();
    test_pattern();
    oled_flush(s_out);
    send_frame();
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "test pattern shown, oled_errors=%lu", (unsigned long)oled_errors());
    render();
    oled_flush(s_out);
    sniffer_start();

    uint32_t last_gen = ~0u;
    bool force = true;
    int64_t last_flush = 0, last_btn = 0, last_stats = 0, last_sent = 0;

    for (;;) {
        uint32_t ev;
        int n = 0;
        while (n < 8192 && sniffer_pop(&ev)) {
            handle_event(ev);
            n++;
        }

        int64_t now = esp_timer_get_time() / 1000;

        if (now - last_btn >= 20) {
            last_btn = now;
            uint8_t p = buttons_poll() | pc_commands();
            if (p & 16) {  // PC asked for a resend (viewer just connected)
                force = true;
            }
            if (p & 1) s_orient = (s_orient + 1) & 3;
            if (p & 2) s_zoom = !s_zoom;
            if (p & 4) s_invert = !s_invert;
            if (p & 8) {
                s_dump_len = -2;  // start capturing at the next frame boundary
                ESP_LOGI(TAG, "dump armed, waiting for the next screen update...");
            }
            if (p & 7) {
                force = true;
                ESP_LOGI(TAG, "orient=%u zoom=%d invert=%d", s_orient, s_zoom, s_invert);
            }
        }

        if (s_frame_len && esp_timer_get_time() - s_frame_last_us > FRAME_IDLE_US) {
            finish_frame();
            if (s_dump_len > 0) {  // bus went quiet: print what we have
                s_dump_len = DUMP_EVENTS < s_dump_len ? DUMP_EVENTS : s_dump_len;
                print_dump();
                s_dump_len = -1;
            }
        }

        if (s_dump_len == DUMP_EVENTS) {
            print_dump();
            s_dump_len = -1;
        }

        if (now - last_flush >= MIRROR_MIN_FRAME_MS && (force || g_src.generation != last_gen)) {
            last_flush = now;
            last_gen = g_src.generation;
            force = false;
            render();
            oled_flush(s_out);
            send_frame();
            last_sent = now;
        } else if (now - last_sent >= 1000) {  // keepalive so a late viewer gets an image
            send_frame();
            last_sent = now;
        }

        if (now - last_stats >= 2000) {
            last_stats = now;
            print_stats();
        }

        if (n == 0) {
            vTaskDelay(1);
        }
    }
}
