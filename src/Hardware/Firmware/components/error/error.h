/**
 * @file error.h
 * @brief 统一错误诊断组件 — 内存环形缓冲 + SPIFFS 持久化日志
 *
 * 设计目标:
 *  - 任何业务组件出错时调用 err_record() 记录一条诊断
 *  - 内存环形缓冲(默认 32 条)用于运行期最近错误检索
 *  - 后台 worker task 周期性把缓冲 flush 到 SPIFFS,文件按
 *    file_max_bytes 轮转,保留 file_keep 个旧文件
 *  - SPIFFS 不可用时静默降级为纯内存模式,绝不阻塞业务
 *  - err_record(NULL, ...) 在 dev 还未初始化的早期阶段安全无副作用
 *
 * 持久化文件布局(默认):
 *   /spiffs/diag/errors.log         当前活跃文件
 *   /spiffs/diag/errors.log.1       上一份(轮转后)
 */

#ifndef ERROR_H
#define ERROR_H

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include "esp_log.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "spiffs.h"

/**
 * @brief 错误等级
 */
typedef enum {
    ERR_LV_DEBUG = 0,    ///< 调试信息
    ERR_LV_INFO,         ///< 一般信息
    ERR_LV_WARN,         ///< 警告
    ERR_LV_ERROR,        ///< 错误
    ERR_LV_FATAL,        ///< 致命错误
} err_level_e;

/**
 * @brief 单条诊断记录(内部环形缓冲单元)
 */
typedef struct {
    uint64_t    ts_ms;          ///< 记录时间(esp_timer_get_time/1000)
    err_level_e level;          ///< 等级
    esp_err_t   code;           ///< esp_err_t 错误码
    char        tag[16];        ///< 来源 TAG
    char        msg[96];        ///< 格式化后的描述
} err_entry_t;

/**
 * @brief error 组件配置
 */
typedef struct {
    spiffs_dev_t *spiffs;           ///< 可选;NULL → 仅内存环形,不持久化
    const char   *log_dir;          ///< NULL → "/spiffs/diag"
    uint16_t      ring_depth;       ///< 0 → 32(条)
    uint32_t      flush_period_ms;  ///< 0 → 60000;UINT32_MAX → 仅手动 flush
    uint32_t      file_max_bytes;   ///< 0 → 4096(轮转阈值)
    uint8_t       file_keep;        ///< 0 → 2(保留旧文件个数)
    uint8_t       task_prio;        ///< 0 → tskIDLE_PRIORITY + 1
    uint16_t      task_stack;       ///< 0 → 3072
} err_config_t;

/**
 * @brief error 组件设备句柄(不透明)
 */
typedef struct err_dev_s err_dev_t;

/**
 * @brief 初始化 error 组件
 *
 * @param dev    输出句柄指针
 * @param config 配置
 * @return esp_err_t ESP_OK / ESP_ERR_INVALID_ARG / ESP_ERR_NO_MEM / ESP_FAIL
 */
esp_err_t err_init(err_dev_t **dev, const err_config_t *config);

/**
 * @brief 反初始化 — 停止 worker task,flush 一次,释放资源
 *
 * @param dev 句柄指针(完成后置 NULL)
 * @return esp_err_t ESP_OK
 */
esp_err_t err_deinit(err_dev_t **dev);

/**
 * @brief 记录一条诊断
 *
 * 安全语义:
 *  - dev=NULL 时安全返回,允许早期未初始化阶段无感调用
 *  - 仅写内存 + 通知 worker,不在调用线程做 fwrite,可在任何 task 调用
 *  - 环形满时丢弃最旧条目
 *
 * @param dev   句柄(可为 NULL)
 * @param lv    等级
 * @param tag   来源 TAG(被截断到 15 字符)
 * @param code  esp_err_t 错误码,无错误传 ESP_OK
 * @param fmt   printf 格式串
 * @return esp_err_t ESP_OK
 */
esp_err_t err_record(err_dev_t *dev,
                     err_level_e lv,
                     const char *tag,
                     esp_err_t code,
                     const char *fmt, ...) __attribute__((format(printf, 5, 6)));

/**
 * @brief 主动把内存环形缓冲 flush 到 SPIFFS(若启用)
 *
 * @param dev 句柄
 * @return esp_err_t ESP_OK / ESP_ERR_INVALID_STATE / ESP_FAIL
 */
esp_err_t err_flush(err_dev_t *dev);

/**
 * @brief 把最近若干条诊断格式化到调用方缓冲(用于状态上报/调试 UI)
 *
 * @param dev     句柄
 * @param out     输出缓冲
 * @param out_len 缓冲容量
 * @return size_t 实际写入字节数(不含尾部 NUL)
 */
size_t err_recent(err_dev_t *dev, char *out, size_t out_len);

/**
 * @brief 把 esp_err_t 错误码翻译成短字符串(包装 esp_err_to_name)
 *
 * @param code esp_err_t
 * @return const char* 静态字符串
 */
const char *err_to_string(esp_err_t code);

/**
 * @brief 等级 → 单字符标签
 *
 * @param lv 等级
 * @return char 'D' 'I' 'W' 'E' 'F'
 */
char err_level_char(err_level_e lv);

#ifdef __cplusplus
}
#endif

#endif /* ERROR_H */
