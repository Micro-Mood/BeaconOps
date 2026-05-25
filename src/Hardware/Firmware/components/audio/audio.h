/* AUDIO_H *//**
 * @file audio.h
 * @brief MAX98357音频驱动库
 * @version 1.0
 */

#ifndef AUDIO_H
#define AUDIO_H

#include "driver/i2s.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 音频配置参数结构体
 */
typedef struct {
    int sample_rate;                    /*!< 采样率 */
    i2s_bits_per_sample_t bit_depth;    /*!< 采样位数 */
    i2s_channel_t channels;             /*!< 声道数 */
    int dma_buf_count;                  /*!< DMA缓冲区数量 */
    int dma_buf_len;                    /*!< DMA缓冲区长度 */
    bool use_apll;                      /*!< 是否使用APLL */
    int tx_timeout_ms;                  /*!< 发送超时时间 */
} audio_config_t;

/**
 * @brief 音频设备句柄
 */
typedef struct {
    i2s_port_t i2s_port;                /*!< I2S端口号 */
    audio_config_t config;              /*!< 音频配置 */
    bool initialized;                   /*!< 初始化标志 */
    SemaphoreHandle_t mutex;            /*!< 互斥锁 */
    SemaphoreHandle_t session_mutex;    /*!< 多音源串行锁（notify/tts 共享）*/
#if CONFIG_PM_ENABLE
    esp_pm_lock_handle_t pm_apb_lock;
    esp_pm_lock_handle_t pm_sleep_lock;
    bool pm_locks_acquired;
#endif
} audio_dev_t;

/**
 * @brief 初始化MAX98357音频设备
 * 
 * @param dev_ptr 指向音频设备句柄指针的指针；如果 *dev_ptr 为 NULL，会在堆上分配并返回已初始化的句柄
 * @param config 音频配置，如果为NULL则使用默认配置
 * @param i2s_port I2S端口号
 * @param bclk_pin 位时钟引脚编号
 * @param lrclk_pin 左右声道时钟引脚编号  
 * @param din_pin 数据输入引脚编号
 * @return esp_err_t 
 */
esp_err_t audio_init(audio_dev_t **dev_ptr, 
                     const audio_config_t *config,
                     i2s_port_t i2s_port,
                     int bclk_pin,
                     int lrclk_pin,
                     int din_pin);

/**
 * @brief 播放音频数据
 * 
 * @param dev 音频设备句柄
 * @param data 音频数据缓冲区
 * @param size 数据大小（字节）
 * @param bytes_written 实际写入的字节数
 * @return esp_err_t 
 */
esp_err_t audio_play(audio_dev_t *dev, const void *data, size_t size, size_t *bytes_written);

/**
 * @brief 播放一段全零 PCM,用于填充播放前后的静音间隔。
 *
 * @param dev    音频设备句柄
 * @param ms     静音时长(毫秒)
 * @return ESP_OK / 错误码
 */
esp_err_t audio_play_silence(audio_dev_t *dev, uint32_t ms);

/**
 * @brief 设置采样率
 * 
 * @param dev 音频设备句柄
 * @param sample_rate 采样率
 * @return esp_err_t 
 */
esp_err_t audio_set_sample_rate(audio_dev_t *dev, uint32_t sample_rate);

/**
 * @brief 获取当前配置
 * 
 * @param dev 音频设备句柄
 * @return const audio_config_t* 
 */
const audio_config_t* audio_get_config(const audio_dev_t *dev);

/**
 * @brief 销毁音频设备，释放资源并置空指针
 * 
 * @param dev_ptr 指向音频设备句柄指针的指针；调用成功后 *dev_ptr 将被置为 NULL
 * @return esp_err_t 
 */
esp_err_t audio_deinit(audio_dev_t **dev_ptr);

/**
 * @brief 获取I2S端口号
 * 
 * @param dev 音频设备句柄
 * @return i2s_port_t 
 */
i2s_port_t audio_get_i2s_port(const audio_dev_t *dev);

/**
 * @brief 占用音频“播放会话”互斥锁。
 *
 * 用于在多个音频源(notify_sound / tts_svc / ...)之间串行化整段播放,
 * 避免 audio_play 之间被其它任务穿插。本锁与 audio_play 内部互斥锁
 * 互不影响 — audio_play 仍按调用粒度自锁。
 *
 * @param dev          音频设备句柄
 * @param timeout_ms   等待超时;portMAX_DELAY 表示无限等待
 * @return ESP_OK / ESP_ERR_TIMEOUT / ESP_ERR_INVALID_ARG
 */
esp_err_t audio_session_acquire(audio_dev_t *dev, uint32_t timeout_ms);

/** 释放播放会话锁。 */
void audio_session_release(audio_dev_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_H */