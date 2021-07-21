#include <cJSON.h>
#include <dirent.h>
#include <esp_log.h>
#include <libesp.h>
#include <libiot.h>
#include <vs1053_player.h>

#include "sdspi.h"
#include "secret.h"

#define DEVICE_NAME "buzzer"

#define SD_MOUNT_POINT "/sdcard"

#define DEFAULT_VOLUME 0x40

#define PIN_SPI_SCLK 5
#define PIN_SPI_MOSI 18
#define PIN_SPI_MISO 19

#define PIN_VS1053_CS 32
#define PIN_VS1053_DCS 33
#define PIN_VS1053_DREQ 15

#define PIN_SDCARD_CS 14

#define IMIN(a, b)             \
    ({                         \
        int __a = (a);         \
        int __b = (b);         \
        __a < __b ? __a : __b; \
    })

#define STRLEN(str) (sizeof((str)) / sizeof(char))

static const char* TAG = "app";

static vs1053_handle_t audio;
static vs1053_player_handle_t player;

#define VOLUME_MIN 0x00
#define VOLUME_MAX 0xFF

static uint8_t clip_volume(int raw_volume) {
    if (raw_volume < VOLUME_MIN) {
        ESP_LOGW(TAG, "volume clipped to min");
        return VOLUME_MIN;
    }
    if (raw_volume > VOLUME_MAX) {
        ESP_LOGW(TAG, "volume clipped to max");
        return VOLUME_MAX;
    }
    return (uint8_t) raw_volume;
}

static bool test_in_progress = false;

static void stop_current_play(bool will_start_test) {
    vs1053_player_cancel(player);
    vs1053_player_sleep_until_player_waiting(player, -1);

    if (test_in_progress) {
        vs1053_reconfigure_to_defaults(audio);
    }
    test_in_progress = will_start_test;
}

static void handle_buzz(const char* data, uint32_t data_len) {
    char* buff = malloc(data_len + 1);
    memcpy(buff, data, data_len);
    buff[data_len] = '\0';

    cJSON* json_root = cJSON_Parse(buff);
    if (!json_root) {
        ESP_LOGW(TAG, "buzz: JSON parse error");
        goto handle_buzz_out;
    }

    const cJSON* json_file = cJSON_GetObjectItemCaseSensitive(json_root, "file");
    if (!json_file || !cJSON_IsString(json_file)) {
        ESP_LOGW(TAG, "buzz: no file or not a string!");
        goto handle_buzz_out;
    }

    const cJSON* json_volume = cJSON_GetObjectItemCaseSensitive(json_root, "volume");
    if (json_volume && !cJSON_IsNumber(json_volume)) {
        ESP_LOGW(TAG, "buzz: volume not a number!");
        goto handle_buzz_out;
    }

    const char* file = json_file->valuestring;
    int volume = json_volume ? clip_volume(json_volume->valueint) : DEFAULT_VOLUME;

    char path[256];
    snprintf(path, sizeof(path) / sizeof(char), "%s/%s", SD_MOUNT_POINT, file);

    stop_current_play(false);
    vs1053_ctrl_set_volume(audio, volume, volume);

    esp_err_t ret = vs1053_player_start_playing_file(player, path);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "buzz: failed! (0x%X)", ret);
        goto handle_buzz_out;
    }

    ESP_LOGI(TAG, "buzz: file='%s', volume=0x%X", file, volume);

handle_buzz_out:
    // It is safe to call this with `json_root == NULL`.
    cJSON_Delete(json_root);
    free(buff);
}

static void handle_action(const char* data, uint32_t data_len) {
    char* buff = malloc(data_len + 1);
    memcpy(buff, data, data_len);
    buff[data_len] = '\0';

    cJSON* json_root = cJSON_Parse(buff);
    if (!json_root) {
        libiot_logf_error(TAG, "action: JSON parse error");
        goto handle_action_out;
    }

    const cJSON* json_type = cJSON_GetObjectItemCaseSensitive(json_root, "type");
    if (!json_type || !cJSON_IsString(json_type)) {
        libiot_logf_error(TAG, "action: no type or not a string!");
        goto handle_action_out;
    }

    if (!strcmp("soft_reset", json_type->valuestring)) {
        ESP_LOGW(TAG, "soft_reset");

        stop_current_play(false);
        vs1053_reconfigure_to_defaults(audio);
    } else if (!strcmp("sine_test", json_type->valuestring)) {
        const cJSON* json_volume = cJSON_GetObjectItemCaseSensitive(json_root, "volume");
        if (json_volume && !cJSON_IsNumber(json_volume)) {
            libiot_logf_error(TAG, "sine_test: volume not a number!");
            goto handle_action_out;
        }

        int volume = json_volume ? clip_volume(json_volume->valueint) : DEFAULT_VOLUME;
        ESP_LOGW(TAG, "sine_test: volume=%u", volume);

        stop_current_play(true);

        vs1053_ctrl_soft_reset(audio);
        vs1053_ctrl_allow_tests(audio, true);
        vs1053_ctrl_set_volume(audio, volume, volume);
        vs1053_test_sine(audio, 0x44);
    } else if (!strcmp("read_sdcard", json_type->valuestring)) {
        ESP_LOGW(TAG, "read_sdcard");

        DIR* dir = opendir(SD_MOUNT_POINT);
        if (!dir) {
            libiot_logf_error(TAG, "read_sdcard: cannot open '" SD_MOUNT_POINT "'");
            goto handle_action_out;
        }

        cJSON* out_json_root = cJSON_CreateObject();
        if (!out_json_root) {
            libiot_logf_error(TAG, "read_sdcard: cannot create JSON root object");
            goto handle_action_read_sdcard_fail_after_dir;
        }

        cJSON* out_json_files = cJSON_CreateArray();
        if (!out_json_files) {
            libiot_logf_error(TAG, "read_sdcard: cannot create JSON array");
            goto handle_action_read_sdcard_fail_after_json;
        }
        cJSON_AddItemToObject(out_json_root, "files", out_json_files);

        ESP_LOGI(TAG, "*** dir listing start ***");

        struct dirent* entry;
        while ((entry = readdir(dir))) {
            ESP_LOGI(TAG, "  %s", entry->d_name);

            cJSON* out_json_entry = cJSON_CreateString(entry->d_name);
            if (!out_json_entry) {
                libiot_logf_error(TAG, "read_sdcard: cannot create JSON string");
                goto handle_action_read_sdcard_fail_after_json;
            }
            cJSON_AddItemToArray(out_json_files, out_json_entry);
        }

        ESP_LOGI(TAG, "*** dir listing end ***");

        char* output = cJSON_PrintUnformatted(out_json_root);
        if (!output) {
            libiot_logf_error(TAG, "read_sdcard: cannot print JSON output");
            goto handle_action_read_sdcard_fail_after_json;
        }

        libiot_mqtt_publish_local("files", 0, 0, output);

        free(output);

    handle_action_read_sdcard_fail_after_json:
        // It is safe to call this with `json_root == NULL`.
        cJSON_Delete(out_json_root);
    handle_action_read_sdcard_fail_after_dir:
        closedir(dir);
    } else {
        libiot_logf_error(TAG, "action: unknown '%.*s'", data_len, data);
    }

handle_action_out:
    // It is safe to call this with `json_root == NULL`.
    cJSON_Delete(json_root);
    free(buff);
}

static __always_inline bool unterm_str_matches(const char* term, const char* unterm, size_t unterm_len) {
    return (unterm_len == strlen(term)) && !memcmp(term, unterm, unterm_len);
}

static void mqtt_event_handler_cb(esp_mqtt_event_handle_t event) {
    esp_mqtt_client_handle_t client = event->client;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED: {
            // Note: libiot automatically subscribes to the `IOT_MQTT_DEVICE_TOPIC()` namespace,
            // but anything outside of that must be explicitly subscribed here.
            assert(esp_mqtt_client_subscribe(client, IOT_MQTT_COMMAND_TOPIC("buzz"), 1) >= 0);
            break;
        }
        case MQTT_EVENT_DATA: {
            if (unterm_str_matches(IOT_MQTT_COMMAND_TOPIC("buzz"), event->topic, event->topic_len)) {
                handle_buzz(event->data, event->data_len);
            }

            if (unterm_str_matches(IOT_MQTT_DEVICE_TOPIC(DEVICE_NAME, "action"), event->topic, event->topic_len)) {
                handle_action(event->data, event->data_len);
            }

            break;
        }
        default: {
            break;
        }
    }
}

void app_run() {
    // Configure the "HSPI" SPI peripheral.
    spi_bus_config_t buscfg = {
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_SCLK | SPICOMMON_BUSFLAG_MOSI | SPICOMMON_BUSFLAG_MISO,

        .miso_io_num = PIN_SPI_MISO,
        .mosi_io_num = PIN_SPI_MOSI,
        .sclk_io_num = PIN_SPI_SCLK,

        .quadwp_io_num = -1,
        .quadhd_io_num = -1,

        .max_transfer_sz = SPI_MAX_DMA_LEN,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(HSPI_HOST, &buscfg, 1));

    // Configure the ESP32 to communicate with the VS1053 on "HSPI".
    vs1053_init(HSPI_HOST, PIN_VS1053_CS, PIN_VS1053_DCS, PIN_VS1053_DREQ, &audio);

    // Note: Must configure the SD card before any other traffic on the SPI bus,
    // due to the unfortunate way SD supports its (non-native) SPI mode.
    // Danger: On the other hand, all other devices on the SPI bus must be registered
    // in the `spi_master` driver before this call, since otherwise their `CS` lines
    // may be floating and they may see SD card traffic as directed at them.

    // Configure the ESP32 to communicate with the SD card on "HSPI",
    // and mount the FAT partition in the virtual filesystem.
    sdmmc_card_t* handle;
    sdspi_mount("/sdcard", HSPI_HOST,
                PIN_SDCARD_CS, SDSPI_SLOT_NO_CD, SDSPI_SLOT_NO_WP,
                false, 5, &handle);

    vs1053_ctrl_set_volume(audio, DEFAULT_VOLUME, DEFAULT_VOLUME);

    ESP_ERROR_CHECK(vs1053_player_create(audio, &player));
}

void app_main() {
    struct node_config config = {
        .name = DEVICE_NAME,
        .ssid = SECRET_WIFI_SSID,
        .pass = SECRET_WIFI_PASS,
        .ps_type = WIFI_PS_NONE,

        .uri = SECRET_MQTT_URI,
        .cert = SECRET_MQTT_CERT,
        .key = SECRET_MQTT_KEY,
        .mqtt_pass = SECRET_MQTT_PASS,
        .mqtt_cb = &mqtt_event_handler_cb,

        .app_run = &app_run,
    };

    libiot_startup(&config);
}
