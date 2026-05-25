/**
 * @file wifi.c
 * @brief Wi-Fi 管理组件实现
 *
 * 策略:
 *  1. 主动扫描,与凭据列表求交集,按 RSSI 降序排序
 *  2. 优先尝试 last-good SSID(从 load_last_good 回调读取)
 *  3. 连接成功后通过 save_last_good 持久化
 *  4. 失败回退完整反向 deinit,绝不 ESP_ERROR_CHECK abort
 */

#include "wifi.h"
#include "config.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "wifi";

/* esp_wifi 子系统全局唯一 — 防重复初始化 */
static bool s_initialized = false;

#define BIT_CONNECTED   BIT0
#define BIT_FAIL        BIT1

#define WIFI_DEF_TIMEOUT_MS     WIFI_CONNECT_TIMEOUT_MS
#define WIFI_DEF_SCAN_MS        WIFI_SCAN_PASSIVE_MS
#define WIFI_MAX_SCORED         16

struct wifi_dev_s {
    wifi_mgr_config_t        cfg;
    const wifi_credential_t *creds;       ///< 实际生效的凭据(可能指向宏 static)
    int                      cred_count;
    EventGroupHandle_t       evt;
    esp_netif_t             *netif;
    esp_event_handler_instance_t inst_wifi;
    esp_event_handler_instance_t inst_ip;
    int                      last_good_idx;
    volatile bool            connected;
    bool                     initialized;
};

/* WIFI_CRED_LIST 宏展开为 { {ssid,pass}, ..., {NULL,NULL} },
 * wifi_credential_t 与该二元组布局兼容,直接 reinterpret 即可。 */
static const wifi_credential_t s_macro_creds[] = WIFI_CRED_LIST;

/* ---- 前向声明 ---------------------------------------------------------- */
static void      wifi_event_handler(void *arg, esp_event_base_t base,
                                    int32_t id, void *data);
static int       wifi_cred_count_of(const wifi_credential_t *list);
static int       wifi_cred_find(wifi_dev_t *dev, const char *ssid);
static esp_err_t wifi_try_one(wifi_dev_t *dev, int idx, uint32_t timeout_ms);
static void      wifi_notify(wifi_dev_t *dev, wifi_event_e e);

/* ============================================================
 *  公共 API
 * ============================================================ */

esp_err_t wifi_mgr_init(wifi_dev_t **dev, const wifi_mgr_config_t *config)
{
    if (!dev || !config) return ESP_ERR_INVALID_ARG;
    if (s_initialized) return ESP_ERR_INVALID_STATE;

    /* persist 回调必须同时给或同时缺 */
    if ((config->load_last_good == NULL) != (config->save_last_good == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_dev_t *d = calloc(1, sizeof(*d));
    if (!d) return ESP_ERR_NO_MEM;

    d->cfg = *config;
    if (d->cfg.connect_timeout_ms == 0) d->cfg.connect_timeout_ms = WIFI_DEF_TIMEOUT_MS;
    if (d->cfg.scan_passive_ms    == 0) d->cfg.scan_passive_ms    = WIFI_DEF_SCAN_MS;

    /* 凭据来源:注入优先,否则用宏 */
    if (d->cfg.cred_list) {
        d->creds = d->cfg.cred_list;
    } else {
        d->creds = s_macro_creds;
    }
    d->cred_count = wifi_cred_count_of(d->creds);
    if (d->cred_count == 0) {
        ESP_LOGE(TAG, "凭据列表为空");
        free(d);
        return ESP_ERR_INVALID_ARG;
    }
    d->last_good_idx = -1;

    /* NVS — Wi-Fi 驱动需要 */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) { ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(ret)); goto fail_free; }

    /* netif + event_loop */
    ret = esp_netif_init();
    if (ret != ESP_OK) { ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(ret)); goto fail_free; }
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event_loop_create: %s", esp_err_to_name(ret));
        goto fail_netif;
    }

    d->netif = esp_netif_create_default_wifi_sta();
    if (!d->netif) { ESP_LOGE(TAG, "create_default_wifi_sta failed"); ret = ESP_FAIL; goto fail_loop; }

    /* 事件 */
    d->evt = xEventGroupCreate();
    if (!d->evt) { ret = ESP_ERR_NO_MEM; goto fail_netif_destroy; }

    ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event_handler, d, &d->inst_wifi);
    if (ret != ESP_OK) goto fail_evt;
    ret = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              wifi_event_handler, d, &d->inst_ip);
    if (ret != ESP_OK) goto fail_evt_wifi;

    /* esp_wifi */
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&wcfg);
    if (ret != ESP_OK) goto fail_evt_ip;

    ret = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (ret != ESP_OK) goto fail_wifi;
    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) goto fail_wifi;
    ret = esp_wifi_start();
    if (ret != ESP_OK) goto fail_wifi;

    /* 持久化的 last_good_ssid */
    if (d->cfg.load_last_good) {
        char ssid[33] = {0};
        d->cfg.load_last_good(ssid, sizeof(ssid), d->cfg.persist_user);
        if (ssid[0]) {
            int idx = wifi_cred_find(d, ssid);
            if (idx >= 0) {
                d->last_good_idx = idx;
                ESP_LOGI(TAG, "last_good_ssid='%s' idx=%d", ssid, idx);
            } else {
                ESP_LOGW(TAG, "持久化的 ssid '%s' 不在凭据列表中", ssid);
            }
        }
    }

    d->initialized = true;
    s_initialized  = true;
    *dev = d;
    ESP_LOGI(TAG, "Wi-Fi 初始化完成 (creds=%d)", d->cred_count);
    return ESP_OK;

fail_wifi:
    esp_wifi_deinit();
fail_evt_ip:
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, d->inst_ip);
fail_evt_wifi:
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, d->inst_wifi);
fail_evt:
    vEventGroupDelete(d->evt);
fail_netif_destroy:
    esp_netif_destroy_default_wifi(d->netif);
fail_loop:
    /* event_loop 是全局,不能在这里 delete(可能其它组件已用) */
fail_netif:
    /* esp_netif_deinit 同样不安全在错误路径调 */
fail_free:
    free(d);
    *dev = NULL;
    return ret != ESP_OK ? ret : ESP_FAIL;
}

esp_err_t wifi_mgr_deinit(wifi_dev_t **dev)
{
    if (!dev || !*dev) return ESP_ERR_INVALID_ARG;
    wifi_dev_t *d = *dev;
    if (!d->initialized) return ESP_ERR_INVALID_STATE;

    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();

    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, d->inst_ip);
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, d->inst_wifi);

    if (d->evt) vEventGroupDelete(d->evt);
    if (d->netif) esp_netif_destroy_default_wifi(d->netif);

    d->initialized = false;
    s_initialized  = false;
    free(d);
    *dev = NULL;
    ESP_LOGI(TAG, "Wi-Fi 反初始化完成");
    return ESP_OK;
}

bool wifi_mgr_is_connected(wifi_dev_t *dev)
{
    return (dev && dev->initialized) ? dev->connected : false;
}

/* ============================================================
 *  连接逻辑
 * ============================================================ */

typedef struct {
    int cred_idx;
    int rssi;
} wifi_scored_t;

static int wifi_scored_cmp(const void *a, const void *b)
{
    return ((const wifi_scored_t *)b)->rssi - ((const wifi_scored_t *)a)->rssi;
}

esp_err_t wifi_mgr_connect_blocking(wifi_dev_t *dev, uint32_t timeout_ms)
{
    if (!dev || !dev->initialized) return ESP_ERR_INVALID_STATE;
    if (timeout_ms == 0) timeout_ms = dev->cfg.connect_timeout_ms;

    wifi_notify(dev, WIFI_EVT_LINKING);

    /* 优先尝试 last-good */
    if (dev->last_good_idx >= 0 && dev->last_good_idx < dev->cred_count) {
        if (wifi_try_one(dev, dev->last_good_idx, timeout_ms / 2) == ESP_OK) {
            return ESP_OK;
        }
    }

    /* 主动扫描 */
    wifi_scan_config_t sc = { 0 };
    sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    esp_err_t ret = esp_wifi_scan_start(&sc, true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "扫描失败,退化为顺序尝试");
        for (int i = 0; i < dev->cred_count; i++) {
            if (wifi_try_one(dev, i, timeout_ms / dev->cred_count) == ESP_OK) {
                return ESP_OK;
            }
        }
        wifi_notify(dev, WIFI_EVT_FAILED);
        return ESP_FAIL;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        wifi_notify(dev, WIFI_EVT_FAILED);
        return ESP_ERR_NOT_FOUND;
    }

    wifi_ap_record_t *aps = calloc(ap_count, sizeof(*aps));
    if (!aps) return ESP_ERR_NO_MEM;
    esp_wifi_scan_get_ap_records(&ap_count, aps);

    /* 打印扫描结果(全部),便于现场排错 */
    ESP_LOGI(TAG, "扫描到 %u 个 AP:", (unsigned)ap_count);
    for (int i = 0; i < ap_count; i++) {
        ESP_LOGI(TAG, "  [%2d] rssi=%4d ch=%2u auth=%d ssid='%s'",
                 i, aps[i].rssi, (unsigned)aps[i].primary,
                 (int)aps[i].authmode, (const char *)aps[i].ssid);
    }
    wifi_scored_t scored[WIFI_MAX_SCORED];
    int sn = 0;
    for (int i = 0; i < ap_count && sn < WIFI_MAX_SCORED; i++) {
        int idx = wifi_cred_find(dev, (const char *)aps[i].ssid);
        if (idx < 0) continue;
        scored[sn].cred_idx = idx;
        scored[sn].rssi     = aps[i].rssi;
        sn++;
    }
    free(aps);

    if (sn == 0) {
        ESP_LOGW(TAG, "周围未发现已知 SSID");
        wifi_notify(dev, WIFI_EVT_FAILED);
        return ESP_ERR_NOT_FOUND;
    }

    qsort(scored, sn, sizeof(scored[0]), wifi_scored_cmp);

    for (int i = 0; i < sn; i++) {
        ESP_LOGI(TAG, "尝试 ssid='%s' rssi=%d",
                 dev->creds[scored[i].cred_idx].ssid, scored[i].rssi);
        if (wifi_try_one(dev, scored[i].cred_idx, timeout_ms / sn) == ESP_OK) {
            return ESP_OK;
        }
    }

    wifi_notify(dev, WIFI_EVT_FAILED);
    return ESP_FAIL;
}

/* ============================================================
 *  内部
 * ============================================================ */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    wifi_dev_t *dev = (wifi_dev_t *)arg;
    if (!dev) return;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        dev->connected = false;
        if (dev->evt) xEventGroupSetBits(dev->evt, BIT_FAIL);
        wifi_notify(dev, WIFI_EVT_DISCONNECTED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        dev->connected = true;
        if (dev->evt) xEventGroupSetBits(dev->evt, BIT_CONNECTED);
        wifi_notify(dev, WIFI_EVT_CONNECTED);
    }
}

static int wifi_cred_count_of(const wifi_credential_t *list)
{
    int n = 0;
    while (list && list[n].ssid) n++;
    return n;
}

static int wifi_cred_find(wifi_dev_t *dev, const char *ssid)
{
    if (!ssid) return -1;
    for (int i = 0; i < dev->cred_count; i++) {
        if (dev->creds[i].ssid && strcmp(dev->creds[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

static esp_err_t wifi_try_one(wifi_dev_t *dev, int idx, uint32_t timeout_ms)
{
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, dev->creds[idx].ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password,
            dev->creds[idx].password ? dev->creds[idx].password : "",
            sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;

    esp_err_t r = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (r != ESP_OK) return r;

    xEventGroupClearBits(dev->evt, BIT_CONNECTED | BIT_FAIL);
    esp_wifi_disconnect();
    r = esp_wifi_connect();
    if (r != ESP_OK) return r;

    EventBits_t bits = xEventGroupWaitBits(
        dev->evt, BIT_CONNECTED | BIT_FAIL,
        pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));

    if (bits & BIT_CONNECTED) {
        if (dev->last_good_idx != idx && dev->cfg.save_last_good) {
            dev->cfg.save_last_good(dev->creds[idx].ssid, dev->cfg.persist_user);
        }
        dev->last_good_idx = idx;
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

static void wifi_notify(wifi_dev_t *dev, wifi_event_e e)
{
    if (dev && dev->cfg.on_status) dev->cfg.on_status(e, dev->cfg.status_user);
}
