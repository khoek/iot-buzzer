#pragma once

#include <driver/sdspi_host.h>

// Note: `max_files` is max number of files opened by software, not max files on
// sd card filesystem. Note: * `pin_cs` is SPI chip select (required),
//       * `pin_cd` is chip detect (use `SDSPI_SLOT_NO_CD` if missing),
//       * `pin_wp` is write protect (use `SDSPI_SLOT_NO_WP` if missing)
void sdspi_mount(const char *mountpoint, spi_host_device_t host,
                 gpio_num_t pin_cs, gpio_num_t pin_cd, gpio_num_t pin_wp,
                 bool format_if_mount_failed, int max_files,
                 sdmmc_card_t **handle);
