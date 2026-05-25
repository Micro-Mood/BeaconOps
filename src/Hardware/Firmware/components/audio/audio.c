#include "audio.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include <stdlib.h>

static const char *TAG = "AUDIO";

#if CONFIG_PM_ENABLE
static esp_err_t audio_pm_locks_create(audio_dev_t *dev)
{
    esp_err_t ret = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "audio_apb", &dev->pm_apb_lock);
    if (ret != ESP_OK) return ret;

    ret = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "audio_sleep", &dev->pm_sleep_lock);
    if (ret != ESP_OK) {
        esp_pm_lock_delete(dev->pm_apb_lock);
        dev->pm_apb_lock = NULL;
        return ret;
    }
    return ESP_OK;
}

static esp_err_t audio_pm_locks_acquire(audio_dev_t *dev)
{
    if (!dev->pm_apb_lock || !dev->pm_sleep_lock) return ESP_OK;
    if (dev->pm_locks_acquired) return ESP_OK;

    esp_err_t ret = esp_pm_lock_acquire(dev->pm_apb_lock);
    if (ret != ESP_OK) return ret;

    ret = esp_pm_lock_acquire(dev->pm_sleep_lock);
    if (ret != ESP_OK) {
        esp_pm_lock_release(dev->pm_apb_lock);
        return ret;
    }

    dev->pm_locks_acquired = true;
    return ESP_OK;
}

static void audio_pm_locks_release(audio_dev_t *dev)
{
    if (!dev->pm_locks_acquired) return;
    if (dev->pm_sleep_lock) esp_pm_lock_release(dev->pm_sleep_lock);
    if (dev->pm_apb_lock) esp_pm_lock_release(dev->pm_apb_lock);
    dev->pm_locks_acquired = false;
}

static void audio_pm_locks_delete(audio_dev_t *dev)
{
    audio_pm_locks_release(dev);
    if (dev->pm_sleep_lock) {
        esp_pm_lock_delete(dev->pm_sleep_lock);
        dev->pm_sleep_lock = NULL;
    }
    if (dev->pm_apb_lock) {
        esp_pm_lock_delete(dev->pm_apb_lock);
        dev->pm_apb_lock = NULL;
    }
}
#else
static esp_err_t audio_pm_locks_create(audio_dev_t *dev) { (void)dev; return ESP_OK; }
static esp_err_t audio_pm_locks_acquire(audio_dev_t *dev) { (void)dev; return ESP_OK; }
static void audio_pm_locks_release(audio_dev_t *dev) { (void)dev; }
static void audio_pm_locks_delete(audio_dev_t *dev) { (void)dev; }
#endif

audio_config_t audio_config_default(void)
{
    audio_config_t config = {
        16000,                          // sample_rate
        I2S_BITS_PER_SAMPLE_16BIT,      // bit_depth
        I2S_CHANNEL_MONO,               // channels
        6,                              // dma_buf_count
        256,                            // dma_buf_len
        false,                          // use_apll
        500                             // tx_timeout_ms
    };
    return config;
}

esp_err_t audio_init(audio_dev_t **dev_ptr, 
                     const audio_config_t *config,
                     i2s_port_t i2s_port,
                     int bclk_pin,
                     int lrclk_pin,
                     int din_pin)
{
    esp_err_t ret = ESP_OK;
    bool allocated_here = false;

    if (dev_ptr == NULL) {
        ESP_LOGE(TAG, "设备句柄指针不能为NULL");
        return ESP_ERR_INVALID_ARG;
    }

    audio_dev_t *dev = *dev_ptr;
    if (dev == NULL) {
        dev = (audio_dev_t *)calloc(1, sizeof(audio_dev_t));
        if (dev == NULL) {
            ESP_LOGE(TAG, "分配audio_dev_t失败");
            return ESP_ERR_NO_MEM;
        }
        allocated_here = true;
        *dev_ptr = dev;
    }

    if (bclk_pin < 0 || lrclk_pin < 0 || din_pin < 0) {
        ESP_LOGE(TAG, "引脚编号无效");
        if (allocated_here) {
            free(dev);
            *dev_ptr = NULL;
        }
        return ESP_ERR_INVALID_ARG;
    }

    dev->i2s_port = i2s_port;
    
    if (config != NULL) {
        dev->config = *config;
    } else {
        dev->config = audio_config_default();
    }

    dev->mutex = xSemaphoreCreateMutex();
    if (dev->mutex == NULL) {
        ESP_LOGE(TAG, "创建互斥锁失败");
        if (allocated_here) {
            free(dev);
            *dev_ptr = NULL;
        }
        return ESP_ERR_NO_MEM;
    }
    dev->session_mutex = xSemaphoreCreateMutex();
    if (dev->session_mutex == NULL) {
        ESP_LOGE(TAG, "创建会话锁失败");
        vSemaphoreDelete(dev->mutex);
        if (allocated_here) {
            free(dev);
            *dev_ptr = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    ret = audio_pm_locks_create(dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "创建PM锁失败: %s", esp_err_to_name(ret));
        vSemaphoreDelete(dev->session_mutex);
        vSemaphoreDelete(dev->mutex);
        if (allocated_here) {
            free(dev);
            *dev_ptr = NULL;
        }
        return ret;
    }

    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate = dev->config.sample_rate,
        .bits_per_sample = dev->config.bit_depth,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .dma_buf_count = dev->config.dma_buf_count,
        .dma_buf_len = dev->config.dma_buf_len,
        .use_apll = dev->config.use_apll,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .tx_desc_auto_clear = true,
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = bclk_pin,
        .ws_io_num = lrclk_pin,
        .data_out_num = din_pin,
        .data_in_num = I2S_PIN_NO_CHANGE,
        .mck_io_num = I2S_PIN_NO_CHANGE,
    };

    ret = i2s_driver_install(dev->i2s_port, &i2s_config, 0, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "安装I2S驱动失败: %s", esp_err_to_name(ret));
        audio_pm_locks_delete(dev);
        vSemaphoreDelete(dev->session_mutex);
        vSemaphoreDelete(dev->mutex);
        if (allocated_here) {
            free(dev);
            *dev_ptr = NULL;
        }
        return ret;
    }

    ret = i2s_set_pin(dev->i2s_port, &pin_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "设置I2S引脚失败: %s", esp_err_to_name(ret));
        i2s_driver_uninstall(dev->i2s_port);
        audio_pm_locks_delete(dev);
        vSemaphoreDelete(dev->session_mutex);
        vSemaphoreDelete(dev->mutex);
        if (allocated_here) {
            free(dev);
            *dev_ptr = NULL;
        }
        return ret;
    }

    ret = i2s_set_clk(dev->i2s_port, dev->config.sample_rate, 
                      dev->config.bit_depth, dev->config.channels);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "设置I2S时钟失败: %s", esp_err_to_name(ret));
        i2s_driver_uninstall(dev->i2s_port);
        audio_pm_locks_delete(dev);
        vSemaphoreDelete(dev->session_mutex);
        vSemaphoreDelete(dev->mutex);
        if (allocated_here) {
            free(dev);
            *dev_ptr = NULL;
        }
        return ret;
    }

    dev->initialized = true;
    ESP_LOGI(TAG, "MAX98357音频设备初始化成功");
    ESP_LOGI(TAG, "  采样率: %dHz", dev->config.sample_rate);
    ESP_LOGI(TAG, "  位深度: %d-bit", dev->config.bit_depth);
    ESP_LOGI(TAG, "  声道: %s", dev->config.channels == I2S_CHANNEL_MONO ? "单声道" : "立体声");
    ESP_LOGI(TAG, "  引脚 - BCLK: GPIO%d, LRCK: GPIO%d, DIN: GPIO%d", 
             bclk_pin, lrclk_pin, din_pin);
    
    return ESP_OK;
}

esp_err_t audio_play(audio_dev_t *dev, const void *data, size_t size, size_t *bytes_written)
{
    if (dev == NULL || data == NULL || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!dev->initialized) {
        ESP_LOGE(TAG, "音频设备未初始化");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(dev->mutex, pdMS_TO_TICKS(dev->config.tx_timeout_ms)) != pdTRUE) {
        ESP_LOGE(TAG, "获取播放互斥锁超时");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = i2s_write(dev->i2s_port, data, size, bytes_written, 
                             pdMS_TO_TICKS(dev->config.tx_timeout_ms));

    xSemaphoreGive(dev->mutex);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "播放音频数据失败: %s", esp_err_to_name(ret));
    } else if (bytes_written != NULL && *bytes_written != size) {
        ESP_LOGW(TAG, "部分数据写入: %d/%d 字节", *bytes_written, size);
    }

    return ret;
}

esp_err_t audio_play_silence(audio_dev_t *dev, uint32_t ms)
{
    if (dev == NULL || ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!dev->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t sr   = (uint32_t)dev->config.sample_rate;
    uint32_t ch   = (dev->config.channels == I2S_CHANNEL_MONO) ? 1 : 2;
    uint32_t bps  = (uint32_t)dev->config.bit_depth / 8;
    uint64_t need = (uint64_t)sr * ch * bps * ms / 1000ULL;
    if (need == 0) return ESP_OK;

    uint8_t zero[256] = {0};
    uint64_t left = need;
    while (left > 0) {
        size_t chunk = (left > sizeof(zero)) ? sizeof(zero) : (size_t)left;
        size_t written = 0;
        esp_err_t r = audio_play(dev, zero, chunk, &written);
        if (r != ESP_OK) return r;
        if (written == 0) return ESP_FAIL;
        left -= written;
    }
    return ESP_OK;
}

esp_err_t audio_set_sample_rate(audio_dev_t *dev, uint32_t sample_rate)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!dev->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(dev->mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = i2s_set_sample_rates(dev->i2s_port, sample_rate);
    if (ret == ESP_OK) {
        dev->config.sample_rate = sample_rate;
        ESP_LOGI(TAG, "采样率设置为: %ldHz", sample_rate);
    } else {
        ESP_LOGE(TAG, "设置采样率失败: %s", esp_err_to_name(ret));
    }

    xSemaphoreGive(dev->mutex);
    return ret;
}

const audio_config_t* audio_get_config(const audio_dev_t *dev)
{
    if (dev == NULL || !dev->initialized) {
        return NULL;
    }
    return &dev->config;
}

i2s_port_t audio_get_i2s_port(const audio_dev_t *dev)
{
    if (dev == NULL) {
        return I2S_NUM_0; // 返回端口号0作为默认值
    }
    return dev->i2s_port;
}

esp_err_t audio_session_acquire(audio_dev_t *dev, uint32_t timeout_ms)
{
    if (dev == NULL || dev->session_mutex == NULL) return ESP_ERR_INVALID_ARG;
    TickType_t to = (timeout_ms == portMAX_DELAY) ? portMAX_DELAY
                                                  : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(dev->session_mutex, to) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = audio_pm_locks_acquire(dev);
    if (ret != ESP_OK) {
        xSemaphoreGive(dev->session_mutex);
        return ret;
    }
    return ESP_OK;
}

void audio_session_release(audio_dev_t *dev)
{
    if (dev == NULL || dev->session_mutex == NULL) return;
    audio_pm_locks_release(dev);
    xSemaphoreGive(dev->session_mutex);
}

esp_err_t audio_deinit(audio_dev_t **dev_ptr)
{
    if (dev_ptr == NULL || *dev_ptr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    audio_dev_t *dev = *dev_ptr;

    if (!dev->initialized) {
        // 即便未初始化，也释放资源（互斥锁等）并置空指针
        if (dev->session_mutex) {
            vSemaphoreDelete(dev->session_mutex);
            dev->session_mutex = NULL;
        }
        if (dev->mutex) {
            vSemaphoreDelete(dev->mutex);
            dev->mutex = NULL;
        }
        free(dev);
        *dev_ptr = NULL;
        return ESP_OK;
    }

    if (xSemaphoreTake(dev->mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    audio_pm_locks_release(dev);
    esp_err_t ret = i2s_driver_uninstall(dev->i2s_port);
    if (ret == ESP_OK) {
        dev->initialized = false;
        ESP_LOGI(TAG, "音频设备去初始化成功");
    } else {
        ESP_LOGE(TAG, "卸载I2S驱动失败: %s", esp_err_to_name(ret));
    }

    xSemaphoreGive(dev->mutex);
    audio_pm_locks_delete(dev);
    vSemaphoreDelete(dev->mutex);
    dev->mutex = NULL;
    if (dev->session_mutex) {
        vSemaphoreDelete(dev->session_mutex);
        dev->session_mutex = NULL;
    }

    free(dev);
    *dev_ptr = NULL;
    
    return ret;
}