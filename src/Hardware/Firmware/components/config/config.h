/**
 * @file config.h
 * @brief 系统配置头文件 — BeaconOps protocol v1
 * @version 1.0
 */
#ifndef CONFIG_H
#define CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// Pin / I2C / I2S / LVGL / SPIFFS / NVS 
// ============================================================
#define I2C_SCL_PIN             GPIO_NUM_2
#define I2C_SDA_PIN             GPIO_NUM_8
#define I2S_BCK_PIN             GPIO_NUM_1
#define I2S_LRCK_PIN            GPIO_NUM_0
#define I2S_DIN_PIN             GPIO_NUM_3
#define LVGL_SPI_MOSI_PIN       GPIO_NUM_7
#define LVGL_SPI_SCLK_PIN       GPIO_NUM_6
#define LVGL_SPI_CS_PIN         GPIO_NUM_5
#define LVGL_SPI_DC_PIN         GPIO_NUM_4
#define LVGL_SPI_RST_PIN        GPIO_NUM_NC
#define LVGL_SPI_BL_PIN         GPIO_NUM_9

#define I2C_PORT_NUM            I2C_NUM_0
#define I2C_FREQUENCY_HZ        100000

#define I2S_PORT_NUM            I2S_NUM_0
#define AUDIO_SAMPLE_RATE       16000
#define AUDIO_CHANNELS          I2S_CHANNEL_MONO
#define AUDIO_BITS_PER_SAMPLE   I2S_BITS_PER_SAMPLE_16BIT
#define AUDIO_DMA_BUF_COUNT     6
#define AUDIO_DMA_BUF_LEN       256
#define AUDIO_TX_TIMEOUT_MS     500

#define LVGL_SPI_HOST           SPI2_HOST
#define LVGL_SPI_FREQUENCY_HZ   63500000
#define LVGL_STACK_SIZE         (4 * 1024)
#define LVGL_BUFFER_HEIGHT      10
#define LVGL_COLOR_DEPTH        16
#define LVGL_DISPLAY_WIDTH      172
#define LVGL_DISPLAY_HEIGHT     320
#define LVGL_X_OFFSET           0
#define LVGL_Y_OFFSET           34

#define NVS_DEFAULT_NAMESPACE   "storage"

#define SETTINGS_DEFAULT { \
    .version = 2, \
    .device_name = "BeaconOps", \
    .device_id = 0, \
    .cw2017_profile_loaded = false, \
    .last_good_ssid = "", \
}

#define SPIFFS_DEFAULT_BASE_PATH        "/spiffs"
#define SPIFFS_DEFAULT_PARTITION_LABEL  "storage"
#define SPIFFS_DEFAULT_MAX_FILES        5
#define SPIFFS_MAX_PATH_LENGTH          256

// ============================================================
// Network / Identity / MQTT  (BeaconOps protocol v1)
// ============================================================
// 固件版本号(LWT online payload 与 health 都用它)
#define FW_VERSION              "BeaconOps-1.1.3"

// MQTT broker — TLS 由 nginx stream 在 :8883 终止,后端转发到本地 Mosquitto :1883
// 证书:Let's Encrypt(ISRG Root X1),固件默认 CA 在 certs.c 已内置
#define BROKER_URI              "mqtts://YOUR_BROKER_HOST:8883"
#define MQTT_KEEPALIVE_S        60
#define MQTT_RECONNECT_MS       5000

// MQTT topics — 与 protocol-v1.md §3 一致;{} 占位运行时填入 device_id (12 hex MAC)
#define MQTT_TOPIC_CMD_FMT      "device/%s/cmd"
#define MQTT_TOPIC_ACK_FMT      "device/%s/uplink/ack"
#define MQTT_TOPIC_EVENT_FMT    "device/%s/uplink/event"
#define MQTT_TOPIC_HEALTH_FMT   "device/%s/uplink/health"
#define MQTT_TOPIC_PROFILE_FMT  "device/%s/uplink/profile"
#define MQTT_TOPIC_STATUS_FMT   "device/%s/status"
#define MQTT_TOPIC_BCAST_ALL    "broadcast/all/cmd"
#define MQTT_TOPIC_BCAST_DEPT_FMT  "broadcast/dept/%s/cmd"

// LWT 与 online payload 运行时拼装,不再用宏字符串(含 device_id)。

// HMAC 鉴权
#define MQTT_HMAC_NONCE_BYTES   16
#define MQTT_HMAC_MAX_SKEW_S    300

// 批次凭证—出厂烧录在固件里(同批设备共享。换批次 = 重烧固件)
// 生产时该文件可以被产线脚本生成的 batch_credentials.h 覆盖
#ifndef BATCH_UUID
#define BATCH_UUID              "YOUR_BATCH_UUID"
#endif
#ifndef BATCH_SECRET
#define BATCH_SECRET            "YOUR_BATCH_SECRET"
#endif

// NVS 命名空间
#define NVS_NS_TX_PENDING       "tx_pending"
#define NVS_NS_WIFI             "wifi"

// Wi-Fi credentials — 多 SSID,运行时自动扫描最强信号,全部出厂烧录
// 格式: {"SSID", "PASSWORD"}, ..., {NULL, NULL}
#define WIFI_CRED_LIST          { \
    {"YOUR_SSID", "YOUR_PASSWORD"}, \
    {NULL, NULL} \
}
#define WIFI_CONNECT_TIMEOUT_MS 15000
#define WIFI_SCAN_PASSIVE_MS    300

// SNTP
#define SNTP_PRIMARY_SERVER     "ntp.aliyun.com"
#define SNTP_FALLBACK_SERVER    "pool.ntp.org"
#define TZ_STRING               "CST-8"

// ============================================================
// Sensor (IMU 行为/摇晃)
// ============================================================
#define SHAKE_LOCKOUT_MS        2500

// ============================================================
// Common
// ============================================================
#define RETRY_DELAY_MS                  10
#define MAX_RETRY_COUNT                 3
#define TRY_FUNC(func, ...)  \
    do { \
        int retry_count = 0; \
        esp_err_t ret; \
        do { \
            ret = func(__VA_ARGS__); \
            if (ret == ESP_OK) { \
                break; \
            } \
            ESP_LOGW("RETRY", "重试失败，正在继续... (%d/%d)", ++retry_count, MAX_RETRY_COUNT); \
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS)); \
        } while (retry_count < MAX_RETRY_COUNT); \
        if (ret != ESP_OK) { \
            ESP_LOGE("RETRY", "函数重试失败"); \
            ESP_ERROR_CHECK(ret); \
        } \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */
