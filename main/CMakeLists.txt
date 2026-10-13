idf_component_register(SRCS "main.c" "sniffer.c" "decoder.c" "oled.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_hw_support esp_driver_gpio esp_driver_i2c esp_driver_usb_serial_jtag esp_timer hal)
