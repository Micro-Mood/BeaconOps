/**
 * @file main.cpp
 * @brief BeaconOps 装配入口
 *
 * 仅做硬件初始化与组件装配。所有业务逻辑都在各 components/ 内,本文件
 * 通过句柄 + 回调把它们串起来。
 *
 * 启动顺序:
 *   1. 硬件 (GPIO / I2C / sensor / LVGL / 背光 / NVS / settings / SPIFFS /
 *      audio / cw2017)
 *   2. 业务 (error / notify / ui)
 *   3. 网络任务 net_bringup_task:wifi → sntp → hmac → mqtt → tx → msg → profile
 *   4. sensor_task:摇晃 / 行为采样转发到 msg + profile
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_pm.h"          /* 动态调频 + auto light sleep */
#include "esp_sleep.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* 硬件 */
#include "i2c_bus_manager.h"
#include "sensor.h"
#include "sensor_task.h"
#include "audio.h"
#include "cw2017.h"
#include "st7789.h"
#include "spiffs.h"
#include "nvslib.h"
#include "settings.h"
#include "backlight_mgr.h"
#include "config.h"

/* 业务组件 */
#include "error.h"
#include "notify.h"
#include "ui.h"
#include "wifi.h"
#include "time_sync.h"
#include "mqtt.h"
#include "identity.h"
#include "event.h"
#include "health.h"
#include "tx.h"
#include "msg.h"
#include "profile.h"

#define CHARGER_CHRG_PIN   GPIO_NUM_20
#define CHARGER_STDBY_PIN  GPIO_NUM_21

static const char *TAG = "APP_MAIN";

/* ============================================================
 *  全局句柄(装配层独占)
 * ============================================================ */
static nvs_dev_t           *g_nvs           = nullptr;
static spiffs_dev_t        *g_spiffs        = nullptr;
static i2c_bus_dev_t       *g_i2c           = nullptr;
static audio_dev_t         *g_audio         = nullptr;
static cw2017_dev_t        *g_cw2017        = nullptr;
static sensor_dev_t        *g_sensor        = nullptr;
static app_settings_t      *g_settings      = nullptr;

static err_dev_t           *g_err           = nullptr;
static notify_dev_t        *g_notify        = nullptr;
static ui_dev_t            *g_ui            = nullptr;

static wifi_dev_t          *g_wifi          = nullptr;
static mqtt_dev_t          *g_mqtt          = nullptr;
static event_dev_t         *g_event         = nullptr;
static health_dev_t        *g_health        = nullptr;
static tx_dev_t            *g_tx            = nullptr;
static msg_dev_t           *g_msg           = nullptr;
static profile_dev_t       *g_profile       = nullptr;

/* ============================================================
 *  硬件初始化(供 TRY_FUNC 使用)
 * ============================================================ */
static esp_err_t app_special_gpio_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << GPIO_NUM_10),
        .mode         = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    return gpio_config(&io);
}

static esp_err_t app_gpio_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << CHARGER_CHRG_PIN) | (1ULL << CHARGER_STDBY_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    return gpio_config(&io);
}

static esp_err_t app_i2c_init(void)
{
    i2c_bus_config_t cfg = {
        .port = I2C_PORT_NUM,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_speed = I2C_FREQUENCY_HZ,
        .pullup_enable = true,
    };
    return i2c_bus_init(&g_i2c, &cfg);
}

static esp_err_t app_sensor_init(void)
{
    sensor_config_t cfg = {
        .i2c_bus = g_i2c,
        .i2c_address = 0x6A,
        .wakeup_threshold = 0x08,
        .wakeup_duration = 0x50,
        .device_name = "LSM6DS3TRC",
    };
    esp_err_t r = sensor_init(&g_sensor, &cfg);
    if (r != ESP_OK) return r;
    return sensor_disable_wakeup(g_sensor);
}

static esp_err_t app_lvgl_init(void)
{
    const st7789_config_t cfg = {
        .lvgl_task_size       = LVGL_STACK_SIZE,
        .lvgl_task_priority   = configMAX_PRIORITIES - 1,
        .lvgl_task_affinity   = -1,
        .lvgl_timer_period_ms = 33,
        .lcd_height_res       = LVGL_DISPLAY_HEIGHT,
        .lcd_vertical_res     = LVGL_DISPLAY_WIDTH,
        .lcd_x_offset         = LVGL_X_OFFSET,
        .lcd_y_offset         = LVGL_Y_OFFSET,
        .lcd_draw_buffer_height = LVGL_BUFFER_HEIGHT,
        .lcd_bits_per_pixel   = LVGL_COLOR_DEPTH,
        .spi_host_device      = LVGL_SPI_HOST,
        .spi_freq             = LVGL_SPI_FREQUENCY_HZ,
        .backlight            = LVGL_SPI_BL_PIN,
        .sclk                 = LVGL_SPI_SCLK_PIN,
        .mosi                 = LVGL_SPI_MOSI_PIN,
        .dc                   = LVGL_SPI_DC_PIN,
        .cs                   = LVGL_SPI_CS_PIN,
        .rst                  = LVGL_SPI_RST_PIN,
    };
    esp_err_t r = lcd_init(cfg);
    if (r != ESP_OK) return r;
    return lvgl_init(cfg);
}

static esp_err_t app_backlight_init(void)
{
    backlight_mgr_cfg_t cfg = {
        .pin = LVGL_SPI_BL_PIN,
        .pwm_freq_hz = 0, .idle_off_ms = 0, .ramp_ms = 0, .wake_grace_ms = 0,
    };
    return backlight_mgr_init(&cfg);
}

static esp_err_t app_nvs_init(void)
{
    return (nvs_init(&g_nvs, NVS_DEFAULT_NAMESPACE) == NVS_OK) ? ESP_OK : ESP_FAIL;
}

static esp_err_t app_settings_init(void)
{
    g_settings = (app_settings_t *)calloc(1, sizeof(app_settings_t));
    if (!g_settings) return ESP_ERR_NO_MEM;
    return settings_load(g_nvs, g_settings);
}

static esp_err_t app_spiffs_init(void)
{
    spiffs_config_t cfg = {
        .base_path = SPIFFS_DEFAULT_BASE_PATH,
        .partition_label = SPIFFS_DEFAULT_PARTITION_LABEL,
        .max_files = SPIFFS_DEFAULT_MAX_FILES,
        .format_if_mount_failed = true,
    };
    return (spiffs_init(&g_spiffs, &cfg) == SPIFFS_OK) ? ESP_OK : ESP_FAIL;
}

static esp_err_t app_audio_init(void)
{
    audio_config_t cfg = {
        .sample_rate = AUDIO_SAMPLE_RATE,
        .bit_depth = AUDIO_BITS_PER_SAMPLE,
        .channels = AUDIO_CHANNELS,
        .dma_buf_count = AUDIO_DMA_BUF_COUNT,
        .dma_buf_len = AUDIO_DMA_BUF_LEN,
        .use_apll = false,
        .tx_timeout_ms = AUDIO_TX_TIMEOUT_MS,
    };
    return audio_init(&g_audio, &cfg, I2S_PORT_NUM, I2S_BCK_PIN, I2S_LRCK_PIN, I2S_DIN_PIN);
}

static esp_err_t app_cw2017_init(void)
{
    cw2017_config_t cfg = {
        .i2c_bus = g_i2c,
        .i2c_addr = CW2017_I2C_ADDR,
        .device_name = "CW2017",
        .battery_profile = NULL,
        .profile_size = 0,
        .write_profile = !g_settings->cw2017_profile_loaded,
    };
    esp_err_t r = cw2017_init(&g_cw2017, &cfg);
    if (r != ESP_OK) return r;
    if (!g_settings->cw2017_profile_loaded) {
        g_settings->cw2017_profile_loaded = true;
        settings_save(g_nvs, g_settings);
    }
    return ESP_OK;
}

/* ---- 业务组件 ---------------------------------------------------------- */

static esp_err_t app_error_init(void)
{
    err_config_t cfg = {
        .spiffs = g_spiffs,
        .log_dir = NULL, .ring_depth = 0, .flush_period_ms = 0,
        .file_max_bytes = 0, .file_keep = 0, .task_prio = 0, .task_stack = 0,
    };
    return err_init(&g_err, &cfg);
}

static esp_err_t app_notify_init(void)
{
    notify_config_t cfg = {
        .audio = g_audio,
        .amplitude = 35000,
        .scratch_ms = 50,
        .queue_depth = 8,
    };
    return notify_init(&g_notify, &cfg);
}

static esp_err_t app_ui_init(void)
{
    ui_config_t cfg = {0};
    return ui_init(&g_ui, &cfg);
}

/* ============================================================
 *  msg ↔ ui / audio / tx 装配回调
 * ============================================================ */

static void cb_msg_ui_show(const char *title, const char *body, int level, void *)
{
    if (level >= UI_LV_EMERG) backlight_mgr_force_on();
    notify_play(g_notify, (notify_level_e)level);
    ui_push_card(g_ui, title, body, level);
}

static void cb_msg_ui_dismiss(void *)
{
    ui_dismiss_front(g_ui);
}

static void cb_msg_ui_stack(const msg_ui_card_view_t *cards, size_t count, void *)
{
    static char front_id[37] = "";

    if (!cards || count == 0) {
        front_id[0] = '\0';
        ui_set_cards(g_ui, NULL, 0);
        return;
    }

    const char *id = cards[0].id ? cards[0].id : "";
    if (id[0] && strcmp(front_id, id) != 0) {
        strlcpy(front_id, id, sizeof(front_id));
        int level = cards[0].level;
        if (level >= UI_LV_EMERG) backlight_mgr_force_on();
    }

    ui_card_view_t ui_cards[MSG_UI_STACK_MAX];
    if (count > MSG_UI_STACK_MAX) count = MSG_UI_STACK_MAX;
    for (size_t i = 0; i < count; ++i) {
        ui_cards[i].id    = cards[i].id;
        ui_cards[i].title = cards[i].title;
        ui_cards[i].body  = cards[i].body;
        ui_cards[i].level = cards[i].level;
    }
    ui_set_cards(g_ui, ui_cards, count);
}

static void cb_msg_notify(int level, void *)
{
    if (level < NOTIFY_LV_INFO || level >= NOTIFY_LV_MAX) level = NOTIFY_LV_INFO;
    if (level >= UI_LV_EMERG) backlight_mgr_force_on();
    notify_play(g_notify, (notify_level_e)level);
}

static void cb_msg_confirm(void *)
{
    notify_play_confirm(g_notify);
}

static void cb_msg_ack(const char *msg_id, msg_ack_kind_e k, void *)
{
    /* msg_ack_kind_e 与 tx_ack_kind_e 数值同步 */
    tx_emit_ack(g_tx, msg_id, (tx_ack_kind_e)k);
}

static void cb_tx_ack_result(const char *msg_id, tx_ack_kind_e k, bool ok, void *)
{
    if (!ok) {
        ESP_LOGW(TAG, "ack 发送失败(已达上限): %s kind=%d", msg_id ? msg_id : "", (int)k);
        /* protocol v1 §3.6: 超限后上报 ack_give_up 事件 */
        if (g_event) {
            const char *kind_str = (k == TX_ACK_ACKED) ? "acknowledged"
                                  : (k == TX_ACK_DISPLAYED) ? "displayed"
                                  : (k == TX_ACK_RECEIVED)  ? "received"
                                  : (k == TX_ACK_EXPIRED)   ? "expired"
                                  : "unknown";
            event_emit(g_event, EVENT_ACK_GIVE_UP, msg_id, kind_str);
        }
        return;
    }
    if (g_msg) msg_on_ack_delivered(g_msg, msg_id, (msg_ack_kind_e)k);
}

/* ============================================================
 *  wifi / sntp / mqtt / profile 回调
 * ============================================================ */

static void cb_wifi_status(wifi_event_e evt, void *)
{
    /* 与 MQTT 同款节流:LINKING/CONNECTED 不发 toast;
     * DISCONNECTED/FAILED 同源 60s 内最多 1 条;从 CONNECTED 掉线时立即提示一次 */
    static bool     was_connected = false;
    static uint32_t last_warn_ms  = 0;
    static const uint32_t WARN_GAP = 60000;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    switch (evt) {
    case WIFI_EVT_LINKING:
        ui_set_wifi(g_ui, UI_WIFI_LINKING);
        break;
    case WIFI_EVT_CONNECTED:
        ui_set_wifi(g_ui, UI_WIFI_OK);
        was_connected = true;
        last_warn_ms  = 0;
        break;
    case WIFI_EVT_DISCONNECTED:
    case WIFI_EVT_FAILED: {
        ui_set_wifi(g_ui,
                    evt == WIFI_EVT_FAILED ? UI_WIFI_FAIL : UI_WIFI_LINKING);
        bool from_connected = was_connected;
        was_connected = false;
        if (from_connected ||
            last_warn_ms == 0 ||
            (now_ms - last_warn_ms) >= WARN_GAP) {
            last_warn_ms = now_ms;
            /* 网络是自愈型故障 → 一律用 WARN(自动消失),
             * 不占用 ERROR 级别(留给需要人工确认的硬件问题) */
            const char *msg = (evt == WIFI_EVT_FAILED)
                              ? "Wi-Fi connect failed"
                              : "Wi-Fi disconnected, reconnecting";
            ui_toast_push(g_ui, UI_TOAST_WARN, msg, 0);
        }
        break;
    }
    }
}

static void cb_wifi_load_ssid(char *out, size_t len, void *)
{
    if (g_settings && out && len) strlcpy(out, g_settings->last_good_ssid, len);
    else if (out && len) out[0] = '\0';
}

static void cb_wifi_save_ssid(const char *ssid, void *)
{
    if (!g_settings || !g_nvs || !ssid) return;
    if (strncmp(g_settings->last_good_ssid, ssid,
                sizeof(g_settings->last_good_ssid)) == 0) return;
    strlcpy(g_settings->last_good_ssid, ssid, sizeof(g_settings->last_good_ssid));
    settings_save(g_nvs, g_settings);
}

static void cb_mqtt_msg(const char *, const char *payload, int len, void *)
{
    if (!g_msg || !payload || len <= 0) return;
    esp_err_t r = msg_ingest(g_msg, payload, len);
    if (r != ESP_OK) ESP_LOGW(TAG, "msg_ingest 丢弃: %s", esp_err_to_name(r));
}

static void cb_mqtt_status(mqtt_event_e evt, void *)
{
    /* MQTT 重连过程会在 DISCONNECTED 与 ERROR 之间反复切换,简单"上次状态变了就 toast"
     * 会刷屏。改为:
     *   - CONNECTED 一旦到达即清错误标志;掉线/错误自上次 toast 起 60s 内静音
     *   - 仅当从 CONNECTED 重新掉线时立刻提示一次 */
    static bool     was_connected   = false;
    static uint32_t last_warn_ms    = 0;
    static const uint32_t WARN_GAP  = 60000;   /* 60s 静默窗 */
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    switch (evt) {
    case MQTT_EVT_CONNECTED:
        ui_set_mqtt(g_ui, UI_MQTT_OK);
        if (!was_connected) {
            ui_toast_clear_errors(g_ui);   /* 网络恢复 → 清错误 */
        }
        was_connected = true;
        last_warn_ms  = 0;                 /* 复位窗口,允许下次掉线立即提示 */
        break;

    case MQTT_EVT_DISCONNECTED:
    case MQTT_EVT_ERROR: {
        ui_set_mqtt(g_ui,
                    evt == MQTT_EVT_ERROR ? UI_MQTT_FAIL : UI_MQTT_LINKING);
        bool from_connected = was_connected;
        was_connected = false;
        if (from_connected ||
            last_warn_ms == 0 ||
            (now_ms - last_warn_ms) >= WARN_GAP) {
            last_warn_ms = now_ms;
            /* MQTT 也是自愈型 → WARN 即可，不需要摇一摇确认 */
            const char *msg = (evt == MQTT_EVT_ERROR)
                              ? "MQTT connection error, retrying"
                              : "MQTT disconnected, reconnecting";
            ui_toast_push(g_ui, UI_TOAST_WARN, msg, 0);
        }
        break;
    }
    }
}

static esp_err_t cb_profile_publish(const char *json, void *)
{
    if (!g_mqtt || !mqtt_is_connected(g_mqtt)) return ESP_ERR_INVALID_STATE;
    /* protocol v1: device/{12hex}/uplink/profile */
    return mqtt_publish_profile(g_mqtt, json);
}

static esp_err_t cb_mqtt_refresh_password(char *out, size_t len, void *)
{
    return identity_build_password(out, len);
}

/* ============================================================
 *  sensor_task 回调
 * ============================================================ */

static void cb_sensor_motion(void *)   { backlight_mgr_kick_motion(); }
static void cb_sensor_shake (void *)
{
    /* 摇一摇:同时作为对 ERROR 级 toast 的手动确认 */
    ui_toast_clear_errors(g_ui);
    ui_set_imu(g_ui, UI_IMU_SHAKE);
    if (g_profile) profile_on_shake(g_profile);
    /* 注意:不要在 wake_grace 内拦截 — motion 几乎一定先于 shake 触发(摇一摇
     * 必然伴随大幅运动),原 §7.7 的 in_wake_grace 守卫会让"熄屏后第一摇"
     * 被吞掉,用户必须摇第二次才能 ack 当前消息。shake 是高门槛事件
     * (需要剧烈、有节奏的多轴加速度),由 sensor_task 自行节流(2s 锁定),
     * 因此不需要 wake_grace 二次过滤。 */
    if (g_msg) msg_on_shake(g_msg);
}
static void cb_sensor_behavior(behavior_state_e st, int intensity, void *)
{
    /* 同步给 UI:behavior_state_e 与 ui_imu_state_e 数值同步(0..4) */
    ui_set_imu(g_ui, (ui_imu_state_e)st);
    if (g_profile) profile_on_behavior(g_profile, st, intensity);
}

/* ============================================================
 *  health 回调(C linkage 给 health_config_t 用)
 * ============================================================ */
extern "C" int cb_health_battery_soc(void)
{
    if (!g_cw2017) return -1;
    uint16_t soc = 0;
    if (cw2017_read_soc(g_cw2017, &soc) != ESP_OK) return -1;
    int pct = (int)(soc / 100);
    return pct > 100 ? 100 : pct;
}
extern "C" bool cb_health_charging(void)
{
    return (gpio_get_level(CHARGER_CHRG_PIN) == 0) ||
           (gpio_get_level(CHARGER_STDBY_PIN) == 0);
}
extern "C" int cb_health_rssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return 0;
}
extern "C" const char *cb_health_ip(void)
{
    static char buf[16];
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr) {
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
        return buf;
    }
    return "";
}
extern "C" uint32_t cb_health_uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}
extern "C" uint32_t cb_health_ack_pending(void)
{
    return (uint32_t)(g_tx ? tx_pending_count(g_tx) : 0);
}
extern "C" uint32_t cb_health_spiffs_pending(void)
{
    return (uint32_t)(g_profile ? profile_queue_count(g_profile) : 0);
}
extern "C" uint32_t cb_health_drop_count(void)
{
    return g_msg ? msg_get_drop_count(g_msg) : 0;
}

/* ============================================================
 *  网络拉起任务
 * ============================================================ */

/* Wi-Fi: 同步初始化句柄(不阻塞,不主动连接)。
 * 必须在 net_bringup_task 启动之前完成,否则主循环里 g_wifi 仍是 NULL,
 * wifi_mgr_is_connected 返回 false 触发"链路掉了重连"误分支,
 * 同时 net_bringup_task 也在 connect — 两个任务竞争 wifi 栈。
 * 句柄就绪后,实际 connect_blocking + SNTP + MQTT 仍异步串行在 net_bringup_task 内。 */
static esp_err_t app_wifi_init(void)
{
    wifi_mgr_config_t wcfg = {
        .cred_list = NULL,
        .on_status = cb_wifi_status, .status_user = NULL,
        .load_last_good = cb_wifi_load_ssid,
        .save_last_good = cb_wifi_save_ssid,
        .persist_user = NULL,
        .connect_timeout_ms = 0, .scan_passive_ms = 0,
    };
    return wifi_mgr_init(&g_wifi, &wcfg);
}

static void net_bringup_task(void *)
{
    /* 1. Wi-Fi connect — 句柄已在 app_wifi_init 同步阶段创建好,
     *    这里只负责 connect。失败一直 retry,SNTP/MQTT 都依赖 wifi。 */
    ui_set_boot_step(g_ui, "Wi-Fi connecting...");
    while (wifi_mgr_connect_blocking(g_wifi, 0) != ESP_OK) {
        ESP_LOGW(TAG, "wifi 连接失败,10 秒后重试");
        ui_set_boot_step(g_ui, "Wi-Fi retry in 10s");
        vTaskDelay(pdMS_TO_TICKS(10000));
        ui_set_boot_step(g_ui, "Wi-Fi connecting...");
    }

    /* 覆盖 DHCP 下发的 DNS,确保不依赖热点/运营商缓存,直接使用公共 DNS */
    {
        esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (nif) {
            esp_netif_dns_info_t d = {};
            d.ip.type = ESP_IPADDR_TYPE_V4;
            /* 主: 阿里 223.5.5.5 */
            d.ip.u_addr.ip4.addr = esp_ip4addr_aton("223.5.5.5");
            esp_netif_set_dns_info(nif, ESP_NETIF_DNS_MAIN, &d);
            /* 备: Google 8.8.8.8 */
            d.ip.u_addr.ip4.addr = esp_ip4addr_aton("8.8.8.8");
            esp_netif_set_dns_info(nif, ESP_NETIF_DNS_BACKUP, &d);
            ESP_LOGI(TAG, "DNS overridden: main=223.5.5.5 backup=8.8.8.8");
        }
    }

    /* 连上 Wi-Fi 后启用 modem-sleep:STA 跟 AP 的 DTIM beacon 醒,
     * beacon 间隙 CPU 可进 light sleep,平均电流由 ~80mA 降到 ~10-15mA */
    esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    if (ps_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps 失败: %s", esp_err_to_name(ps_err));
    } else {
        ESP_LOGI(TAG, "WiFi PS: MAX_MODEM (DTIM 保持)");
    }

    /* 2. SNTP — MQTT 的 HMAC 密码以 wall-clock 为输入,SNTP 没真同步
     *    就连 MQTT 必然被服务端拒绝(且不会自动重算重连),所以这里必须
     *    一直等到时间真就绪;长时间未同步给用户一次 WARN 提示。 */
    ui_set_boot_step(g_ui, "Time sync...");
    time_sync_config_t scfg = {0};
    time_sync_start(&scfg);
    {
        bool warned = false;
        while (time_sync_wait(30000) != ESP_OK) {
            ESP_LOGW(TAG, "SNTP 30s 仍未同步,继续等待");
            if (!warned) {
                ui_toast_push(g_ui, UI_TOAST_WARN,
                              "Time sync pending, MQTT delayed", 0);
                warned = true;
            }
        }
    }

    /* 3. 身份 + MQTT(identity_build_password 补出 CONNECT 密码) */
    ui_set_boot_step(g_ui, "Identity...");
    esp_err_t id_err = identity_init();
    if (id_err != ESP_OK) {
        ESP_LOGE(TAG, "identity_init 失败: %s(未烧录 batch 凭证?)", esp_err_to_name(id_err));
        ui_toast_push(g_ui, UI_TOAST_ERROR, "Device not provisioned", 0);
        vTaskDelete(NULL); return;
    }
    const char *device_id  = identity_get_device_id();
    const char *batch_uuid = identity_get_batch_uuid();
    ESP_LOGI(TAG, "identity ok device=%s batch=%s", device_id, batch_uuid);

    /* event 先创建(mqtt 后续绑定),开机期异常可缓冲 */
    if (event_init(&g_event, NULL, device_id) != ESP_OK) {
        ESP_LOGW(TAG, "event_init 失败");
    }

    ui_set_boot_step(g_ui, "MQTT connecting...");
    mqtt_config_t qcfg = {0};
    qcfg.username   = batch_uuid;
    qcfg.password   = NULL;
    qcfg.refresh_password = cb_mqtt_refresh_password;
    qcfg.refresh_user = NULL;
    qcfg.device_id  = device_id;
    qcfg.dept       = nullptr;  /* TODO: settings.dept */
    qcfg.online_payload = NULL;  /* 让 mqtt.c 运行时拼接 fw+ts */
    qcfg.on_msg     = cb_mqtt_msg;
    qcfg.on_status  = cb_mqtt_status;
    if (mqtt_init(&g_mqtt, &qcfg) != ESP_OK) {
        ESP_LOGE(TAG, "mqtt_init 失败"); vTaskDelete(NULL); return;
    }
    if (g_event) event_bind_mqtt(g_event, g_mqtt);

    /* 4. tx — 持久 ack ring,需要 mqtt */
    tx_config_t tcfg = {
        .mqtt = g_mqtt,
        .on_result = cb_tx_ack_result,
        .result_user = NULL,
    };
    if (tx_init(&g_tx, &tcfg) != ESP_OK) ESP_LOGE(TAG, "tx_init 失败");

    /* 5. msg — 调度器,回调驱动 ui/audio/tx */
    msg_config_t mcfg = {
        .ui_show    = cb_msg_ui_show, .ui_dismiss = cb_msg_ui_dismiss,
        .ui_stack   = cb_msg_ui_stack,
        .ui_user    = NULL,
        .on_tts     = NULL,           .tts_user   = NULL,
        .on_ack     = cb_msg_ack,     .ack_user   = NULL,
        .on_notify  = cb_msg_notify,  .notify_user = NULL,
        .on_confirm = cb_msg_confirm, .confirm_user = NULL,
        .spiffs_lru_path    = "/spiffs/lru/msg_id.bin",
        .spiffs_msg_prefix  = "/spiffs/mq_",
        .msg_store_max      = 0,
        .lru_flush_period_s = 0,
        .min_display_ms     = 0, .tick_period_ms = 0,
        .task_prio = 0, .task_stack = 0,
    };
    if (msg_init(&g_msg, &mcfg) != ESP_OK) ESP_LOGE(TAG, "msg_init 失败");

    /* 6. profile — 60 秒窗心跳 */
    ui_set_boot_step(g_ui, "Starting services...");
    profile_config_t pcfg = {
        .window_s = 0,
        .publish_fn = cb_profile_publish, .publish_user = NULL,
        .task_prio = 0, .task_stack = 0,
        .spiffs = g_spiffs,             /* store-and-forward 队列 */
        .queue_max = 0,                 /* 默认 200 条 */
        .drain_per_tick = 0,            /* 默认每次补发 8 条 */
    };
    if (profile_init(&g_profile, &pcfg) != ESP_OK) ESP_LOGE(TAG, "profile_init 失败");

    /* 6b. health — 周期健康上报 */
    {
        health_config_t hcfg = {
            .mqtt            = g_mqtt,
            .device_id       = identity_get_device_id(),
            .batch_uuid      = identity_get_batch_uuid(),
            .period_s        = 30,
            .get_battery_soc = cb_health_battery_soc,
            .get_charging    = cb_health_charging,
            .get_rssi        = cb_health_rssi,
            .get_ip          = cb_health_ip,
            .get_uptime_s    = cb_health_uptime_s,
            .get_spiffs_pending = cb_health_spiffs_pending,
            .get_ack_pending = cb_health_ack_pending,
            .get_drop_count  = cb_health_drop_count,
        };
        if (health_init(&g_health, &hcfg) != ESP_OK) ESP_LOGE(TAG, "health_init 失败");
    }

    /* 7. 周期任务:HH:MM 刷新 + 电池/充电 + Wi-Fi 保活
     *    - 充电状态每 500ms 检测一次(响应拔插 USB)
     *    - 时间/电量/wifi 链路检查每 5s 做一次
     */
    char hhmm[8] = "--:--";
    bool last_charging  = false;
    bool ever_connected = false;   /* edge: 只有曾经连过才进重连分支 */
    int  slow_tick      = 10;      /* 首轮立即走一次慢路径(不等 5s) */
    while (1) {
        /* —— 快速:充电检测 (TP4057 CHRG/STDBY 低有效) —— */
        bool chrg_low  = (gpio_get_level(CHARGER_CHRG_PIN)  == 0);
        bool stdby_low = (gpio_get_level(CHARGER_STDBY_PIN) == 0);
        bool charging  = chrg_low || stdby_low;
        if (charging != last_charging) {
            ESP_LOGI(TAG, "充电状态变化: %d → %d (CHRG=%d STDBY=%d)",
                     (int)last_charging, (int)charging,
                     (int)chrg_low, (int)stdby_low);
            last_charging = charging;
        }
        ui_set_charging(g_ui, charging);

        /* —— 慢速:每 10 个 tick (≈5s) 一次 —— */
        if (++slow_tick >= 10) {
            slow_tick = 0;
            /* SNTP 未同步时 time(NULL) 返回 1970,显示 --:-- 更老实 */
            if (time_sync_is_synced()) {
                time_sync_get_hhmm(hhmm, sizeof(hhmm));
            } else {
                strcpy(hhmm, "--:--");
            }

            int bat_pct = -1;
            if (g_cw2017) {
                uint16_t soc = 0;
                if (cw2017_read_soc(g_cw2017, &soc) == ESP_OK) {
                    bat_pct = (int)(soc / 100);
                    if (bat_pct > 100) bat_pct = 100;
                }
            }
            ui_set_status(g_ui, hhmm, bat_pct);

            /* —— 低电量提示(仅在非充电时报) —— */
            static int last_bat_warned = 100;
            if (!charging && bat_pct >= 0) {
                if (bat_pct <= 5 && last_bat_warned > 5) {
                    ui_toast_push(g_ui, UI_TOAST_ERROR, "Battery critical, charge now", 0);
                } else if (bat_pct <= 20 && last_bat_warned > 20) {
                    ui_toast_push(g_ui, UI_TOAST_WARN, "Battery low, please charge", 0);
                }
                last_bat_warned = bat_pct;
            } else if (charging) {
                last_bat_warned = 100;  /* 充电时复位阈值 */
            }

            /* —— RSSI 上报到 UI —— */
            if (wifi_mgr_is_connected(g_wifi)) {
                wifi_ap_record_t ap;
                if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                    ui_set_rssi(g_ui, ap.rssi);
                }
            }

            /* —— 主页:日期 + 星期 + 用户姓名 + 今日步数(来自 LSM6DS3TR-C 内置 pedometer) —— */
            time_t now_t = 0; time(&now_t);
            struct tm tmv; localtime_r(&now_t, &tmv);
            char date_buf[32];
            static const char *wkdays[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
            if (time_sync_is_synced()) {
                snprintf(date_buf, sizeof(date_buf), "%s %02d/%02d",
                         wkdays[tmv.tm_wday], tmv.tm_mon + 1, tmv.tm_mday);
            } else {
                strcpy(date_buf, "--- --/--");
            }
            const char *uname = (g_settings && g_settings->device_name[0])
                                ? g_settings->device_name : "BeaconOps";
            const char *wid = identity_get_device_id();

            /* —— 今日步数(LSM6DS3TR-C 内置 pedometer)+ NVS 持久化 ——
             *   HW 16-bit 计数器上电清零;软件维护 32-bit 单调增 last_total。
             *   today_persisted = 已写入 NVS 的今日步数(开机/上次落盘前那段)。
             *   base_total      = 本轮(累计未落盘)的零点。
             *   显示值 = today_persisted + (last_total - base_total)。
             * 持久化触发:
             *   ① SNTP 首次同步后:读 NVS、设基线
             *   ② 跨天:归档昨天 + 立刻写 0
             *   ③ 未落盘步数 ≥ PEDO_NVS_THRESHOLD:刷盘
             * SNTP 未同步前 steps_today=-1,UI 显示 "--",避免把启动头几秒
             * 误算成今日基线,等 wall clock 可信再开始统计。
             */
            #define PEDO_NVS_KEY_YDAY   "pedo_yday"
            #define PEDO_NVS_KEY_TODAY  "pedo_today"
            #define PEDO_NVS_THRESHOLD  50

            int steps_today = -1;
            if (g_sensor && time_sync_is_synced()) {
                static bool    pedo_inited     = false;
                static int32_t saved_yday      = -1;
                static int32_t today_persisted = 0;
                static int32_t last_total      = 0;
                static int32_t base_total      = 0;

                uint16_t cur = 0;
                if (sensor_get_step_count(g_sensor, &cur) == ESP_OK) {
                    /* 1) HW 16-bit → SW 32-bit wrap 补偿 */
                    int32_t cur32    = (int32_t)cur;
                    int32_t prev_low = last_total & 0xFFFF;
                    if (cur32 < prev_low) last_total += (1 << 16);
                    last_total = (last_total & ~0xFFFF) | cur32;

                    /* 2) 首次(SNTP 已同步):从 NVS 恢复今日进度 */
                    if (!pedo_inited) {
                        int32_t v_yday = -1, v_today = 0;
                        if (g_nvs) {
                            nvs_get_i32(g_nvs->handle, PEDO_NVS_KEY_YDAY,  &v_yday);
                            nvs_get_i32(g_nvs->handle, PEDO_NVS_KEY_TODAY, &v_today);
                        }
                        saved_yday      = v_yday;
                        today_persisted = (v_yday == tmv.tm_yday) ? v_today : 0;
                        base_total      = last_total;
                        pedo_inited     = true;
                        ESP_LOGI(TAG, "pedo: restored yday=%d today=%d",
                                 (int)saved_yday, (int)today_persisted);
                    }

                    /* 3) 跨天:归档(归零)并立刻落盘 */
                    if (saved_yday != tmv.tm_yday) {
                        saved_yday      = tmv.tm_yday;
                        today_persisted = 0;
                        base_total      = last_total;
                        if (g_nvs) {
                            nvs_set_i32(g_nvs->handle, PEDO_NVS_KEY_YDAY,  saved_yday);
                            nvs_set_i32(g_nvs->handle, PEDO_NVS_KEY_TODAY, today_persisted);
                            nvs_commit(g_nvs->handle);
                        }
                        ESP_LOGI(TAG, "pedo: day rollover → yday=%d", (int)saved_yday);
                    }

                    int32_t uncommitted = last_total - base_total;
                    if (uncommitted < 0) uncommitted = 0;
                    steps_today = (int)(today_persisted + uncommitted);

                    /* 4) 阈值落盘:50 步一次 → 行走时约 1 写/分钟 */
                    if (uncommitted >= PEDO_NVS_THRESHOLD && g_nvs) {
                        today_persisted += uncommitted;
                        base_total       = last_total;
                        nvs_set_i32(g_nvs->handle, PEDO_NVS_KEY_YDAY,  saved_yday);
                        nvs_set_i32(g_nvs->handle, PEDO_NVS_KEY_TODAY, today_persisted);
                        nvs_commit(g_nvs->handle);
                    }
                }
            }
            ui_show_home(g_ui, hhmm, date_buf, uname, wid, steps_today);

            /* 同步给 profile,下一个 60s 窗口随 delta 一起上传(负值会被省略) */
            if (g_profile) profile_set_steps_today(g_profile, steps_today);

            /* edge-triggered reconnect: 只有 "曾经连上 → 现在掉了" 才主动重连。
             * 启动头几秒 g_wifi 已就绪但 net_bringup_task 还没 connect 完,
             * 此时 is_connected=false 是合法 LINKING 状态,千万不能再抢一把
             * connect_blocking,否则两个任务同时连同一个 wifi 栈,行为未定义。 */
            if (wifi_mgr_is_connected(g_wifi)) {
                ever_connected = true;
            } else if (ever_connected) {
                ESP_LOGW(TAG, "wifi 链路掉了,重连中");
                ui_set_wifi(g_ui, UI_WIFI_LINKING);
                while (wifi_mgr_connect_blocking(g_wifi, 0) != ESP_OK) {
                    vTaskDelay(pdMS_TO_TICKS(10000));
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ============================================================
 *  app_main
 * ============================================================ */

extern "C" void app_main(void)
{
    ESP_LOGW(TAG, "===============APP START===============");

    /* 启用动态调频 + auto light sleep
     * - 空闲时 CPU 从 160MHz 降到 40MHz, FreeRTOS tickless idle 期间进 light sleep
     * - Wi-Fi 依靠 DTIM beacon 保持连接(后续 esp_wifi_set_ps 设 MAX_MODEM)
     * - GPIO 电平保持,IO10/LCDRST 不会被误拉低 */
    esp_pm_config_t pm_cfg = {
        .max_freq_mhz       = 160,
        .min_freq_mhz       = 40,
        .light_sleep_enable = true,
    };
    esp_err_t pm_err = esp_pm_configure(&pm_cfg);
    if (pm_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure 失败: %s", esp_err_to_name(pm_err));
    } else {
        ESP_LOGI(TAG, "PM: 40-160MHz DFS + auto light sleep 已启用");
    }

    TRY_FUNC(app_special_gpio_init);
    TRY_FUNC(app_gpio_init);
    TRY_FUNC(app_i2c_init);
    TRY_FUNC(app_sensor_init);
    TRY_FUNC(app_lvgl_init);
    TRY_FUNC(app_backlight_init);
    TRY_FUNC(app_nvs_init);
    TRY_FUNC(app_settings_init);
    TRY_FUNC(app_spiffs_init);
    TRY_FUNC(app_audio_init);
    TRY_FUNC(app_cw2017_init);

    TRY_FUNC(app_error_init);
    TRY_FUNC(app_notify_init);
    TRY_FUNC(app_ui_init);
    TRY_FUNC(app_wifi_init);   /* 同步建 g_wifi 句柄;不连接 */

    /* 网络拉起在独立任务,失败不影响本地 UI */
    xTaskCreate(net_bringup_task, "net_bringup", 3 * 1024,
                NULL, tskIDLE_PRIORITY + 3, NULL);

    /* sensor_task — 转发摇晃/行为到 msg/profile */
    sensor_task_cfg_t scfg = {
        .dev = g_sensor,
        .on_motion   = cb_sensor_motion,
        .on_shake    = cb_sensor_shake,
        .on_behavior = cb_sensor_behavior,
        .user        = NULL,
    };
    sensor_task_start(&scfg);

    ESP_LOGI(TAG, "[HEAP@xxx] free=%u largest=%u min=%u",
        (unsigned)esp_get_free_heap_size(),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)esp_get_minimum_free_heap_size());

    // while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
