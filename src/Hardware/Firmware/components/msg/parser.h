/**
 * @file parser.h
 * @brief MQTT JSON command → msg_t decoder.
 *
 * Expected schema (server-defined, see 01-通信协议规格.md):
 * {
 *   "id":         "uuid",            // optional; absent → "" (no dedup)
 *   "level":      "info|notice|warn|emergency",   // default info
 *   "ts":         1747200000,        // optional unix s; default = now
 *   "ttl":        60,                // optional seconds; 0 / absent = no expiry
 *   "title":      "string",          // optional
 *   "body":       "string",          // optional
 *   "audio_text": "string"           // optional → has_audio flag set
 * }
 */
#ifndef MSG_PARSER_H
#define MSG_PARSER_H

#include "msg.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Parse a (not necessarily NUL-terminated) JSON payload into @p out.
 *
 * @p out must be zero-initialised by the caller. On success, caller owns the
 * heap allocations inside @p out and must call msg_clear(out).
 * On failure, @p out is left clean (no allocations leaked).
 */
esp_err_t msg_parse(const char *payload, int len, msg_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MSG_PARSER_H */
