/**
 * @file mqtt.c
 * @brief MQTT 客户端实现 — BeaconOps protocol v1
 */

#include "mqtt.h"
#include "config.h"
#include "certs.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "mqtt_client.h"

static const char *TAG = "mqtt";

#define TOPIC_BUF   96
#define ONLINE_BUF  160
#define PASSWORD_BUF 160

struct mqtt_dev_s {
    mqtt_config_t            cfg;
    esp_mqtt_client_handle_t client;
    char  topic_cmd        [TOPIC_BUF];
    char  topic_ack        [TOPIC_BUF];
    char  topic_event      [TOPIC_BUF];
    char  topic_health     [TOPIC_BUF];
    char  topic_profile    [TOPIC_BUF];
    char  topic_status     [TOPIC_BUF];
    char  topic_bcast_dept [TOPIC_BUF];  // 可空
    char  password         [PASSWORD_BUF];
    char  online_payload   [ONLINE_BUF];
    volatile bool connected;
    bool          custom_online_payload;
    bool          initialized;
};

static void mqtt_evt_handler(void *handler_args, esp_event_base_t base,
                             int32_t id, void *data);

static int build_topic(char *out, size_t cap, const char *fmt, const char *arg)
{
    int n = snprintf(out, cap, fmt, arg);
    return (n <= 0 || (size_t)n >= cap) ? -1 : n;
}

static esp_err_t copy_cstr(char *dst, size_t cap, const char *src)
{
    size_t len;
    if (!dst || cap == 0 || !src) return ESP_ERR_INVALID_ARG;
    len = strlen(src);
    if (len >= cap) return ESP_ERR_INVALID_SIZE;
    memcpy(dst, src, len + 1);
    return ESP_OK;
}

static esp_err_t refresh_runtime_connect_state(mqtt_dev_t *d)
{
    int n;

    if (!d) return ESP_ERR_INVALID_ARG;

    if (d->cfg.refresh_password) {
        esp_err_t r = d->cfg.refresh_password(d->password, sizeof(d->password),
                                              d->cfg.refresh_user);
        if (r != ESP_OK) return r;
    } else if (d->cfg.password && d->cfg.password[0]) {
        esp_err_t r = copy_cstr(d->password, sizeof(d->password), d->cfg.password);
        if (r != ESP_OK) return r;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    if (d->custom_online_payload) return ESP_OK;

    n = snprintf(d->online_payload, ONLINE_BUF,
                 "{\"online\":true,\"fw\":\"%s\",\"batch\":\"%s\",\"ts\":%lld}",
                 FW_VERSION, d->cfg.username ? d->cfg.username : "", (long long)time(NULL));
    return (n <= 0 || (size_t)n >= ONLINE_BUF) ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

static void fill_mqtt_client_config(mqtt_dev_t *d, esp_mqtt_client_config_t *mc)
{
    memset(mc, 0, sizeof(*mc));
    mc->broker.address.uri = d->cfg.broker_uri;
    if (strncmp(d->cfg.broker_uri, "mqtts://", 8) == 0) {
        mc->broker.verification.certificate = d->cfg.ca_pem;
    }
    mc->credentials.client_id               = d->cfg.device_id;
    mc->credentials.username                = d->cfg.username;
    mc->credentials.authentication.password = d->password;
    mc->session.keepalive                   = d->cfg.keepalive_s;
    mc->session.last_will.topic             = d->topic_status;
    mc->session.last_will.msg               = "{\"online\":false}";
    mc->session.last_will.msg_len           = (int)strlen("{\"online\":false}");
    mc->session.last_will.qos               = 1;
    mc->session.last_will.retain            = 1;
    mc->network.reconnect_timeout_ms        = d->cfg.reconnect_ms;
}

esp_err_t mqtt_init(mqtt_dev_t **dev, const mqtt_config_t *config)
{
    if (!dev || !config) return ESP_ERR_INVALID_ARG;
    if (!config->username || !config->device_id ||
        (!config->password && !config->refresh_password))
        return ESP_ERR_INVALID_ARG;

    mqtt_dev_t *d = calloc(1, sizeof(*d));
    if (!d) return ESP_ERR_NO_MEM;
    d->cfg = *config;

    if (!d->cfg.broker_uri)         d->cfg.broker_uri  = BROKER_URI;
    if (!d->cfg.ca_pem)             d->cfg.ca_pem      = certs_get_isrg_root_x1();
    if (d->cfg.keepalive_s  == 0)   d->cfg.keepalive_s = MQTT_KEEPALIVE_S;
    if (d->cfg.reconnect_ms == 0)   d->cfg.reconnect_ms= MQTT_RECONNECT_MS;

    if (build_topic(d->topic_cmd,     TOPIC_BUF, MQTT_TOPIC_CMD_FMT,     d->cfg.device_id) < 0 ||
        build_topic(d->topic_ack,     TOPIC_BUF, MQTT_TOPIC_ACK_FMT,     d->cfg.device_id) < 0 ||
        build_topic(d->topic_event,   TOPIC_BUF, MQTT_TOPIC_EVENT_FMT,   d->cfg.device_id) < 0 ||
        build_topic(d->topic_health,  TOPIC_BUF, MQTT_TOPIC_HEALTH_FMT,  d->cfg.device_id) < 0 ||
        build_topic(d->topic_profile, TOPIC_BUF, MQTT_TOPIC_PROFILE_FMT, d->cfg.device_id) < 0 ||
        build_topic(d->topic_status,  TOPIC_BUF, MQTT_TOPIC_STATUS_FMT,  d->cfg.device_id) < 0) {
        free(d);
        return ESP_ERR_INVALID_SIZE;
    }
    if (d->cfg.dept && d->cfg.dept[0]) {
        if (build_topic(d->topic_bcast_dept, TOPIC_BUF,
                        MQTT_TOPIC_BCAST_DEPT_FMT, d->cfg.dept) < 0) {
            d->topic_bcast_dept[0] = '\0';
        }
    }

    if (d->cfg.online_payload && d->cfg.online_payload[0]) {
        if (copy_cstr(d->online_payload, sizeof(d->online_payload),
                      d->cfg.online_payload) != ESP_OK) {
            free(d);
            return ESP_ERR_INVALID_SIZE;
        }
        d->custom_online_payload = true;
    }

    {
        esp_err_t r = refresh_runtime_connect_state(d);
        esp_mqtt_client_config_t mc;

        if (r != ESP_OK) {
            free(d);
            return r;
        }

        /* esp-mqtt client */
        fill_mqtt_client_config(d, &mc);
        d->client = esp_mqtt_client_init(&mc);
    }
    if (!d->client) { free(d); return ESP_FAIL; }

    esp_err_t r = esp_mqtt_client_register_event(d->client, ESP_EVENT_ANY_ID,
                                                  mqtt_evt_handler, d);
    if (r != ESP_OK) { esp_mqtt_client_destroy(d->client); free(d); return r; }

    r = esp_mqtt_client_start(d->client);
    if (r != ESP_OK) { esp_mqtt_client_destroy(d->client); free(d); return r; }

    d->initialized = true;
    *dev = d;
    ESP_LOGI(TAG, "MQTT init uri=%s id=%s", d->cfg.broker_uri, d->cfg.device_id);
    return ESP_OK;
}

esp_err_t mqtt_deinit(mqtt_dev_t **dev)
{
    if (!dev || !*dev) return ESP_ERR_INVALID_ARG;
    mqtt_dev_t *d = *dev;
    if (!d->initialized) return ESP_ERR_INVALID_STATE;
    if (d->client) {
        esp_mqtt_client_stop(d->client);
        esp_mqtt_client_destroy(d->client);
        d->client = NULL;
    }
    d->connected = false;
    d->initialized = false;
    free(d);
    *dev = NULL;
    return ESP_OK;
}

bool mqtt_is_connected(mqtt_dev_t *dev)
{
    return (dev && dev->initialized) ? dev->connected : false;
}

esp_err_t mqtt_publish(mqtt_dev_t *dev, const char *topic,
                       const char *payload, int qos, bool retain)
{
    if (!dev || !dev->initialized || !dev->client) return ESP_ERR_INVALID_STATE;
    if (!topic || !payload)                        return ESP_ERR_INVALID_ARG;
    int mid = esp_mqtt_client_publish(dev->client, topic,
                                      payload, 0, qos, retain ? 1 : 0);
    return (mid < 0) ? ESP_FAIL : ESP_OK;
}

esp_err_t mqtt_publish_ack(mqtt_dev_t *dev, const char *payload, int qos)
{
    return mqtt_publish(dev, dev ? dev->topic_ack : NULL, payload, qos, false);
}
esp_err_t mqtt_publish_event(mqtt_dev_t *dev, const char *payload)
{
    return mqtt_publish(dev, dev ? dev->topic_event : NULL, payload, 1, false);
}
esp_err_t mqtt_publish_health(mqtt_dev_t *dev, const char *payload)
{
    return mqtt_publish(dev, dev ? dev->topic_health : NULL, payload, 0, false);
}
esp_err_t mqtt_publish_profile(mqtt_dev_t *dev, const char *payload)
{
    return mqtt_publish(dev, dev ? dev->topic_profile : NULL, payload, 1, false);
}

static void subscribe_all(mqtt_dev_t *d)
{
    if (!d->client) return;
    esp_mqtt_client_subscribe(d->client, d->topic_cmd,        1);
    esp_mqtt_client_subscribe(d->client, MQTT_TOPIC_BCAST_ALL, 1);
    if (d->topic_bcast_dept[0]) {
        esp_mqtt_client_subscribe(d->client, d->topic_bcast_dept, 1);
    }
}

static bool topic_eq(const char *a, int alen, const char *b)
{
    int bl = (int)strlen(b);
    return (alen == bl) && (strncmp(a, b, (size_t)bl) == 0);
}

static void dispatch_data(mqtt_dev_t *d, esp_mqtt_event_handle_t e)
{
    if (!d->cfg.on_msg || !e || e->topic_len <= 0) return;
    /* 仅命中订阅的 4 类 topic 才回调 */
    if (topic_eq(e->topic, e->topic_len, d->topic_cmd) ||
        topic_eq(e->topic, e->topic_len, MQTT_TOPIC_BCAST_ALL) ||
        (d->topic_bcast_dept[0] && topic_eq(e->topic, e->topic_len, d->topic_bcast_dept))) {
        d->cfg.on_msg(e->topic, e->data, e->data_len, d->cfg.msg_user);
        return;
    }
}

static void mqtt_evt_handler(void *handler_args, esp_event_base_t base,
                             int32_t id, void *data)
{
    (void)base;
    mqtt_dev_t *d = (mqtt_dev_t *)handler_args;
    if (!d) return;
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_BEFORE_CONNECT:
        if (d->client) {
            esp_mqtt_client_config_t mc;
            esp_err_t r = refresh_runtime_connect_state(d);
            if (r != ESP_OK) {
                ESP_LOGE(TAG, "refresh auth failed: %s", esp_err_to_name(r));
                break;
            }
            fill_mqtt_client_config(d, &mc);
            r = esp_mqtt_set_config(d->client, &mc);
            if (r != ESP_OK) {
                ESP_LOGE(TAG, "esp_mqtt_set_config failed: %s", esp_err_to_name(r));
            }
        }
        break;

    case MQTT_EVENT_CONNECTED:
        d->connected = true;
        ESP_LOGI(TAG, "connected");
        if (d->client) {
            /* online 状态,retain;LWT 自动接管 offline */
            esp_mqtt_client_publish(d->client, d->topic_status,
                                    d->online_payload,
                                    (int)strlen(d->online_payload), 1, 1);
        }
        subscribe_all(d);
        if (d->cfg.on_status) d->cfg.on_status(MQTT_EVT_CONNECTED, d->cfg.status_user);
        break;

    case MQTT_EVENT_DISCONNECTED:
        d->connected = false;
        ESP_LOGW(TAG, "disconnected");
        if (d->cfg.on_status) d->cfg.on_status(MQTT_EVT_DISCONNECTED, d->cfg.status_user);
        break;

    case MQTT_EVENT_DATA:
        dispatch_data(d, e);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "error");
        if (d->cfg.on_status) d->cfg.on_status(MQTT_EVT_ERROR, d->cfg.status_user);
        break;

    default:
        break;
    }
}
