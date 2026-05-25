/**
 * @file identity.h
 * @brief 设备身份 + HMAC 鉴权 — BeaconOps protocol v1
 *
 * - device_id: 12 hex MAC,运行时从 efuse 读出
 * - batch_uuid / batch_secret: config.h 编译时常量(BATCH_UUID/BATCH_SECRET)
 *   出厂烧录主固件即带凭证,同批设备共享。
 * - mqtt CONNECT password = "<ts>:<nonce>:<hmac_hex>"
 *   其中 hmac_hex = HMAC_SHA256(batch_secret, device_id||"|"||ts||"|"||nonce)
 */
#ifndef IDENTITY_H
#define IDENTITY_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IDENTITY_DEVICE_ID_LEN   12       // 12 hex chars
#define IDENTITY_BATCH_UUID_MAX  64
#define IDENTITY_SECRET_MAX     128
#define IDENTITY_PASSWORD_MAX   160       // ts:nonce_hex:hmac_hex 足够

/**
 * 一次性初始化:读 efuse MAC,加载 NVS provisioning。
 * 返回 ESP_OK 表示 batch 凭证已加载;否则需进入 SoftAP 配网模式。
 */
esp_err_t identity_init(void);

/** 返回 12 hex 字符,以 \0 结尾。常驻 RAM,无需 free。 */
const char *identity_get_device_id(void);

/** 返回 batch_uuid 字符串。identity_init 未成功时返回空字符串。 */
const char *identity_get_batch_uuid(void);

/**
 * 为本次 CONNECT 构造 password。
 * 内部生成 random nonce,读当前 unix 时间(需 SNTP 完成),计算 HMAC。
 * out 至少 IDENTITY_PASSWORD_MAX 字节。
 */
esp_err_t identity_build_password(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif // IDENTITY_H
