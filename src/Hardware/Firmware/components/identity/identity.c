/**
 * @file identity.c
 *
 * batch_uuid / batch_secret 来自 config.h 编译时常量(BATCH_UUID / BATCH_SECRET),
 * 出厂烧录主固件即带凭证,不再依赖 NVS provisioning。换批次 = 重新编译并烧录。
 */
#include "identity.h"
#include "config.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "mbedtls/md.h"

#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "identity";

static char s_device_id[IDENTITY_DEVICE_ID_LEN + 1] = {0};
static bool s_ready = false;

static void hex_encode(const uint8_t *in, size_t in_len, char *out) {
    static const char *H = "0123456789abcdef";
    for (size_t i = 0; i < in_len; ++i) {
        out[2 * i]     = H[(in[i] >> 4) & 0xF];
        out[2 * i + 1] = H[in[i] & 0xF];
    }
    out[2 * in_len] = '\0';
}

esp_err_t identity_init(void) {
    uint8_t mac[6] = {0};
    esp_err_t r = esp_efuse_mac_get_default(mac);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "efuse mac get failed: %s", esp_err_to_name(r));
        return r;
    }
    hex_encode(mac, 6, s_device_id);
    s_ready = true;
    ESP_LOGI(TAG, "device_id=%s batch=%s", s_device_id, BATCH_UUID);
    return ESP_OK;
}

const char *identity_get_device_id(void) {
    return s_device_id;
}

const char *identity_get_batch_uuid(void) {
    return BATCH_UUID;
}

esp_err_t identity_build_password(char *out, size_t out_len) {
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (out_len < IDENTITY_PASSWORD_MAX) return ESP_ERR_INVALID_ARG;

    time_t now = time(NULL);
    if (now < 1700000000) {  // sanity: SNTP 未完成
        ESP_LOGE(TAG, "time not synced");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t nonce[MQTT_HMAC_NONCE_BYTES];
    esp_fill_random(nonce, sizeof(nonce));
    char nonce_hex[MQTT_HMAC_NONCE_BYTES * 2 + 1];
    hex_encode(nonce, sizeof(nonce), nonce_hex);

    char message[64 + sizeof(nonce_hex) + 32];
    int ml = snprintf(message, sizeof(message), "%s|%lld|%s",
                      s_device_id, (long long)now, nonce_hex);
    if (ml < 0 || ml >= (int)sizeof(message)) return ESP_FAIL;

    uint8_t mac[32];
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int rc = mbedtls_md_hmac(info,
                              (const uint8_t *)BATCH_SECRET, strlen(BATCH_SECRET),
                              (const uint8_t *)message, ml,
                              mac);
    if (rc != 0) {
        ESP_LOGE(TAG, "hmac fail rc=%d", rc);
        return ESP_FAIL;
    }
    char mac_hex[65];
    hex_encode(mac, 32, mac_hex);
    int pl = snprintf(out, out_len, "%lld:%s:%s",
                      (long long)now, nonce_hex, mac_hex);
    if (pl < 0 || pl >= (int)out_len) return ESP_FAIL;
    return ESP_OK;
}
