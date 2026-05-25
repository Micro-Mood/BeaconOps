/**
 * @file parser.c
 * @brief BeaconOps protocol v1 严格解析。
 *
 * Schema(唯一支持):
 *   { "id":"...", "ts":..., "ttl":..., "level":"info|notice|warn|emergency",
 *     "title":"", "body":"", "ack":"none|received|displayed|acknowledged" }
 *
 * 拒收规则:
 *   - JSON parse 失败             → ESP_ERR_INVALID_ARG (caller 应 event_emit PARSE_REJECT)
 *   - level 非 4 值之一            → ESP_ERR_INVALID_ARG
 *   - title=="" && body==""       → ESP_ERR_INVALID_ARG (无显示内容)
 *
 * 自动修正:
 *   - level=warn|emergency        → ttl 强制 0(永不过期)
 *   - 未提供 ack                  → 默认 "none";level=warn|emergency 强制 acknowledged
 */
#include "parser.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"

static const char *TAG = "msg_parser";

static int parse_level(const char *s, msg_level_e *out)
{
    if (!s) return -1;
    if      (!strcasecmp(s, "emergency")) { *out = MSG_LEVEL_EMERG;  return 0; }
    else if (!strcasecmp(s, "warn"))      { *out = MSG_LEVEL_WARN;   return 0; }
    else if (!strcasecmp(s, "notice"))    { *out = MSG_LEVEL_NOTICE; return 0; }
    else if (!strcasecmp(s, "info"))      { *out = MSG_LEVEL_INFO;   return 0; }
    return -1;
}

static int parse_ack(const char *s, msg_ack_mode_e *out)
{
    if (!s) return -1;
    if      (!strcasecmp(s, "none"))         { *out = MSG_ACK_MODE_NONE;         return 0; }
    else if (!strcasecmp(s, "received"))     { *out = MSG_ACK_MODE_RECEIVED;     return 0; }
    else if (!strcasecmp(s, "displayed"))    { *out = MSG_ACK_MODE_DISPLAYED;    return 0; }
    else if (!strcasecmp(s, "acknowledged")) { *out = MSG_ACK_MODE_ACKNOWLEDGED; return 0; }
    return -1;
}

static char *dup_or_null(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

esp_err_t msg_parse(const char *payload, int len, msg_t *out)
{
    if (!payload || len <= 0 || !out) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_ParseWithLength(payload, (size_t)len);
    if (!root) {
        ESP_LOGW(TAG, "JSON parse failed (%d bytes)", len);
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    const cJSON *jid    = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON *jlevel = cJSON_GetObjectItemCaseSensitive(root, "level");
    const cJSON *jts    = cJSON_GetObjectItemCaseSensitive(root, "ts");
    const cJSON *jttl   = cJSON_GetObjectItemCaseSensitive(root, "ttl");
    const cJSON *jtitle = cJSON_GetObjectItemCaseSensitive(root, "title");
    const cJSON *jbody  = cJSON_GetObjectItemCaseSensitive(root, "body");
    const cJSON *jack   = cJSON_GetObjectItemCaseSensitive(root, "ack");

    if (cJSON_IsString(jid) && jid->valuestring) {
        strlcpy(out->id, jid->valuestring, sizeof(out->id));
    }

    /* level — 必须明确给且合法 */
    msg_level_e level = MSG_LEVEL_INFO;
    if (!cJSON_IsString(jlevel) || parse_level(jlevel->valuestring, &level) != 0) {
        ESP_LOGW(TAG, "bad level (id=%s)", out->id);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    out->level = level;

    /* ack — 可选;高级别强制 acknowledged */
    msg_ack_mode_e ack = MSG_ACK_MODE_NONE;
    if (cJSON_IsString(jack)) {
        if (parse_ack(jack->valuestring, &ack) != 0) {
            ESP_LOGW(TAG, "bad ack (id=%s)", out->id);
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (level == MSG_LEVEL_WARN || level == MSG_LEVEL_EMERG) {
        ack = MSG_ACK_MODE_ACKNOWLEDGED;
    }
    out->ack_mode = ack;

    /* ts / ttl */
    time_t now = 0; time(&now);
    bool sntp_ok = ((uint32_t)now >= 1700000000u);
    out->arrive_ts = cJSON_IsNumber(jts) ? (uint32_t)jts->valuedouble : (uint32_t)now;
    out->enqueue_ts  = (uint32_t)(esp_timer_get_time() / 1000ULL);
    out->shown_at_ms = 0;

    uint32_t ttl = (cJSON_IsNumber(jttl) && jttl->valuedouble > 0) ? (uint32_t)jttl->valuedouble : 0;
    if (level == MSG_LEVEL_WARN || level == MSG_LEVEL_EMERG) ttl = 0;
    if (ttl > 0 && sntp_ok && out->arrive_ts >= 1700000000u) {
        out->expire_ts = out->arrive_ts + ttl;
    } else {
        out->expire_ts = 0;
    }

    /* 文本 */
    if (cJSON_IsString(jtitle) && jtitle->valuestring) {
        out->title = dup_or_null(jtitle->valuestring);
    }
    if (cJSON_IsString(jbody) && jbody->valuestring) {
        out->body = dup_or_null(jbody->valuestring);
    }
    out->audio_text = NULL;
    if (out->title || out->body) out->flags |= MSG_FLAG_HAS_DISPLAY;

    cJSON_Delete(root);

    if ((out->flags & MSG_FLAG_HAS_DISPLAY) == 0) {
        msg_clear(out);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}
