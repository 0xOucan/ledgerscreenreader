// Bit-level SPI sniffer. Core 1 polls the three lines through the dedicated
// GPIO CPU instruction (a few cycles per sample, >10 MS/s) with interrupts
// disabled, so a 1 MHz bus is oversampled ~10x and no edge is missed. This
// handles any word length and any CS behaviour, unlike the SPI slave driver.
#include "sniffer.h"

#include "config.h"
#include "driver/dedic_gpio.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_private/esp_clk.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/dedic_gpio_cpu_ll.h"

#define RING_SIZE 16384u  // power of two
#define WORD_MASK ((1u << SNIFF_WORD_BITS) - 1u)

static const char *TAG = "sniffer";

static DRAM_ATTR uint32_t s_ring[RING_SIZE];
static volatile uint32_t s_head;  // written by core 1
static volatile uint32_t s_tail;  // written by core 0
static volatile uint32_t s_overflows;

static inline void IRAM_ATTR push(uint32_t ev)
{
    uint32_t head = s_head;
    if (head - __atomic_load_n(&s_tail, __ATOMIC_ACQUIRE) >= RING_SIZE) {
        s_overflows++;
        return;
    }
    s_ring[head & (RING_SIZE - 1)] = ev;
    __atomic_store_n(&s_head, head + 1, __ATOMIC_RELEASE);
}

bool sniffer_pop(uint32_t *ev)
{
    uint32_t tail = s_tail;
    if (tail == __atomic_load_n(&s_head, __ATOMIC_ACQUIRE)) {
        return false;
    }
    *ev = s_ring[tail & (RING_SIZE - 1)];
    __atomic_store_n(&s_tail, tail + 1, __ATOMIC_RELEASE);
    return true;
}

uint32_t sniffer_overflows(void)
{
    return s_overflows;
}

static void IRAM_ATTR sniff_loop(uint32_t cs_m, uint32_t clk_m, uint32_t d_m, uint32_t cycles_per_us)
{
    const uint32_t gap_cycles = FRAME_GAP_US * cycles_per_us;
    uint32_t cs_released_at = esp_cpu_get_cycle_count();
    const uint32_t cs_active_val = SNIFF_CS_ACTIVE_LOW ? 0 : cs_m;
    const uint32_t clk_edge_val = SNIFF_SAMPLE_RISING ? clk_m : 0;

    uint32_t prev = dedic_gpio_cpu_ll_read_in();
    uint32_t shift = 0, n = 0, total = 0;

    for (;;) {
        uint32_t in = dedic_gpio_cpu_ll_read_in();
        uint32_t changed = in ^ prev;
        if (!changed) {
            continue;
        }
        bool active = (in & cs_m) == cs_active_val;
        if (changed & cs_m) {
            uint32_t now = esp_cpu_get_cycle_count();
            if (!active) {
                uint32_t t = total > 0xFFFF ? 0xFFFF : total;
                push((EV_CS_END << 30) | (n << 16) | t);
                cs_released_at = now;
            } else if (now - cs_released_at > gap_cycles) {
                uint32_t gap_us = (now - cs_released_at) / cycles_per_us;
                push((EV_FRAME << 30) | (gap_us > 0xFFFF ? 0xFFFF : gap_us));
            }
            shift = n = total = 0;
        }
        if ((changed & clk_m) && active && (in & clk_m) == clk_edge_val) {
            shift = (shift << 1) | ((in & d_m) ? 1 : 0);
            total++;
            if (++n == SNIFF_WORD_BITS) {
                push((EV_WORD << 30) | (SNIFF_WORD_BITS << 16) | (shift & WORD_MASK));
                shift = n = 0;
            }
        }
        prev = in;
    }
}

static void sniffer_task(void *arg)
{
    const int pins[] = {PIN_SNIFF_CS, PIN_SNIFF_CLK, PIN_SNIFF_MOSI};
    for (int i = 0; i < 3; i++) {
        gpio_reset_pin(pins[i]);
        gpio_set_direction(pins[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(pins[i], GPIO_FLOATING);
    }

    dedic_gpio_bundle_config_t cfg = {
        .gpio_array = pins,
        .array_size = 3,
        .flags = {.in_en = 1},
    };
    dedic_gpio_bundle_handle_t bundle;
    ESP_ERROR_CHECK(dedic_gpio_new_bundle(&cfg, &bundle));
    uint32_t off;
    ESP_ERROR_CHECK(dedic_gpio_get_in_offset(bundle, &off));

    ESP_LOGI(TAG, "sniffing CS=%d CLK=%d DATA=%d, %d-bit words on core %d",
             PIN_SNIFF_CS, PIN_SNIFF_CLK, PIN_SNIFF_MOSI, SNIFF_WORD_BITS, xPortGetCoreID());
    vTaskDelay(pdMS_TO_TICKS(50));  // let the log flush

    portDISABLE_INTERRUPTS();
    sniff_loop(1u << off, 1u << (off + 1), 1u << (off + 2), esp_clk_cpu_freq() / 1000000);
}

void sniffer_start(void)
{
    xTaskCreatePinnedToCore(sniffer_task, "sniffer", 4096, NULL, configMAX_PRIORITIES - 1, NULL, 1);
}
