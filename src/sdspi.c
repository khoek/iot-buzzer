#include "sdspi.h"

#include <esp_vfs_fat.h>

// If `format_if_mount_failed` is true when `sdspi_mount()` is called, and we
// need to reformat the SD card, then this allocation unit size is used,
#define REFORMAT_ALLOCATION_UNIT_SIZE (16 * 1024)

// Note: `max_files` is max number of files opened by software, not max files on
// sd card filesystem. Note: * `pin_cs` is SPI chip select (required),
//       * `pin_cd` is chip detect (use `SDSPI_SLOT_NO_CD` if missing),
//       * `pin_wp` is write protect (use `SDSPI_SLOT_NO_WP` if missing)
void sdspi_mount(const char *mountpoint, spi_host_device_t spi_host,
                 gpio_num_t pin_cs, gpio_num_t pin_cd, gpio_num_t pin_wp,
                 bool format_if_mount_failed, int max_files,
                 sdmmc_card_t **handle) {
    sdmmc_host_t host = {
        .flags = SDMMC_HOST_FLAG_SPI | SDMMC_HOST_FLAG_DEINIT_ARG,
        .slot = spi_host,
        .max_freq_khz = SDMMC_FREQ_DEFAULT,
        .io_voltage = 3.3f,
        .init = &sdspi_host_init,
        .set_bus_width = NULL,
        .get_bus_width = NULL,
        .set_bus_ddr_mode = NULL,
        .set_card_clk = &sdspi_host_set_card_clk,
        .do_transaction = &sdspi_host_do_transaction,
        .deinit_p = &sdspi_host_remove_device,
        .io_int_enable = &sdspi_host_io_int_enable,
        .io_int_wait = &sdspi_host_io_int_wait,
        .command_timeout_ms = 0,
    };

    // This initializes the slot without card detect (CD) and write protect (WP)
    // signals. Modify slot_config.gpio_cd and slot_config.gpio_wp if your board
    // has these signals.
    sdspi_device_config_t slot_config = {
        .host_id = spi_host,
        .gpio_cs = pin_cs,
        .gpio_cd = pin_cd,
        .gpio_wp = pin_wp,
        .gpio_int = GPIO_NUM_NC,
    };

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = format_if_mount_failed,
        .max_files =
            max_files,  // Note: Max number of files opened by software, not max
                        // files on sd card filesystem.
        .allocation_unit_size = REFORMAT_ALLOCATION_UNIT_SIZE};

    ESP_ERROR_CHECK(esp_vfs_fat_sdspi_mount(mountpoint, &host, &slot_config,
                                            &mount_config, handle));
}
