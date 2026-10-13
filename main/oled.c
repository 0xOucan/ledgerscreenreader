#include "oled.h"

#include "config.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "oled";
static i2c_master_dev_handle_t s_dev;
static uint32_t s_errors;

uint32_t oled_errors(void)
{
    return s_errors;
}

static void cmds(const uint8_t *c, size_t n)
{
    uint8_t buf[32];
    buf[0] = 0x00;  // control byte: command stream
    for (size_t i = 0; i < n; i++) {
        buf[i + 1] = c[i];
    }
    if (i2c_master_transmit(s_dev, buf, n + 1, 100) != ESP_OK) {
        s_errors++;
    }
}

void oled_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_OLED_SDA,
        .scl_io_num = PIN_OLED_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    if (i2c_master_probe(bus, OLED_I2C_ADDR, 100) != ESP_OK) {
        ESP_LOGE(TAG, "no OLED answering at 0x%02X (check SDA/SCL wiring or try 0x3D)", OLED_I2C_ADDR);
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OLED_I2C_ADDR,
        .scl_speed_hz = OLED_I2C_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev));

    static const uint8_t init[] = {
        0xAE,        // display off
        0xD5, 0x80,  // clock divide
        0xA8, 0x3F,  // multiplex 64
        0xD3, 0x00,  // display offset
        0x40,        // start line 0
#if OLED_IS_SH1106
        0xAD, 0x8B,  // DC-DC on
#else
        0x8D, 0x14,  // charge pump on
        0x20, 0x02,  // page addressing mode
#endif
        0xA1,        // segment remap
        0xC8,        // COM scan descending
        0xDA, 0x12,  // COM pins
        0x81, 0xCF,  // contrast
        0xD9, 0xF1,  // precharge
        0xDB, 0x40,  // VCOMH
        0xA4,        // display from RAM
        0xA6,        // normal (not inverted)
        0xAF,        // display on
    };
    cmds(init, sizeof(init));
}

void oled_flush(const uint8_t fb[OLED_H / 8][OLED_W])
{
    const uint8_t col = OLED_IS_SH1106 ? 2 : 0;  // SH1106 glass starts at RAM column 2
    uint8_t buf[OLED_W + 1];
    buf[0] = 0x40;  // control byte: data stream
    for (int p = 0; p < OLED_H / 8; p++) {
        uint8_t addr[] = {(uint8_t)(0xB0 | p), (uint8_t)(col & 0x0F), (uint8_t)(0x10 | (col >> 4))};
        cmds(addr, sizeof(addr));
        for (int x = 0; x < OLED_W; x++) {
            buf[x + 1] = fb[p][x];
        }
        if (i2c_master_transmit(s_dev, buf, sizeof(buf), 100) != ESP_OK) {
            s_errors++;
        }
    }
}
